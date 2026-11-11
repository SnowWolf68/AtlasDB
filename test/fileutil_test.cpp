#include "io/FileUtil.h"
#include "io/FileSystem.h"
#include "log.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

extern "C" int __real_open(const char *, int, ...);
extern "C" ssize_t __real_pread(int, void *, std::size_t, off_t);
extern "C" ssize_t __real_pwrite(int, const void *, std::size_t, off_t);
extern "C" int __real_fstat(int, struct stat *);
extern "C" int __real_fsync(int);
extern "C" int __real_unlink(const char *);
extern "C" int __real_close(int);

namespace {

namespace fs = std::filesystem;
using atlas::io::AccessMode;
using atlas::io::FileObj;
using atlas::io::append_all;
using atlas::io::atomic_write_all_replace;
using atlas::io::read_exact_at;
using atlas::io::write_all_at;
using Bytes = std::vector<std::uint8_t>;

void check(bool condition, const char *expression, int line) {
	if (condition) { return; }
	std::ostringstream message;
	message << "line " << line << ": check failed: " << expression;
	throw std::runtime_error(message.str());
}

#define CHECK(expression) check((expression), #expression, __LINE__)

template <typename Operation>
std::string expect_error(Operation operation) {
	try { operation(); }
	catch (const std::exception &error) { return error.what(); }
	throw std::runtime_error("expected an exception");
}

class OwnedFd {
public:
	explicit OwnedFd(int fd) : fd_(fd) { CHECK(fd_ >= 0); }
	OwnedFd(const OwnedFd &) = delete;
	OwnedFd &operator=(const OwnedFd &) = delete;
	~OwnedFd() { if (fd_ >= 0) { __real_close(fd_); } }
	int get() const { return fd_; }
private:
	int fd_;
};

class TempDirectory {
public:
	TempDirectory() {
		char name[] = "/tmp/atlasdb-fileutil-XXXXXX";
		CHECK(::mkdtemp(name) != nullptr);
		path_ = name;
	}
	~TempDirectory() {
		std::error_code ignored;
		fs::remove_all(path_, ignored);
	}
	const std::string &path() const { return path_; }
private:
	std::string path_;
};

class WorkingDirectory {
public:
	explicit WorkingDirectory(const std::string &path)
		: original_(__real_open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC)) {
		CHECK(::chdir(path.c_str()) == 0);
	}
	~WorkingDirectory() { ::fchdir(original_.get()); }
private:
	OwnedFd original_;
};

// Fixtures and observations use real syscalls, independent of the utility
// under test and the injected short-transfer/error paths.
void seed(const std::string &path, const Bytes &data) {
	OwnedFd fd(__real_open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600));
	std::size_t done = 0;
	while (done < data.size()) {
		const ssize_t count = __real_pwrite(fd.get(), data.data() + done,
			data.size() - done, static_cast<off_t>(done));
		if (count == -1 && errno == EINTR) { continue; }
		CHECK(count > 0);
		done += static_cast<std::size_t>(count);
	}
}

Bytes read_fd(int fd) {
	Bytes result;
	std::array<std::uint8_t, 256> buffer {};
	while (true) {
		const ssize_t count = __real_pread(fd, buffer.data(), buffer.size(),
			static_cast<off_t>(result.size()));
		if (count == -1 && errno == EINTR) { continue; }
		CHECK(count >= 0);
		if (count == 0) { return result; }
		result.insert(result.end(), buffer.begin(), buffer.begin() + count);
	}
}

Bytes read_path(const std::string &path) {
	OwnedFd fd(__real_open(path.c_str(), O_RDONLY));
	return read_fd(fd.get());
}

std::size_t descriptor_count() {
	std::size_t result = 0;
	for (const auto &entry : fs::directory_iterator("/proc/self/fd")) {
		(void)entry;
		++result;
	}
	return result;
}

class Fixture {
public:
	Fixture(const std::string &path, const Bytes &initial) : path(path) {
		seed(path, initial);
		fd = __real_open(path.c_str(), O_RDWR | O_CLOEXEC);
		CHECK(fd >= 0);
		file = FileObj(fd, path, AccessMode::ReadWrite);
	}
	std::string path;
	int fd = -1; // Non-owning observation; file owns this descriptor.
	FileObj file;
};

struct IoPlan {
	std::size_t chunk = std::numeric_limits<std::size_t>::max();
	unsigned interruptions = 0;
	int stop_after = -1;
	int error = EIO;
	bool return_zero = false;
	unsigned calls = 0;
	unsigned successful_calls = 0;
};

enum class Event { TempOpen, TempSync, DirectoryOpen, DirectorySync, TempUnlink };
enum class ExitPoint { None, AfterPartialWrite, AfterTempSync, BeforeDirectorySync };
constexpr int kInjectedExit = 73;

struct Faults {
	Faults(const std::string &target, const std::string &parent, int fd)
		: target(target), temporary(target + ".tmp"), parent(parent), data_fd(fd) {}
	std::string target;
	std::string temporary;
	std::string parent;
	int data_fd;
	int temp_fd = -1;
	int directory_fd = -1;
	IoPlan reads;
	IoPlan writes;
	int temp_open_error = 0;
	int directory_open_error = 0;
	int temp_sync_error = 0;
	int directory_sync_error = 0;
	int data_stat_error = 0;
	int temp_stat_error = 0;
	int unlink_error = 0;
	bool unlink_disappeared = false;
	bool remove_temp_after_sync = false;
	ExitPoint exit_point = ExitPoint::None;
	unsigned temp_open_calls = 0;
	unsigned directory_open_calls = 0;
	unsigned temp_sync_calls = 0;
	unsigned directory_sync_calls = 0;
	unsigned other_sync_calls = 0;
	unsigned data_stat_calls = 0;
	unsigned temp_stat_calls = 0;
	unsigned unlink_calls = 0;
	std::vector<Event> events;
	bool inspect_publication = false;
	Bytes old_contents;
	Bytes new_contents;
	bool old_visible_before_file_sync = false;
	bool complete_temp_before_file_sync = false;
	bool new_visible_before_directory_sync = false;
	bool temp_absent_before_directory_sync = false;
};

// Wrapping is local to this executable, one fixture, and the current thread.
thread_local Faults *active_faults = nullptr;

