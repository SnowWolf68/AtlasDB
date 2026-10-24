#include "codec.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using Bytes = std::vector<std::uint8_t>;

void check(bool condition, const char *expression, int line) {
	if (condition) { return; }
	std::ostringstream message;
	message << "line " << line << ": check failed: " << expression;
	throw std::runtime_error(message.str());
}

#define CHECK(expression) check((expression), #expression, __LINE__)

void test_integer_append_layout() {
	Bytes data{0xaa, 0xbb};
	codec::append_u8(data, 0x80);
	codec::append_u16_be(data, 0x1234);
	codec::append_u32_be(data, 0x89abcdef);
	codec::append_u64_be(data, UINT64_C(0x0123456789abcdef));
	const Bytes expected{0xaa, 0xbb, 0x80, 0x12, 0x34,
		0x89, 0xab, 0xcd, 0xef,
		0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef};
	CHECK(data == expected);
}

void test_integer_read_layout_and_offsets() {
	// A hand-written fixture prevents matching encoder/decoder mistakes from
	// passing only because their round trip agrees. Multibyte reads are unaligned.
	const Bytes data{0xaa, 0xbb, 0x80, 0x12, 0x34,
		0x89, 0xab, 0xcd, 0xef,
		0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef};
	const Bytes original = data;
	std::size_t pos = 2;
	CHECK(codec::read_u8(data, pos) == 0x80);
	CHECK(pos == 2);
	pos = 3;
	CHECK(codec::read_u16_be(data, pos) == 0x1234);
	CHECK(pos == 3);
	pos = 5;
	CHECK(codec::read_u32_be(data, pos) == UINT32_C(0x89abcdef));
	CHECK(pos == 5);
	pos = 9;
	CHECK(codec::read_u64_be(data, pos) == UINT64_C(0x0123456789abcdef));
	CHECK(pos == 9);
	CHECK(codec::read_u64_be(data, pos) == UINT64_C(0x0123456789abcdef));
	CHECK(data == original);
}

template <typename UInt, typename Append, typename Read>
void check_integer_values(Append append, Read read) {
	const UInt maximum = std::numeric_limits<UInt>::max();
	Bytes zeros;
	append(zeros, UInt{0});
	CHECK(zeros == Bytes(sizeof(UInt), 0));
	Bytes ones;
	append(ones, maximum);
	CHECK(ones == Bytes(sizeof(UInt), 0xff));
	CHECK(read(zeros, 0) == 0);
	CHECK(read(ones, 0) == maximum);

	for (std::size_t bit = 0; bit < sizeof(UInt) * 8; ++bit) {
		const UInt value = static_cast<UInt>(UINT64_C(1) << bit);
		Bytes data{0xfe};
		append(data, value);
		CHECK(data.size() == 1 + sizeof(UInt));
		CHECK(data.front() == 0xfe);
		CHECK(read(data, 1) == value);
	}
}

void test_integer_values() {
	check_integer_values<std::uint8_t>(codec::append_u8, codec::read_u8);
	check_integer_values<std::uint16_t>(codec::append_u16_be, codec::read_u16_be);
	check_integer_values<std::uint32_t>(codec::append_u32_be, codec::read_u32_be);
	check_integer_values<std::uint64_t>(codec::append_u64_be, codec::read_u64_be);
	for (unsigned value = 0; value <= 255; ++value) {
		Bytes data;
		codec::append_u8(data, static_cast<std::uint8_t>(value));
		CHECK(codec::read_u8(data, 0) == value);
	}
}

template <typename Read>
void check_integer_bounds(std::size_t width, Read read) {
	for (std::size_t size = 0; size <= width + 1; ++size) {
		const Bytes data(size, 0xff);
		for (std::size_t pos = 0; pos <= size + 1; ++pos) {
			const bool fits = pos <= size && width <= size - pos;
			CHECK(read(data, pos).has_value() == fits);
		}
		const auto maximum = std::numeric_limits<std::size_t>::max();
		CHECK(!read(data, maximum));
		CHECK(!read(data, maximum - width + 1));
	}
}

void test_integer_read_bounds() {
	check_integer_bounds(1, codec::read_u8);
	check_integer_bounds(2, codec::read_u16_be);
	check_integer_bounds(4, codec::read_u32_be);
	check_integer_bounds(8, codec::read_u64_be);
}

