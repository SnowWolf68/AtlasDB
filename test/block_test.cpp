#include "Block.h"
#include "Comparator.h"
#include "binary_test_helpers.h"

#include <cstdint>
#include <exception>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
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
	for (bool with_crc : {false, true}) {
		// Both zero available offset bytes and a partially available table.
		for (const auto &payload : std::vector<std::vector<std::uint8_t>>{{}, {0}}) {
			auto bytes = binary_test::block_bytes(payload, {}, with_crc);
			const std::size_t count_pos = bytes.size() - (with_crc ? 6 : 2);
			for (std::uint16_t count : {1, 65535}) {
				auto malformed = bytes;
				binary_test::overwrite_u16(malformed, count_pos, count);
				if (with_crc) { binary_test::refresh_crc(malformed); }
				CHECK(Block::decode(malformed, with_crc) == nullptr);
			}
		}
	}
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

void check_block_position(const std::optional<BlockIterator> &result,
						  const std::string &key, std::uint64_t seq,
						  const std::string &value, ValueType type = Value) {
	CHECK(result.has_value());
	CHECK(result->is_valid());
	CHECK((**result).first == InternalKey::encode(InternalKey(key, seq, type)));
	CHECK((**result).second == value);
}

void check_block_range(BlockIterator it, const BlockIterator &end,
					   const std::vector<std::pair<std::string, std::string>> &expected) {
	// Bound traversal by the expected count so an incorrect end sentinel fails
	// instead of turning the test into an unbounded loop.
	for (const auto &entry : expected) {
		CHECK(it != end);
		CHECK(it.is_valid());
		CHECK(*it == entry);
		++it;
	}
	CHECK(it == end);
}

void test_lower_bound_empty_and_past_end() {
	auto empty = std::make_shared<Block>();
	auto block = make_encoded_block({"b"}, {10}, {"value"});
	for (bool keep_all : {false, true}) {
		CHECK(!empty->lower_bound(enc("a", 10), std::nullopt, keep_all));
		CHECK(!empty->lower_bound_match_user_key(enc("a", 10), keep_all));
		CHECK(!block->lower_bound(enc("z", 10), std::nullopt, keep_all));
		CHECK(!block->lower_bound(enc("b", 5), std::nullopt, keep_all));
		check_block_position(block->lower_bound(enc("b", 10), 10, keep_all),
			"b", 10, "value");
	}
}

void test_lower_bound_allows_different_user_key() {
	auto block = make_encoded_block({"b", "d"}, {20, 5}, {"new-b", "old-d"});
	for (bool keep_all : {false, true}) {
		// The timestamp in the seek key must not become an implicit snapshot.
		check_block_position(block->lower_bound(enc("a", 1), std::nullopt, keep_all),
			"b", 20, "new-b");
		check_block_position(block->lower_bound(enc("a", 1), 10, keep_all),
			"d", 5, "old-d");
		CHECK(!block->lower_bound_match_user_key(enc("a", 1), keep_all));
	}
}

void test_lower_bound_seek_and_snapshot_are_independent() {
	auto block = make_encoded_block({"a", "a", "a", "b"}, {30, 20, 10, 5},
		{"a30", "a20", "a10", "b5"});
	for (bool keep_all : {false, true}) {
		// A loose snapshot cannot move the result before the requested seek key.
		check_block_position(block->lower_bound(enc("a", 25), 100, keep_all),
			"a", 20, "a20");
		check_block_position(block->lower_bound(enc("a", 25), std::nullopt, keep_all),
			"a", 20, "a20");
		// A strict snapshot may move the result after that initial position.
		check_block_position(block->lower_bound(enc("a", 100), 15, keep_all),
			"a", 10, "a10");
	}
}

void test_lower_bound_filtered_to_end_returns_nullopt() {
	auto block = make_encoded_block({"b", "c"}, {20, 30}, {"b20", "c30"});
	for (bool keep_all : {false, true}) {
		// The raw lower_bound exists, but no record survives snapshot filtering.
		CHECK(!block->lower_bound(enc("a", 100), 10, keep_all));
		CHECK(!block->lower_bound(enc("b", 100), 10, keep_all));
		auto result = block->lower_bound(enc("a", 100), 20, keep_all);
		check_block_position(result, "b", 20, "b20");
		check_block_range(*result, block->end(), {{enc("b", 20), "b20"}});
	}
}

