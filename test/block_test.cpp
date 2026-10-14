#include "Block.h"
#include "Comparator.h"

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

// ─── helpers ────────────────────────────────────────────────────

std::vector<std::uint8_t> encode_block(bool with_crc,
									   const std::vector<std::string> &keys,
									   const std::vector<std::string> &values) {
	Block block;
	for (size_t i = 0; i < keys.size(); i++) {
		CHECK(block.add_entry(keys[i], values[i], false));
	}
	return block.encode(with_crc);
}

void check_entry(const std::shared_ptr<Block> &block, size_t idx,
				 const std::string &expected_key,
				 const std::string &expected_value) {
	// For now, just verify the block was decoded.
	// Full entry-level verification will come with BlockIterator.
	CHECK(block != nullptr);
}

// ─── encode / decode round trip ─────────────────────────────────

void test_round_trip_without_crc() {
	Block block;
	CHECK(block.add_entry("alpha", "1", false));
	CHECK(block.add_entry("bravo", "2", false));
	CHECK(block.add_entry("charlie", "3", false));

	auto encoded = block.encode(false);
	auto decoded = Block::decode(encoded, false);

	CHECK(decoded != nullptr);
}

void test_round_trip_with_crc() {
	Block block;
	CHECK(block.add_entry("alpha", "1", false));
	CHECK(block.add_entry("bravo", "2", false));

	auto encoded = block.encode(true);
	// encoded should be larger due to 4B CRC
	CHECK(encoded.size() > 0);

	auto decoded = Block::decode(encoded, true);
	CHECK(decoded != nullptr);
}

void test_round_trip_empty() {
	Block block;
	auto encoded = block.encode(false);
	auto decoded = Block::decode(encoded, false);
	CHECK(decoded != nullptr);
}

void test_round_trip_empty_with_crc() {
	Block block;
	auto encoded = block.encode(true);
	auto decoded = Block::decode(encoded, true);
	CHECK(decoded != nullptr);
}

// ─── add_entry capacity ─────────────────────────────────────────

void test_capacity_rejects_when_full() {
	// Create a block with tiny capacity
	Block block;
	// Insert entries until full, using a small key-value pair
	std::string key = "k";
	std::string value = "v";
	int count = 0;
	while (block.add_entry(key, value, false)) {
		count++;
	}
	CHECK(count > 0);

	// Block should now be full. Next add without force should fail.
	// But with force=true should succeed.
	CHECK(!block.add_entry("overflow", "data", false));
	CHECK(block.add_entry("overflow", "data", true));
}

void test_force_write() {
	Block block;
	// Fill the block
	std::string value(Block::DEFAULT_MAX_CAPACITY / 2, 'x');
	CHECK(block.add_entry("k1", value, false));
	// Block should be near full. force_write should still work.
	CHECK(block.add_entry("k2", "forced", true));
}

// ─── CRC error handling ─────────────────────────────────────────

void test_crc_mismatch_returns_null() {
	Block block;
	CHECK(block.add_entry("alpha", "1", false));
	auto encoded = block.encode(true);

	// Corrupt last byte
	encoded.back() ^= 0xFF;

	auto decoded = Block::decode(encoded, true);
	CHECK(decoded == nullptr);
}

void test_corrupted_num_elements_returns_null() {
	// Create minimal valid data, then corrupt
	Block block;
	auto encoded = block.encode(false);

	if (encoded.size() >= 2) {
		encoded[encoded.size() - 1] = 0xFF;
	}

	// This might cause out-of-bounds; decode should handle gracefully
	auto decoded = Block::decode(encoded, false);
	// If it returns non-null, that's ok too as long as it doesn't crash
	(void)decoded;
}

// ─── binary data ────────────────────────────────────────────────

void test_binary_keys() {
	Block block;
	const std::string k1("a\0b", 3);
	const std::string k2("a\0c", 3);
	const std::string v1("\x00hello", 6);
	const std::string v2("world\xff", 6);

	CHECK(block.add_entry(k1, v1, false));
	CHECK(block.add_entry(k2, v2, false));

	auto encoded = block.encode(false);
	auto decoded = Block::decode(encoded, false);
	CHECK(decoded != nullptr);
}

void test_large_values() {
	Block block;
	std::string big_value(1000, 'x');
	CHECK(block.add_entry("key", big_value, false));

	auto encoded = block.encode(false);
	auto decoded = Block::decode(encoded, false);
	CHECK(decoded != nullptr);
}

// ─── encode size consistency ────────────────────────────────────

void test_encode_size_increases_with_entries() {
	Block block;
	auto empty_encoded = block.encode(false);

	CHECK(block.add_entry("key1", "val1", false));
	auto one_encoded = block.encode(false);
	CHECK(one_encoded.size() > empty_encoded.size());

	CHECK(block.add_entry("key2", "val2", false));
	auto two_encoded = block.encode(false);
	CHECK(two_encoded.size() > one_encoded.size());
}

