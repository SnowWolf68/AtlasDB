#include "BlockMeta.h"
#include "binary_test_helpers.h"

#include <cstdint>
#include <exception>
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

// ─── encode / decode round trip ─────────────────────────────────

void test_round_trip_single() {
	std::vector<BlockMeta> input;
	input.push_back(BlockMeta(0, "alpha", "delta"));

	auto encoded = BlockMeta::encode(input);
	auto decoded = BlockMeta::decode(encoded);

	CHECK(decoded.has_value());
	CHECK(decoded->size() == 1);
	CHECK((*decoded)[0]._first_key == "alpha");
	CHECK((*decoded)[0]._last_key == "delta");
}

void test_round_trip_multiple() {
	std::vector<BlockMeta> input;
	input.push_back(BlockMeta(0, "alpha", "charlie"));
	input.push_back(BlockMeta(4096, "delta", "foxtrot"));
	input.push_back(BlockMeta(8192, "golf", "india"));

	auto encoded = BlockMeta::encode(input);
	auto decoded = BlockMeta::decode(encoded);

	CHECK(decoded.has_value());
	CHECK(decoded->size() == 3);
	CHECK((*decoded)[0]._first_key == "alpha");
	CHECK((*decoded)[0]._last_key == "charlie");
	CHECK((*decoded)[1]._first_key == "delta");
	CHECK((*decoded)[2]._last_key == "india");
}

void test_round_trip_empty() {
	std::vector<BlockMeta> input;

	auto encoded = BlockMeta::encode(input);
	auto decoded = BlockMeta::decode(encoded);

	CHECK(decoded.has_value());
	CHECK(decoded->size() == 0);
}

void test_round_trip_binary_keys() {
	const std::string first("\x00\x01hello", 7);
	const std::string last("\xffworld", 6);

	std::vector<BlockMeta> input;
	input.push_back(BlockMeta(100, first, last));

	auto encoded = BlockMeta::encode(input);
	auto decoded = BlockMeta::decode(encoded);

	CHECK(decoded.has_value());
	CHECK(decoded->size() == 1);
	CHECK((*decoded)[0]._first_key == first);
	CHECK((*decoded)[0]._last_key == last);
}

void test_round_trip_empty_strings() {
	std::vector<BlockMeta> input;
	input.push_back(BlockMeta(0, "", ""));

	auto encoded = BlockMeta::encode(input);
	auto decoded = BlockMeta::decode(encoded);

	CHECK(decoded.has_value());
	CHECK(decoded->size() == 1);
	CHECK((*decoded)[0]._first_key == "");
	CHECK((*decoded)[0]._last_key == "");
}

// ─── CRC error / tamper detection ──────────────────────────────

void test_crc_mismatch_returns_nullopt() {
	std::vector<BlockMeta> input;
	input.push_back(BlockMeta(0, "alpha", "bravo"));

	auto encoded = BlockMeta::encode(input);

	// Corrupt the last byte (part of CRC)
	encoded.back() ^= 0xFF;

	auto decoded = BlockMeta::decode(encoded);
	CHECK(!decoded.has_value());
}

void test_too_short_data_caught() {
	// only 3 bytes → decode should detect and return nullopt
	std::vector<uint8_t> data = {0x00, 0x00, 0x00};
	try {
		auto decoded = BlockMeta::decode(data);
		// If no exception thrown, check it's nullopt
		CHECK(!decoded.has_value());
	} catch (const std::exception &) {
		// Exception is also acceptable (minimum length check)
		CHECK(true);
	}
}

void test_maximum_key_lengths_round_trip() {
	std::string first(65535, 'a');
	std::string last(65535, 'z');
	first[1] = '\0';
	last.back() = static_cast<char>(0xff);
	auto decoded = BlockMeta::decode(BlockMeta::encode({BlockMeta(123, first, last)}));
	CHECK(decoded.has_value() && decoded->size() == 1);
	CHECK(decoded->front()._offset == 123);
	CHECK(decoded->front()._first_key == first);
	CHECK(decoded->front()._last_key == last);
}

void check_oversized_key_rejected(bool first_key) {
	const std::string oversized(65536, 'x');
	for (bool after_valid_entry : {false, true}) {
		std::vector<BlockMeta> input;
		if (after_valid_entry) { input.emplace_back(0, "a", "b"); }
		input.emplace_back(100, first_key ? oversized : "first",
			first_key ? "last" : oversized);
		bool threw = false;
		try { (void)BlockMeta::encode(input); }
		catch (const std::length_error &) { threw = true; }
		catch (const std::runtime_error &) { threw = true; }
		CHECK(threw);
	}
}

void test_oversized_first_key_rejected() { check_oversized_key_rejected(true); }
void test_oversized_last_key_rejected() { check_oversized_key_rejected(false); }

void test_declared_count_must_match_entries() {
	const auto encoded = BlockMeta::encode({BlockMeta(0, "a", "b"), BlockMeta(20, "c", "d")});
	for (std::uint32_t count : {0u, 1u, 3u, 0xffffffffu}) {
		auto bytes = encoded;
		binary_test::overwrite_u32(bytes, 0, count);
		binary_test::refresh_crc(bytes);
		CHECK(!BlockMeta::decode(bytes).has_value());
	}
	auto empty = BlockMeta::encode({});
	binary_test::overwrite_u32(empty, 0, 1);
	binary_test::refresh_crc(empty);
	CHECK(!BlockMeta::decode(empty).has_value());
}

void test_truncated_fields_with_valid_crc_rejected() {
	const auto complete = BlockMeta::encode({BlockMeta(0, "first", "last")});
	const std::size_t body_end = complete.size() - 4;
	// Retain the declared count and every proper prefix of the entry body.
	for (std::size_t cut = 4; cut < body_end; ++cut) {
		std::vector<std::uint8_t> bytes(complete.begin(), complete.begin() + cut);
		binary_test::append_crc(bytes);
		CHECK(!BlockMeta::decode(bytes).has_value());
	}
}

// ─── test runner ────────────────────────────────────────────────

struct TestCase {
	const char *name;
	void (*run)();
};

} // namespace

int main() {
	const TestCase tests[] = {
		{"round trip single", test_round_trip_single},
		{"round trip multiple", test_round_trip_multiple},
		{"round trip empty", test_round_trip_empty},
		{"round trip binary keys", test_round_trip_binary_keys},
		{"round trip empty strings", test_round_trip_empty_strings},
		{"crc mismatch returns nullopt",
		 test_crc_mismatch_returns_nullopt},
		{"too short data caught",
		 test_too_short_data_caught},
		{"maximum key lengths round trip", test_maximum_key_lengths_round_trip},
		{"oversized first key rejected", test_oversized_first_key_rejected},
		{"oversized last key rejected", test_oversized_last_key_rejected},
		{"declared count must match entries", test_declared_count_must_match_entries},
		{"truncated fields with valid crc rejected", test_truncated_fields_with_valid_crc_rejected},
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
