#include "io/FileObj.h"
#include "log.h"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <vector>

extern "C" ssize_t __real_pread(int, void *, std::size_t, off_t);
extern "C" ssize_t __real_pwrite(int, const void *, std::size_t, off_t);
extern "C" int __real_fstat(int, struct stat *);
extern "C" int __real_ftruncate(int, off_t);
extern "C" int __real_fsync(int);
extern "C" int __real_close(int);

namespace {

using atlas::io::AccessMode;
using atlas::io::FileObj;
using Bytes = std::vector<std::uint8_t>;

static_assert(!std::is_copy_constructible_v<FileObj>);
static_assert(!std::is_copy_assignable_v<FileObj>);
static_assert(std::is_move_constructible_v<FileObj>);
static_assert(std::is_move_assignable_v<FileObj>);

void check(bool condition, const char *expression, int line) {
	if (condition) { return; }
	std::ostringstream message;
	message << "line " << line << ": check failed: " << expression;
	throw std::runtime_error(message.str());
}

#define CHECK(expression) check((expression), #expression, __LINE__)

template <typename Operation>
void expect_error(Operation operation) {
	bool caught = false;
	try { operation(); }
	catch (const std::exception &) { caught = true; }
	CHECK(caught);
}

class OwnedFd {
public:
	explicit OwnedFd(int fd = -1) : fd_(fd) {}
	OwnedFd(const OwnedFd &) = delete;
	OwnedFd &operator=(const OwnedFd &) = delete;
	~OwnedFd() { if (fd_ >= 0) { __real_close(fd_); } }
	int get() const { return fd_; }
	int release() { return std::exchange(fd_, -1); }
	void reset(int fd) {
		if (fd_ >= 0) { __real_close(fd_); }
		fd_ = fd;
	}
private:
	int fd_;
};

class TempFile {
public:
	TempFile() {
		char name[] = "/tmp/atlasdb-fileobj-XXXXXX";
		fd_.reset(::mkstemp(name));
		CHECK(fd_.get() >= 0);
		name_ = name;
		// The inode lives until its handles close, even if a test is terminated.
		CHECK(::unlink(name) == 0);
	}
	int fd() const { return fd_.get(); }
	const std::string &path() const { return name_; }
	FileObj take() {
		return FileObj(fd_.release(), name_, AccessMode::ReadWrite);
	}
private:
	OwnedFd fd_;
	std::string name_;
};

bool descriptor_open(int fd) {
	errno = 0;
	const int result = ::fcntl(fd, F_GETFD);
	if (result >= 0) { return true; }
	CHECK(errno == EBADF);
	return false;
}

void seed(int fd, const Bytes &data) {
	std::size_t done = 0;
	while (done < data.size()) {
		const ssize_t result = __real_pwrite(fd, data.data() + done,
			data.size() - done, static_cast<off_t>(done));
		if (result == -1 && errno == EINTR) { continue; }
		CHECK(result > 0);
		done += static_cast<std::size_t>(result);
	}
}

Bytes raw_read(int fd, std::size_t offset, std::size_t length) {
	Bytes result(length);
	std::size_t done = 0;
	while (done < length) {
		const ssize_t count = __real_pread(fd, result.data() + done,
			length - done, static_cast<off_t>(offset + done));
		if (count == -1 && errno == EINTR) { continue; }
		CHECK(count > 0);
		done += static_cast<std::size_t>(count);
	}
	return result;
}

std::size_t raw_size(int fd) {
	struct stat info {};
	CHECK(__real_fstat(fd, &info) == 0);
	CHECK(info.st_size >= 0);
	return static_cast<std::size_t>(info.st_size);
}

Bytes sample() {
	Bytes result(256);
	for (std::size_t i = 0; i < result.size(); ++i) {
		result[i] = static_cast<std::uint8_t>(i);
	}
	return result;
}

struct IoPlan {
	std::size_t chunk = std::numeric_limits<std::size_t>::max();
	unsigned interruptions = 0;
	int stop_after = -1;
	int error = EIO;
	bool return_zero = false;
	std::size_t calls = 0;
	std::size_t successful_calls = 0;
};

struct Faults {
	explicit Faults(int descriptor) : fd(descriptor) {}
	int fd;
	IoPlan reads;
	IoPlan writes;
	int stat_error = 0;
	int truncate_error = 0;
	int sync_error = 0;
	bool close_error = false;
	unsigned stat_calls = 0;
	unsigned truncate_calls = 0;
	unsigned sync_calls = 0;
	unsigned close_calls = 0;
};

// Injection is limited to one descriptor in the test's current thread.
thread_local Faults *active_faults = nullptr;

class Inject {
public:
	explicit Inject(int fd) : state(fd), previous_(active_faults) {
		active_faults = &state;
	}
	Inject(const Inject &) = delete;
	Inject &operator=(const Inject &) = delete;
	~Inject() { active_faults = previous_; }
	Faults state;
private:
	Faults *previous_;
};

Faults *faults_for(int fd) {
	return active_faults && active_faults->fd == fd ? active_faults : nullptr;
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
		plan.successful_calls >= static_cast<std::size_t>(plan.stop_after)) {
		if (plan.return_zero) { result = 0; }
		else { errno = plan.error; result = -1; }
		return true;
	}
	return false;
}