class Inject {
public:
	Inject(const std::string &target, const std::string &parent, int fd)
		: state(target, parent, fd), previous_(active_faults) { active_faults = &state; }
	Inject(const Inject &) = delete;
	Inject &operator=(const Inject &) = delete;
	~Inject() { active_faults = previous_; }
	Faults state;
private:
	Faults *previous_;
};

bool matches_parent(const Faults &faults, const char *path) {
	return faults.parent == path ||
		(faults.parent == "." && std::string(path) == "./");
}

bool watched_io(int fd) {
	return active_faults &&
		(fd == active_faults->data_fd || fd == active_faults->temp_fd);
}

bool forced_result(IoPlan &plan, ssize_t &result) {
	++plan.calls;
	if (plan.interruptions > 0) {
		--plan.interruptions;
		errno = EINTR;
		result = -1;
		return true;
	}
	if (plan.stop_after >= 0 &&
		plan.successful_calls >= static_cast<unsigned>(plan.stop_after)) {
		if (plan.return_zero) { result = 0; }
		else { errno = plan.error; result = -1; }
		return true;
	}
	return false;
}

void test_exact_read_offset_and_binary(const std::string &dir) {
	Fixture fixture(dir + "/read", Bytes{0, 0x80, 0xff, 0x7f, 9});
	CHECK(::lseek(fixture.fd, 4, SEEK_SET) == 4);
	Bytes buffer(3, 0x55);
	buffer.reserve(32); // size, not capacity, is the requested length.
	Inject injection(fixture.path, dir, fixture.fd);
	read_exact_at(fixture.file, buffer, 1);
	CHECK((buffer == Bytes{0x80, 0xff, 0x7f}));
	CHECK(::lseek(fixture.fd, 0, SEEK_CUR) == 4);
	CHECK(fixture.file.is_open());
}

void test_exact_read_eof_throws(const std::string &dir) {
	Fixture fixture(dir + "/eof", Bytes{1, 2, 3});
	Bytes buffer(3);
	expect_error([&] { read_exact_at(fixture.file, buffer, 1); });
	expect_error([&] { read_exact_at(fixture.file, buffer, 3); });
	expect_error([&] { read_exact_at(fixture.file, buffer, 100); });
	CHECK(fixture.file.is_open());
}

void test_exact_read_short_and_eintr(const std::string &dir) {
	Fixture fixture(dir + "/short-read", Bytes{0, 1, 2, 0x80, 0xff, 5, 6});
	Inject injection(fixture.path, dir, fixture.fd);
	injection.state.reads.chunk = 2;
	injection.state.reads.interruptions = 2;
	Bytes buffer(5);
	read_exact_at(fixture.file, buffer, 1);
	CHECK((buffer == Bytes{1, 2, 0x80, 0xff, 5}));
	CHECK(injection.state.reads.successful_calls == 3);
	CHECK(injection.state.reads.calls == 5);
}

void test_exact_read_eagain_throws(const std::string &dir) {
	for (int progress : {0, 1}) {
		Fixture fixture(dir + "/read-eagain", Bytes{1, 2, 3, 4, 5});
		Inject injection(fixture.path, dir, fixture.fd);
		injection.state.reads.chunk = 2;
		injection.state.reads.stop_after = progress;
		injection.state.reads.error = EAGAIN;
		Bytes buffer(5);
		expect_error([&] { read_exact_at(fixture.file, buffer, 0); });
		CHECK(injection.state.reads.successful_calls == static_cast<unsigned>(progress));
		CHECK(read_path(fixture.path) == (Bytes{1, 2, 3, 4, 5}));
	}
}

void test_write_all_preserves_neighbors(const std::string &dir) {
	Fixture fixture(dir + "/write", Bytes{1, 2, 3, 4, 5, 6});
	CHECK(::lseek(fixture.fd, 5, SEEK_SET) == 5);
	const Bytes input{0, 0xff, 0x80};
	Inject injection(fixture.path, dir, fixture.fd);
	write_all_at(fixture.file, input, 1);
	CHECK(read_path(fixture.path) == (Bytes{1, 0, 0xff, 0x80, 5, 6}));
	CHECK(input == (Bytes{0, 0xff, 0x80}));
	CHECK(::lseek(fixture.fd, 0, SEEK_CUR) == 5);
	CHECK(injection.state.other_sync_calls == 0);
	CHECK(fixture.file.is_open());
}

void test_write_all_short_and_eintr(const std::string &dir) {
	Fixture fixture(dir + "/short-write", Bytes{9, 9, 9, 9, 9, 9, 9});
	Inject injection(fixture.path, dir, fixture.fd);
	injection.state.writes.chunk = 2;
	injection.state.writes.interruptions = 2;
	write_all_at(fixture.file, Bytes{0, 1, 2, 0xff, 0x80}, 1);
	CHECK(read_path(fixture.path) == (Bytes{9, 0, 1, 2, 0xff, 0x80, 9}));
	CHECK(injection.state.writes.successful_calls == 3);
	CHECK(injection.state.writes.calls == 5);
}

void test_write_all_eagain_throws(const std::string &dir) {
	for (int progress : {0, 1}) {
		Fixture fixture(dir + "/write-eagain", Bytes{9, 9, 9, 9, 9, 9});
		Inject injection(fixture.path, dir, fixture.fd);
		injection.state.writes.chunk = 2;
		injection.state.writes.stop_after = progress;
		injection.state.writes.error = EAGAIN;
		expect_error([&] { write_all_at(fixture.file, Bytes{1, 2, 3, 4}, 1); });
		CHECK(injection.state.writes.successful_calls == static_cast<unsigned>(progress));
		const Bytes expected = progress ? Bytes{9, 1, 2, 9, 9, 9} : Bytes(6, 9);
		CHECK(read_path(fixture.path) == expected); // Failed writes need not roll back.
	}
}

void test_append_all_binary_and_repeated(const std::string &dir) {
	Fixture fixture(dir + "/append", Bytes{1, 2});
	CHECK(::lseek(fixture.fd, 1, SEEK_SET) == 1);
	Inject injection(fixture.path, dir, fixture.fd);
	append_all(fixture.file, Bytes{0, 0xff});
	append_all(fixture.file, Bytes{0x80, 3});
	CHECK(read_path(fixture.path) == (Bytes{1, 2, 0, 0xff, 0x80, 3}));
	CHECK(::lseek(fixture.fd, 0, SEEK_CUR) == 1);
	CHECK(injection.state.other_sync_calls == 0);
	CHECK(fixture.file.is_open());
}

