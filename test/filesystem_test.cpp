#include "io/FileSystem.h"
#include "log.h"

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
#include <set>
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
extern "C" int __real_unlink(const char *);
extern "C" int __real_fsync(int);
extern "C" int __real_close(int);

namespace {

namespace fs = std::filesystem;
using atlas::io::AccessMode;
using atlas::io::CreateMode;
using atlas::io::FileSystem;
using atlas::io::OpenOptions;
using Bytes = std::vector<std::uint8_t>;

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
	explicit OwnedFd(int fd) : fd_(fd) { CHECK(fd_ >= 0); }
	OwnedFd(const OwnedFd &) = delete;
	OwnedFd &operator=(const OwnedFd &) = delete;
	~OwnedFd() { if (fd_ >= 0) { __real_close(fd_); } }
	int get() const { return fd_; }
	int release() { return std::exchange(fd_, -1); }
private:
	int fd_;
};

class TempDirectory {
public:
	TempDirectory() {
		char name[] = "/tmp/atlasdb-filesystem-XXXXXX";
		CHECK(::mkdtemp(name) != nullptr);
		path_ = name;
	}
	~TempDirectory() {
		std::error_code error;
		fs::remove_all(path_, error);
	}
	const std::string &path() const { return path_; }
private:
	std::string path_;
};

// Fixtures and observations bypass the production wrappers being tested.
void write_fixture(const std::string &path, const Bytes &data) {
	OwnedFd fd(__real_open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600));
	std::size_t done = 0;
	while (done < data.size()) {
		const ssize_t result = ::write(fd.get(), data.data() + done, data.size() - done);
		if (result == -1 && errno == EINTR) { continue; }
		CHECK(result > 0);
		done += static_cast<std::size_t>(result);
	}
}

Bytes read_fixture(const std::string &path) {
	OwnedFd fd(__real_open(path.c_str(), O_RDONLY));
	Bytes result;
	std::array<std::uint8_t, 128> buffer {};
	while (true) {
		const ssize_t count = ::read(fd.get(), buffer.data(), buffer.size());
		if (count == -1 && errno == EINTR) { continue; }
		CHECK(count >= 0);
		if (count == 0) { return result; }
		result.insert(result.end(), buffer.begin(), buffer.begin() + count);
	}
}

mode_t permissions(const std::string &path) {
	struct stat info {};
	CHECK(::stat(path.c_str(), &info) == 0);
	return info.st_mode & 0777;
}

bool descriptor_open(int fd) {
	errno = 0;
	if (::fcntl(fd, F_GETFD) >= 0) { return true; }
	CHECK(errno == EBADF);
	return false;
}

std::size_t descriptor_count() {
	std::size_t result = 0;
	for (const auto &entry : fs::directory_iterator("/proc/self/fd")) {
		(void)entry;
		++result;
	}
	return result;
}

OpenOptions options(AccessMode access, CreateMode create, bool truncate = false) {
	OpenOptions result;
	result._access_mode = access;
	result._create_mode = create;
	result._truncate = truncate;
	return result;
}

enum class BeforeOpen { None, Remove, Create };

struct Faults {
	explicit Faults(const std::string &watched) : path(watched) {}
	std::string path;
	int fd = -1;
	int open_error = 0;
	int unlink_error = 0;
	int sync_error = 0;
	int close_error = 0;
	BeforeOpen before_open = BeforeOpen::None;
	bool action_applied = false;
	bool close_on_exec = false;
	bool opened_directory = false;
	unsigned open_calls = 0;
	unsigned unlink_calls = 0;
	unsigned sync_calls = 0;
	unsigned close_calls = 0;
};

thread_local Faults *active_faults = nullptr;

class Inject {
public:
	explicit Inject(const std::string &path) : state(path), previous_(active_faults) {
		active_faults = &state;
	}
	Inject(const Inject &) = delete;
	Inject &operator=(const Inject &) = delete;
	~Inject() { active_faults = previous_; }
	Faults state;
private:
	Faults *previous_;
};

Faults *faults_for(const char *path) {
	return active_faults && active_faults->path == path ? active_faults : nullptr;
}

Faults *faults_for(int fd) {
	return active_faults && active_faults->fd == fd ? active_faults : nullptr;
}