void test_default_and_closed_operations() {
	FileObj file;
	CHECK(!file.is_open());
	file.close();
	file.close();
	Bytes buffer(1);
	expect_error([&] { file.check_is_open(); });
	expect_error([&] { file.read_at(0, buffer); });
	expect_error([&] { file.write_at(0, buffer); });
	expect_error([&] { file.append(buffer); });
	expect_error([&] { file.truncate(0); });
	expect_error([&] { file.size(); });
	expect_error([&] { file.sync(); });
	Bytes empty;
	expect_error([&] { file.read_at(0, empty); });
	expect_error([&] { file.write_at(0, empty); });
	expect_error([&] { file.append(empty); });
}

void test_explicit_close_is_idempotent() {
	TempFile temporary;
	const int fd = temporary.fd();
	Inject injection(fd);
	{
		auto file = temporary.take();
		CHECK(file.is_open());
		file.check_is_open();
		file.close();
		CHECK(!file.is_open());
		CHECK(!descriptor_open(fd));
		file.close();
	}
	CHECK(injection.state.close_calls == 1);
}

void test_destructor_closes_resource() {
	TempFile temporary;
	const int fd = temporary.fd();
	Inject injection(fd);
	{ auto file = temporary.take(); CHECK(file.is_open()); }
	CHECK(!descriptor_open(fd));
	CHECK(injection.state.close_calls == 1);
}

void test_move_constructor_transfers_ownership() {
	TempFile temporary;
	const int fd = temporary.fd();
	seed(fd, Bytes{0x00, 0x80, 0xff});
	auto source = temporary.take();
	FileObj target(std::move(source));
	CHECK(!source.is_open());
	CHECK(target.is_open());
	CHECK(target.get_file_path() == temporary.path());
	source.close();
	CHECK(descriptor_open(fd));
	Bytes buffer(3);
	CHECK(target.read_at(0, buffer) == 3);
	CHECK((buffer == Bytes{0x00, 0x80, 0xff}));
	target.close();
	CHECK(!descriptor_open(fd));
}

void test_move_assignment_to_empty() {
	TempFile temporary;
	const int fd = temporary.fd();
	seed(fd, Bytes{7});
	auto source = temporary.take();
	FileObj target;
	CHECK(&(target = std::move(source)) == &target);
	CHECK(!source.is_open());
	CHECK(target.is_open());
	CHECK(target.get_file_path() == temporary.path());
	Bytes buffer(1);
	CHECK(target.read_at(0, buffer) == 1 && buffer[0] == 7);
	source.close();
	CHECK(descriptor_open(fd));
}

void test_move_assignment_swaps_live_resources() {
	TempFile first, second;
	const int first_fd = first.fd(), second_fd = second.fd();
	seed(first_fd, Bytes{1, 2});
	seed(second_fd, Bytes{3, 4, 5});
	auto source = first.take();
	auto target = second.take();
	target = std::move(source);
	// AtlasDB deliberately uses swap assignment: source owns target's old fd.
	CHECK(target.is_open() && source.is_open());
	CHECK(target.get_file_path() == first.path());
	CHECK(source.get_file_path() == second.path());
	Bytes a(2), b(3);
	CHECK(target.read_at(0, a) == 2 && (a == Bytes{1, 2}));
	CHECK(source.read_at(0, b) == 3 && (b == Bytes{3, 4, 5}));
	source.close();
	CHECK(!descriptor_open(second_fd));
	CHECK(descriptor_open(first_fd));
	target.close();
	CHECK(!descriptor_open(first_fd));
}