void test_append_all_short_and_eintr(const std::string &dir) {
	Fixture fixture(dir + "/short-append", Bytes{9});
	Inject injection(fixture.path, dir, fixture.fd);
	injection.state.writes.chunk = 2;
	injection.state.writes.interruptions = 2;
	append_all(fixture.file, Bytes{0, 1, 2, 0xff, 0x80});
	CHECK(read_path(fixture.path) == (Bytes{9, 0, 1, 2, 0xff, 0x80}));
	CHECK(injection.state.writes.successful_calls == 3);
	CHECK(injection.state.writes.calls == 5);
}

void test_append_all_eagain_throws(const std::string &dir) {
	for (int progress : {0, 1}) {
		Fixture fixture(dir + "/append-eagain", Bytes{9});
		Inject injection(fixture.path, dir, fixture.fd);
		injection.state.writes.chunk = 2;
		injection.state.writes.stop_after = progress;
		injection.state.writes.error = EAGAIN;
		expect_error([&] { append_all(fixture.file, Bytes{1, 2, 3, 4}); });
		CHECK(injection.state.writes.successful_calls == static_cast<unsigned>(progress));
		CHECK(read_path(fixture.path) == (progress ? Bytes{9, 1, 2} : Bytes{9}));
	}
}

void test_zero_length_helpers(const std::string &dir) {
	Fixture fixture(dir + "/zero", Bytes{1, 2, 3});
	Inject injection(fixture.path, dir, fixture.fd);
	Bytes empty;
	read_exact_at(fixture.file, empty, 100);
	write_all_at(fixture.file, empty, 100);
	append_all(fixture.file, empty);
	CHECK(empty.empty());
	CHECK(read_path(fixture.path) == (Bytes{1, 2, 3}));
	CHECK(injection.state.reads.calls == 0);
	CHECK(injection.state.writes.calls == 0);
	CHECK(injection.state.other_sync_calls == 0);
}

void test_closed_helpers_reject(const std::string &dir) {
	Fixture fixture(dir + "/closed", Bytes{1});
	fixture.file.close();
	for (std::size_t size : {std::size_t{0}, std::size_t{1}}) {
		Bytes buffer(size);
		expect_error([&] { read_exact_at(fixture.file, buffer, 0); });
		expect_error([&] { write_all_at(fixture.file, buffer, 0); });
		expect_error([&] { append_all(fixture.file, buffer); });
	}
	CHECK(read_path(fixture.path) == Bytes{1});
}

void test_io_errors_propagate(const std::string &dir) {
	Fixture fixture(dir + "/io-errors", Bytes{9, 9, 9});
	Inject injection(fixture.path, dir, fixture.fd);
	injection.state.reads.stop_after = 0;
	injection.state.writes.stop_after = 0;
	Bytes buffer(2);
	expect_error([&] { read_exact_at(fixture.file, buffer, 0); });
	expect_error([&] { write_all_at(fixture.file, Bytes{1, 2}, 0); });
	expect_error([&] { append_all(fixture.file, Bytes{1, 2}); });
	CHECK(injection.state.reads.calls == 1);
	CHECK(injection.state.writes.calls == 2);
	CHECK(read_path(fixture.path) == (Bytes{9, 9, 9}));
	CHECK(fixture.file.is_open());
}

void test_exact_read_partial_error_and_retry(const std::string &dir) {
	const Bytes original{1, 2, 0x80, 0xff, 5};
	Fixture fixture(dir + "/partial-read-error", original);
	Inject injection(fixture.path, dir, fixture.fd);
	injection.state.reads.chunk = 2;
	injection.state.reads.stop_after = 1;
	Bytes buffer(5);
	expect_error([&] { read_exact_at(fixture.file, buffer, 0); });
	CHECK(injection.state.reads.successful_calls == 1);
	CHECK(read_path(fixture.path) == original);
	injection.state.reads = {};
	read_exact_at(fixture.file, buffer, 0);
	CHECK(buffer == original);
}

void test_write_all_partial_error_and_retry(const std::string &dir) {
	Fixture fixture(dir + "/partial-write-error", Bytes(5, 9));
	Inject injection(fixture.path, dir, fixture.fd);
	injection.state.writes.chunk = 2;
	injection.state.writes.stop_after = 1;
	expect_error([&] { write_all_at(fixture.file, Bytes{1, 2, 3, 4}, 1); });
	CHECK(injection.state.writes.successful_calls == 1);
	CHECK(read_path(fixture.path) == (Bytes{9, 1, 2, 9, 9}));
	injection.state.writes = {};
	write_all_at(fixture.file, Bytes{1, 2, 3, 4}, 1);
	CHECK(read_path(fixture.path) == (Bytes{9, 1, 2, 3, 4}));
}

void test_append_all_partial_error_and_retry(const std::string &dir) {
	Fixture fixture(dir + "/partial-append-error", Bytes{9});
	Inject injection(fixture.path, dir, fixture.fd);
	injection.state.writes.chunk = 2;
	injection.state.writes.stop_after = 1;
	expect_error([&] { append_all(fixture.file, Bytes{1, 2, 3, 4}); });
	CHECK(injection.state.writes.successful_calls == 1);
	CHECK(read_path(fixture.path) == (Bytes{9, 1, 2}));
	injection.state.writes = {};
	append_all(fixture.file, Bytes{3, 4});
	CHECK(read_path(fixture.path) == (Bytes{9, 1, 2, 3, 4}));
}

void test_append_size_error_does_not_write_and_can_retry(const std::string &dir) {
	Fixture fixture(dir + "/append-stat-error", Bytes{9});
	Inject injection(fixture.path, dir, fixture.fd);
	injection.state.data_stat_error = EIO;
	expect_error([&] { append_all(fixture.file, Bytes{1, 2}); });
	CHECK(injection.state.data_stat_calls == 1);
	CHECK(injection.state.writes.calls == 0);
	CHECK(read_path(fixture.path) == Bytes{9});
	injection.state.data_stat_error = 0;
	append_all(fixture.file, Bytes{1, 2});
	CHECK(read_path(fixture.path) == (Bytes{9, 1, 2}));
	CHECK(injection.state.other_sync_calls == 0);
}

