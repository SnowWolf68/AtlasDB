#include "util.h"

#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

class TestFailure : public std::runtime_error {
public:
	explicit TestFailure(const std::string &message)
		: std::runtime_error(message) {}
};

void check(bool condition, const char *expression, int line) {
	if (condition) {
		return;
	}

	std::ostringstream message;
	message << "line " << line << ": check failed: " << expression;
	throw TestFailure(message.str());
}

#define CHECK(expression) check((expression), #expression, __LINE__)

// ─── helper functions ──────────────────────────────────────────

std::string tmp_path(const std::string &name) {
	return "/tmp/atlasdb_test_" + name;
}

void rm_tmp(const std::string &path) {
	std::remove(path.c_str());
}

// ─── get_bytes / decode round trip ─────────────────────────────

void test_uint16t_round_trip() {
	std::vector<uint16_t> values = {0, 1, 255, 256, 0xABCD, 0xFFFF};
	for (auto v : values) {
		auto bytes = get_bytes_of_uint16t(v);
		CHECK(bytes.size() == 2);
		auto decoded = decode_to_uint16(bytes.begin(), bytes.end());
		CHECK(decoded == v);
	}
}

void test_uint32t_round_trip() {
	std::vector<uint32_t> values = {0, 1, 255, 256, 0xABCDEF12, 0xFFFFFFFF};
	for (auto v : values) {
		auto bytes = get_bytes_of_uint32t(v);
		CHECK(bytes.size() == 4);
		auto decoded = decode_to_uint32(bytes.begin(), bytes.end());
		CHECK(decoded == v);
	}
}

// ─── create and read ──────────────────────────────────────────

void test_create_and_read() {
	std::string path = tmp_path("test_create");
	std::vector<uint8_t> data = {'h', 'e', 'l', 'l', 'o'};

	auto w = FileObj::create_and_write(path, data);
	CHECK(w.is_valid());
	CHECK(w.size() == 5);
	w.close();

	auto r = FileObj::open_read(path);
	CHECK(r.is_valid());
	auto read_back = r.read(0, 5);
	CHECK(read_back.size() == 5);
	CHECK(read_back[0] == 'h');
	CHECK(read_back[4] == 'o');

	r.close();
	rm_tmp(path);
}

void test_read_uint32_middle() {
	std::string path = tmp_path("test_uint32");
	std::vector<uint8_t> data = {0x00, 0x00, 0x01, 0x02, 0x03};

	auto w = FileObj::create_and_write(path, data);
	w.close();

	auto f = FileObj::open_read(path);
	CHECK(f.is_valid());
	auto val = f.read_uint32(1);
	CHECK(val == 0x00010203U);
	f.close();
	rm_tmp(path);
}

void test_read_uint16_middle() {
	std::string path = tmp_path("test_uint16");
	std::vector<uint8_t> data = {0x12, 0x34, 0x56};

	auto w = FileObj::create_and_write(path, data);
	w.close();

	auto f = FileObj::open_read(path);
	auto val = f.read_uint16(1);  // 0x34 0x56
	CHECK(val == 0x3456U);
	f.close();
	rm_tmp(path);
}

void test_read_uint8() {
	std::string path = tmp_path("test_uint8");
	std::vector<uint8_t> data = {0x12, 0x34};

	auto w = FileObj::create_and_write(path, data);
	w.close();

	auto f = FileObj::open_read(path);
	CHECK(f.read_uint8(0) == 0x12);
	CHECK(f.read_uint8(1) == 0x34);
	f.close();
	rm_tmp(path);
}

// ─── open_read ─────────────────────────────────────────────────

void test_open_read_existing() {
	std::string path = tmp_path("test_open");
	std::vector<uint8_t> data = {'w', 'o', 'r', 'l', 'd'};

	auto w = FileObj::create_and_write(path, data);
	w.close();

	auto r = FileObj::open_read(path);
	CHECK(r.is_valid());
	CHECK(r.size() == 5);

	auto read_back = r.read(0, 5);
	CHECK(read_back[0] == 'w');
	CHECK(read_back[4] == 'd');

	r.close();
	rm_tmp(path);
}

void test_open_read_nonexistent() {
	auto f = FileObj::open_read("/tmp/atlasdb_test_nonexistent_xyz.xyz");
	CHECK(!f.is_valid());
}

// ─── append ────────────────────────────────────────────────────

void test_append_data() {
	std::string path = tmp_path("test_append");
	std::vector<uint8_t> data = {'a', 'b'};

	auto w = FileObj::create_and_write(path, data);
	CHECK(w.size() == 2);
	std::vector<uint8_t> more = {'c', 'd'};
	CHECK(w.append_data(more));
	CHECK(w.size() == 4);
	w.close();

	auto r = FileObj::open_read(path);
	auto all = r.read(0, 4);
	CHECK(all[0] == 'a');
	CHECK(all[3] == 'd');
	r.close();
	rm_tmp(path);
}

// ─── close ─────────────────────────────────────────────────────

void test_close_then_operations_return_fail() {
	std::string path = tmp_path("test_close");
	std::vector<uint8_t> data = {'x'};

	auto f = FileObj::create_and_write(path, data);
	CHECK(f.close());

	// After close, read on the closed fd should throw
	bool threw = false;
	try {
		auto rd = f.read(0, 1);
		(void)rd;
	} catch (const std::exception &) {
		threw = true;
	}
	CHECK(threw);
	rm_tmp(path);
}

// ─── empty data ────────────────────────────────────────────────