void test_self_move_and_closed_moves() {
	TempFile temporary;
	seed(temporary.fd(), Bytes{0xff});
	auto file = temporary.take();
	FileObj &alias = file;
	file = std::move(alias);
	CHECK(file.get_file_path() == temporary.path());
	Bytes buffer(1);
	CHECK(file.read_at(0, buffer) == 1 && buffer[0] == 0xff);
	file.close();
	FileObj moved(std::move(file));
	CHECK(!file.is_open() && !moved.is_open());
	moved = std::move(file);
	CHECK(!file.is_open() && !moved.is_open());
}

void test_direct_swap_keeps_paths_with_handles() {
	TempFile first, second;
	const int first_fd = first.fd(), second_fd = second.fd();
	seed(first_fd, Bytes{1});
	seed(second_fd, Bytes{2, 3});
	auto a = first.take();
	auto b = second.take();
	a.swap(std::move(b));
	CHECK(a.get_file_path() == second.path());
	CHECK(b.get_file_path() == first.path());
	Bytes a_data(2), b_data(1);
	CHECK(a.read_at(0, a_data) == 2 && (a_data == Bytes{2, 3}));
	CHECK(b.read_at(0, b_data) == 1 && b_data == Bytes{1});
	FileObj empty;
	CHECK(empty.get_file_path().empty());
	a.swap(std::move(empty));
	CHECK(!a.is_open() && empty.is_open());
	CHECK(empty.get_file_path() == second.path());
	a.close();
	CHECK(descriptor_open(second_fd));
	empty.close();
	b.close();
	CHECK(!descriptor_open(first_fd) && !descriptor_open(second_fd));
}

void test_move_empty_into_live_swaps_ownership() {
	TempFile temporary;
	const int fd = temporary.fd();
	seed(fd, Bytes{0x80, 0xff});
	auto target = temporary.take();
	FileObj source;
	target = std::move(source);
	CHECK(!target.is_open() && source.is_open());
	CHECK(source.get_file_path() == temporary.path());
	target.close();
	CHECK(descriptor_open(fd));
	Bytes buffer(2);
	CHECK(source.read_at(0, buffer) == 2 && (buffer == Bytes{0x80, 0xff}));
	source.close();
	CHECK(!descriptor_open(fd));
}

void test_zero_descriptor_is_owned_and_usable() {
	// Each case runs in a child, so replacing stdin cannot affect the runner.
	TempFile temporary;
	seed(temporary.fd(), Bytes{0x80, 0xff});
	OwnedFd backing(::fcntl(temporary.fd(), F_DUPFD_CLOEXEC, 3));
	CHECK(backing.get() >= 3);
	auto original = temporary.take();
	original.close();
	CHECK(::dup2(backing.get(), STDIN_FILENO) == STDIN_FILENO);
	FileObj file(STDIN_FILENO, temporary.path(), AccessMode::ReadWrite);
	CHECK(file.is_open());
	Bytes buffer(2);
	CHECK(file.read_at(0, buffer) == 2 && (buffer == Bytes{0x80, 0xff}));
	CHECK(file.write_at(1, Bytes{9}) == 1);
	CHECK((raw_read(backing.get(), 0, 2) == Bytes{0x80, 9}));
	file.close();
	CHECK(!file.is_open() && !descriptor_open(STDIN_FILENO));
}

void test_read_error_before_progress_can_retry() {
	TempFile temporary;
	const int fd = temporary.fd();
	seed(fd, Bytes{1, 0x80, 0xff});
	auto file = temporary.take();
	Inject injection(fd);
	injection.state.reads.stop_after = 0;
	Bytes buffer(3, 0xaa);
	expect_error([&] { file.read_at(0, buffer); });
	CHECK(injection.state.reads.calls == 1);
	CHECK(buffer == Bytes(3, 0xaa));
	CHECK(file.is_open());
	injection.state.reads = {};
	CHECK(file.read_at(0, buffer) == 3 && (buffer == Bytes{1, 0x80, 0xff}));
}

void test_positional_read_preserves_cursor_and_buffer() {
	TempFile temporary;
	const int fd = temporary.fd();
	const auto data = sample();
	seed(fd, data);
	auto file = temporary.take();
	CHECK(::lseek(fd, 99, SEEK_SET) == 99);
	Bytes buffer(37, 0xaa);
	buffer.reserve(100);
	const auto capacity = buffer.capacity();
	CHECK(file.read_at(13, buffer) == buffer.size());
	CHECK(buffer == Bytes(data.begin() + 13, data.begin() + 50));
	CHECK(buffer.size() == 37 && buffer.capacity() == capacity);
	CHECK(::lseek(fd, 0, SEEK_CUR) == 99);
}

