#include "net/buffer.h"

#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void check(bool condition, const char *expression, int line) {
	if (condition) { return; }
	std::ostringstream message;
	message << "line " << line << ": check failed: " << expression;
	throw std::runtime_error(message.str());
}

#define CHECK(expression) check((expression), #expression, __LINE__)

std::string as_string(const std::vector<char> &bytes) {
	return std::string(bytes.begin(), bytes.end());
}

void test_partial_read_and_binary_data() {
	Buffer buffer;
	const char bytes[] = {'a', '\0', 'b', 'c', 'd'};
	buffer.append(bytes, 5);
	CHECK(buffer.get_readable_size() == 5);
	CHECK(buffer.peek() == std::string_view(bytes, 5));
	CHECK(!buffer.read(6).has_value());
	CHECK(buffer.get_readable_size() == 5);

	auto first = buffer.read(2);
	CHECK(first.has_value());
	CHECK(as_string(*first) == std::string(bytes, 2));
	CHECK(buffer.peek() == "bcd");
	CHECK(buffer.erase(2));
	CHECK(buffer.peek() == "d");
	CHECK(buffer.erase(1));
	CHECK(buffer.get_readable_size() == 0);
}

void test_append_after_consumption_compacts_data() {
	Buffer buffer;
	buffer.append("abcdefghijkl", 12);
	CHECK(buffer.erase(8));
	buffer.append("0123456789", 10);
	CHECK(buffer.get_readable_size() == 14);
	CHECK(buffer.peek() == "ijkl0123456789");
	CHECK(as_string(*buffer.read(4)) == "ijkl");
	CHECK(buffer.peek() == "0123456789");
}

void test_append_grows_when_total_free_space_is_insufficient() {
	Buffer buffer;
	const std::vector<char> first(16, 'x');
	const std::vector<char> second(17, 'y');
	buffer.append(first);
	buffer.append(second);
	CHECK(buffer.get_readable_size() == 33);
	CHECK(buffer.peek() == std::string(16, 'x') + std::string(17, 'y'));
	CHECK(as_string(*buffer.read(33)) ==
		  std::string(16, 'x') + std::string(17, 'y'));
	CHECK(buffer.peek().empty());
}

void test_reuse_after_full_consumption() {
	Buffer buffer;
	for (int i = 0; i < 50; ++i) {
		buffer.append("abc", 3);
		CHECK(buffer.peek() == "abc");
		CHECK(buffer.erase(3));
		CHECK(buffer.peek().empty());
	}
	buffer.append("tail", 4);
	CHECK(buffer.peek() == "tail");
}

void test_invalid_lengths_preserve_data() {
	Buffer buffer;
	buffer.append("abc", 3);
	CHECK(!buffer.read(-1).has_value());
	CHECK(!buffer.erase(-1));
	CHECK(!buffer.erase(4));
	buffer.append("ignored", -1);
	CHECK(buffer.peek() == "abc");
	CHECK(buffer.read(0).has_value());
	CHECK(buffer.peek() == "abc");
}

struct TestCase {
	const char *name;
	void (*run)();
};

} // namespace

int main() {
	const TestCase tests[] = {
		{"partial read and binary data", test_partial_read_and_binary_data},
		{"append after consumption compacts data",
		 test_append_after_consumption_compacts_data},
		{"append grows when free space is insufficient",
		 test_append_grows_when_total_free_space_is_insufficient},
		{"reuse after full consumption", test_reuse_after_full_consumption},
		{"invalid lengths preserve data", test_invalid_lengths_preserve_data},
	};

	std::size_t passed = 0;
	for (const auto &test : tests) {
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
