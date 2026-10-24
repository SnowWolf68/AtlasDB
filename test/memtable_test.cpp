#include "MemTable.h"

#include <cstdint>
#include <cstdio>
#include <exception>
#include <iostream>
#include <memory>
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

void check_result_is_notfound(const LookUpResult &result) {
	CHECK(result._value_type == NotFound);
}

void check_result_is_value(const LookUpResult &result,
						   const std::string &expected_value,
						   std::uint64_t expected_seq) {
	CHECK(result._value_type == Value);
	CHECK(result._value == expected_value);
	CHECK(result._commit_sequence == expected_seq);
}

void check_result_is_tombstone(const LookUpResult &result,
							   std::uint64_t expected_seq) {
	CHECK(result._value_type == TombStone);
	CHECK(result._commit_sequence == expected_seq);
}

// ─── basic CRUD ─────────────────────────────────────────────────

void test_empty_memtable_get() {
	MemTable table;
	auto result = table.get("anything", 100);
	check_result_is_notfound(result);
}

void test_put_and_get() {
	MemTable table;
	CHECK(table.put("alpha", "hello", 10)._is_add);

	auto result = table.get("alpha", 10);
	check_result_is_value(result, "hello", 10);

	result = table.get("alpha", 100);
	check_result_is_value(result, "hello", 10);
}

void test_put_multiple_keys() {
	MemTable table;
	CHECK(table.put("alpha", "1", 10)._is_add);
	CHECK(table.put("bravo", "2", 20)._is_add);
	CHECK(table.put("charlie", "3", 30)._is_add);

	check_result_is_value(table.get("alpha", 100), "1", 10);
	check_result_is_value(table.get("bravo", 100), "2", 20);
	check_result_is_value(table.get("charlie", 100), "3", 30);
	check_result_is_notfound(table.get("delta", 100));
}

void test_put_same_key_different_sequence() {
	MemTable table;
	CHECK(table.put("alpha", "first", 10)._is_add);
	CHECK(table.put("alpha", "second", 20)._is_add);
	CHECK(table.put("alpha", "third", 30)._is_add);

	// get with seq=50 should see the latest version (seq=30)
	check_result_is_value(table.get("alpha", 50), "third", 30);
	// get with seq=15 should see seq=10
	check_result_is_value(table.get("alpha", 15), "first", 10);
	// get with seq=5 should see nothing
	check_result_is_notfound(table.get("alpha", 5));
}

void test_remove_and_get() {
	MemTable table;
	CHECK(table.put("alpha", "hello", 10)._is_add);
	CHECK(table.remove("alpha", 20)._is_success);

	// get with seq=30 should see the tombstone
	check_result_is_tombstone(table.get("alpha", 30), 20);
	// get with seq=15 should still see the value
	check_result_is_value(table.get("alpha", 15), "hello", 10);
}

void test_tombstone_not_confused_with_empty_value() {
	MemTable table;
	// empty value is a legitimate value
	CHECK(table.put("alpha", "", 10)._is_add);

	auto result = table.get("alpha", 10);
	check_result_is_value(result, "", 10);

	// remove the key
	CHECK(table.remove("alpha", 20)._is_success);

	result = table.get("alpha", 100);
	check_result_is_tombstone(result, 20);
}

// ─── edge cases ─────────────────────────────────────────────────

void test_empty_key() {
	MemTable table;
	CHECK(table.put("", "empty-key-value", 10)._is_add);

	auto result = table.get("", 10);
	check_result_is_value(result, "empty-key-value", 10);

	CHECK(table.remove("", 20)._is_success);
	check_result_is_tombstone(table.get("", 100), 20);
}

void test_binary_key_and_value() {
	MemTable table;
	const std::string binary_key("a\0b\xff", 4);
	const std::string binary_value("\x00hello\xff", 7);

	CHECK(table.put(binary_key, binary_value, 10)._is_add);

	auto result = table.get(binary_key, 100);
	check_result_is_value(result, binary_value, 10);
}

void test_get_nonexistent_key() {
	MemTable table;
	CHECK(table.put("alpha", "1", 10)._is_add);

	check_result_is_notfound(table.get("bravo", 10));
	check_result_is_notfound(table.get("ALPHA", 10));
	check_result_is_notfound(table.get("alphaa", 10));
}

// ─── freeze ─────────────────────────────────────────────────────