void test_eof_returns_actual_length() {
	TempFile temporary;
	seed(temporary.fd(), Bytes{1, 2, 3, 4, 5});
	auto file = temporary.take();
	Bytes buffer(6, 0xaa);
	CHECK(file.read_at(3, buffer) == 2);
	CHECK((buffer == Bytes{4, 5, 0xaa, 0xaa, 0xaa, 0xaa}));
	const auto previous = buffer;
	CHECK(file.read_at(5, buffer) == 0 && buffer == previous);
	CHECK(file.read_at(100, buffer) == 0 && buffer == previous);
	CHECK(file.truncate(0));
	CHECK(file.read_at(0, buffer) == 0 && buffer == previous);
}

void test_zero_length_operations() {
	TempFile temporary;
	const int fd = temporary.fd();
	seed(fd, Bytes{1, 2, 3});
	auto file = temporary.take();
	Inject injection(fd);
	CHECK(::lseek(fd, 2, SEEK_SET) == 2);
	Bytes empty;
	empty.reserve(128); // Capacity is not the requested read length.
	CHECK(file.read_at(0, empty) == 0 && empty.empty());
	CHECK(file.write_at(99, empty) == 0);
	CHECK(file.append(empty) == 0);
	CHECK(injection.state.reads.calls == 0);
	CHECK(injection.state.writes.calls == 0);
	CHECK(raw_size(fd) == 3);
	CHECK((raw_read(fd, 0, 3) == Bytes{1, 2, 3}));
	CHECK(::lseek(fd, 0, SEEK_CUR) == 2);
}

void test_write_overwrites_only_requested_range() {
	TempFile temporary;
	const int fd = temporary.fd();
	seed(fd, Bytes{1, 2, 3, 4, 5, 6});
	auto file = temporary.take();
	CHECK(::lseek(fd, 5, SEEK_SET) == 5);
	const Bytes input{0x00, 0x80};
	CHECK(file.write_at(2, input) == input.size());
	CHECK((input == Bytes{0x00, 0x80}));
	CHECK((raw_read(fd, 0, 6) == Bytes{1, 2, 0x00, 0x80, 5, 6}));
	CHECK(file.write_at(4, Bytes{0xff}) == 1);
	CHECK(file.size() == 6);
	CHECK(::lseek(fd, 0, SEEK_CUR) == 5);
}

void test_write_extends_file_with_zero_gap() {
	TempFile temporary;
	const int fd = temporary.fd();
	seed(fd, Bytes{1, 2});
	auto file = temporary.take();
	CHECK(file.write_at(5, Bytes{0x80, 0xff}) == 2);
	CHECK(file.size() == 7);
	CHECK((raw_read(fd, 0, 7) == Bytes{1, 2, 0, 0, 0, 0x80, 0xff}));
}

void test_large_sparse_file_offsets() {
	constexpr std::uint64_t offset64 = (UINT64_C(1) << 32) + 17;
	if (offset64 + 3 > std::numeric_limits<std::size_t>::max() ||
		offset64 + 3 > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
		return;
	}
	TempFile temporary;
	const int fd = temporary.fd();
	auto file = temporary.take();
	const auto offset = static_cast<std::size_t>(offset64);
	CHECK(file.write_at(offset, Bytes{0x80, 0xff}) == 2);
	CHECK(file.size() == offset + 2);
	CHECK(file.append(Bytes{0x42}) == 1);
	Bytes buffer(4, 0xaa);
	CHECK(file.read_at(offset - 1, buffer) == 4);
	CHECK((buffer == Bytes{0, 0x80, 0xff, 0x42}));
	CHECK(raw_size(fd) == offset + 3);
	CHECK(file.truncate(0));
	CHECK(file.size() == 0);
}

void test_append_uses_current_end() {
	TempFile temporary;
	const int fd = temporary.fd();
	seed(fd, Bytes{1, 2, 3});
	auto file = temporary.take();
	CHECK(::lseek(fd, 1, SEEK_SET) == 1);
	const Bytes input{0, 0x80};
	CHECK(file.append(input) == 2);
	CHECK(file.append(Bytes{0xff}) == 1);
	CHECK((input == Bytes{0, 0x80}));
	CHECK((raw_read(fd, 0, 6) == Bytes{1, 2, 3, 0, 0x80, 0xff}));
	CHECK(::lseek(fd, 0, SEEK_CUR) == 1);
	CHECK(file.truncate(2));
	CHECK(file.append(Bytes{7, 8}) == 2);
	CHECK(file.size() == 4);
	CHECK((raw_read(fd, 0, 4) == Bytes{1, 2, 7, 8}));
}