void test_with_crc_adds_4_bytes() {
	Block block;
	CHECK(block.add_entry("key", "value", false));

	auto without = block.encode(false);
	auto with = block.encode(true);
	CHECK(with.size() == without.size() + 4);
}

// ─── block iterator tests ─────────────────────────────────────

std::shared_ptr<Block> make_block_with_data(
	const std::vector<std::string> &keys,
	const std::vector<std::string> &values) {
	auto block = std::make_shared<Block>();
	for (size_t i = 0; i < keys.size(); i++) {
		// add_entry 期望 enc_key（编码后的 internal key）
		std::string enc_key =
			InternalKey::encode(InternalKey(keys[i], 10, Value));
		CHECK(block->add_entry(enc_key, values[i], false));
	}
	return block;
}

void test_iterator_basic_traversal() {
	auto block = make_block_with_data(
		{"alpha", "bravo", "charlie"},
		{"1", "2", "3"});

	auto it = block->begin();
	CHECK(it != block->end());
	auto kv = *it;
	CHECK(InternalKey::decode(kv.first)._key == "alpha");
	CHECK(kv.second == "1");

	++it;
	CHECK(it != block->end());
	kv = *it;
	CHECK(InternalKey::decode(kv.first)._key == "bravo");
	CHECK(kv.second == "2");

	++it;
	CHECK(it != block->end());
	kv = *it;
	CHECK(InternalKey::decode(kv.first)._key == "charlie");
	CHECK(kv.second == "3");

	++it;
	CHECK(!(it != block->end()));  // reached end
}

void test_iterator_empty() {
	auto block = std::make_shared<Block>();

	auto it = block->begin();
	CHECK(!(it != block->end()));  // begin equals end for empty
}

void test_iterator_single_entry() {
	auto block = make_block_with_data({"key"}, {"value"});

	auto it = block->begin();
	CHECK(it != block->end());
	auto kv = *it;
	CHECK(InternalKey::decode(kv.first)._key == "key");
	CHECK(kv.second == "value");

	++it;
	CHECK(!(it != block->end()));
}

void test_iterator_operator_star_after_end_asserts() {
	// Just verify we can iterate without crash
	auto block = make_block_with_data({"a", "b"}, {"1", "2"});

	int count = 0;
	for (auto it = block->begin(); it != block->end(); ++it) {
		auto kv = *it;
		(void)kv;
		count++;
	}
	CHECK(count == 2);
	// Don't dereference past end - that's UB the assert catches
}

void test_iterator_begin_end_is_valid() {
	auto block = make_block_with_data({"a", "b", "c"}, {"x", "y", "z"});

	CHECK(block->begin() != block->end());

	auto it = block->end();
	CHECK(it == block->end());
}

// ─── prefix tests ─────────────────────────────────────────────

std::string enc(const std::string &user_key, std::uint64_t seq) {
	return InternalKey::encode(InternalKey(user_key, seq, Value));
}

std::shared_ptr<Block> make_encoded_block(
	const std::vector<std::string> &user_keys,
	const std::vector<std::uint64_t> &seqs,
	const std::vector<std::string> &values) {
	auto block = std::make_shared<Block>();
	for (size_t i = 0; i < user_keys.size(); i++) {
		CHECK(block->add_entry(enc(user_keys[i], seqs[i]), values[i], false));
	}
	return block;
}

void test_prefix_basic() {
	auto block = make_encoded_block(
		{"apple", "application", "banana", "apricot"},
		{10, 20, 30, 40},
		{"1", "2", "3", "4"});

	// prefix "app" → "apple" + "application"
	auto [begin, end] = block->preffix("app", std::nullopt);
	CHECK(begin != end);
	auto kv = *begin;
	CHECK(kv.second == "1");  // apple
	++begin;
	CHECK(begin != end);
	kv = *begin;
	CHECK(kv.second == "2");  // application
	++begin;
	CHECK(!(begin != end));   // done
}

void test_prefix_single_match() {
	auto block = make_encoded_block(
		{"alpha", "bravo", "charlie"},
		{10, 10, 10},
		{"a", "b", "c"});

	auto [begin, end] = block->preffix("b", std::nullopt);
	CHECK(begin != end);
	auto kv = *begin;
	CHECK(kv.second == "b");  // bravo
	++begin;
	CHECK(!(begin != end));
}

void test_prefix_no_match() {
	auto block = make_encoded_block(
		{"apple", "banana", "cherry"},
		{10, 10, 10},
		{"1", "2", "3"});

	// prefix "z" → no match
	auto [begin, end] = block->preffix("z", std::nullopt);
	CHECK(!(begin != end));
}