void test_freeze_triggers_and_data_persists() {
	// Use a very small threshold so freeze triggers quickly
	// SEQ_AND_VALTYPE_SIZE = 8, so each put adds key+value+8 bytes
	constexpr std::size_t tiny_threshold = 100;
	MemTable table(tiny_threshold,
				   std::make_shared<InternalKeyComparator>());

	// Write enough to trigger freeze (key=4 + value=10 + 8 = 22 each)
	for (int i = 0; i < 10; i++) {
		std::string key = "key-" + std::to_string(i);
		std::string value = "value-" + std::to_string(i);
		CHECK(table.put(key, value, static_cast<std::uint64_t>(i + 1))._is_add);
	}

	// Data written before freeze should still be accessible
	check_result_is_value(table.get("key-0", 100), "value-0", 1);
	check_result_is_value(table.get("key-5", 100), "value-5", 6);
	// A key that was never written
	check_result_is_notfound(table.get("key-99", 100));
}

void test_freeze_then_put_new_data() {
	constexpr std::size_t tiny_threshold = 100;
	MemTable table(tiny_threshold,
				   std::make_shared<InternalKeyComparator>());

	// Phase 1: trigger freeze
	CHECK(table.put("foo", "old", 10)._is_add);
	for (int i = 0; i < 10; i++) {
		table.put("bulk-" + std::to_string(i),
				  "padding-data-that-takes-space", 20)._is_add;
	}

	// Phase 2: after freeze, write fresh data to new active table
	CHECK(table.put("foo", "new", 30)._is_add);

	// get with large seq should see the newest version (active)
	check_result_is_value(table.get("foo", 100), "new", 30);
	// get with older seq should see the frozen version
	check_result_is_value(table.get("foo", 15), "old", 10);
}

void test_freeze_then_remove() {
	constexpr std::size_t tiny_threshold = 100;
	MemTable table(tiny_threshold,
				   std::make_shared<InternalKeyComparator>());

	// Phase 1: insert and trigger freeze
	CHECK(table.put("foo", "old-value", 10)._is_add);
	for (int i = 0; i < 10; i++) {
		table.put("bulk-" + std::to_string(i),
				  "padding-data-that-takes-space", 20)._is_add;
	}

	// Phase 2: remove in new active table
	CHECK(table.remove("foo", 30)._is_success);

	// Get with large seq should see tombstone (from active)
	check_result_is_tombstone(table.get("foo", 100), 30);
	// Get with older seq should still see frozen value
	check_result_is_value(table.get("foo", 15), "old-value", 10);
}

// ─── batch operations ───────────────────────────────────────────

void test_put_batch_and_get_batch() {
	MemTable table;
	std::vector<std::pair<std::string, std::string>> kvs = {
		{"k1", "v1"}, {"k2", "v2"}, {"k3", "v3"}};
	std::vector<std::uint64_t> seqs = {10, 20, 30};

	CHECK(table.put_batch(kvs, seqs));

	std::vector<std::string> keys = {"k1", "k2", "k3"};
	auto results = table.get_batch(keys, {100, 100, 100});

	check_result_is_value(results[0], "v1", 10);
	check_result_is_value(results[1], "v2", 20);
	check_result_is_value(results[2], "v3", 30);
}

void test_get_batch_mixed_found_and_notfound() {
	MemTable table;
	CHECK(table.put("exists1", "a", 10)._is_add);
	CHECK(table.put("exists2", "b", 20)._is_add);

	std::vector<std::string> keys = {"exists1", "missing", "exists2"};
	auto results = table.get_batch(keys, {100, 100, 100});

	check_result_is_value(results[0], "a", 10);
	check_result_is_notfound(results[1]);
	check_result_is_value(results[2], "b", 20);
}

void test_remove_batch() {
	MemTable table;
	CHECK(table.put("k1", "v1", 10)._is_add);
	CHECK(table.put("k2", "v2", 20)._is_add);
	CHECK(table.put("k3", "v3", 30)._is_add);

	CHECK(table.remove_batch({"k1", "k3"}, {40, 50}));

	check_result_is_tombstone(table.get("k1", 100), 40);
	check_result_is_value(table.get("k2", 100), "v2", 20);
	check_result_is_tombstone(table.get("k3", 100), 50);
}