void test_truncate_shrinks_and_extends() {
	TempFile temporary;
	const int fd = temporary.fd();
	seed(fd, Bytes{1, 2, 3, 4, 5});
	auto file = temporary.take();
	CHECK(::lseek(fd, 4, SEEK_SET) == 4);
	CHECK(file.truncate(2));
	CHECK(file.size() == 2 && (raw_read(fd, 0, 2) == Bytes{1, 2}));
	CHECK(file.truncate(5));
	CHECK((raw_read(fd, 0, 5) == Bytes{1, 2, 0, 0, 0}));
	CHECK(::lseek(fd, 0, SEEK_CUR) == 4);
	CHECK(file.truncate(0));
	CHECK(file.size() == 0);
	CHECK(file.append(Bytes{9}) == 1);
	CHECK((raw_read(fd, 0, 1) == Bytes{9}));
}

void test_sync_calls_fsync() {
	TempFile temporary;
	const int fd = temporary.fd();
	auto file = temporary.take();
	CHECK(file.write_at(0, Bytes{1, 2, 3}) == 3);
	Inject injection(fd);
	file.sync();
	CHECK(injection.state.sync_calls == 1);
	CHECK(file.is_open());
	CHECK((raw_read(fd, 0, 3) == Bytes{1, 2, 3}));
}

void test_short_reads_and_eintr() {
	TempFile temporary;
	const int fd = temporary.fd();
	const auto data = sample();
	seed(fd, data);
	auto file = temporary.take();
	Inject injection(fd);
	injection.state.reads.chunk = 7;
	injection.state.reads.interruptions = 2;
	Bytes buffer(53);
	CHECK(file.read_at(13, buffer) == 53);
	CHECK(buffer == Bytes(data.begin() + 13, data.begin() + 66));
	CHECK(injection.state.reads.successful_calls > 1);
	CHECK(injection.state.reads.interruptions == 0);
}

void check_short_writes(bool append) {
	TempFile temporary;
	const int fd = temporary.fd();
	seed(fd, Bytes{0xaa, 0xbb, 0xcc, 0xdd});
	auto file = temporary.take();
	Inject injection(fd);
	injection.state.writes.chunk = 3;
	injection.state.writes.interruptions = 2;
	const auto data = sample();
	const auto offset = append ? 4u : 2u;
	const auto count = append ? file.append(data) : file.write_at(offset, data);
	CHECK(count == data.size());
	CHECK(raw_read(fd, offset, data.size()) == data);
	CHECK((raw_read(fd, 0, 2) == Bytes{0xaa, 0xbb}));
	CHECK(raw_size(fd) == offset + data.size());
	CHECK(injection.state.writes.successful_calls > 1);
	CHECK(injection.state.writes.interruptions == 0);
}

void test_short_writes_and_eintr() { check_short_writes(false); }
void test_short_appends_and_eintr() { check_short_writes(true); }

void test_read_eagain_returns_progress() {
	TempFile temporary;
	const int fd = temporary.fd();
	seed(fd, Bytes{1, 2, 3, 4, 5, 6});
	auto file = temporary.take();
	Inject injection(fd);
	auto &plan = injection.state.reads;
	plan.chunk = 3;
	plan.stop_after = 1;
	plan.error = EAGAIN;
	Bytes buffer(6, 0xaa);
	CHECK(file.read_at(1, buffer) == 3);
	CHECK((buffer == Bytes{2, 3, 4, 0xaa, 0xaa, 0xaa}));
	CHECK(plan.calls == 2);
	plan = {};
	plan.stop_after = 0;
	plan.error = EAGAIN;
	const auto previous = buffer;
	// This zero is "would block", not proof of EOF: data still exists.
	CHECK(file.read_at(0, buffer) == 0 && buffer == previous);
	CHECK(raw_size(fd) == 6);
}