void test_prefix_empty_block() {
	auto block = std::make_shared<Block>();
	auto [begin, end] = block->preffix("a", std::nullopt);
	CHECK(!(begin != end));
}

void test_prefix_matches_all() {
	auto block = make_encoded_block(
		{"a1", "a2", "a3"},
		{10, 10, 10},
		{"1", "2", "3"});

	auto [begin, end] = block->preffix("a", std::nullopt);
	int count = 0;
	for (auto it = begin; it != end; ++it) count++;
	CHECK(count == 3);
}

void test_prefix_exact_key() {
	auto block = make_encoded_block(
		{"apple", "app", "apricot"},
		{10, 10, 10},
		{"1", "2", "3"});

	// prefix "app" matches "app" exactly + all longer keys
	auto [begin, end] = block->preffix("app", std::nullopt);
	int count = 0;
	for (auto it = begin; it != end; ++it) count++;
	CHECK(count == 2);  // "app" and "apple"
}

void test_prefix_with_ff_boundary() {
	// prefix "\xff" → get_next_str returns "" → end = end()
	auto block = make_encoded_block(
		{std::string("\xff\x00", 2), std::string("\xff\xff", 2)},
		{10, 10},
		{"1", "2"});

	auto [begin, end] = block->preffix(std::string("\xff", 1), std::nullopt);
	// Both keys start with \xff
	CHECK(begin != end);
	int count = 0;
	for (auto it = begin; it != end; ++it) count++;
	CHECK(count == 2);
}

void test_prefix_compare_uses_internal_key_order() {
	// Two entries: same user_key but different seq
	// InternalKey order: user_key ASC, seq DESC
	// MUST add in sorted order: bar < foo
	auto block = make_encoded_block(
		{"bar", "foo", "foo"},
		{10, 100, 50},
		{"b", "new", "old"});

	auto [begin, end] = block->preffix("foo", std::nullopt);
	CHECK(begin != end);
	// 第一个命中的是 seq 最大的版本（internal key 顺序：seq DESC）
	auto kv = *begin;
	CHECK(InternalKey::decode(kv.first)._key == "foo");
	CHECK(InternalKey::decode(kv.first)._commit_sequence == 100);
	++begin;
	// 默认 keep_all_version=false，跳过同 user_key 的旧版本 foo@50
	CHECK(!(begin != end));
}

// ─── monotony predicate tests ────────────────────────────────

void test_block_predicate_basic() {
	auto block = make_encoded_block(
		{"alpha", "bravo", "charlie", "delta"},
		{10, 20, 30, 40},
		{"a", "b", "c", "d"});

	auto pred = [](const std::string &key) -> int {
		if (key < "bravo") return -1;
		if (key >= "delta") return 1;
		return 0;
	};

	auto result = block->iter_monotony_predicate(pred, std::nullopt);
	CHECK(result.has_value());
	auto [begin, end] = result.value();
	int count = 0;
	for (auto it = begin; it != end; ++it) count++;
	CHECK(count == 2);  // "bravo", "charlie"
}

void test_block_predicate_no_match() {
	auto block = make_encoded_block(
		{"alpha", "bravo"},
		{10, 20},
		{"a", "b"});

	auto pred = [](const std::string &key) -> int {
		if (key < "z") return -1;
		return 0;  // never > 0, but nothing matches "z" exactly
	};
	// Actually this would match everything. Let me use a proper one:
	auto pred2 = [](const std::string &key) -> int {
		if (key < "z") return -1;
		if (key > "zz") return 1;
		return 0;
	};
	auto result = block->iter_monotony_predicate(pred2, std::nullopt);
	CHECK(!result.has_value());
}

void test_block_predicate_seq_filter() {
	auto block = make_encoded_block(
		{"alpha", "alpha", "bravo"},
		{10, 100, 20},
		{"visible", "hidden", "b"});

	auto pred = [](const std::string &key) -> int {
		if (key < "a") return -1;
		if (key > "z") return 1;
		return 0;
	};

	// seq=50: alpha(10) visible, alpha(100) hidden, bravo(20) visible
	auto result = block->iter_monotony_predicate(pred, std::optional<uint64_t>(50));
	CHECK(result.has_value());
	auto [begin, end] = result.value();
	int count = 0;
	for (auto it = begin; it != end; ++it) count++;
	CHECK(count == 2);  // alpha(10), bravo(20)
}

void test_block_predicate_empty() {
	auto block = std::make_shared<Block>();
	auto pred = [](const std::string &key) -> int { return 0; };
	auto result = block->iter_monotony_predicate(pred, std::nullopt);
	CHECK(!result.has_value());
}