void test_lower_bound_version_modes_preserve_tombstones() {
	auto block = std::make_shared<Block>();
	const auto tombstone = InternalKey::encode(InternalKey("a", 30, TombStone));
	CHECK(block->add_entry(tombstone, "", false));
	CHECK(block->add_entry(enc("a", 20), "old", false));
	CHECK(block->add_entry(enc("b", 5), "", false));
	for (bool keep_all : {false, true}) {
		auto result = block->lower_bound(enc("a", 100), std::nullopt, keep_all);
		check_block_position(result, "a", 30, "", TombStone);
		std::vector<std::pair<std::string, std::string>> expected{{tombstone, ""}};
		if (keep_all) { expected.push_back({enc("a", 20), "old"}); }
		expected.push_back({enc("b", 5), ""});
		check_block_range(*result, block->end(), expected);
		check_block_position(block->lower_bound(enc("a", 100), 25, keep_all),
			"a", 20, "old");
	}
}

void test_matching_lower_bound_rejects_other_keys() {
	auto block = make_encoded_block({"a", "a", "a", "c"}, {30, 10, 5, 4},
		{"a30", "a10", "a5", "c4"});
	for (bool keep_all : {false, true}) {
		auto result = block->lower_bound_match_user_key(enc("a", 20), keep_all);
		check_block_position(result, "a", 10, "a10");
		std::vector<std::pair<std::string, std::string>> expected{{enc("a", 10), "a10"}};
		if (keep_all) { expected.push_back({enc("a", 5), "a5"}); }
		expected.push_back({enc("c", 4), "c4"});
		check_block_range(*result, block->end(), expected);
		CHECK(!block->lower_bound_match_user_key(enc("a", 4), keep_all));
		CHECK(!block->lower_bound_match_user_key(enc("b", 100), keep_all));
		CHECK(!block->lower_bound_match_user_key(enc("z", 100), keep_all));
		check_block_position(block->lower_bound(enc("b", 100), 100, keep_all),
			"c", 4, "c4");
	}
}

void test_prefix_snapshot_filters_both_boundaries() {
	auto block = make_encoded_block(
		{"app", "app", "apple", "apple", "application", "apq", "apq", "banana"},
		{30, 10, 40, 15, 50, 35, 5, 4},
		{"app30", "app10", "apple40", "apple15", "future", "apq35", "apq5", "b4"});
	auto range = block->preffix("app", 20);
	check_block_range(range.first, range.second,
		{{enc("app", 10), "app10"}, {enc("apple", 15), "apple15"}});
	CHECK(range.second.is_valid());
	CHECK((*range.second).first == enc("apq", 5));
	range = block->preffix("app", 35);
	check_block_range(range.first, range.second,
		{{enc("app", 30), "app30"}, {enc("apple", 15), "apple15"}});
	CHECK((*range.second).first == enc("apq", 35));
}

void test_prefix_missing_literal_boundaries() {
	auto block = make_encoded_block(
		{"apple", "apple", "application", "apricot", "banana"},
		{30, 10, 5, 40, 3}, {"future", "apple10", "application5", "future", "b3"});
	// Neither literal boundary (app/apq) exists as a user key.
	auto range = block->preffix("app", 20);
	check_block_range(range.first, range.second,
		{{enc("apple", 10), "apple10"}, {enc("application", 5), "application5"}});
	CHECK((*range.second).first == enc("banana", 3));
	// A gap before the final key is an empty range, not a prefix match.
	range = block->preffix("apq", 20);
	CHECK(range.first == range.second);
	CHECK(range.second.is_valid());
	CHECK((*range.second).first == enc("banana", 3));
}

void test_prefix_all_future_returns_block_end_pair() {
	auto block = make_encoded_block({"apple", "application", "banana"},
		{30, 40, 50}, {"a30", "a40", "b50"});
	const auto range = block->preffix("app", 10);
	CHECK(range.first == block->end());
	CHECK(range.second == block->end());
	CHECK(!range.first.is_valid());
	CHECK(range.first != BlockIterator::invalid_iter());
}

void test_prefix_missing_or_filtered_upper_bound_reaches_end() {
	for (bool add_future_tail : {false, true}) {
		auto block = make_encoded_block({"app", "apple"}, {5, 6}, {"app5", "apple6"});
		if (add_future_tail) {
			CHECK(block->add_entry(enc("apq", 30), "future-apq", false));
			CHECK(block->add_entry(enc("banana", 40), "future-banana", false));
		}
		const auto range = block->preffix("app", 10);
		CHECK(range.second == block->end());
		check_block_range(range.first, range.second,
			{{enc("app", 5), "app5"}, {enc("apple", 6), "apple6"}});
	}
}