void check_write_eagain(bool append) {
	for (int completed_calls : {0, 1}) {
		TempFile temporary;
		const int fd = temporary.fd();
		seed(fd, Bytes{0xaa, 0xbb});
		auto file = temporary.take();
		Inject injection(fd);
		auto &plan = injection.state.writes;
		plan.chunk = 3;
		plan.stop_after = completed_calls;
		plan.error = EAGAIN;
		const Bytes data{1, 2, 3, 4, 5, 6, 7};
		const auto count = append ? file.append(data) : file.write_at(2, data);
		CHECK(count == (completed_calls == 0 ? 0u : 3u));
		CHECK(raw_size(fd) == 2 + count);
		CHECK((raw_read(fd, 0, 2) == Bytes{0xaa, 0xbb}));
		CHECK(raw_read(fd, 2, count) == Bytes(data.begin(), data.begin() + count));
		// The caller owns the retry policy for partial completion.
		plan = {};
		const Bytes remaining(data.begin() + count, data.end());
		CHECK((append ? file.append(remaining) : file.write_at(2 + count, remaining))
			== remaining.size());
		CHECK(raw_read(fd, 2, data.size()) == data);
	}
}

void test_write_eagain_returns_progress() { check_write_eagain(false); }
void test_append_eagain_returns_progress() { check_write_eagain(true); }

void test_read_error_after_partial_progress() {
	TempFile temporary;
	const int fd = temporary.fd();
	seed(fd, Bytes{1, 2, 3, 4, 5});
	auto file = temporary.take();
	Inject injection(fd);
	injection.state.reads.chunk = 2;
	injection.state.reads.stop_after = 1;
	Bytes buffer(5, 0xaa);
	expect_error([&] { file.read_at(0, buffer); });
	CHECK((buffer == Bytes{1, 2, 0xaa, 0xaa, 0xaa}));
	CHECK(file.is_open());
}

void check_write_error(bool append, bool zero, int completed_calls) {
	TempFile temporary;
	const int fd = temporary.fd();
	seed(fd, Bytes{0xaa, 0xbb});
	auto file = temporary.take();
	Inject injection(fd);
	auto &plan = injection.state.writes;
	plan.chunk = 2;
	plan.stop_after = completed_calls;
	plan.return_zero = zero;
	const Bytes data{1, 2, 3, 4, 5};
	expect_error([&] {
		if (append) { file.append(data); }
		else { file.write_at(2, data); }
	});
	CHECK(plan.calls == static_cast<std::size_t>(completed_calls + 1));
	const auto prefix = completed_calls == 0 ? 0u : 2u;
	CHECK(raw_size(fd) == 2 + prefix);
	CHECK((raw_read(fd, 0, 2) == Bytes{0xaa, 0xbb}));
	CHECK(raw_read(fd, 2, prefix) == Bytes(data.begin(), data.begin() + prefix));
	CHECK(file.is_open()); // Failure need not roll back a transferred prefix.
}

void test_write_errors_preserve_transferred_prefix() {
	check_write_error(false, false, 0);
	check_write_error(false, false, 1);
}

void test_append_errors_preserve_transferred_prefix() {
	check_write_error(true, false, 0);
	check_write_error(true, false, 1);
}

void test_zero_progress_write_is_an_error() {
	for (bool append : {false, true}) {
		check_write_error(append, true, 0);
		check_write_error(append, true, 1);
	}
}

void test_kernel_access_modes() {
	TempFile temporary;
	seed(temporary.fd(), Bytes{1, 2, 3});
	const auto path = "/proc/self/fd/" + std::to_string(temporary.fd());
	OwnedFd read_fd(::open(path.c_str(), O_RDONLY));
	CHECK(read_fd.get() >= 0);
	FileObj reader(read_fd.release(), path, AccessMode::ReadOnly);
	Bytes buffer(3);
	CHECK(reader.read_at(0, buffer) == 3 && (buffer == Bytes{1, 2, 3}));
	expect_error([&] { reader.write_at(0, Bytes{4}); });
	expect_error([&] { reader.append(Bytes{4}); });
	expect_error([&] { reader.truncate(0); });
	CHECK(reader.is_open());
	OwnedFd write_fd(::open(path.c_str(), O_WRONLY));
	CHECK(write_fd.get() >= 0);
	FileObj writer(write_fd.release(), path, AccessMode::WriteOnly);
	expect_error([&] { writer.read_at(0, buffer); });
	CHECK(writer.write_at(1, Bytes{9}) == 1);
	CHECK(writer.append(Bytes{8}) == 1);
	CHECK((raw_read(temporary.fd(), 0, 4) == Bytes{1, 9, 3, 8}));
}