void test_append_bytes() {
	const Bytes source{0x00, 0x80, 0xff};
	Bytes empty;
	codec::append_bytes(empty, source);
	CHECK(empty == source);
	Bytes data{0xaa};
	codec::append_bytes(data, source);
	CHECK((data == Bytes{0xaa, 0x00, 0x80, 0xff}));
	CHECK((source == Bytes{0x00, 0x80, 0xff}));
	codec::append_bytes(data, Bytes{});
	CHECK((data == Bytes{0xaa, 0x00, 0x80, 0xff}));
	Bytes both_empty;
	codec::append_bytes(both_empty, Bytes{});
	CHECK(both_empty.empty());
}

void test_append_string() {
	const std::string source("A\0\x80\xff", 4);
	Bytes empty;
	codec::append_string(empty, source);
	CHECK((empty == Bytes{0x41, 0x00, 0x80, 0xff}));
	Bytes data{0xaa};
	codec::append_string(data, source);
	// Exactly four payload bytes: no extra C string terminator or length prefix.
	CHECK((data == Bytes{0xaa, 0x41, 0x00, 0x80, 0xff}));
	CHECK(source == std::string("A\0\x80\xff", 4));
	codec::append_string(data, "");
	CHECK((data == Bytes{0xaa, 0x41, 0x00, 0x80, 0xff}));
	Bytes both_empty;
	codec::append_string(both_empty, "");
	CHECK(both_empty.empty());
}

void test_read_bytes() {
	Bytes data{0xaa, 0x00, 0x80, 0xff, 0xbb};
	const Bytes original = data;
	std::size_t pos = 1;
	auto result = codec::read_bytes(data, pos, 3);
	CHECK(result.has_value());
	CHECK((*result == Bytes{0x00, 0x80, 0xff}));
	CHECK(pos == 1);
	CHECK(codec::read_bytes(data, pos, 3) == result);
	CHECK(codec::read_bytes(data, 0, data.size()) == data);
	CHECK((codec::read_bytes(data, 3, 2) == Bytes{0xff, 0xbb}));
	CHECK(data == original);
	(*result)[0] = 0x12;
	CHECK(data == original);
	data[2] = 0x34;
	CHECK((*result)[1] == 0x80);
}

void test_read_string() {
	Bytes data{0xaa, 0x41, 0x00, 0x80, 0xff};
	const Bytes original = data;
	std::size_t pos = 1;
	auto result = codec::read_string(data, pos, 4);
	CHECK(result.has_value());
	CHECK(*result == std::string("A\0\x80\xff", 4));
	CHECK(pos == 1);
	CHECK(codec::read_string(data, pos, 4) == result);
	CHECK(codec::read_string(data, 2, 1) == std::string(1, '\0'));
	CHECK(data == original);
	(*result)[0] = 'B';
	CHECK(data == original);
	data[3] = 0x12;
	CHECK(static_cast<unsigned char>((*result)[2]) == 0x80);
}

void test_zero_length_reads() {
	for (const Bytes &data : {Bytes{}, Bytes{0x00, 0x80, 0xff}}) {
		for (std::size_t pos = 0; pos <= data.size(); ++pos) {
			const auto bytes = codec::read_bytes(data, pos, 0);
			const auto string = codec::read_string(data, pos, 0);
			CHECK(bytes.has_value());
			CHECK(bytes->empty());
			CHECK(string.has_value());
			CHECK(string->empty());
		}
		CHECK(!codec::read_bytes(data, data.size() + 1, 0));
		CHECK(!codec::read_string(data, data.size() + 1, 0));
		const auto maximum = std::numeric_limits<std::size_t>::max();
		CHECK(!codec::read_bytes(data, maximum, 0));
		CHECK(!codec::read_string(data, maximum, 0));
	}
}