void test_atomic_publication_order_and_binary(const std::string &dir) {
	const Bytes old_data(20, 9);
	const Bytes new_data{0, 0x80, 0xff, 7};
	Fixture fixture(dir + "/publish", old_data);
	OwnedFd existing_reader(__real_open(fixture.path.c_str(), O_RDONLY));
	const auto count = descriptor_count();
	Inject injection(fixture.path, dir, fixture.fd);
	auto &state = injection.state;
	state.inspect_publication = true;
	state.old_contents = old_data;
	state.new_contents = new_data;
	state.writes.chunk = 2;
	state.writes.interruptions = 2;
	atomic_write_all_replace(fixture.file, new_data);
	CHECK(state.writes.successful_calls == 2 && state.writes.calls == 4);
	CHECK(read_path(fixture.path) == new_data);
	CHECK(!fs::exists(state.temporary));
	CHECK(read_fd(existing_reader.get()) == old_data);
	CHECK(state.old_visible_before_file_sync);
	CHECK(state.complete_temp_before_file_sync);
	CHECK(state.new_visible_before_directory_sync);
	CHECK(state.temp_absent_before_directory_sync);
	CHECK(state.temp_sync_calls > 0 && state.directory_sync_calls > 0);
	const auto file_sync = std::find(state.events.begin(), state.events.end(), Event::TempSync);
	const auto directory_open = std::find(state.events.begin(), state.events.end(), Event::DirectoryOpen);
	const auto directory_sync = std::find(state.events.begin(), state.events.end(), Event::DirectorySync);
	CHECK(file_sync < directory_open && directory_open < directory_sync);
	CHECK(state.temp_fd == -1 && state.directory_fd == -1);
	CHECK(descriptor_count() == count);
}

void test_atomic_bare_filename(const std::string &dir) {
	WorkingDirectory cwd(dir);
	Fixture fixture("bare", Bytes{9, 9, 9});
	Inject injection(fixture.path, ".", fixture.fd);
	atomic_write_all_replace(fixture.file, Bytes{0, 0xff});
	CHECK(read_path("bare") == (Bytes{0, 0xff}));
	CHECK(!fs::exists("bare.tmp"));
	CHECK(injection.state.directory_open_calls > 0);
	CHECK(injection.state.directory_sync_calls > 0);
}

void test_atomic_relative_parent(const std::string &dir) {
	WorkingDirectory cwd(dir);
	CHECK(fs::create_directory("nested"));
	Fixture fixture("nested/data", Bytes{9, 9});
	Inject injection(fixture.path, "nested", fixture.fd);
	atomic_write_all_replace(fixture.file, Bytes{0x80});
	CHECK(read_path("nested/data") == Bytes{0x80});
	CHECK(!fs::exists("nested/data.tmp"));
	CHECK(injection.state.directory_sync_calls > 0);
}

void test_atomic_reuses_stale_temp(const std::string &dir) {
	Fixture fixture(dir + "/stale", Bytes{9, 9});
	seed(fixture.path + ".tmp", Bytes(64, 0xff));
	atomic_write_all_replace(fixture.file, Bytes{0, 0x80, 7});
	CHECK(read_path(fixture.path) == (Bytes{0, 0x80, 7}));
	CHECK(!fs::exists(fixture.path + ".tmp"));
}

void test_atomic_empty_reuses_stale_temp(const std::string &dir) {
	Fixture fixture(dir + "/empty", Bytes{9, 9, 9});
	seed(fixture.path + ".tmp", Bytes(64, 0xff));
	Inject injection(fixture.path, dir, fixture.fd);
	atomic_write_all_replace(fixture.file, {});
	CHECK(read_path(fixture.path).empty());
	CHECK(!fs::exists(fixture.path + ".tmp"));
	CHECK(injection.state.writes.calls == 0);
	CHECK(injection.state.temp_sync_calls > 0);
	CHECK(injection.state.directory_sync_calls > 0);
}

void test_atomic_repeated_replacements_release_handles(const std::string &dir) {
	Fixture fixture(dir + "/repeat", Bytes{9, 9, 9});
	const auto count = descriptor_count();
	for (std::uint8_t iteration = 0; iteration < 12; ++iteration) {
		seed(fixture.path + ".tmp", Bytes(32, 0xff));
		const Bytes replacement = iteration % 2 ? Bytes{} : Bytes{iteration, 0, 0x80};
		atomic_write_all_replace(fixture.file, replacement);
		CHECK(read_path(fixture.path) == replacement);
		CHECK(!fs::exists(fixture.path + ".tmp"));
		CHECK(descriptor_count() == count);
	}
}

void test_atomic_publishes_when_target_name_is_absent(const std::string &dir) {
	unsigned index = 0;
	for (const Bytes &replacement : {Bytes{1, 0x80, 0xff}, Bytes{}}) {
		Fixture fixture(dir + "/new-name-" + std::to_string(index++), Bytes{9});
		// The supplied file is still open, but publication must create its
		// now-absent target name. This does not impose a closed-file contract.
		CHECK(__real_unlink(fixture.path.c_str()) == 0);
		CHECK(!fs::exists(fixture.path));
		const auto count = descriptor_count();
		atomic_write_all_replace(fixture.file, replacement);
		CHECK(read_path(fixture.path) == replacement);
		CHECK(!fs::exists(fixture.path + ".tmp"));
		CHECK(descriptor_count() == count);
	}
}

void test_atomic_temp_open_failure(const std::string &dir) {
	const Bytes original{9, 9, 9};
	const Bytes stale{0xff, 0x80};
	Fixture fixture(dir + "/open-failure", original);
	seed(fixture.path + ".tmp", stale);
	const auto count = descriptor_count();
	Inject injection(fixture.path, dir, fixture.fd);
	injection.state.temp_open_error = EACCES;
	expect_error([&] { atomic_write_all_replace(fixture.file, Bytes{1, 2}); });
	CHECK(injection.state.temp_open_calls == 1);
	CHECK(injection.state.writes.calls == 0);
	CHECK(injection.state.directory_open_calls == 0);
	CHECK(injection.state.unlink_calls == 0);
	CHECK(read_path(fixture.path) == original);
	CHECK(read_path(fixture.path + ".tmp") == stale);
	CHECK(descriptor_count() == count);
}