void test_default_options(const std::string &) {
	OpenOptions plain;
	OpenOptions braces {};
	for (const auto &value : {plain, braces}) {
		CHECK(value._access_mode == AccessMode::ReadOnly);
		CHECK(value._create_mode == CreateMode::OpenExisting);
		CHECK(!value._truncate);
		CHECK(value._permissions == 0644);
	}
}

void test_open_existing_and_missing(const std::string &dir) {
	const auto path = dir + "/existing";
	const auto count = descriptor_count();
	expect_error([&] { FileSystem::open(path, OpenOptions{}); });
	CHECK(!fs::exists(path));
	CHECK(descriptor_count() == count);
	const Bytes expected{0, 0x80, 0xff, 42};
	write_fixture(path, expected);
	{
		auto file = FileSystem::open(path, OpenOptions{});
		CHECK(file.is_open());
		CHECK(file.size() == expected.size());
		Bytes buffer(expected.size());
		CHECK(file.read_at(0, buffer) == expected.size());
		CHECK(buffer == expected);
	}
	CHECK(read_fixture(path) == expected);
	CHECK(descriptor_count() == count);
}

void test_access_modes(const std::string &dir) {
	const AccessMode modes[] = {AccessMode::ReadOnly, AccessMode::WriteOnly, AccessMode::ReadWrite};
	for (const auto mode : modes) {
		const auto path = dir + "/access";
		write_fixture(path, Bytes{1, 2, 3, 4});
		auto file = FileSystem::open(path, options(mode, CreateMode::OpenExisting));
		Bytes buffer(4);
		if (mode == AccessMode::WriteOnly) {
			expect_error([&] { file.read_at(0, buffer); });
		} else {
			CHECK(file.read_at(0, buffer) == 4 && (buffer == Bytes{1, 2, 3, 4}));
		}
		if (mode == AccessMode::ReadOnly) {
			expect_error([&] { file.write_at(1, Bytes{9}); });
			expect_error([&] { file.append(Bytes{8}); });
			CHECK((read_fixture(path) == Bytes{1, 2, 3, 4}));
		} else {
			CHECK(file.write_at(1, Bytes{9}) == 1);
			CHECK(file.append(Bytes{8}) == 1);
			CHECK((read_fixture(path) == Bytes{1, 9, 3, 4, 8}));
		}
	}
}

void test_open_or_create_missing(const std::string &dir) {
	const auto path = dir + "/new";
	auto file = FileSystem::open(path, options(AccessMode::ReadWrite, CreateMode::OpenOrCreate));
	CHECK(file.is_open() && file.size() == 0);
	CHECK(fs::is_regular_file(path));
	CHECK(file.write_at(0, Bytes{0, 0xff}) == 2);
	CHECK((read_fixture(path) == Bytes{0, 0xff}));
}

void test_open_or_create_preserves_existing(const std::string &dir) {
	const auto path = dir + "/existing";
	const Bytes original{1, 2, 0x80, 0xff};
	write_fixture(path, original);
	struct stat before {}, after {};
	CHECK(::stat(path.c_str(), &before) == 0);
	{
		auto file = FileSystem::open(path, options(AccessMode::ReadWrite, CreateMode::OpenOrCreate));
		CHECK(file.size() == original.size());
	}
	CHECK(::stat(path.c_str(), &after) == 0);
	CHECK(before.st_dev == after.st_dev && before.st_ino == after.st_ino);
	CHECK(read_fixture(path) == original);
}

void test_exclusive_creation(const std::string &dir) {
	const auto path = dir + "/exclusive";
	auto config = options(AccessMode::ReadWrite, CreateMode::CreateNew);
	{
		auto file = FileSystem::open(path, config);
		CHECK(file.is_open());
		CHECK(file.write_at(0, Bytes{1, 2, 3}) == 3);
	}
	const auto count = descriptor_count();
	for (bool truncate : {false, true}) {
		config._truncate = truncate;
		expect_error([&] { FileSystem::open(path, config); });
		CHECK((read_fixture(path) == Bytes{1, 2, 3}));
		CHECK(descriptor_count() == count);
	}
}