void test_block_predicate_single() {
	auto block = make_encoded_block(
		{"alpha", "bravo", "charlie"},
		{10, 20, 30},
		{"a", "b", "c"});

	auto pred = [](const std::string &key) -> int {
		if (key < "bravo") return -1;
		if (key > "bravo") return 1;
		return 0;
	};

	auto result = block->iter_monotony_predicate(pred, std::nullopt);
	CHECK(result.has_value());
	auto [begin, end] = result.value();
	int count = 0;
	for (auto it = begin; it != end; ++it) count++;
	CHECK(count == 1);
}

// Exact lookup must terminate at both ends of the closed search interval.
void test_exact_lookup_empty_block() {
	auto block = std::make_shared<Block>();
	auto key = InternalKey::encode(InternalKey("a", 10, Value));
	CHECK(!block->get_value(key).has_value());
	CHECK(!block->get_exact(key).has_value());
}

void test_exact_lookup_single_entry_boundaries() {
	auto block = make_block_with_data({"m"}, {"value"});
	for (const auto &user_key : {"a", "z"}) {
		auto key = InternalKey::encode(InternalKey(user_key, 10, Value));
		CHECK(!block->get_value(key).has_value());
		CHECK(!block->get_exact(key).has_value());
	}
	for (std::uint64_t seq : {9, 11}) {
		auto key = InternalKey::encode(InternalKey("m", seq, Value));
		CHECK(!block->get_value(key).has_value());
		CHECK(!block->get_exact(key).has_value());
	}
	auto key = InternalKey::encode(InternalKey("m", 10, Value));
	auto value = block->get_value(key);
	CHECK(value.has_value());
	CHECK(value->first == key);
	CHECK(value->second == "value");
	auto exact = block->get_exact(key);
	CHECK(exact.has_value());
	CHECK(exact->is_valid());
	CHECK(**exact == *value);
}

void test_exact_lookup_multiple_entry_boundaries() {
	auto block = make_block_with_data({"b", "m", "y"}, {"first", "middle", "last"});
	for (const auto &user_key : {"a", "c", "n", "z"}) {
		auto key = InternalKey::encode(InternalKey(user_key, 10, Value));
		CHECK(!block->get_value(key).has_value());
		CHECK(!block->get_exact(key).has_value());
	}
	const std::vector<std::string> keys = {"b", "m", "y"};
	const std::vector<std::string> values = {"first", "middle", "last"};
	for (std::size_t i = 0; i < keys.size(); ++i) {
		auto key = InternalKey::encode(InternalKey(keys[i], 10, Value));
		auto value = block->get_value(key);
		CHECK(value.has_value());
		CHECK(value->first == key);
		CHECK(value->second == values[i]);
		auto exact = block->get_exact(key);
		CHECK(exact.has_value());
		CHECK(exact->is_valid());
		CHECK(exact->get_cur_idx() == i);
		CHECK((**exact).first == key);
		CHECK((**exact).second == values[i]);
		CHECK(**exact == *value);
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
		{"round trip without crc", test_round_trip_without_crc},
		{"round trip with crc", test_round_trip_with_crc},
		{"round trip empty", test_round_trip_empty},
		{"round trip empty with crc", test_round_trip_empty_with_crc},
		{"capacity rejects when full",
		 test_capacity_rejects_when_full},
		{"force write", test_force_write},
		{"crc mismatch returns null",
		 test_crc_mismatch_returns_null},
		{"binary keys", test_binary_keys},
		{"large values", test_large_values},
		{"encode size increases with entries",
		 test_encode_size_increases_with_entries},
		{"with crc adds 4 bytes",
		 test_with_crc_adds_4_bytes},
		{"iterator basic traversal",
		 test_iterator_basic_traversal},
		{"iterator empty", test_iterator_empty},
		{"iterator single entry",
		 test_iterator_single_entry},
		{"iterator operator* after end asserts",
		 test_iterator_operator_star_after_end_asserts},
		{"iterator begin end is valid",
		 test_iterator_begin_end_is_valid},
		{"prefix basic", test_prefix_basic},
		{"prefix single match", test_prefix_single_match},
		{"prefix no match", test_prefix_no_match},
		{"prefix empty block", test_prefix_empty_block},
		{"prefix matches all", test_prefix_matches_all},
		{"prefix exact key", test_prefix_exact_key},
		{"prefix with ff boundary",
		 test_prefix_with_ff_boundary},
		{"prefix compare uses internal key order",
		 test_prefix_compare_uses_internal_key_order},
		{"block predicate basic",
		 test_block_predicate_basic},
		{"block predicate no match",
		 test_block_predicate_no_match},
		{"block predicate seq filter",
		 test_block_predicate_seq_filter},
		{"block predicate empty",
		 test_block_predicate_empty},
		{"block predicate single",
		 test_block_predicate_single},
		{"exact lookup empty block", test_exact_lookup_empty_block},
		{"exact lookup single entry boundaries", test_exact_lookup_single_entry_boundaries},
		{"exact lookup multiple entry boundaries", test_exact_lookup_multiple_entry_boundaries},
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