void test_atomic_write_failure_cleans_temp(const std::string &dir) {
	const Bytes original{9, 9, 9};
	Fixture fixture(dir + "/write-failure", original);
	const auto count = descriptor_count();
	Inject injection(fixture.path, dir, fixture.fd);
	injection.state.writes.chunk = 2;
	injection.state.writes.stop_after = 1;
	injection.state.writes.error = ENOSPC;
	expect_error([&] { atomic_write_all_replace(fixture.file, Bytes{1, 2, 3, 4}); });
	CHECK(injection.state.writes.successful_calls == 1);
	CHECK(injection.state.writes.calls == 2);
	CHECK(injection.state.temp_sync_calls == 0);
	CHECK(injection.state.directory_open_calls == 0);
	CHECK(injection.state.unlink_calls == 1);
	CHECK(read_path(fixture.path) == original);
	CHECK(!fs::exists(fixture.path + ".tmp"));
	CHECK(descriptor_count() == count);
}

void test_atomic_incomplete_write_is_not_published(const std::string &dir) {
	for (int progress : {0, 1}) {
		Fixture fixture(dir + "/incomplete", Bytes{9, 9, 9});
		const auto count = descriptor_count();
		Inject injection(fixture.path, dir, fixture.fd);
		injection.state.writes.chunk = 2;
		injection.state.writes.stop_after = progress;
		injection.state.writes.error = EAGAIN;
		expect_error([&] { atomic_write_all_replace(fixture.file, Bytes{1, 2, 3, 4}); });
		CHECK(injection.state.writes.successful_calls == static_cast<unsigned>(progress));
		CHECK(injection.state.temp_sync_calls == 0);
		CHECK(injection.state.directory_open_calls == 0);
		CHECK(read_path(fixture.path) == (Bytes{9, 9, 9}));
		CHECK(!fs::exists(fixture.path + ".tmp"));
		CHECK(descriptor_count() == count);
	}
}

void test_atomic_zero_progress_is_not_published(const std::string &dir) {
	Fixture fixture(dir + "/zero-write", Bytes{9});
	Inject injection(fixture.path, dir, fixture.fd);
	injection.state.writes.stop_after = 0;
	injection.state.writes.return_zero = true;
	expect_error([&] { atomic_write_all_replace(fixture.file, Bytes{1, 2}); });
	CHECK(injection.state.writes.calls == 1);
	CHECK(injection.state.temp_sync_calls == 0);
	CHECK(read_path(fixture.path) == Bytes{9});
	CHECK(!fs::exists(fixture.path + ".tmp"));
}

void test_atomic_file_sync_failure(const std::string &dir) {
	Fixture fixture(dir + "/file-sync-failure", Bytes{9, 9});
	const auto count = descriptor_count();
	Inject injection(fixture.path, dir, fixture.fd);
	injection.state.temp_sync_error = EIO;
	expect_error([&] { atomic_write_all_replace(fixture.file, Bytes{1, 2, 3}); });
	CHECK(injection.state.temp_sync_calls == 1);
	CHECK(injection.state.directory_open_calls == 0);
	CHECK(injection.state.unlink_calls == 1);
	CHECK(read_path(fixture.path) == (Bytes{9, 9}));
	CHECK(!fs::exists(fixture.path + ".tmp"));
	CHECK(descriptor_count() == count);
}

void test_atomic_size_error_preserves_target_and_cleans(const std::string &dir) {
	Fixture fixture(dir + "/temp-stat-error", Bytes{9, 9});
	const auto count = descriptor_count();
	Inject injection(fixture.path, dir, fixture.fd);
	injection.state.temp_stat_error = EIO;
	expect_error([&] { atomic_write_all_replace(fixture.file, Bytes{1, 2, 3}); });
	CHECK(injection.state.temp_stat_calls == 1);
	CHECK(injection.state.writes.calls == 0);
	CHECK(injection.state.temp_sync_calls == 0 && injection.state.directory_open_calls == 0);
	CHECK(injection.state.unlink_calls == 1);
	CHECK(read_path(fixture.path) == (Bytes{9, 9}));
	CHECK(!fs::exists(fixture.path + ".tmp"));
	CHECK(descriptor_count() == count);
	injection.state.temp_stat_error = 0;
	atomic_write_all_replace(fixture.file, Bytes{4});
	CHECK(read_path(fixture.path) == Bytes{4});
}

void test_atomic_missing_temp_rename_error_preserves_old_target(const std::string &dir) {
	Fixture fixture(dir + "/missing-temp-rename", Bytes{9, 9});
	const auto count = descriptor_count();
	Inject injection(fixture.path, dir, fixture.fd);
	injection.state.remove_temp_after_sync = true;
	bool rename_error = false;
	try { atomic_write_all_replace(fixture.file, Bytes{1, 2, 3}); }
	catch (const fs::filesystem_error &) { rename_error = true; }
	CHECK(rename_error);
	CHECK(injection.state.temp_sync_calls == 1);
	CHECK(injection.state.directory_open_calls == 0);
	CHECK(injection.state.unlink_calls == 1);
	CHECK(read_path(fixture.path) == (Bytes{9, 9}));
	CHECK(!fs::exists(fixture.path + ".tmp"));
	CHECK(descriptor_count() == count);
	injection.state.remove_temp_after_sync = false;
	atomic_write_all_replace(fixture.file, Bytes{4});
	CHECK(read_path(fixture.path) == Bytes{4});
}