void test_exclusive_creation_rejects_symlinks(const std::string &dir) {
	const auto target = dir + "/target", link = dir + "/link";
	CHECK(::symlink(target.c_str(), link.c_str()) == 0);
	expect_error([&] {
		FileSystem::open(link, options(AccessMode::ReadWrite, CreateMode::CreateNew));
	});
	CHECK(fs::is_symlink(link));
	CHECK(!fs::exists(target));
	write_fixture(target, Bytes{7});
	expect_error([&] {
		FileSystem::open(link, options(AccessMode::ReadWrite, CreateMode::CreateNew, true));
	});
	CHECK((read_fixture(target) == Bytes{7}));
}

void test_writable_truncation(const std::string &dir) {
	const auto path = dir + "/truncate";
	for (const auto access : {AccessMode::WriteOnly, AccessMode::ReadWrite}) {
		write_fixture(path, Bytes{1, 2, 3});
		auto file = FileSystem::open(path, options(access, CreateMode::OpenExisting, true));
		CHECK(file.size() == 0 && read_fixture(path).empty());
		CHECK(file.append(Bytes{9}) == 1);
		CHECK((read_fixture(path) == Bytes{9}));
	}
}

void test_open_existing_truncate_missing_does_not_create(const std::string &dir) {
	const auto path = dir + "/missing-truncate";
	const auto count = descriptor_count();
	for (const auto access : {AccessMode::WriteOnly, AccessMode::ReadWrite}) {
		Inject injection(path);
		expect_error([&] {
			FileSystem::open(path, options(access, CreateMode::OpenExisting, true));
		});
		CHECK(injection.state.open_calls == 1);
		CHECK(!fs::exists(path));
		CHECK(descriptor_count() == count);
	}
}

void test_open_or_create_truncation_combinations(const std::string &dir) {
	unsigned index = 0;
	for (const auto access : {AccessMode::WriteOnly, AccessMode::ReadWrite}) {
		for (bool existing : {false, true}) {
			const auto path = dir + "/create-truncate-" + std::to_string(index++);
			struct stat before {}, after {};
			if (existing) {
				write_fixture(path, Bytes{1, 2, 3, 4});
				CHECK(::stat(path.c_str(), &before) == 0);
			}
			auto file = FileSystem::open(path, options(access, CreateMode::OpenOrCreate, true));
			CHECK(file.is_open() && file.size() == 0);
			CHECK(read_fixture(path).empty());
			CHECK(::stat(path.c_str(), &after) == 0);
			if (existing) {
				CHECK(before.st_dev == after.st_dev && before.st_ino == after.st_ino);
				CHECK((before.st_mode & 0777) == (after.st_mode & 0777));
			}
			CHECK(file.append(Bytes{0x80}) == 1);
			CHECK(read_fixture(path) == Bytes{0x80});
		}
	}
}

void test_open_can_return_descriptor_zero(const std::string &dir) {
	const auto path = dir + "/descriptor-zero";
	write_fixture(path, Bytes{0x80, 0xff});
	// Child isolation makes closing stdin local to this case.
	if (descriptor_open(STDIN_FILENO)) { CHECK(__real_close(STDIN_FILENO) == 0); }
	Inject injection(path);
	auto file = FileSystem::open(path, OpenOptions{});
	CHECK(injection.state.fd == STDIN_FILENO);
	CHECK(file.is_open() && file.get_file_path() == path);
	Bytes buffer(2);
	CHECK(file.read_at(0, buffer) == 2 && (buffer == Bytes{0x80, 0xff}));
	file.close();
	CHECK(!descriptor_open(STDIN_FILENO));
}

void test_readonly_truncation_has_no_side_effects(const std::string &dir) {
	const auto existing = dir + "/existing";
	write_fixture(existing, Bytes{1, 2, 3});
	for (const auto create : {CreateMode::OpenExisting, CreateMode::OpenOrCreate, CreateMode::CreateNew}) {
		expect_error([&] {
			FileSystem::open(existing, options(AccessMode::ReadOnly, create, true));
		});
		CHECK((read_fixture(existing) == Bytes{1, 2, 3}));
	}
	const auto missing = dir + "/missing";
	for (const auto create : {CreateMode::OpenOrCreate, CreateMode::CreateNew}) {
		expect_error([&] {
			FileSystem::open(missing, options(AccessMode::ReadOnly, create, true));
		});
		CHECK(!fs::exists(missing));
	}
}