void test_get_batch_across_active_and_frozen() {
	constexpr std::size_t tiny_threshold = 100;
	MemTable table(tiny_threshold,
				   std::make_shared<InternalKeyComparator>());

	// Phase 1: write and trigger freeze
	CHECK(table.put("frozen-key", "frozen-value", 10)._is_add);
	for (int i = 0; i < 10; i++) {
		table.put("bulk-" + std::to_string(i),
				  "padding-data-that-takes-space", 20)._is_add;
	}

	// Phase 2: write new data to active
	CHECK(table.put("active-key", "active-value", 30)._is_add);

	// Batch get both keys
	std::vector<std::string> keys = {"frozen-key", "active-key", "missing"};
	auto results = table.get_batch(keys, {100, 100, 100});

	check_result_is_value(results[0], "frozen-value", 10); // from frozen
	check_result_is_value(results[1], "active-value", 30); // from active
	check_result_is_notfound(results[2]);
}

// ─── prefix query tests ────────────────────────────────────────

void test_prefix_basic() {
	MemTable table;
	CHECK(table.put("apple", "1", 10)._is_add);
	CHECK(table.put("application", "2", 20)._is_add);
	CHECK(table.put("banana", "3", 30)._is_add);
	CHECK(table.put("apricot", "4", 40)._is_add);

	// prefix "app" should match "apple" and "application"
	auto it = table.begin_preffix("app", 100);
	CHECK(it != table.end());
	auto item = *it;
	// "app" < "apple" < "application" < "apricot"
	CHECK(InternalKey::decode(item.first)._key == "apple");
	CHECK(item.second == "1");
	++it;
	CHECK(it != table.end());
	item = *it;
	CHECK(InternalKey::decode(item.first)._key == "application");
	CHECK(item.second == "2");
	++it;
	CHECK(!(it != table.end())); // end
}

void test_prefix_no_match() {
	MemTable table;
	CHECK(table.put("apple", "1", 10)._is_add);
	CHECK(table.put("banana", "2", 20)._is_add);

	// prefix "z" should return empty iterator
	auto it = table.begin_preffix("z", 100);
	CHECK(!(it != table.end()));
}

void test_prefix_with_frozen() {
	constexpr std::size_t tiny = 100;
	MemTable table(tiny, std::make_shared<InternalKeyComparator>());

	// Phase 1: frozen data
	CHECK(table.put("alpha", "frozen-alpha", 10)._is_add);
	CHECK(table.put("beta", "frozen-beta", 20)._is_add);
	// Force freeze by bulk writes
	for (int i = 0; i < 10; i++) {
		table.put("bulk-" + std::to_string(i),
				  "padding-data-that-takes-space", 30)._is_add;
	}

	// Phase 2: active data
	CHECK(table.put("alpha", "active-alpha", 40)._is_add);
	CHECK(table.put("gamma", "active-gamma", 50)._is_add);

	// prefix "a" - active "alpha"(seq=40) should shadow frozen "alpha"(seq=10)
	auto it = table.begin_preffix("a", 100);
	auto item = *it;
	CHECK(InternalKey::decode(item.first)._key == "alpha");
	CHECK(item.second == "active-alpha"); // newer version
}

void test_prefix_tombstone_skip() {
	constexpr std::size_t tiny = 100;
	MemTable table(tiny, std::make_shared<InternalKeyComparator>());

	// Frozen: put then delete
	CHECK(table.put("alpha", "value", 10)._is_add);
	for (int i = 0; i < 10; i++) {
		table.put("bulk-" + std::to_string(i),
				  "padding-data-that-takes-space", 30)._is_add;
	}
	CHECK(table.remove("alpha", 20)._is_success);

	// prefix "a" - TombStone should be skipped
	auto it = table.begin_preffix("a", 100);
	CHECK(!(it != table.end())); // nothing visible
}

void test_prefix_empty_prefix() {
	MemTable table;
	CHECK(table.put("a", "1", 10)._is_add);
	CHECK(table.put("b", "2", 20)._is_add);

	// empty prefix → all keys
	auto it = table.begin_preffix("", 100);
	CHECK(it != table.end());
	auto item = *it;
	CHECK(InternalKey::decode(item.first)._key == "a");
	++it;
	item = *it;
	CHECK(InternalKey::decode(item.first)._key == "b");
	++it;
	CHECK(!(it != table.end()));
}

// ─── monotony predicate tests ─────────────────────────────────

void test_monotony_predicate_basic() {
	MemTable table;
	CHECK(table.put("alpha", "1", 10)._is_add);
	CHECK(table.put("bravo", "2", 20)._is_add);
	CHECK(table.put("charlie", "3", 30)._is_add);
	CHECK(table.put("delta", "4", 40)._is_add);

	// predicate: "bravo" <= key < "delta"
	auto pred = [](const std::string &key) -> int {
		if (key < "bravo") return -1;
		if (key >= "delta") return 1;
		return 0;
	};

	auto it = table.iter_monotony_predicate(
		pred, MAX_COMMIT_SEQUENCE, true, false);
	auto item = *it;
	CHECK(InternalKey::decode(item.first)._key == "bravo");
	CHECK(item.second == "2");
	++it;
	item = *it;
	CHECK(InternalKey::decode(item.first)._key == "charlie");
	CHECK(item.second == "3");
	++it;
	CHECK(!(it != table.end()));
}

