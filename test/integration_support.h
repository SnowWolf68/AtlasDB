#pragma once

// Linux-only integration tests: isolate both hangs and ./data in a subprocess.
// The parent owns directory cleanup, including when a child times out.
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace integration {
using namespace std::chrono_literals;
inline void check(bool ok, const char *expr, int line) {
	if (!ok) { throw std::runtime_error("line " + std::to_string(line) + ": " + expr); }
}
#define CHECK(expr) ::integration::check((expr), #expr, __LINE__)

template<class T> T await(std::future<T> &future) {
	CHECK(future.wait_for(5s) == std::future_status::ready);
	return future.get();
}

inline std::atomic<bool> fail_wal_write{false};
inline std::atomic<int> injected_errors{0};
inline std::string wal_prefix;

struct Case { const char *name; std::function<void()> run; };
inline int run(const std::vector<Case> &cases, int argc, char **argv) {
	std::size_t selected = 0, passed = 0;
	for (const auto &test : cases) {
		if (argc > 1 && test.name != std::string(argv[1])) { continue; }
		++selected;
		char pattern[] = "/tmp/atlas-integration-XXXXXX";
		const char *created = ::mkdtemp(pattern);
		CHECK(created != nullptr);
		const std::string directory = created;
		std::cout.flush(); std::cerr.flush();
		const pid_t child = ::fork();
		CHECK(child >= 0);
		if (child == 0) {
			::alarm(20);
			try {
				std::filesystem::current_path(directory);
				wal_prefix = directory + "/data/wal-";
				test.run();
				std::cout.flush(); std::cerr.flush(); ::_exit(0);
			} catch (const std::exception &error) {
				std::cerr << "[FAIL] " << test.name << ": " << error.what() << std::endl;
				::_exit(1);
			}
		}
		int status = 0;
		pid_t waited;
		do { waited = ::waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
		std::filesystem::remove_all(directory);
		if (waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
			++passed;
			std::cout << "[PASS] " << test.name << '\n';
		} else {
			std::cerr << "[FAIL] " << test.name << ": child status " << status << '\n';
		}
	}
	std::cout << passed << '/' << selected << " tests passed\n";
	return selected && selected == passed ? 0 : 1;
}
} // namespace integration

// Only WAL files in this case's temporary data directory can fail. In
// particular, eventfd wakeups, stdout and unrelated tests are unaffected.
extern "C" ssize_t __real_pwrite(int, const void *, size_t, off_t);
extern "C" ssize_t __wrap_pwrite(int fd, const void *data, size_t size, off_t offset) {
	if (integration::fail_wal_write.load()) {
		char target[4096];
		const auto link = "/proc/self/fd/" + std::to_string(fd);
		const auto n = ::readlink(link.c_str(), target, sizeof(target));
		if (n > 0 && std::string(target, static_cast<std::size_t>(n)).rfind(
			integration::wal_prefix, 0) == 0 && integration::fail_wal_write.exchange(false)) {
			++integration::injected_errors;
			errno = EIO;
			return -1;
		}
	}
	return __real_pwrite(fd, data, size, offset);
}