void test_creation_permissions_and_umask(const std::string &dir) {
	struct Case { mode_t requested, mask, expected; };
	const Case cases[] = {{0644, 0022, 0644}, {0644, 0077, 0600}, {0600, 0000, 0600}};
	unsigned index = 0;
	for (const auto &test : cases) {
		// Each case runs in a child process; this never changes the test runner's mask.
		::umask(test.mask);
		const auto path = dir + "/permissions-" + std::to_string(index++);
		auto config = options(AccessMode::ReadWrite, CreateMode::CreateNew);
		config._permissions = test.requested;
		auto file = FileSystem::open(path, config);
		CHECK(permissions(path) == test.expected);
	}
}

void test_permissions_do_not_change_existing(const std::string &dir) {
	const auto path = dir + "/existing";
	write_fixture(path, Bytes{1, 2});
	CHECK(::chmod(path.c_str(), 0640) == 0);
	auto config = options(AccessMode::ReadWrite, CreateMode::OpenOrCreate);
	config._permissions = 0600;
	auto file = FileSystem::open(path, config);
	CHECK(permissions(path) == 0640);
	CHECK((read_fixture(path) == Bytes{1, 2}));
}

void test_descriptor_flags(const std::string &dir) {
	const AccessMode accesses[] = {AccessMode::ReadOnly, AccessMode::WriteOnly, AccessMode::ReadWrite};
	const int native_modes[] = {O_RDONLY, O_WRONLY, O_RDWR};
	unsigned index = 0;
	for (int access = 0; access < 3; ++access) {
		for (const auto create : {CreateMode::OpenExisting, CreateMode::OpenOrCreate, CreateMode::CreateNew}) {
			const auto path = dir + "/flags-" + std::to_string(index++);
			if (create != CreateMode::CreateNew) { write_fixture(path, Bytes{1}); }
			Inject injection(path);
			auto file = FileSystem::open(path, options(accesses[access], create));
			const int fd = injection.state.fd;
			CHECK(fd >= 0 && injection.state.open_calls == 1);
			CHECK(::fcntl(fd, F_GETFD) & FD_CLOEXEC);
			const int flags = ::fcntl(fd, F_GETFL);
			CHECK(flags >= 0 && (flags & O_ACCMODE) == native_modes[access]);
		}
	}
}

void test_open_or_create_after_removal(const std::string &dir) {
	const auto path = dir + "/removed-before-open";
	write_fixture(path, Bytes{1, 2, 3});
	Inject injection(path);
	injection.state.before_open = BeforeOpen::Remove;
	auto file = FileSystem::open(path, options(AccessMode::ReadWrite, CreateMode::OpenOrCreate));
	CHECK(injection.state.action_applied);
	CHECK(file.is_open() && file.size() == 0);
	CHECK(fs::exists(path));
}

void test_open_or_create_after_creation(const std::string &dir) {
	const auto path = dir + "/created-before-open";
	Inject injection(path);
	injection.state.before_open = BeforeOpen::Create;
	auto file = FileSystem::open(path, options(AccessMode::ReadWrite, CreateMode::OpenOrCreate));
	CHECK(injection.state.action_applied);
	CHECK(file.is_open() && file.size() == 3);
	CHECK((read_fixture(path) == Bytes{0xaa, 0x80, 0xff}));
}

void test_open_failures(const std::string &dir) {
	const auto path = dir + "/open-failure";
	const auto count = descriptor_count();
	for (int error : {EACCES, EMFILE}) {
		Inject injection(path);
		injection.state.open_error = error;
		expect_error([&] {
			FileSystem::open(path, options(AccessMode::ReadWrite, CreateMode::OpenOrCreate));
		});
		CHECK(injection.state.open_calls == 1 && injection.state.fd == -1);
		CHECK(!fs::exists(path));
		CHECK(descriptor_count() == count);
	}
	expect_error([&] {
		FileSystem::open(dir + "/missing-parent/file",
			options(AccessMode::ReadWrite, CreateMode::OpenOrCreate));
	});
	CHECK(!fs::exists(dir + "/missing-parent"));
}