void test_atomic_rename_failure_and_retry(const std::string &dir) {
	Fixture fixture(dir + "/rename-failure", Bytes{9});
	// Deterministically reject a real rename: change the destination to a
	// directory after opening the old regular file. No C++ ABI wrapping needed.
	CHECK(__real_unlink(fixture.path.c_str()) == 0);
	CHECK(fs::create_directory(fixture.path));
	const auto count = descriptor_count();
	Inject injection(fixture.path, dir, fixture.fd);
	bool rename_error = false;
	try { atomic_write_all_replace(fixture.file, Bytes{1, 2}); }
	catch (const fs::filesystem_error &) { rename_error = true; }
	CHECK(rename_error);
	CHECK(fs::is_directory(fixture.path));
	CHECK(fs::is_empty(fixture.path));
	CHECK(!fs::exists(fixture.path + ".tmp"));
	CHECK(injection.state.temp_sync_calls == 1);
	CHECK(injection.state.directory_open_calls == 0);
	CHECK(injection.state.unlink_calls == 1);
	CHECK(descriptor_count() == count);
	CHECK(fs::remove(fixture.path));
	seed(fixture.path, Bytes{9});
	atomic_write_all_replace(fixture.file, Bytes{3, 4});
	CHECK(read_path(fixture.path) == (Bytes{3, 4}));
	CHECK(!fs::exists(fixture.path + ".tmp"));
	CHECK(descriptor_count() == count);
}

void test_atomic_parent_open_failure_after_publication(const std::string &dir) {
	Fixture fixture(dir + "/parent-open-failure", Bytes{9, 9});
	const auto count = descriptor_count();
	Inject injection(fixture.path, dir, fixture.fd);
	injection.state.directory_open_error = EACCES;
	expect_error([&] { atomic_write_all_replace(fixture.file, Bytes{1, 2, 3}); });
	CHECK(injection.state.temp_sync_calls == 1);
	CHECK(injection.state.directory_open_calls == 1);
	CHECK(injection.state.directory_sync_calls == 0);
	CHECK(read_path(fixture.path) == (Bytes{1, 2, 3}));
	CHECK(!fs::exists(fixture.path + ".tmp"));
	CHECK(descriptor_count() == count);
	injection.state.directory_open_error = 0;
	atomic_write_all_replace(fixture.file, Bytes{4});
	CHECK(read_path(fixture.path) == Bytes{4});
	CHECK(!fs::exists(fixture.path + ".tmp"));
	CHECK(descriptor_count() == count);
}

void test_atomic_parent_sync_failure_after_publication(const std::string &dir) {
	Fixture fixture(dir + "/parent-sync-failure", Bytes{9, 9});
	const auto count = descriptor_count();
	Inject injection(fixture.path, dir, fixture.fd);
	injection.state.directory_sync_error = EIO;
	expect_error([&] { atomic_write_all_replace(fixture.file, Bytes{1, 2, 3}); });
	CHECK(injection.state.temp_sync_calls == 1);
	CHECK(injection.state.directory_sync_calls == 1);
	CHECK(read_path(fixture.path) == (Bytes{1, 2, 3}));
	CHECK(!fs::exists(fixture.path + ".tmp"));
	CHECK(injection.state.temp_fd == -1 && injection.state.directory_fd == -1);
	CHECK(descriptor_count() == count);
	injection.state.directory_sync_error = 0;
	atomic_write_all_replace(fixture.file, Bytes{4});
	CHECK(read_path(fixture.path) == Bytes{4});
	CHECK(!fs::exists(fixture.path + ".tmp"));
	CHECK(descriptor_count() == count);
}

void test_atomic_cleanup_failure_preserves_primary_and_retries(const std::string &dir) {
	Fixture fixture(dir + "/cleanup-failure", Bytes{9, 9});
	const auto count = descriptor_count();
	Inject injection(fixture.path, dir, fixture.fd);
	injection.state.temp_sync_error = EIO;
	injection.state.unlink_error = EACCES;
	const auto error = expect_error([&] { atomic_write_all_replace(fixture.file, Bytes{1, 2, 3}); });
	CHECK(injection.state.temp_sync_calls == 1 && injection.state.unlink_calls == 1);
	// Distinguish the primary fsync error from the secondary unlink error.
	CHECK(error.find("fsync") != std::string::npos);
	CHECK(error.find("unlink") == std::string::npos);
	CHECK(read_path(fixture.path) == (Bytes{9, 9}));
	CHECK(read_path(fixture.path + ".tmp") == (Bytes{1, 2, 3}));
	CHECK(descriptor_count() == count);
	injection.state.temp_sync_error = 0;
	injection.state.unlink_error = 0;
	atomic_write_all_replace(fixture.file, Bytes{4});
	CHECK(read_path(fixture.path) == Bytes{4});
	CHECK(!fs::exists(fixture.path + ".tmp"));
	CHECK(descriptor_count() == count);
}

void test_atomic_cleanup_already_missing_preserves_primary(const std::string &dir) {
	Fixture fixture(dir + "/missing-cleanup", Bytes{9});
	Inject injection(fixture.path, dir, fixture.fd);
	injection.state.temp_sync_error = EIO;
	injection.state.unlink_disappeared = true;
	const auto error = expect_error([&] { atomic_write_all_replace(fixture.file, Bytes{1, 2}); });
	CHECK(injection.state.unlink_calls == 1);
	CHECK(error.find("fsync") != std::string::npos);
	CHECK(read_path(fixture.path) == Bytes{9});
	CHECK(!fs::exists(fixture.path + ".tmp"));
}

void test_atomic_rename_error_type_survives_cleanup_failure(const std::string &dir) {
	Fixture fixture(dir + "/rename-cleanup", Bytes{9});
	CHECK(__real_unlink(fixture.path.c_str()) == 0);
	CHECK(fs::create_directory(fixture.path));
	const auto count = descriptor_count();
	Inject injection(fixture.path, dir, fixture.fd);
	injection.state.unlink_error = EACCES;
	bool rename_error = false;
	try { atomic_write_all_replace(fixture.file, Bytes{1, 2}); }
	catch (const fs::filesystem_error &) { rename_error = true; }
	CHECK(rename_error); // The cleanup runtime_error must not replace this type.
	CHECK(injection.state.unlink_calls == 1);
	CHECK(fs::is_directory(fixture.path));
	CHECK(read_path(fixture.path + ".tmp") == (Bytes{1, 2}));
	CHECK(descriptor_count() == count);
}

