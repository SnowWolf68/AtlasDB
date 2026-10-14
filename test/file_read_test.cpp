#include "util.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
enum class Mode { Normal, ShortReads, Interleaved };
Mode mode = Mode::Normal;
int watched_fd = -1;
int pread_calls = 0;
thread_local bool pause_reader = false;
std::mutex gate;
std::condition_variable changed;
bool reader_paused = false;
bool other_read_finished = false;

void check(bool condition, const char *expression) {
	if (!condition) { throw std::runtime_error(expression); }
}
#define CHECK(expression) check((expression), #expression)

bool pause_for_other_read() {
	std::unique_lock<std::mutex> lock(gate);
	reader_paused = true;
	changed.notify_all();
	return changed.wait_for(lock, std::chrono::seconds(3),
							[] { return other_read_finished; });
}

std::vector<std::uint8_t> contents() {
	std::vector<std::uint8_t> result(256);
	for (std::size_t i = 0; i < result.size(); ++i) {
		result[i] = static_cast<std::uint8_t>(i);
	}
	return result;
}

void cursor_preserved(const std::string &path) {
	auto data = contents();
	auto file = FileObj::create_and_write(path, data);
	file.close();
	int fd = ::open(path.c_str(), O_RDONLY);
	CHECK(fd >= 0);
	FileObj reader(path, fd);
	CHECK(::lseek(fd, 42, SEEK_SET) == 42);
	auto actual = reader.read(13, 100);
	CHECK(actual == std::vector<std::uint8_t>(data.begin() + 13, data.begin() + 113));
	CHECK(::lseek(fd, 0, SEEK_CUR) == 42);
}

void short_reads_and_eintr(const std::string &path) {
	auto data = contents();
	auto file = FileObj::create_and_write(path, data);
	file.close();
	int fd = ::open(path.c_str(), O_RDONLY);
	CHECK(fd >= 0);
	FileObj reader(path, fd);
	watched_fd = fd;
	mode = Mode::ShortReads;
	auto actual = reader.read(13, 100);
	CHECK(pread_calls > 2);
	CHECK(actual == std::vector<std::uint8_t>(data.begin() + 13, data.begin() + 113));
}

void concurrent_positions(const std::string &path) {
	auto data = contents();
	auto file = FileObj::create_and_write(path, data);
	file.close();
	int fd = ::open(path.c_str(), O_RDONLY);
	CHECK(fd >= 0);
	FileObj reader(path, fd);
	watched_fd = fd;
	mode = Mode::Interleaved;
	std::vector<std::uint8_t> first, second;
	std::exception_ptr worker_error, main_error;
	std::thread worker([&] {
		pause_reader = true;
		try { first = reader.read(0, 64); }
		catch (...) { worker_error = std::current_exception(); }
	});
	try {
		{
			std::unique_lock<std::mutex> lock(gate);
			CHECK(changed.wait_for(lock, std::chrono::seconds(2),
								  [] { return reader_paused; }));
		}
		second = reader.read(128, 64);
	} catch (...) { main_error = std::current_exception(); }
	{
		std::lock_guard<std::mutex> lock(gate);
		other_read_finished = true;
	}
	changed.notify_all();
	worker.join();
	if (worker_error) { std::rethrow_exception(worker_error); }
	if (main_error) { std::rethrow_exception(main_error); }
	CHECK(first == std::vector<std::uint8_t>(data.begin(), data.begin() + 64));
	CHECK(second == std::vector<std::uint8_t>(data.begin() + 128, data.begin() + 192));
}
} // namespace

extern "C" off_t __real_lseek(int, off_t, int);
extern "C" ssize_t __real_pread(int, void *, std::size_t, off_t);

// Pause after the old seek, or before a positional read, so another thread
// completes its read first. This exposes shared-cursor interference reliably.
extern "C" off_t __wrap_lseek(int fd, off_t offset, int whence) {
	off_t result = __real_lseek(fd, offset, whence);
	if (fd == watched_fd && mode == Mode::Interleaved && pause_reader && result >= 0) {
		if (!pause_for_other_read()) { errno = ETIMEDOUT; return -1; }
	}
	return result;
}

extern "C" ssize_t __wrap_pread(int fd, void *buffer, std::size_t count, off_t offset) {
	if (fd == watched_fd && mode == Mode::ShortReads) {
		if (++pread_calls == 1) { errno = EINTR; return -1; }
		count = std::min(count, std::size_t{7});
	}
	if (fd == watched_fd && mode == Mode::Interleaved && pause_reader) {
		if (!pause_for_other_read()) { errno = ETIMEDOUT; return -1; }
	}
	return __real_pread(fd, buffer, count, offset);
}

int main() {
	struct Case { const char *name; void (*run)(const std::string &); };
	const Case cases[] = {
		{"positional read preserves cursor", cursor_preserved},
		{"short reads and EINTR preserve content", short_reads_and_eintr},
		{"same fd concurrent positions", concurrent_positions},
	};
	int failures = 0;
	for (const auto &test : cases) {
		std::cout.flush();
		char directory[] = "/tmp/atlasdb-file-read-XXXXXX";
		if (!::mkdtemp(directory)) { return 2; }
		pid_t child = ::fork();
		if (child < 0) {
			std::filesystem::remove_all(directory);
			return 2;
		}
		if (child == 0) {
			::alarm(6);
			try {
				test.run(std::string(directory) + "/data.bin");
				::_exit(0);
			} catch (const std::exception &error) {
				std::cerr << test.name << ": " << error.what() << '\n';
				::_exit(1);
			}
		}
		int status = 0;
		pid_t waited;
		do { waited = ::waitpid(child, &status, 0); }
		while (waited == -1 && errno == EINTR);
		std::filesystem::remove_all(directory);
		if (waited != child) { return 2; }
		bool passed = WIFEXITED(status) && WEXITSTATUS(status) == 0;
		std::cout << (passed ? "[PASS] " : "[FAIL] ") << test.name << '\n';
		if (!passed) { ++failures; }
	}
	return failures ? 1 : 0;
}