void test_exists_and_directory_creation(const std::string &dir) {
	const auto directory = dir + "/created", file = dir + "/file";
	CHECK(!FileSystem::exists(directory));
	CHECK(FileSystem::create_directory(directory));
	CHECK(FileSystem::exists(directory) && fs::is_directory(directory));
	CHECK(!FileSystem::create_directory(directory));
	write_fixture(file, Bytes{1});
	CHECK(FileSystem::exists(file));
	expect_error([&] { FileSystem::create_directory(file); });
	CHECK((read_fixture(file) == Bytes{1}));
	// The current API is create_directory, rather than recursive create_directories.
	expect_error([&] { FileSystem::create_directory(dir + "/missing-parent/child"); });
	CHECK(!fs::exists(dir + "/missing-parent"));
}

void test_directory_listing(const std::string &dir) {
	CHECK(FileSystem::list_directory(dir).empty());
	write_fixture(dir + "/data file", Bytes{1, 2});
	write_fixture(dir + "/.hidden", Bytes{});
	CHECK(::mkdir((dir + "/sub").c_str(), 0700) == 0);
	write_fixture(dir + "/sub/nested", Bytes{3});
	CHECK(::symlink("data file", (dir + "/link").c_str()) == 0);
	std::set<std::string> names;
	const auto entries = FileSystem::list_directory(dir);
	for (const auto &entry : entries) {
		CHECK(entry.path().parent_path() == fs::path(dir));
		const auto name = entry.path().filename().string();
		names.insert(name);
		if (name == "data file") { CHECK(entry.is_regular_file() && entry.file_size() == 2); }
		if (name == "sub") { CHECK(entry.is_directory()); }
		if (name == "link") { CHECK(entry.is_symlink()); }
	}
	CHECK(entries.size() == 4);
	CHECK((names == std::set<std::string>{"data file", ".hidden", "sub", "link"}));
	expect_error([&] { FileSystem::list_directory(dir + "/data file"); });
	expect_error([&] { FileSystem::list_directory(dir + "/missing"); });
}

void test_rename_and_replacement(const std::string &dir) {
	const auto source = dir + "/source", target = dir + "/target";
	write_fixture(source, Bytes{1, 0x80, 0xff});
	FileSystem::rename(source, target);
	CHECK(!fs::exists(source));
	CHECK((read_fixture(target) == Bytes{1, 0x80, 0xff}));
	write_fixture(source, Bytes{7, 8});
	FileSystem::rename(source, target);
	CHECK(!fs::exists(source) && (read_fixture(target) == Bytes{7, 8}));
	expect_error([&] { FileSystem::rename(source, target); });
	CHECK((read_fixture(target) == Bytes{7, 8}));
}

void test_directory_rename_and_nonempty_destination(const std::string &dir) {
	const auto source = dir + "/source-dir", target = dir + "/target-dir";
	CHECK(::mkdir(source.c_str(), 0700) == 0);
	write_fixture(source + "/child", Bytes{1, 0x80});
	FileSystem::rename(source, target);
	CHECK(!fs::exists(source));
	CHECK((read_fixture(target + "/child") == Bytes{1, 0x80}));
	const auto occupied = dir + "/occupied";
	CHECK(::mkdir(occupied.c_str(), 0700) == 0);
	write_fixture(occupied + "/other", Bytes{9});
	expect_error([&] { FileSystem::rename(target, occupied); });
	CHECK((read_fixture(target + "/child") == Bytes{1, 0x80}));
	CHECK(read_fixture(occupied + "/other") == Bytes{9});
}

void test_rename_replaces_symlink_name(const std::string &dir) {
	const auto source = dir + "/source", link = dir + "/link", backing = dir + "/backing";
	write_fixture(source, Bytes{0x80, 0xff});
	write_fixture(backing, Bytes{9, 9});
	CHECK(::symlink(backing.c_str(), link.c_str()) == 0);
	FileSystem::rename(source, link);
	CHECK(!fs::exists(source) && !fs::is_symlink(link));
	CHECK((read_fixture(link) == Bytes{0x80, 0xff}));
	CHECK((read_fixture(backing) == Bytes{9, 9}));
}

void test_file_removal_and_directory_rejection(const std::string &dir) {
	const auto path = dir + "/file";
	write_fixture(path, Bytes{1, 2});
	CHECK(FileSystem::remove_file(path));
	CHECK(!fs::exists(path));
	CHECK(!FileSystem::remove_file(path));
	const auto directory = dir + "/directory";
	CHECK(::mkdir(directory.c_str(), 0700) == 0);
	expect_error([&] { FileSystem::remove_file(directory); });
	CHECK(fs::is_directory(directory));
	write_fixture(directory + "/child", Bytes{3});
	expect_error([&] { FileSystem::remove_file(directory); });
	CHECK((read_fixture(directory + "/child") == Bytes{3}));
}