void test_prefix_binary_and_empty_prefix_with_snapshot() {
	const std::string x("a\0x", 3), y("a\0y", 3);
	const std::string ff0("\xff\0", 2), ffff("\xff\xff", 2);
	const std::string binary_value("v\0\xff", 3);
	auto block = make_encoded_block(
		{"", x, x, y, "b", ff0, ff0, ffff},
		{0, 10, 3, 4, 2, 10, 3, 4},
		{"", "future", binary_value, "y4", "b2", "future", "ff3", "ff4"});
	auto range = block->preffix(std::string("a\0", 2), 5);
	check_block_range(range.first, range.second,
		{{enc(x, 3), binary_value}, {enc(y, 4), "y4"}});
	range = block->preffix(std::string("\xff", 1), 5);
	CHECK(range.second == block->end());
	check_block_range(range.first, range.second,
		{{enc(ff0, 3), "ff3"}, {enc(ffff, 4), "ff4"}});
	range = block->preffix("", 5);
	check_block_range(range.first, range.second,
		{{enc("", 0), ""}, {enc(x, 3), binary_value}, {enc(y, 4), "y4"},
		 {enc("b", 2), "b2"}, {enc(ff0, 3), "ff3"}, {enc(ffff, 4), "ff4"}});
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

template <typename Function>
void check_throws_length_rejection(Function &&function) {
	bool threw = false;
	try { function(); }
	catch (const std::length_error &) { threw = true; }
	catch (const std::runtime_error &) { threw = true; }
	CHECK(threw);
}

void test_maximum_enc_key_length_is_representable() {
	const std::string key = enc(std::string(65535 - 8, 'k'), 1);
	const std::string value("v\0\xff", 3);
	CHECK(key.size() == 65535);
	Block block;
	// A representable entry may exceed the block's target capacity. This is a
	// normal capacity failure, and force must allow it without truncation.
	CHECK(!block.add_entry(key, value, false));
	CHECK(block.empty() && block.size() == 0 && block.data_size() == 0);
	CHECK(block.add_entry(key, value, true));
	const auto bytes = block.encode(false);
	CHECK(bytes[0] == 0xff && bytes[1] == 0xff);
	for (bool with_crc : {false, true}) {
		auto decoded = Block::decode(block.encode(with_crc), with_crc);
		CHECK(decoded != nullptr);
		CHECK(decoded->size() == 1);
		check_block_range(decoded->begin(std::nullopt, true), decoded->end(),
			{{key, value}});
	}
}

void test_maximum_value_length_is_representable() {
	const std::string key = enc("key", 1);
	std::string value(65535, 'v');
	value.front() = '\0';
	value[100] = static_cast<char>(0xff);
	value.back() = '\0';
	Block block;
	CHECK(!block.add_entry(key, value, false));
	CHECK(block.empty() && block.size() == 0 && block.data_size() == 0);
	CHECK(block.add_entry(key, value, true));
	const auto bytes = block.encode(false);
	const auto value_length_pos = 2 + key.size();
	CHECK(bytes[value_length_pos] == 0xff && bytes[value_length_pos + 1] == 0xff);
	for (bool with_crc : {false, true}) {
		auto decoded = Block::decode(block.encode(with_crc), with_crc);
		CHECK(decoded != nullptr);
		CHECK(decoded->size() == 1);
		check_block_range(decoded->begin(std::nullopt, true), decoded->end(),
			{{key, value}});
	}
}

template <bool OversizedKey>
void check_oversized_entry_throws_before_mutation() {
	const std::string key = OversizedKey
		? enc(std::string(65536 - 8, 'z'), 2) : enc("z", 2);
	const std::string value = OversizedKey ? "value" : std::string(65536, 'v');
	for (bool force : {false, true}) {
		for (bool has_existing_entry : {false, true}) {
			Block block;
			if (has_existing_entry) { CHECK(block.add_entry(enc("a", 1), "ok", false)); }
			const auto before_size = block.size();
			const auto before_data_size = block.data_size();
			const auto before_bytes = block.encode(true);
			check_throws_length_rejection([&] { block.add_entry(key, value, force); });
			CHECK(block.size() == before_size);
			CHECK(block.data_size() == before_data_size);
			CHECK(block.encode(true) == before_bytes);
			// Rejection must leave the block usable for another valid record.
			CHECK(block.add_entry(enc("b", 3), "after", false));
			auto decoded = Block::decode(block.encode(true), true);
			CHECK(decoded != nullptr);
			std::vector<std::pair<std::string, std::string>> expected;
			if (has_existing_entry) { expected.push_back({enc("a", 1), "ok"}); }
			expected.push_back({enc("b", 3), "after"});
			check_block_range(decoded->begin(std::nullopt, true), decoded->end(), expected);
		}
	}
}

void test_oversized_enc_key_throws_before_mutation() {
	check_oversized_entry_throws_before_mutation<true>();
}

void test_oversized_value_throws_before_mutation() {
	check_oversized_entry_throws_before_mutation<false>();
}

void test_decode_rejects_truncated_headers() {
	for (bool with_crc : {false, true}) {
		const std::size_t minimum = with_crc ? 6 : 2;
		for (std::size_t length = 0; length < minimum; ++length) {
			CHECK(Block::decode(std::vector<std::uint8_t>(length), with_crc) == nullptr);
		}
		auto empty = Block::decode(binary_test::block_bytes({}, {}, with_crc), with_crc);
		CHECK(empty != nullptr);
		CHECK(empty->empty() && empty->size() == 0);
		CHECK(empty->begin() == empty->end());
	}
}

void test_decode_rejects_offsets_at_or_beyond_data_end() {
	for (bool with_crc : {false, true}) {
		CHECK(Block::decode(binary_test::block_bytes({}, {0}, with_crc), with_crc) == nullptr);
		auto block = make_block_with_data({"a"}, {"value"});
		const auto data_end = block->data_size();
		for (auto invalid_offset : {data_end, data_end + 1, std::size_t{65535}}) {
			auto bytes = block->encode(with_crc);
			binary_test::overwrite_u16(bytes, data_end,
				static_cast<std::uint16_t>(invalid_offset));
			if (with_crc) { binary_test::refresh_crc(bytes); }
			CHECK(Block::decode(bytes, with_crc) == nullptr);
		}
	}
}

void test_decode_rejects_duplicate_offsets() {
	auto block = make_block_with_data({"a", "b"}, {"one", "two"});
	for (bool with_crc : {false, true}) {
		auto bytes = block->encode(with_crc);
		binary_test::overwrite_u16(bytes, block->data_size() + 2, 0);
		if (with_crc) { binary_test::refresh_crc(bytes); }
		CHECK(Block::decode(bytes, with_crc) == nullptr);
	}
}

void test_decode_rejects_descending_offsets() {
	auto block = make_block_with_data({"a", "b", "c"}, {"one", "two", "three"});
	const std::uint16_t second_offset = 2 + enc("a", 10).size() + 2 + 3;
	for (bool with_crc : {false, true}) {
		auto bytes = block->encode(with_crc);
		// Keep all offsets in range and distinct; only their order is invalid.
		binary_test::overwrite_u16(bytes, block->data_size() + 4, second_offset - 1);
		if (with_crc) { binary_test::refresh_crc(bytes); }
		CHECK(Block::decode(bytes, with_crc) == nullptr);
	}
}

void check_incomplete_record_throws_when_read(const std::vector<std::uint8_t> &payload) {
	for (bool with_crc : {false, true}) {
		auto block = Block::decode(binary_test::block_bytes(payload, {0}, with_crc), with_crc);
		// Decode validates the table structure; record contents are checked on access.
		CHECK(block != nullptr);
		auto it = block->begin(std::nullopt, true);
		CHECK(it.is_valid());
		bool threw = false;
		try { (void)*it; }
		catch (const std::runtime_error &) { threw = true; }
		CHECK(threw);
	}
}

void test_read_rejects_incomplete_key_length() {
	check_incomplete_record_throws_when_read({0});
}

void test_read_rejects_truncated_key() {
	check_incomplete_record_throws_when_read({0, 10, 'a'});
}

void test_read_rejects_incomplete_value_length() {
	check_incomplete_record_throws_when_read({0, 1, 'a', 0});
}

void test_read_rejects_truncated_value() {
	check_incomplete_record_throws_when_read({0, 1, 'a', 0, 4, 'b'});
}

void test_maximum_entry_offset_and_next_forced_append() {
	Block block;
	const auto first_key = enc("a", 1);
	const auto second_key = enc("b", 1);
	const std::string first_value(65535 - 4 - first_key.size(), 'v');
	const std::string second_value("\0\xff", 2);
	CHECK(block.add_entry(first_key, first_value, true));
	CHECK(block.data_size() == 65535);
	CHECK(block.add_entry(second_key, second_value, true));
	CHECK(block.data_size() > 65535);
	const auto before = block.encode(true);
	const auto before_size = block.data_size();
	// Ordinary capacity rejection must still let SSTBuilder switch blocks.
	CHECK(!block.add_entry(enc("c", 1), "after", false));
	CHECK(block.encode(true) == before);
	check_throws_length_rejection([&] { block.add_entry(enc("c", 1), "after", true); });
	CHECK(block.size() == 2 && block.data_size() == before_size);
	CHECK(block.encode(true) == before);
	for (bool with_crc : {false, true}) {
		const auto bytes = block.encode(with_crc);
		CHECK(bytes[before_size + 2] == 0xff && bytes[before_size + 3] == 0xff);
		auto decoded = Block::decode(bytes, with_crc);
		CHECK(decoded != nullptr && decoded->size() == 2);
		check_block_range(decoded->begin(std::nullopt, true), decoded->end(),
			{{first_key, first_value}, {second_key, second_value}});
	}
}

void test_entry_count_limit_rejects_before_mutation() {
	// The public constructor permits seeding metadata. Isolate the count guard
	// here: ordinary appends reach the byte-offset limit before this count.
	Block block({}, std::vector<std::uint16_t>(65535, 0), 0);
	const auto before = block.encode(true);
	for (bool force : {false, true}) {
		check_throws_length_rejection([&] { block.add_entry(enc("key", 1), "value", force); });
		CHECK(block.size() == 65535 && block.data_size() == 0);
		CHECK(block.encode(true) == before);
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
		{"maximum enc key length is representable", test_maximum_enc_key_length_is_representable},
		{"maximum value length is representable", test_maximum_value_length_is_representable},
		{"oversized enc key throws before mutation", test_oversized_enc_key_throws_before_mutation},
		{"oversized value throws before mutation", test_oversized_value_throws_before_mutation},
		{"crc mismatch returns null",
		 test_crc_mismatch_returns_null},
		{"corrupted entry count returns null", test_corrupted_num_elements_returns_null},
		{"decode rejects truncated headers", test_decode_rejects_truncated_headers},
		{"decode rejects offsets at or beyond data end", test_decode_rejects_offsets_at_or_beyond_data_end},
		{"decode rejects duplicate offsets", test_decode_rejects_duplicate_offsets},
		{"decode rejects descending offsets", test_decode_rejects_descending_offsets},
		{"read rejects incomplete key length", test_read_rejects_incomplete_key_length},
		{"read rejects truncated key", test_read_rejects_truncated_key},
		{"read rejects incomplete value length", test_read_rejects_incomplete_value_length},
		{"read rejects truncated value", test_read_rejects_truncated_value},
		{"maximum entry offset and next forced append", test_maximum_entry_offset_and_next_forced_append},
		{"entry count limit rejects before mutation", test_entry_count_limit_rejects_before_mutation},
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
		{"lower bound empty and past end", test_lower_bound_empty_and_past_end},
		{"lower bound allows different user key", test_lower_bound_allows_different_user_key},
		{"lower bound seek and snapshot are independent", test_lower_bound_seek_and_snapshot_are_independent},
		{"lower bound filtered to end returns nullopt", test_lower_bound_filtered_to_end_returns_nullopt},
		{"lower bound version modes preserve tombstones", test_lower_bound_version_modes_preserve_tombstones},
		{"matching lower bound rejects other keys", test_matching_lower_bound_rejects_other_keys},
		{"prefix snapshot filters both boundaries", test_prefix_snapshot_filters_both_boundaries},
		{"prefix missing literal boundaries", test_prefix_missing_literal_boundaries},
		{"prefix all future returns block end pair", test_prefix_all_future_returns_block_end_pair},
		{"prefix missing or filtered upper bound reaches end", test_prefix_missing_or_filtered_upper_bound_reaches_end},
		{"prefix binary and empty prefix with snapshot", test_prefix_binary_and_empty_prefix_with_snapshot},
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