void test_create_write_and_read_empty() {
	std::string path = tmp_path("test_empty");
	std::vector<uint8_t> data;

	auto f = FileObj::create_and_write(path, data);
	CHECK(f.size() == 0);

	auto read_back = f.read(0, 0);
	CHECK(read_back.empty());

	f.close();
	rm_tmp(path);
}

// ─── atomic_write ──────────────────────────────────────────────

void test_atomic_write_publishes_and_replaces() {
	std::string path = tmp_path("test_atomic_write");
	std::string tmp = path + ".tmp";

	CHECK(FileObj::atomic_write(path, get_bytes_of_uint64t(7)));
	CHECK(!std::filesystem::exists(tmp)); // tmp consumed by rename
	{
		auto f = FileObj::open_read(path);
		CHECK(f.is_valid());
		CHECK(f.size() == 8);
		CHECK(f.read_uint64(0) == 7);
		f.close();
	}

	// A second publish atomically replaces the first one.
	CHECK(FileObj::atomic_write(path, get_bytes_of_uint64t(9)));
	CHECK(!std::filesystem::exists(tmp));
	{
		auto f = FileObj::open_read(path);
		CHECK(f.size() == 8);
		CHECK(f.read_uint64(0) == 9);
		f.close();
	}

	rm_tmp(path);
	rm_tmp(tmp);
}

std::size_t open_fd_count() {
	// directory_iterator itself holds one descriptor; both counts include it.
	std::size_t count = 0;
	for (const auto &entry :
		 std::filesystem::directory_iterator("/proc/self/fd")) {
		(void)entry;
		++count;
	}
	return count;
}

void test_atomic_write_leaks_no_descriptor() {
#if defined(__linux__)
	std::string path = tmp_path("test_atomic_fd");
	const std::size_t before = open_fd_count();
	for (int i = 0; i < 30; i++) {
		CHECK(FileObj::atomic_write(path, get_bytes_of_uint64t(i)));
	}
	CHECK(open_fd_count() == before);
	rm_tmp(path);
	rm_tmp(path + ".tmp");
#else
	std::cout << "[SKIP] /proc/self/fd descriptor accounting unavailable\n";
#endif
}

void test_legacy_cursor_read_and_zero_length_eof() {
	const auto path = tmp_path("cursor_read_boundaries");
	const std::vector<std::uint8_t> data = {'a', 0, 'b', 0xff, 'c'};
	CHECK(FileObj::create_and_write(path, data).is_valid());
	auto file = FileObj::open_read(path);
	CHECK(file.read_continue(0).empty());
	CHECK((file.read_continue(2) == std::vector<std::uint8_t>{'a', 0}));
	CHECK((file.read_continue(20) == std::vector<std::uint8_t>{'b', 0xff, 'c'}));
	CHECK(file.read_continue(1).empty());
	CHECK(file.seek_begin());
	CHECK(file.read_continue(data.size()) == data);
	CHECK(file.close());
	bool threw = false;
	try { (void)file.read_continue(1); } catch (const std::runtime_error &) { threw = true; }
	CHECK(threw);
	rm_tmp(path);
}

void test_legacy_binary_write_uint64_and_clear() {
	const auto path = tmp_path("legacy_write_and_clear");
	CHECK(FileObj::create_and_write(path, {}).is_valid());
	auto file = FileObj::open_rw(path);
	const std::vector<std::uint8_t> bytes = {0, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77};
	CHECK(FileObj::get_bytes_of_uint64(0x0011223344556677ULL) == bytes);
	CHECK(file.write_uint64(0x0011223344556677ULL) == bytes.size());
	CHECK(file.read(0, bytes.size()) == bytes);
	CHECK(file.clear()); CHECK(file.size() == 0);
	CHECK(file.seek_begin()); // truncate does not promise to reset the cursor
	CHECK(file.write_data({0, 0xff, 'x'}) == 3);
	CHECK((file.read(0, 3) == std::vector<std::uint8_t>{0, 0xff, 'x'}));
	CHECK(file.close());
	CHECK(!file.clear());
	rm_tmp(path);
}

// ─── test runner ────────────────────────────────────────────────

struct TestCase {
	const char *name;
	void (*run)();
};

} // namespace

int main() {
	const TestCase tests[] = {
		{"uint16t round trip", test_uint16t_round_trip},
		{"uint32t round trip", test_uint32t_round_trip},
		{"create and read", test_create_and_read},
		{"read uint32 middle", test_read_uint32_middle},
		{"read uint16 middle", test_read_uint16_middle},
		{"read uint8", test_read_uint8},
		{"open read existing", test_open_read_existing},
		{"open read nonexistent", test_open_read_nonexistent},
		{"append data", test_append_data},
		{"close then operations fail", test_close_then_operations_return_fail},
		{"create write and read empty",
		 test_create_write_and_read_empty},
		{"atomic write publishes and replaces",
		 test_atomic_write_publishes_and_replaces},
		{"atomic write leaks no descriptor",
		 test_atomic_write_leaks_no_descriptor},
		{"legacy cursor reads handle zero length and EOF", test_legacy_cursor_read_and_zero_length_eof},
		{"legacy binary writes and clear", test_legacy_binary_write_uint64_and_clear},
	};

	std::size_t passed = 0;
	for (const TestCase &test : tests) {
		try {
			test.run();
			++passed;
			std::cout << "[PASS] " << test.name << '\n';
		} catch (const std::exception &error) {
			std::cerr << "[FAIL] " << test.name << ": " << error.what()
					  << '\n';
		}
	}

	std::cout << passed << '/' << (sizeof(tests) / sizeof(tests[0]))
			  << " tests passed\n";
	return passed == (sizeof(tests) / sizeof(tests[0])) ? 0 : 1;
}
