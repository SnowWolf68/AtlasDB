#include "Comparator.h"
#include "Record.h"

#include <cstddef>
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
	if (condition) { return; }

	std::ostringstream message;
	message << "line " << line << ": check failed: " << expression;
	throw TestFailure(message.str());
}

template <typename Function>
void check_throws_logic_error(Function &&function, const char *expression,
							  int line) {
	try {
		function();
	} catch (const std::logic_error &) {
		return;
	} catch (const std::exception &error) {
		std::ostringstream message;
		message << "line " << line << ": " << expression
				<< " threw a different std::exception: " << error.what();
		throw TestFailure(message.str());
	} catch (...) {
		std::ostringstream message;
		message << "line " << line << ": " << expression
				<< " threw a non-standard exception";
		throw TestFailure(message.str());
	}

	std::ostringstream message;
	message << "line " << line << ": " << expression
			<< " did not throw std::logic_error";
	throw TestFailure(message.str());
}

#define CHECK(expression) check((expression), #expression, __LINE__)
#define CHECK_THROWS_LOGIC_ERROR(expression)                                  \
	check_throws_logic_error([&]() { static_cast<void>(expression); },          \
							 #expression, __LINE__)

void check_round_trip(const std::string &key, std::uint64_t sequence,
					  ValueType value_type) {
	const InternalKey original(key, sequence, value_type);
	const std::string encoded = InternalKey::encode(original);

	CHECK(encoded.size() == key.size() + 8);

	const InternalKey decoded = InternalKey::decode(encoded);
	CHECK(decoded._key == key);
	CHECK(decoded._commit_sequence == sequence);
	CHECK(decoded._value_type == value_type);
}

void test_sequence_round_trip() {
	constexpr std::uint64_t max_sequence =
		(std::uint64_t{1} << 56) - 1;

	const std::vector<std::uint64_t> sequences = {
		0,
		1,
		127,
		128,
		255,
		256,
		0x01020304050607ULL,
		max_sequence,
	};

	for (const std::uint64_t sequence : sequences) {
		check_round_trip("atlas-key", sequence, Value);
		check_round_trip("atlas-key", sequence, TombStone);
	}
}

void test_empty_and_binary_keys() {
	check_round_trip("", 42, Value);

	const std::string binary_key("a\0b\xff", 4);
	check_round_trip(binary_key, 0x00ff80, TombStone);
}

void test_encoded_layout() {
	const std::string key = "key";
	const InternalKey internal_key(key, 0x01020304050607ULL, Value);
	const std::string encoded = InternalKey::encode(internal_key);

	const std::uint8_t expected_suffix[] = {
		0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x01,
	};

	CHECK(encoded.substr(0, key.size()) == key);
	for (std::size_t i = 0; i < sizeof(expected_suffix); ++i) {
		const auto actual = static_cast<std::uint8_t>(
			static_cast<unsigned char>(encoded[key.size() + i]));
		CHECK(actual == expected_suffix[i]);
	}
}

void test_invalid_sizes() {
	constexpr std::uint64_t first_invalid_sequence =
		std::uint64_t{1} << 56;

	CHECK_THROWS_LOGIC_ERROR(
		InternalKey::encode(InternalKey("key", first_invalid_sequence, Value)));
	CHECK_THROWS_LOGIC_ERROR(InternalKey::decode(std::string(7, '\0')));
}

void test_internal_key_comparator() {
	const InternalKeyComparator comparator;

	const std::string alpha_seq_10_value =
		InternalKey::encode(InternalKey("alpha", 10, Value));
	const std::string alpha_seq_10_tombstone =
		InternalKey::encode(InternalKey("alpha", 10, TombStone));
	const std::string alpha_seq_9_value =
		InternalKey::encode(InternalKey("alpha", 9, Value));
	const std::string beta_seq_100_value =
		InternalKey::encode(InternalKey("beta", 100, Value));

	// user key 升序，sequence 不参与不同 user key 之间的优先级。
	CHECK(comparator(alpha_seq_9_value, beta_seq_100_value) < 0);
	CHECK(comparator(beta_seq_100_value, alpha_seq_9_value) > 0);

	// 相同 user key 时，sequence 降序。
	CHECK(comparator(alpha_seq_10_value, alpha_seq_9_value) < 0);
	CHECK(comparator(alpha_seq_9_value, alpha_seq_10_value) > 0);

	// 相同 user key 和 sequence 时，ValueType 数值降序。
	CHECK(comparator(alpha_seq_10_value, alpha_seq_10_tombstone) < 0);
	CHECK(comparator(alpha_seq_10_tombstone, alpha_seq_10_value) > 0);

	// 三个字段全部相同才相等。
	CHECK(comparator(alpha_seq_10_value, alpha_seq_10_value) == 0);
}

struct TestCase {
	const char *name;
	void (*run)();
};

} // namespace

int main() {
	const TestCase tests[] = {
		{"sequence round trip", test_sequence_round_trip},
		{"empty and binary keys", test_empty_and_binary_keys},
		{"encoded layout", test_encoded_layout},
		{"invalid sizes", test_invalid_sizes},
		{"internal key comparator", test_internal_key_comparator},
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

	std::cout << passed << '/'
			  << (sizeof(tests) / sizeof(tests[0])) << " tests passed\n";

	return passed == (sizeof(tests) / sizeof(tests[0])) ? 0 : 1;
}