void test_stat_failure_prevents_append() {
	TempFile temporary;
	const int fd = temporary.fd();
	seed(fd, Bytes{1, 2, 3});
	auto file = temporary.take();
	Inject injection(fd);
	injection.state.stat_error = EIO;
	expect_error([&] { file.size(); });
	expect_error([&] { file.append(Bytes{4}); });
	CHECK(injection.state.stat_calls == 2);
	CHECK(injection.state.writes.calls == 0);
	CHECK(file.is_open());
	CHECK((raw_read(fd, 0, 3) == Bytes{1, 2, 3}));
}

void test_truncate_error_preserves_contents() {
	TempFile temporary;
	const int fd = temporary.fd();
	seed(fd, Bytes{1, 2, 3});
	auto file = temporary.take();
	Inject injection(fd);
	injection.state.truncate_error = EIO;
	expect_error([&] { file.truncate(1); });
	CHECK(injection.state.truncate_calls == 1);
	CHECK(raw_size(fd) == 3 && (raw_read(fd, 0, 3) == Bytes{1, 2, 3}));
	CHECK(file.is_open());
}

void test_sync_failure_is_reported() {
	TempFile temporary;
	const int fd = temporary.fd();
	auto file = temporary.take();
	Inject injection(fd);
	injection.state.sync_error = EIO;
	expect_error([&] { file.sync(); });
	CHECK(injection.state.sync_calls == 1);
	CHECK(file.is_open());
}

void test_close_failure_does_not_close_reused_fd() {
	OwnedFd replacement;
	int fd = -1;
	{
		TempFile temporary;
		fd = temporary.fd();
		Inject injection(fd);
		auto file = temporary.take();
		injection.state.close_error = true;
		expect_error([&] { file.close(); });
		CHECK(!file.is_open());
		CHECK(!descriptor_open(fd));
		replacement.reset(::open("/dev/null", O_RDONLY));
		CHECK(replacement.get() >= 0);
		if (replacement.get() != fd) {
			CHECK(::dup2(replacement.get(), fd) == fd);
			replacement.reset(fd);
		}
		file.close();
		CHECK(injection.state.close_calls == 1);
		// FileObj's destructor runs while the replacement owns that same number.
	}
	CHECK(replacement.get() == fd && descriptor_open(fd));
}

void test_destructor_absorbs_close_failure() {
	TempFile temporary;
	const int fd = temporary.fd();
	Inject injection(fd);
	injection.state.close_error = true;
	{ auto file = temporary.take(); }
	CHECK(injection.state.close_calls == 1);
	CHECK(!descriptor_open(fd));
}

void test_concurrent_positional_reads() {
	TempFile temporary;
	const int fd = temporary.fd();
	const auto data = sample();
	seed(fd, data);
	auto file = temporary.take();
	CHECK(::lseek(fd, 111, SEEK_SET) == 111);
	std::exception_ptr worker_error;
	std::thread worker([&] {
		try {
			for (int i = 0; i < 100; ++i) {
				Bytes buffer(41);
				CHECK(file.read_at(7, buffer) == 41);
				CHECK(buffer == Bytes(data.begin() + 7, data.begin() + 48));
			}
		} catch (...) { worker_error = std::current_exception(); }
	});
	std::exception_ptr main_error;
	try {
		for (int i = 0; i < 100; ++i) {
			Bytes buffer(67);
			CHECK(file.read_at(151, buffer) == 67);
			CHECK(buffer == Bytes(data.begin() + 151, data.begin() + 218));
		}
	} catch (...) { main_error = std::current_exception(); }
	worker.join();
	if (worker_error) { std::rethrow_exception(worker_error); }
	if (main_error) { std::rethrow_exception(main_error); }
	CHECK(::lseek(fd, 0, SEEK_CUR) == 111);
}

struct TestCase {
	const char *name;
	void (*run)();
};

} // namespace

extern "C" ssize_t __wrap_pread(int fd, void *buffer, std::size_t count, off_t offset) {
	auto *faults = faults_for(fd);
	if (!faults) { return __real_pread(fd, buffer, count, offset); }
	auto &plan = faults->reads;
	ssize_t result;
	if (forced_result(plan, result)) { return result; }
	result = __real_pread(fd, buffer, std::min(count, plan.chunk), offset);
	if (result > 0) { ++plan.successful_calls; }
	return result;
}

extern "C" ssize_t __wrap_pwrite(int fd, const void *buffer, std::size_t count, off_t offset) {
	auto *faults = faults_for(fd);
	if (!faults) { return __real_pwrite(fd, buffer, count, offset); }
	auto &plan = faults->writes;
	ssize_t result;
	if (forced_result(plan, result)) { return result; }
	result = __real_pwrite(fd, buffer, std::min(count, plan.chunk), offset);
	if (result > 0) { ++plan.successful_calls; }
	return result;
}