void test_symlink_removal_preserves_targets(const std::string &dir) {
	const auto target = dir + "/target", directory = dir + "/directory", link = dir + "/link";
	write_fixture(target, Bytes{0x80, 0xff});
	CHECK(::mkdir(directory.c_str(), 0700) == 0);
	for (const auto &destination : {target, directory, dir + "/missing"}) {
		CHECK(::symlink(destination.c_str(), link.c_str()) == 0);
		CHECK(FileSystem::remove_file(link));
		CHECK(!fs::is_symlink(link));
		CHECK((read_fixture(target) == Bytes{0x80, 0xff}));
		CHECK(fs::is_directory(directory));
	}
}

void test_unlink_errors_preserve_file(const std::string &dir) {
	const auto path = dir + "/file";
	write_fixture(path, Bytes{1, 2, 3});
	for (int error : {EACCES, EIO}) {
		Inject injection(path);
		injection.state.unlink_error = error;
		expect_error([&] { FileSystem::remove_file(path); });
		CHECK(injection.state.unlink_calls == 1);
		CHECK((read_fixture(path) == Bytes{1, 2, 3}));
	}
}

void test_sync_directory_releases_handles(const std::string &dir) {
	const auto count = descriptor_count();
	Inject injection(dir);
	for (int i = 0; i < 16; ++i) { FileSystem::sync_dir(dir); }
	CHECK(injection.state.open_calls == 16);
	CHECK(injection.state.sync_calls == 16 && injection.state.close_calls == 16);
	CHECK(injection.state.opened_directory && injection.state.close_on_exec);
	CHECK(!descriptor_open(injection.state.fd));
	CHECK(descriptor_count() == count);
}

void test_sync_directory_open_failures(const std::string &dir) {
	const auto count = descriptor_count();
	const auto file = dir + "/file";
	write_fixture(file, Bytes{1});
	expect_error([&] { FileSystem::sync_dir(file); });
	expect_error([&] { FileSystem::sync_dir(dir + "/missing"); });
	Inject injection(dir);
	injection.state.open_error = EACCES;
	expect_error([&] { FileSystem::sync_dir(dir); });
	CHECK(injection.state.open_calls == 1);
	CHECK(injection.state.sync_calls == 0 && injection.state.close_calls == 0);
	CHECK(descriptor_count() == count);
}

void check_sync_failure(const std::string &dir, bool fail_sync, bool fail_close) {
	const auto count = descriptor_count();
	Inject injection(dir);
	injection.state.sync_error = fail_sync ? EIO : 0;
	injection.state.close_error = fail_close ? EIO : 0;
	// Either cause may be reported when both operations fail. The contract is
	// failure propagation and resource release, not a particular exception text.
	expect_error([&] { FileSystem::sync_dir(dir); });
	CHECK(injection.state.open_calls == 1 && injection.state.sync_calls == 1);
	CHECK(injection.state.close_calls == 1);
	CHECK(!descriptor_open(injection.state.fd));
	CHECK(descriptor_count() == count);
}

void test_fsync_failure_releases_handle(const std::string &dir) {
	check_sync_failure(dir, true, false);
}

void test_sync_close_failure_releases_handle(const std::string &dir) {
	check_sync_failure(dir, false, true);
}

void test_sync_double_failure_releases_handle(const std::string &dir) {
	check_sync_failure(dir, true, true);
}

void test_explicit_close(const std::string &dir) {
	const auto path = dir + "/file";
	write_fixture(path, Bytes{1});
	{
		OwnedFd fd(__real_open(path.c_str(), O_RDONLY));
		const int number = fd.release();
		FileSystem::close(number);
		CHECK(!descriptor_open(number));
	}
	expect_error([&] { FileSystem::close(-1); });
	Inject injection(path);
	OwnedFd fd(__real_open(path.c_str(), O_RDONLY));
	const int number = fd.release();
	injection.state.fd = number;
	injection.state.close_error = EIO;
	expect_error([&] { FileSystem::close(number); });
	CHECK(injection.state.close_calls == 1); // Linux close errors must not cause a retry.
	CHECK(!descriptor_open(number));
}