void test_monotony_predicate_no_match() {
	MemTable table;
	CHECK(table.put("alpha", "1", 10)._is_add);
	CHECK(table.put("bravo", "2", 20)._is_add);

	// predicate: key >= "z"
	auto pred = [](const std::string &key) -> int {
		if (key < "z") return -1;
		if (key > "z") return 1;
		return 0;
	};

	auto it = table.iter_monotony_predicate(
		pred, MAX_COMMIT_SEQUENCE, true, false);
	CHECK(!(it != table.end()));  // empty
}

void test_monotony_predicate_commit_sequence_filter() {
	MemTable table;
	CHECK(table.put("alpha", "visible", 10)._is_add);
	CHECK(table.put("alpha", "hidden", 100)._is_add);
	CHECK(table.put("bravo", "visible", 20)._is_add);

	// predicate: all keys
	auto pred = [](const std::string &key) -> int { return 0; };

	// seq=50: should see "alpha" seq=10 and "bravo" seq=20
	// but NOT "alpha" seq=100
	auto it = table.iter_monotony_predicate(pred, 50, true, false);
	CHECK(it != table.end());
	auto item = *it;
	CHECK(InternalKey::decode(item.first)._key == "alpha");
	CHECK(item.second == "visible");
	CHECK(InternalKey::decode(item.first)._commit_sequence == 10);
	++it;
	item = *it;
	CHECK(InternalKey::decode(item.first)._key == "bravo");
	CHECK(item.second == "visible");
	++it;
	CHECK(!(it != table.end()));
}

void test_monotony_predicate_all_visible_with_max_seq() {
	MemTable table;
	CHECK(table.put("alpha", "val1", 10)._is_add);
	CHECK(table.put("bravo", "val2", 20)._is_add);

	auto pred = [](const std::string &key) -> int { return 0; };

	// default seq = MAX → all visible
	auto it = table.iter_monotony_predicate(
		pred, MAX_COMMIT_SEQUENCE, true, false);
	int count = 0;
	while (it != table.end()) {
		++it;
		count++;
	}
	CHECK(count == 2);
}

void test_monotony_predicate_with_frozen() {
	constexpr std::size_t tiny = 100;
	MemTable table(tiny, std::make_shared<InternalKeyComparator>());

	// frozen data
	CHECK(table.put("alpha", "frozen", 10)._is_add);
	for (int i = 0; i < 10; i++) {
		table.put("bulk-" + std::to_string(i),
				  "padding-data-that-takes-space", 30)._is_add;
	}

	// active data
	CHECK(table.put("alpha", "active", 50)._is_add);

	auto pred = [](const std::string &key) -> int { return 0; };
	// seq=40: frozen alpha(10) visible, active alpha(50) NOT visible
	auto it = table.iter_monotony_predicate(pred, 40, true, false);
	auto item = *it;
	CHECK(InternalKey::decode(item.first)._key == "alpha");
	CHECK(item.second == "frozen");
	CHECK(InternalKey::decode(item.first)._commit_sequence == 10);
}

// ─── heap iterator advancement ─────────────────────────────────

struct ExpectedHeapEntry {
	std::string key;
	std::string value;
	std::uint64_t seq;
	ValueType type = Value;
};

void check_heap_entries(HeapIterator it,
						const std::vector<ExpectedHeapEntry> &expected) {
	for (const auto &entry : expected) {
		CHECK(it.is_valid());
		const auto actual = *it;
		CHECK(actual.first == InternalKey::encode(
			InternalKey(entry.key, entry.seq, entry.type)));
		CHECK(actual.second == entry.value);
		CHECK(&(++it) == &it);
	}
	CHECK(!it.is_valid());
	CHECK(it == HeapIterator(true));
	CHECK(&(++it) == &it);
	CHECK(&(++it) == &it);
	CHECK(!it.is_valid());
}

