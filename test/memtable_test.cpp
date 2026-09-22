#include "MemTable.h"

#include <cstdint>
#include <cstdio>
#include <exception>
#include <iostream>
#include <memory>
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