void test_payload_read_bounds() {
	const auto maximum = std::numeric_limits<std::size_t>::max();
	for (std::size_t size = 0; size <= 8; ++size) {
		const Bytes data(size, 0x41);
		for (std::size_t pos = 0; pos <= size + 1; ++pos) {
			for (std::size_t length = 0; length <= size + 1; ++length) {
				const bool fits = pos <= size && length <= size - pos;
				CHECK(codec::read_bytes(data, pos, length).has_value() == fits);
				CHECK(codec::read_string(data, pos, length).has_value() == fits);
			}
			CHECK(!codec::read_bytes(data, pos, maximum));
			CHECK(!codec::read_string(data, pos, maximum));
		}
		CHECK(!codec::read_bytes(data, maximum, 1));
		CHECK(!codec::read_string(data, maximum, 1));
		CHECK(!codec::read_bytes(data, maximum - 1, 3));
		CHECK(!codec::read_string(data, maximum - 1, 3));
	}
}

void test_self_append_empty() {
	Bytes data;
	codec::append_bytes(data, data);
	CHECK(data.empty());
}

void test_self_append_with_reallocation() {
	Bytes data{0x00, 0x80, 0xff};
	// Fill any spare capacity so doubling must allocate, without relying on
	// implementation-specific vector growth or shrink_to_fit behavior.
	data.resize(data.capacity(), 0x42);
	const Bytes original = data;
	Bytes expected = original;
	expected.insert(expected.end(), original.begin(), original.end());
	const auto old_capacity = data.capacity();
	codec::append_bytes(data, data);
	CHECK(data == expected);
	CHECK(data.capacity() > old_capacity);
}

void test_self_append_with_spare_capacity() {
	Bytes data{0x00, 0x80, 0xff};
	data.reserve(32);
	const auto old_capacity = data.capacity();
	codec::append_bytes(data, data);
	CHECK((data == Bytes{0x00, 0x80, 0xff, 0x00, 0x80, 0xff}));
	CHECK(data.capacity() == old_capacity);
}

void test_mixed_record() {
	// Representative WAL fields: type, count, key length/key, value length/value.
	Bytes encoded;
	codec::append_u8(encoded, 0x02);
	codec::append_u32_be(encoded, 1);
	codec::append_u16_be(encoded, 3);
	codec::append_string(encoded, std::string("k\0\xff", 3));
	codec::append_u16_be(encoded, 2);
	codec::append_bytes(encoded, Bytes{0x80, 0x00});
	const Bytes expected{0x02, 0, 0, 0, 1, 0, 3, 0x6b, 0, 0xff,
		0, 2, 0x80, 0};
	CHECK(encoded == expected);
	CHECK(codec::read_u8(expected, 0) == 2);
	CHECK(codec::read_u32_be(expected, 1) == 1);
	CHECK(codec::read_u16_be(expected, 5) == 3);
	CHECK(codec::read_string(expected, 7, 3) == std::string("k\0\xff", 3));
	CHECK(codec::read_u16_be(expected, 10) == 2);
	CHECK((codec::read_bytes(expected, 12, 2) == Bytes{0x80, 0}));
}

struct TestCase {
	const char *name;
	void (*run)();
};

} // namespace

int main() {
	const TestCase tests[] = {
		{"integer append layout", test_integer_append_layout},
		{"integer read layout and offsets", test_integer_read_layout_and_offsets},
		{"integer boundary and bit values", test_integer_values},
		{"integer read bounds", test_integer_read_bounds},
		{"append bytes", test_append_bytes},
		{"append string", test_append_string},
		{"read bytes", test_read_bytes},
		{"read string", test_read_string},
		{"zero length reads", test_zero_length_reads},
		{"payload read bounds", test_payload_read_bounds},
		{"empty self append", test_self_append_empty},
		{"self append with reallocation", test_self_append_with_reallocation},
		{"self append with spare capacity", test_self_append_with_spare_capacity},
		{"mixed record", test_mixed_record},
	};
	std::size_t passed = 0;
	for (const auto &test : tests) {
		try {
			test.run();
			++passed;
			std::cout << "[PASS] " << test.name << '\n';
		} catch (const std::exception &error) {
			std::cerr << "[FAIL] " << test.name << ": " << error.what() << '\n';
		} catch (...) {
			std::cerr << "[FAIL] " << test.name << ": unknown exception\n";
		}
	}
	const auto total = sizeof(tests) / sizeof(tests[0]);
	std::cout << passed << '/' << total << " tests passed\n";
	return passed == total ? 0 : 1;
}