// These tests use the container constructor directly. Do not call clean or ++
// before observing the first result: initialization must establish that state.
void test_heap_container_constructor_builds_unsorted_input() {
	for (bool keep_all : {false, true}) {
		std::vector<HeapIterator::HeapItem> items{
			{"z", "last", 3, Value},
			{"a", "old", 5, Value},
			{"b", "", 4, Value},
			{"a", "new", 9, Value},
		};
		HeapIterator it(std::move(items), false, keep_all);
		CHECK(it.is_valid());
		const HeapIterator &view = it;
		CHECK((*view).first == InternalKey::encode(InternalKey("a", 9, Value)));
		CHECK((*view).second == "new");
		const auto first = *view;
		CHECK(*view == first);
		if (keep_all) {
			check_heap_entries(std::move(it),
				{{"a", "new", 9}, {"a", "old", 5}, {"b", "", 4},
				 {"z", "last", 3}});
		} else {
			check_heap_entries(std::move(it),
				{{"a", "new", 9}, {"b", "", 4}, {"z", "last", 3}});
		}
	}
}

void test_heap_container_constructor_filters_future_head() {
	for (bool skip_tombstone : {false, true}) {
		for (bool keep_all : {false, true}) {
			std::vector<HeapIterator::HeapItem> items{
				{"b", "b", 4, Value},
				{"a", "future", 20, Value},
				{"a", "old", 5, Value},
				{"a", "", 30, TombStone},
				{"a", "at-snapshot", 10, Value},
			};
			HeapIterator it(std::move(items), skip_tombstone, keep_all, 10);
			if (keep_all) {
				check_heap_entries(std::move(it),
					{{"a", "at-snapshot", 10}, {"a", "old", 5}, {"b", "b", 4}});
			} else {
				check_heap_entries(std::move(it),
					{{"a", "at-snapshot", 10}, {"b", "b", 4}});
			}
		}
	}
}

void test_heap_container_constructor_skips_leading_tombstones() {
	for (bool keep_all : {false, true}) {
		std::vector<HeapIterator::HeapItem> items{
			{"c", "", 5, Value},
			{"a", "old-a", 2, Value},
			{"b", "", 7, TombStone},
			{"a", "", 6, TombStone},
			{"b", "old-b", 3, Value},
		};
		HeapIterator it(std::move(items), true, keep_all, 10);
		// Tombstones hide their older values even when all versions are requested;
		// an empty Value payload is still a valid result.
		check_heap_entries(std::move(it), {{"c", "", 5}});
	}
}

void test_heap_container_constructor_orders_snapshot_and_tombstone_filters() {
	for (bool skip_tombstone : {false, true}) {
		for (bool keep_all : {false, true}) {
			std::vector<HeapIterator::HeapItem> items{
				{"b", "", 20, TombStone},
				{"a", "old-a", 5, Value},
				{"b", "old-b", 3, Value},
				{"a", "future-a", 30, Value},
				{"b", "visible-b", 8, Value},
				{"a", "", 10, TombStone},
			};
			HeapIterator it(std::move(items), skip_tombstone, keep_all, 15);
			std::vector<ExpectedHeapEntry> expected;
			if (!skip_tombstone) {
				expected.push_back({"a", "", 10, TombStone});
				if (keep_all) { expected.push_back({"a", "old-a", 5}); }
			}
			expected.push_back({"b", "visible-b", 8});
			if (keep_all) { expected.push_back({"b", "old-b", 3}); }
			check_heap_entries(std::move(it), expected);
		}
	}
}

void test_heap_container_constructor_all_future_is_end() {
	for (bool skip_tombstone : {false, true}) {
		for (bool keep_all : {false, true}) {
			std::vector<HeapIterator::HeapItem> items{
				{"b", "", 30, TombStone}, {"a", "future", 20, Value},
			};
			HeapIterator it(std::move(items), skip_tombstone, keep_all, 10);
			check_heap_entries(std::move(it), {});
		}
	}
}

void test_heap_container_constructor_all_deleted_is_end() {
	for (bool keep_all : {false, true}) {
		std::vector<HeapIterator::HeapItem> items{
			{"b", "old-b", 2, Value},
			{"a", "", 8, TombStone},
			{"b", "", 9, TombStone},
			{"a", "old-a", 1, Value},
		};
		HeapIterator it(std::move(items), true, keep_all, 10);
		check_heap_entries(std::move(it), {});
	}
}

void test_heap_container_constructor_empty_input() {
	for (bool skip_tombstone : {false, true}) {
		for (bool keep_all : {false, true}) {
			HeapIterator it(std::vector<HeapIterator::HeapItem>{},
				skip_tombstone, keep_all, 0);
			check_heap_entries(it, {});
			bool threw = false;
			try { (void)*it; }
			catch (const std::runtime_error &) { threw = true; }
			CHECK(threw);
		}
	}
}