void check_process_exit_and_retry(const std::string &dir, ExitPoint point) {
	const Bytes original{9, 9, 9}, attempted{1, 0x80, 0xff, 4};
	Fixture fixture(dir + "/process-exit", original);
	const auto count = descriptor_count();
	std::cout.flush();
	std::cerr.flush();
	const pid_t worker = ::fork();
	CHECK(worker >= 0);
	if (worker == 0) {
		::alarm(2);
		try {
			Inject injection(fixture.path, dir, fixture.fd);
			injection.state.exit_point = point;
			if (point == ExitPoint::AfterPartialWrite) { injection.state.writes.chunk = 2; }
			atomic_write_all_replace(fixture.file, attempted);
		} catch (...) { ::_exit(2); }
		::_exit(3); // Reaching here means the intended exit point was missed.
	}
	int status = 0;
	pid_t waited;
	do { waited = ::waitpid(worker, &status, 0); }
	while (waited == -1 && errno == EINTR);
	CHECK(waited == worker);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == kInjectedExit);
	if (point == ExitPoint::BeforeDirectorySync) {
		CHECK(read_path(fixture.path) == attempted);
		CHECK(!fs::exists(fixture.path + ".tmp"));
	} else {
		CHECK(read_path(fixture.path) == original);
		const Bytes expected_temp = point == ExitPoint::AfterPartialWrite
			? Bytes(attempted.begin(), attempted.begin() + 2) : attempted;
		CHECK(read_path(fixture.path + ".tmp") == expected_temp);
	}
	CHECK(descriptor_count() == count);
	// This exercises process exit and retry, not loss of the kernel's cache
	// or a real power failure. No automatic FileObj destructors ran in worker.
	atomic_write_all_replace(fixture.file, Bytes{7});
	CHECK(read_path(fixture.path) == Bytes{7});
	CHECK(!fs::exists(fixture.path + ".tmp"));
	CHECK(descriptor_count() == count);
}

void test_process_exit_during_temp_write_can_retry(const std::string &dir) {
	check_process_exit_and_retry(dir, ExitPoint::AfterPartialWrite);
}

void test_process_exit_after_temp_sync_can_retry(const std::string &dir) {
	check_process_exit_and_retry(dir, ExitPoint::AfterTempSync);
}

void test_process_exit_after_rename_can_retry(const std::string &dir) {
	check_process_exit_and_retry(dir, ExitPoint::BeforeDirectorySync);
}

struct TestCase {
	const char *name;
	void (*run)(const std::string &);
};

} // namespace

