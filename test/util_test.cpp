#include "util.h"

#include <cstdint>
#include <exception>
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