void test_heap_container_constructor_default_snapshot_and_binary_data() {
	const std::string binary_key("a\0b", 3);
	const std::string binary_value("v\0\xff", 3);
	for (bool explicit_nullopt : {false, true}) {
		std::vector<HeapIterator::HeapItem> items{
			{binary_key, binary_value, MAX_COMMIT_SEQUENCE, Value},
			{"", "", 0, Value},
		};
		HeapIterator it = explicit_nullopt
			? HeapIterator(std::move(items), true, true, std::nullopt)
			: HeapIterator(std::move(items), true, true);
		check_heap_entries(std::move(it),
			{{"", "", 0}, {binary_key, binary_value, MAX_COMMIT_SEQUENCE}});
	}
}

void test_begin_filters_before_first_observation() {
	MemTable table;
	table.put("", "future-first-key", 20);
	table.put("a", "old", 1);
	table.remove("a", 8);
	table.put("b", "", 5);
	for (bool keep_all : {false, true}) {
		check_heap_entries(table.begin(10, keep_all, true), {{"b", "", 5}});
		check_heap_entries(table.begin(0, keep_all, true), {});
	}
}

void test_heap_empty_increment_and_dereference() {
	for (bool keep_all : {false, true}) {
		for (bool skip_tombstone : {false, true}) {
			HeapIterator it(skip_tombstone, std::uint64_t{10}, keep_all);
			check_heap_entries(it, {});
			bool threw = false;
			try { (void)*it; }
			catch (const std::runtime_error &) { threw = true; }
			CHECK(threw);
		}
	}
}

void test_heap_increment_version_modes() {
	MemTable table;
	table.put("a", "old", 1);
	table.put("a", "new", 2);
	table.put("b", "", 3);
	table.put("b", "future", 30);
	for (bool skip_tombstone : {false, true}) {
		check_heap_entries(table.begin(10, false, skip_tombstone),
			{{"a", "new", 2}, {"b", "", 3}});
		check_heap_entries(table.begin(10, true, skip_tombstone),
			{{"a", "new", 2}, {"a", "old", 1}, {"b", "", 3}});
	}
}

void test_heap_increment_consecutive_visible_tombstones() {
	// The small threshold spreads these versions over active and frozen tables.
	MemTable table(32);
	table.put("a", "a", 1);
	table.put("b", "old-b", 2);
	table.put("c", "old-c", 3);
	table.remove("b", 4);
	table.remove("c", 5);
	table.put("d", "d", 6);
	for (bool keep_all : {false, true}) {
		check_heap_entries(table.begin(10, keep_all, true),
			{{"a", "a", 1}, {"d", "d", 6}});
	}
	check_heap_entries(table.begin(10, false, false),
		{{"a", "a", 1}, {"b", "", 4, TombStone},
		 {"c", "", 5, TombStone}, {"d", "d", 6}});
	check_heap_entries(table.begin(10, true, false),
		{{"a", "a", 1}, {"b", "", 4, TombStone}, {"b", "old-b", 2},
		 {"c", "", 5, TombStone}, {"c", "old-c", 3}, {"d", "d", 6}});
}

void test_heap_increment_filters_future_tombstone_first() {
	MemTable table;
	table.put("a", "a", 1);
	table.put("b", "old", 5);
	table.put("b", "visible", 10);
	table.put("c", "c", 12);
	table.remove("b", 20);
	for (bool skip_tombstone : {false, true}) {
		check_heap_entries(table.begin(15, false, skip_tombstone),
			{{"a", "a", 1}, {"b", "visible", 10}, {"c", "c", 12}});
		check_heap_entries(table.begin(15, true, skip_tombstone),
			{{"a", "a", 1}, {"b", "visible", 10}, {"b", "old", 5},
			 {"c", "c", 12}});
	}
}

void test_heap_increment_skips_future_keys_and_tail() {
	MemTable table;
	table.put("a", "a", 1);
	table.put("b", "future", 20);
	table.remove("c", 30);
	table.put("d", "d", 4);
	table.put("e", "future", 50);
	for (bool keep_all : {false, true}) {
		for (bool skip_tombstone : {false, true}) {
			check_heap_entries(table.begin(10, keep_all, skip_tombstone),
				{{"a", "a", 1}, {"d", "d", 4}});
			check_heap_entries(table.begin(0, keep_all, skip_tombstone), {});
		}
	}
}