struct TestCase {
	const char *name;
	void (*run)(const std::string &);
};

} // namespace

extern "C" int __wrap_open(const char *path, int flags, ...) {
	mode_t mode = 0;
	if ((flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE) {
		va_list args;
		va_start(args, flags);
		mode = va_arg(args, mode_t);
		va_end(args);
	}
	auto *faults = faults_for(path);
	if (faults) {
		++faults->open_calls;
		if (faults->open_error) { errno = faults->open_error; return -1; }
		const auto action = std::exchange(faults->before_open, BeforeOpen::None);
		if (action == BeforeOpen::Remove) {
			CHECK(__real_unlink(path) == 0);
			faults->action_applied = true;
		} else if (action == BeforeOpen::Create) {
			write_fixture(path, Bytes{0xaa, 0x80, 0xff});
			faults->action_applied = true;
		}
	}
	const int fd = __real_open(path, flags, mode);
	if (faults) {
		faults->fd = fd;
		if (fd >= 0) {
			faults->close_on_exec = (::fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0;
			struct stat info {};
			CHECK(::fstat(fd, &info) == 0);
			faults->opened_directory = S_ISDIR(info.st_mode);
		}
	}
	return fd;
}

extern "C" int __wrap_unlink(const char *path) {
	if (auto *faults = faults_for(path)) {
		++faults->unlink_calls;
		if (faults->unlink_error) { errno = faults->unlink_error; return -1; }
	}
	return __real_unlink(path);
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
		const int result = __real_close(fd);
		if (result == 0 && faults->close_error) {
			// Linux may release a valid descriptor before reporting a close error.
			errno = faults->close_error;
			return -1;
		}
		return result;
	}
	return __real_close(fd);
}

int main() {
	Log::get_logger().set_minimum_log_level(Log::FATAL);
	const TestCase tests[] = {
		{"default options", test_default_options},
		{"open existing and missing files", test_open_existing_and_missing},
		{"kernel access modes and positional writes", test_access_modes},
		{"open or create missing file", test_open_or_create_missing},
		{"open or create preserves existing file", test_open_or_create_preserves_existing},
		{"exclusive creation preserves existing content", test_exclusive_creation},
		{"exclusive creation rejects symlinks", test_exclusive_creation_rejects_symlinks},
		{"writable truncation", test_writable_truncation},
		{"OpenExisting with truncate never creates missing files", test_open_existing_truncate_missing_does_not_create},
		{"OpenOrCreate truncation creates or reuses the same file", test_open_or_create_truncation_combinations},
		{"open accepts descriptor zero", test_open_can_return_descriptor_zero},
		{"readonly truncation has no side effects", test_readonly_truncation_has_no_side_effects},
		{"creation permissions and umask", test_creation_permissions_and_umask},
		{"creation permissions do not change existing file", test_permissions_do_not_change_existing},
		{"CLOEXEC and access flags across open modes", test_descriptor_flags},
		{"open or create after concurrent removal", test_open_or_create_after_removal},
		{"open or create after concurrent creation", test_open_or_create_after_creation},
		{"open failures leave no resources", test_open_failures},
		{"exists and single-level directory creation", test_exists_and_directory_creation},
		{"directory listing entries and errors", test_directory_listing},
		{"rename, replace and failure preservation", test_rename_and_replacement},
		{"directory rename preserves nonempty destinations on failure", test_directory_rename_and_nonempty_destination},
		{"rename replaces a symlink name without modifying its target", test_rename_replaces_symlink_name},
		{"file removal and directory rejection", test_file_removal_and_directory_rejection},
		{"symlink removal preserves targets", test_symlink_removal_preserves_targets},
		{"unlink errors preserve file", test_unlink_errors_preserve_file},
		{"directory sync releases handles", test_sync_directory_releases_handles},
		{"directory sync open failures", test_sync_directory_open_failures},
		{"fsync failure releases handle", test_fsync_failure_releases_handle},
		{"sync close failure releases handle", test_sync_close_failure_releases_handle},
		{"sync double failure releases handle", test_sync_double_failure_releases_handle},
		{"explicit close and close failure", test_explicit_close},
	};
	std::size_t passed = 0;
	for (const auto &test : tests) {
		// Parent-owned directories can be removed even if a child terminates early.
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