extern "C" int __wrap_open(const char *path, int flags, ...) {
	mode_t mode = 0;
	if ((flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE) {
		va_list arguments;
		va_start(arguments, flags);
		mode = va_arg(arguments, mode_t);
		va_end(arguments);
	}
	auto *faults = active_faults;
	const bool temporary = faults && faults->temporary == path;
	const bool directory = faults && matches_parent(*faults, path);
	if (temporary) {
		++faults->temp_open_calls;
		faults->events.push_back(Event::TempOpen);
		if (faults->temp_open_error) { errno = faults->temp_open_error; return -1; }
	} else if (directory) {
		++faults->directory_open_calls;
		faults->events.push_back(Event::DirectoryOpen);
		if (faults->exit_point == ExitPoint::BeforeDirectorySync) { ::_exit(kInjectedExit); }
		if (faults->directory_open_error) { errno = faults->directory_open_error; return -1; }
	}
	const int fd = __real_open(path, flags, mode);
	if (temporary && fd >= 0) { faults->temp_fd = fd; }
	if (directory && fd >= 0) { faults->directory_fd = fd; }
	return fd;
}

extern "C" ssize_t __wrap_pread(int fd, void *buffer, std::size_t count, off_t offset) {
	if (!watched_io(fd)) { return __real_pread(fd, buffer, count, offset); }
	auto &plan = active_faults->reads;
	ssize_t result;
	if (forced_result(plan, result)) { return result; }
	result = __real_pread(fd, buffer, std::min(count, plan.chunk), offset);
	if (result > 0) { ++plan.successful_calls; }
	return result;
}

extern "C" ssize_t __wrap_pwrite(int fd, const void *buffer, std::size_t count, off_t offset) {
	if (!watched_io(fd)) { return __real_pwrite(fd, buffer, count, offset); }
	auto &plan = active_faults->writes;
	ssize_t result;
	if (forced_result(plan, result)) { return result; }
	result = __real_pwrite(fd, buffer, std::min(count, plan.chunk), offset);
	if (result > 0) {
		++plan.successful_calls;
		if (fd == active_faults->temp_fd && active_faults->exit_point == ExitPoint::AfterPartialWrite) {
			::_exit(kInjectedExit);
		}
	}
	return result;
}

extern "C" int __wrap_fstat(int fd, struct stat *info) {
	auto *faults = active_faults;
	int injected_error = 0;
	if (faults && fd == faults->temp_fd) {
		++faults->temp_stat_calls;
		injected_error = faults->temp_stat_error;
	} else if (faults && fd == faults->data_fd) {
		++faults->data_stat_calls;
		injected_error = faults->data_stat_error;
	}
	if (injected_error) { errno = injected_error; return -1; }
	return __real_fstat(fd, info);
}

extern "C" int __wrap_fsync(int fd) {
	auto *faults = active_faults;
	if (!faults) { return __real_fsync(fd); }
	int injected_error = 0;
	if (fd == faults->temp_fd) {
		++faults->temp_sync_calls;
		faults->events.push_back(Event::TempSync);
		if (faults->inspect_publication) {
			faults->old_visible_before_file_sync = read_path(faults->target) == faults->old_contents;
			faults->complete_temp_before_file_sync = read_path(faults->temporary) == faults->new_contents;
		}
		injected_error = faults->temp_sync_error;
	} else if (fd == faults->directory_fd) {
		++faults->directory_sync_calls;
		faults->events.push_back(Event::DirectorySync);
		if (faults->inspect_publication) {
			faults->new_visible_before_directory_sync = read_path(faults->target) == faults->new_contents;
			faults->temp_absent_before_directory_sync = !fs::exists(faults->temporary);
		}
		injected_error = faults->directory_sync_error;
	} else if (fd == faults->data_fd) {
		++faults->other_sync_calls;
	}
	if (injected_error) { errno = injected_error; return -1; }
	const int result = __real_fsync(fd);
	if (result == 0 && fd == faults->temp_fd) {
		if (faults->remove_temp_after_sync) {
			CHECK(__real_unlink(faults->temporary.c_str()) == 0);
		}
		if (faults->exit_point == ExitPoint::AfterTempSync) { ::_exit(kInjectedExit); }
	}
	return result;
}

extern "C" int __wrap_unlink(const char *path) {
	auto *faults = active_faults;
	if (faults && faults->temporary == path) {
		++faults->unlink_calls;
		faults->events.push_back(Event::TempUnlink);
		if (faults->unlink_error) { errno = faults->unlink_error; return -1; }
		if (faults->unlink_disappeared) {
			const int result = __real_unlink(path);
			if (result != 0) { return result; }
			errno = ENOENT;
			return -1;
		}
	}
	return __real_unlink(path);
}

extern "C" int __wrap_close(int fd) {
	if (active_faults) {
		if (fd == active_faults->temp_fd) { active_faults->temp_fd = -1; }
		if (fd == active_faults->directory_fd) { active_faults->directory_fd = -1; }
	}
	return __real_close(fd);
}

int main() {
	Log::get_logger().set_minimum_log_level(Log::FATAL);
	const TestCase tests[] = {
		{"exact offset read with binary data and caller buffer", test_exact_read_offset_and_binary},
		{"exact read rejects partial and immediate EOF", test_exact_read_eof_throws},
		{"exact read completes short reads and EINTR", test_exact_read_short_and_eintr},
		{"exact read rejects zero and partial EAGAIN progress", test_exact_read_eagain_throws},
		{"write all preserves neighbors, cursor and input", test_write_all_preserves_neighbors},
		{"write all completes short writes and EINTR", test_write_all_short_and_eintr},
		{"write all rejects zero and partial EAGAIN progress", test_write_all_eagain_throws},
		{"append all handles binary and repeated writes", test_append_all_binary_and_repeated},
		{"append all completes short writes and EINTR", test_append_all_short_and_eintr},
		{"append all rejects zero and partial EAGAIN progress", test_append_all_eagain_throws},
		{"zero length helpers preserve contents without syncing", test_zero_length_helpers},
		{"closed files reject all helpers including empty requests", test_closed_helpers_reject},
		{"read and write errors propagate without closing caller file", test_io_errors_propagate},
		{"exact read can retry after a partial I/O error", test_exact_read_partial_error_and_retry},
		{"write all can retry after a partial I/O error", test_write_all_partial_error_and_retry},
		{"append all can retry the suffix after a partial I/O error", test_append_all_partial_error_and_retry},
		{"append size failure causes no write and permits retry", test_append_size_error_does_not_write_and_can_retry},
		{"atomic binary replacement checks visibility and sync order", test_atomic_publication_order_and_binary},
		{"atomic replacement supports bare filenames", test_atomic_bare_filename},
		{"atomic replacement supports relative parent directories", test_atomic_relative_parent},
		{"atomic replacement truncates and reuses stale temp", test_atomic_reuses_stale_temp},
		{"empty atomic replacement truncates stale temp", test_atomic_empty_reuses_stale_temp},
		{"repeated atomic replacements release descriptors", test_atomic_repeated_replacements_release_handles},
		{"atomic publication creates an absent target name", test_atomic_publishes_when_target_name_is_absent},
		{"temp open failure preserves target and existing temp", test_atomic_temp_open_failure},
		{"partial write error cleans temp and preserves target", test_atomic_write_failure_cleans_temp},
		{"incomplete atomic writes never publish", test_atomic_incomplete_write_is_not_published},
		{"zero progress atomic writes never publish", test_atomic_zero_progress_is_not_published},
		{"temp sync failure cleans temp and preserves target", test_atomic_file_sync_failure},
		{"temp size failure cleans temp before any write", test_atomic_size_error_preserves_target_and_cleans},
		{"rename failure from missing temp preserves old target", test_atomic_missing_temp_rename_error_preserves_old_target},
		{"rename failure cleans temp and permits retry", test_atomic_rename_failure_and_retry},
		{"parent open failure reports error after publication", test_atomic_parent_open_failure_after_publication},
		{"parent sync failure reports error after publication", test_atomic_parent_sync_failure_after_publication},
		{"cleanup error preserves primary sync error and permits retry", test_atomic_cleanup_failure_preserves_primary_and_retries},
		{"already absent temp preserves primary error", test_atomic_cleanup_already_missing_preserves_primary},
		{"cleanup error preserves original rename exception type", test_atomic_rename_error_type_survives_cleanup_failure},
		{"process exit during temp write permits a complete retry", test_process_exit_during_temp_write_can_retry},
		{"process exit after temp sync permits a complete retry", test_process_exit_after_temp_sync_can_retry},
		{"process exit after rename permits a complete retry", test_process_exit_after_rename_can_retry},
	};
	std::size_t passed = 0;
	for (const auto &test : tests) {
		// Parent owns cleanup; child isolation bounds injected failures, cwd
		// changes, and infinite loops without leaking other tests' fixtures.
		TempDirectory temporary;
		std::cout.flush();
		std::cerr.flush();
		const pid_t child = ::fork();
		if (child == -1) { std::cerr << "fork failed\n"; return 2; }
		if (child == 0) {
			::alarm(5);
			try { test.run(temporary.path()); ::_exit(0); }
			catch (const std::exception &error) {
				std::cerr << test.name << ": " << error.what() << '\n';
			} catch (...) { std::cerr << test.name << ": unknown exception\n"; }
			::_exit(1);
		}
		int status = 0;
		pid_t waited;
		do { waited = ::waitpid(child, &status, 0); }
		while (waited == -1 && errno == EINTR);
		if (waited != child) { std::cerr << "waitpid failed\n"; return 2; }
		const bool success = WIFEXITED(status) && WEXITSTATUS(status) == 0;
		std::cout << (success ? "[PASS] " : "[FAIL] ") << test.name;
		if (WIFSIGNALED(status)) { std::cout << " (signal " << WTERMSIG(status) << ')'; }
		std::cout << '\n';
		if (success) { ++passed; }
	}
	const auto total = sizeof(tests) / sizeof(tests[0]);
	std::cout << passed << '/' << total << " tests passed\n";
	return passed == total ? 0 : 1;
}
