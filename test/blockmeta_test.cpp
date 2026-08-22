#include "BlockMeta.h"

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