void test_heap_increment_tombstone_between_versions() {
	MemTable table;
	table.put("", "old", 10);
	table.remove("", 20);
	table.put("", "new", 30);
	const std::string binary_key("b\0x", 3);
	const std::string binary_value("v\0x", 3);
	table.put(binary_key, binary_value, 5);
	check_heap_entries(table.begin(30, true, true),
		{{"", "new", 30}, {binary_key, binary_value, 5}});
	check_heap_entries(table.begin(30, true, false),
		{{"", "new", 30}, {"", "", 20, TombStone}, {"", "old", 10},
		 {binary_key, binary_value, 5}});
	for (bool skip_tombstone : {false, true}) {
		check_heap_entries(table.begin(30, false, skip_tombstone),
			{{"", "new", 30}, {binary_key, binary_value, 5}});
	}
}

// ─── get_total_size / clear ─────────────────────────────────────

void test_get_total_size_empty() {
	MemTable table;
	CHECK(table.get_total_size() == 0);
}

void test_get_total_size_tracks_puts_and_remove() {
	MemTable table;

	// each entry counts key + value + 8 (seq + value type)
	CHECK(table.put("alpha", "hello", 10)._is_add); // 5 + 5 + 8 = 18
	CHECK(table.get_total_size() == 18);

	CHECK(table.put("beta", "x", 20)._is_add); // 4 + 1 + 8 = 13
	CHECK(table.get_total_size() == 31);

	// remove stores a tombstone entry: key + empty + 8
	CHECK(table.remove("beta", 30)._is_success); // 4 + 0 + 8 = 12
	CHECK(table.get_total_size() == 43);
}

void test_get_total_size_across_freeze_and_flush() {
	MemTable table;

	// 50k entries x (7 key + 70 value + 8) = 4.25MB > 4MB static
	// threshold, so this really freezes the active table
	const int A_COUNT = 50000;
	const std::size_t ENTRY_BYTES = 7 + 70 + 8;
	std::string big_value(70, 'v');
	std::vector<std::pair<std::string, std::string>> kvs;
	std::vector<std::uint64_t> seqs;
	for (int i = 0; i < A_COUNT; i++) {
		char buf[16];
		std::snprintf(buf, sizeof(buf), "a-%05d", i);
		kvs.push_back({std::string(buf), big_value});
		seqs.push_back(static_cast<std::uint64_t>(i + 1));
	}
	CHECK(table.put_batch(kvs, seqs));

	// active + frozen both counted
	CHECK(table.get_total_size() ==
		  static_cast<std::size_t>(A_COUNT) * ENTRY_BYTES);

	// flushing the frozen table must drop its bytes from the total
	SSTBuilder builder;
	std::string sst_path = "/tmp/atlasdb_memtable_test_freeze.sst";
	std::vector<std::uint64_t> flushed_seqs;
	std::shared_ptr<SST> sst =
		table.flush_last(builder, sst_path, 1, flushed_seqs)._sst;
	CHECK(sst != nullptr);
	// frozen part was > 4MB; only the post-freeze tail remains
	CHECK(table.get_total_size() <
		  static_cast<std::size_t>(A_COUNT) * ENTRY_BYTES);
	CHECK(table.get_total_size() < 4ULL * 1024 * 1024);

	std::remove(sst_path.c_str());
}

void test_clear_resets_active_data() {
	MemTable table;
	CHECK(table.put("alpha", "hello", 10)._is_add);
	CHECK(table.put("bravo", "world", 20)._is_add);
	CHECK(table.get_total_size() > 0);

	table.clear();

	CHECK(table.get_total_size() == 0);
	check_result_is_notfound(table.get("alpha", 100));
	check_result_is_notfound(table.get("bravo", 100));

	// the table stays usable after clear
	CHECK(table.put("charlie", "again", 30)._is_add);
	check_result_is_value(table.get("charlie", 100), "again", 30);
	CHECK(table.get_total_size() == 7 + 5 + 8); // "charlie" + "again" + 8
}