extern "C" int __wrap_fstat(int fd, struct stat *info) {
	if (auto *faults = faults_for(fd)) {
		++faults->stat_calls;
		if (faults->stat_error) { errno = faults->stat_error; return -1; }
	}
	return __real_fstat(fd, info);
}

extern "C" int __wrap_ftruncate(int fd, off_t length) {
	if (auto *faults = faults_for(fd)) {
		++faults->truncate_calls;
		if (faults->truncate_error) { errno = faults->truncate_error; return -1; }
	}
	return __real_ftruncate(fd, length);
}

extern "C" int __wrap_fsync(int fd) {
	if (auto *faults = faults_for(fd)) {
		++faults->sync_calls;
		if (faults->sync_error) { errno = faults->sync_error; return -1; }
	}
	return __real_fsync(fd);
}

extern "C" int __wrap_close(int fd) {
	if (auto *faults = faults_for(fd)) {
		++faults->close_calls;
		if (faults->close_error) {
			// Model Linux releasing the number before a later close error.
			const int result = __real_close(fd);
			if (result != 0) { return result; }
			errno = EIO;
			return -1;
		}
	}
	return __real_close(fd);
}

int main() {
	Log::get_logger().set_minimum_log_level(Log::FATAL);
	const TestCase tests[] = {
		{"default and closed operations", test_default_and_closed_operations},
		{"explicit close is idempotent", test_explicit_close_is_idempotent},
		{"destructor closes resource", test_destructor_closes_resource},
		{"move constructor transfers ownership", test_move_constructor_transfers_ownership},
		{"move assignment to empty", test_move_assignment_to_empty},
		{"move assignment swaps live resources", test_move_assignment_swaps_live_resources},
		{"self move and closed moves", test_self_move_and_closed_moves},
		{"direct swap keeps paths paired with file handles", test_direct_swap_keeps_paths_with_handles},
		{"moving empty into live swaps ownership", test_move_empty_into_live_swaps_ownership},
		{"descriptor zero is owned and usable", test_zero_descriptor_is_owned_and_usable},
		{"read error before progress permits retry", test_read_error_before_progress_can_retry},
		{"positional read preserves cursor and buffer", test_positional_read_preserves_cursor_and_buffer},
		{"EOF returns actual length", test_eof_returns_actual_length},
		{"zero length operations", test_zero_length_operations},
		{"write preserves neighboring bytes and cursor", test_write_overwrites_only_requested_range},
		{"write extends with a zero gap", test_write_extends_file_with_zero_gap},
		{"large sparse file offsets", test_large_sparse_file_offsets},
		{"append uses current end", test_append_uses_current_end},
		{"truncate shrinks and extends", test_truncate_shrinks_and_extends},
		{"sync calls fsync", test_sync_calls_fsync},
		{"short reads and EINTR", test_short_reads_and_eintr},
		{"short writes and EINTR", test_short_writes_and_eintr},
		{"short appends and EINTR", test_short_appends_and_eintr},
		{"read EAGAIN returns progress", test_read_eagain_returns_progress},
		{"write EAGAIN returns progress", test_write_eagain_returns_progress},
		{"append EAGAIN returns progress", test_append_eagain_returns_progress},
		{"read error after partial progress", test_read_error_after_partial_progress},
		{"write errors preserve transferred prefix", test_write_errors_preserve_transferred_prefix},
		{"append errors preserve transferred prefix", test_append_errors_preserve_transferred_prefix},
		{"zero progress write is an error", test_zero_progress_write_is_an_error},
		{"kernel access modes", test_kernel_access_modes},
		{"stat failure prevents append", test_stat_failure_prevents_append},
		{"truncate error preserves contents", test_truncate_error_preserves_contents},
		{"sync failure is reported", test_sync_failure_is_reported},
		{"close failure does not close reused fd", test_close_failure_does_not_close_reused_fd},
		{"destructor absorbs close failure", test_destructor_absorbs_close_failure},
		{"concurrent positional reads", test_concurrent_positional_reads},
	};
	std::size_t passed = 0;
	for (const auto &test : tests) {
		std::cout.flush();
		std::cerr.flush();
		const pid_t child = ::fork();
		if (child == -1) { std::cerr << "fork failed\n"; return 2; }
		if (child == 0) {
			// Isolate descriptor ownership and catch infinite zero-progress loops.
			::alarm(5);
			try { test.run(); ::_exit(0); }
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