void test_clear_with_frozen_tables() {
	MemTable table;

	// real freeze, same payload as above
	const int A_COUNT = 50000;
	std::string big_value(70, 'v');
	std::vector<std::pair<std::string, std::string>> kvs;
	std::vector<std::uint64_t> seqs;
	for (int i = 0; i < A_COUNT; i++) {
		char buf[16];
		std::snprintf(buf, sizeof(buf), "a-%05d", i);
		kvs.push_back({std::string(buf), big_value});
		seqs.push_back(static_cast<std::uint64_t>(i + 1));
	}
	CHECK(table.put_batch(kvs, seqs));
	CHECK(table.get_total_size() > 4ULL * 1024 * 1024);

	table.clear();

	CHECK(table.get_total_size() == 0);
	// frozen queue is empty too: nothing left to flush
	SSTBuilder builder;
	std::string sst_path = "/tmp/atlasdb_memtable_test_clear.sst";
	std::vector<std::uint64_t> flushed_seqs;
	std::shared_ptr<SST> sst =
		table.flush_last(builder, sst_path, 1, flushed_seqs)._sst;
	CHECK(sst == nullptr);

	// still usable after clearing a frozen table
	CHECK(table.put("alpha", "hello", 10)._is_add);
	check_result_is_value(table.get("alpha", 100), "hello", 10);
}

// ─── test runner ────────────────────────────────────────────────

struct TestCase {
	const char *name;
	void (*run)();
};

} // namespace

int main() {
	const TestCase tests[] = {
		{"empty memtable get", test_empty_memtable_get},
		{"put and get", test_put_and_get},
		{"put multiple keys", test_put_multiple_keys},
		{"put same key different sequence",
		 test_put_same_key_different_sequence},
		{"remove and get", test_remove_and_get},
		{"tombstone not confused with empty value",
		 test_tombstone_not_confused_with_empty_value},
		{"empty key", test_empty_key},
		{"binary key and value", test_binary_key_and_value},
		{"get nonexistent key", test_get_nonexistent_key},
		{"freeze triggers and data persists",
		 test_freeze_triggers_and_data_persists},
		{"freeze then put new data", test_freeze_then_put_new_data},
		{"freeze then remove", test_freeze_then_remove},
		{"put batch and get batch", test_put_batch_and_get_batch},
		{"get batch mixed found and notfound",
		 test_get_batch_mixed_found_and_notfound},
		{"remove batch", test_remove_batch},
		{"get batch across active and frozen",
		 test_get_batch_across_active_and_frozen},
		{"prefix basic", test_prefix_basic},
		{"prefix no match", test_prefix_no_match},
		{"prefix with frozen", test_prefix_with_frozen},
		{"prefix tombstone skip", test_prefix_tombstone_skip},
		{"prefix empty prefix", test_prefix_empty_prefix},
		{"monotony predicate basic",
		 test_monotony_predicate_basic},
		{"monotony predicate no match",
		 test_monotony_predicate_no_match},
		{"monotony predicate seq filter",
		 test_monotony_predicate_commit_sequence_filter},
		{"monotony predicate all visible",
		 test_monotony_predicate_all_visible_with_max_seq},
		{"monotony predicate with frozen",
		 test_monotony_predicate_with_frozen},
		{"heap empty increment and dereference",
		 test_heap_empty_increment_and_dereference},
		{"heap container constructor builds unsorted input",
		 test_heap_container_constructor_builds_unsorted_input},
		{"heap container constructor filters future head",
		 test_heap_container_constructor_filters_future_head},
		{"heap container constructor skips leading tombstones",
		 test_heap_container_constructor_skips_leading_tombstones},
		{"heap container constructor orders snapshot and tombstone filters",
		 test_heap_container_constructor_orders_snapshot_and_tombstone_filters},
		{"heap container constructor all future is end",
		 test_heap_container_constructor_all_future_is_end},
		{"heap container constructor all deleted is end",
		 test_heap_container_constructor_all_deleted_is_end},
		{"heap container constructor empty input",
		 test_heap_container_constructor_empty_input},
		{"heap container constructor default snapshot and binary data",
		 test_heap_container_constructor_default_snapshot_and_binary_data},
		{"begin filters before first observation",
		 test_begin_filters_before_first_observation},
		{"heap increment version modes", test_heap_increment_version_modes},
		{"heap increment consecutive visible tombstones",
		 test_heap_increment_consecutive_visible_tombstones},
		{"heap increment filters future tombstone first",
		 test_heap_increment_filters_future_tombstone_first},
		{"heap increment skips future keys and tail",
		 test_heap_increment_skips_future_keys_and_tail},
		{"heap increment tombstone between versions",
		 test_heap_increment_tombstone_between_versions},
		{"get total size empty", test_get_total_size_empty},
		{"get total size tracks puts and remove",
		 test_get_total_size_tracks_puts_and_remove},
		{"get total size across freeze and flush",
		 test_get_total_size_across_freeze_and_flush},
		{"clear resets active data", test_clear_resets_active_data},
		{"clear with frozen tables", test_clear_with_frozen_tables},
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
