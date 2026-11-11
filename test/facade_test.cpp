#include "lsm.h"
#include "TranContext.h"
#include "Record.h"

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
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
	if (condition) { return; }

	std::ostringstream message;
	message << "line " << line << ": check failed: " << expression;
	throw TestFailure(message.str());
}

#define CHECK(expression) check((expression), #expression, __LINE__)

// ─── helpers ────────────────────────────────────────────────────

struct Entry {
	std::string key;
	std::string value;
};

std::size_t count_sst_files(const std::string &dir) {
	std::size_t count = 0;
	for (const auto &entry : std::filesystem::directory_iterator(dir)) {
		if (entry.is_regular_file() &&
			entry.path().filename().string().rfind("sst_", 0) == 0) {
			count++;
		}
	}
	return count;
}

std::vector<Entry> collect_range(LSM &db, const std::string &lo,
								 const std::string &hi) {
	std::vector<Entry> entries;
	HeapIterator it = db.range(lo, hi);
	while (it.is_valid()) {
		InternalKey ik = InternalKey::decode((*it).first);
		entries.push_back({ik._key, (*it).second});
		++it;
	}
	return entries;
}

std::vector<Entry> collect_scan(LSM &db) {
	std::vector<Entry> entries;
	LSMEngineIterator it = db.begin(), end = db.end();
	while (it != end) {
		InternalKey ik = InternalKey::decode((*it).first);
		entries.push_back({ik._key, (*it).second});
		++it;
	}
	return entries;
}

// ─── 1. 基本读写删 ──────────────────────────────────────────────

void test_basic_put_get_remove() {
	std::string dir = "/tmp/facade_test_basic";
	std::filesystem::remove_all(dir);
	{
		LSM db(dir);
		auto s1 = db.put("alpha", "1");
		auto s2 = db.put("bravo", "2");
		CHECK(s1.has_value() && s2.has_value() && *s2 > *s1); // 取号单调

		auto res = db.get("alpha");
		CHECK(res.is_found() && res._value == "1");
		auto miss = db.get("charlie");
		CHECK(!miss.is_found());

		db.remove("alpha");
		CHECK(!db.get("alpha").is_found());
		CHECK(db.get("bravo").is_found());
	}
	std::filesystem::remove_all(dir);
}

// ─── 2. 读己之写:put 返回的 seq 即最新视图快照点 ───────────────

void test_read_your_writes() {
	std::string dir = "/tmp/facade_test_ryw";
	std::filesystem::remove_all(dir);
	{
		LSM db(dir);
		db.put("k", "old");
		auto seq = db.put("k", "new");
		CHECK(seq.has_value());
		// 最新视图直接可见自己的写(facade get 无 seq 版 = 当前水位)
		CHECK(db.get("k").is_found() && db.get("k")._value == "new");
	}
	std::filesystem::remove_all(dir);
}

// ─── 3. range 半开区间 ──────────────────────────────────────────

void test_range_half_open() {
	std::string dir = "/tmp/facade_test_range";
	std::filesystem::remove_all(dir);
	{
		LSM db(dir);
		for (char c = 'a'; c <= 'f'; c++) {
			db.put(std::string(1, c), std::string("v") + c);
		}
		auto entries = collect_range(db, "b", "e");
		CHECK(entries.size() == 3); // b,c,d(e 不含)
		CHECK(entries[0].key == "b" && entries[1].key == "c" &&
			  entries[2].key == "d");
		// 空区间
		CHECK(collect_range(db, "x", "z").empty());
		CHECK(collect_range(db, "e", "b").empty());
	}
	std::filesystem::remove_all(dir);
}

// ─── 4. range 与全扫描对账(混合数据 + 墓碑) ─────────────────────

void test_range_reconcile_with_scan() {
	std::string dir = "/tmp/facade_test_reconcile";
	std::filesystem::remove_all(dir);
	{
		LSM db(dir);
		for (int i = 0; i < 10; i++) {
			db.put("key" + std::to_string(i), "v1-" + std::to_string(i));
		}
		db.flush();
		for (int i = 5; i < 15; i++) {
			db.put("key" + std::to_string(i), "v2-" + std::to_string(i));
		}
		db.remove("key7");
		db.put("key3", "v3-3");

		std::string lo = "key2", hi = "key9";
		std::vector<Entry> expected;
		for (const Entry &e : collect_scan(db)) {
			if (lo <= e.key && e.key < hi) { expected.push_back(e); }
		}
		auto actual = collect_range(db, lo, hi);
		CHECK(actual.size() == expected.size());
		for (std::size_t i = 0; i < actual.size() && i < expected.size();
			 i++) {
			CHECK(actual[i].key == expected[i].key);
			CHECK(actual[i].value == expected[i].value);
		}
		// 语义抽查
		bool saw_key7 = false, saw_key3 = false;
		for (const Entry &e : actual) {
			if (e.key == "key7") { saw_key7 = true; }
			if (e.key == "key3") { saw_key3 = (e.value == "v3-3"); }
		}
		CHECK(!saw_key7);
		CHECK(saw_key3);
	}
	std::filesystem::remove_all(dir);
}

// ─── 5. begin/end 全库扫描:有序 + 去重 + 墓碑抑制 ───────────────

void test_scan() {
	std::string dir = "/tmp/facade_test_scan";
	std::filesystem::remove_all(dir);
	{
		LSM db(dir);
		db.put("b", "1");
		db.put("a", "2");
		db.flush();
		db.put("a", "3"); // 覆盖 SST 里的旧值
		db.put("c", "4");
		db.remove("b");

		auto entries = collect_scan(db);
		CHECK(entries.size() == 2);
		CHECK(entries[0].key == "a" && entries[0].value == "3");
		CHECK(entries[1].key == "c" && entries[1].value == "4");
	}
	std::filesystem::remove_all(dir);
}

// ─── 6. close:写被优雅拒绝,读继续可用 ──────────────────────────

void test_close_semantics() {
	std::string dir = "/tmp/facade_test_close";
	std::filesystem::remove_all(dir);
	{
		LSM db(dir);
		db.put("k", "v");
		db.close();
		// close 后:写优雅拒绝(nullopt,不是崩溃)
		CHECK(!db.put("k2", "v2").has_value());
		auto put_results = db.put_batch({{"a", "1"}, {"b", "2"}});
		CHECK(put_results.size() == 2);
		CHECK(!put_results[0] && !put_results[1]);
		auto remove_results = db.remove_batch({"k", "missing"});
		CHECK(remove_results.size() == 2);
		CHECK(!remove_results[0] && !remove_results[1]);
		CHECK(!db.remove("k").has_value());
		// 读照常(数据已落盘)
		auto res = db.get("k");
		CHECK(res.is_found() && res._value == "v");
	}
	std::filesystem::remove_all(dir);
}

// ─── 7. flush 落盘 + 析构后 reopen 数据存活 ─────────────────────

void test_flush_and_reopen() {
	std::string dir = "/tmp/facade_test_reopen";
	std::filesystem::remove_all(dir);
	{
		LSM db(dir);
		for (int i = 0; i < 50; i++) {
			db.put("k" + std::to_string(i), "v" + std::to_string(i));
		}
		CHECK(count_sst_files(dir) == 0); // 都在 memtable
		db.flush();
		CHECK(count_sst_files(dir) == 1); // 显式 flush 落盘
		db.put("tail", "in-memtable");    // flush 后又写(靠析构 close 落盘)
	}
	{
		LSM db(dir); // 析构后 reopen:memtable 尾巴 + SST 数据全部存活
		for (int i = 0; i < 50; i++) {
			auto res = db.get("k" + std::to_string(i));
			CHECK(res.is_found() && res._value == "v" + std::to_string(i));
		}
		CHECK(db.get("tail").is_found() && db.get("tail")._value == "in-memtable");
	}
	std::filesystem::remove_all(dir);
}

// ─── 8. 跨 LSM 实例的 seq 水位续接 ──────────────────────────────

void test_watermark_across_instances() {
	std::string dir = "/tmp/facade_test_watermark";
	std::filesystem::remove_all(dir);
	std::uint64_t max_seq = 0;
	{
		LSM db(dir);
		for (int i = 0; i < 10; i++) {
			auto ret = db.put("k" + std::to_string(i), "v");
			CHECK(ret.has_value());
			max_seq = std::max(max_seq, *ret);
		}
	}
	{
		LSM db(dir);
		auto ret = db.put("fresh", "v");
		CHECK(ret.has_value());
		CHECK(*ret > max_seq); // 新实例取号严格大于上实例最大
	}
	std::filesystem::remove_all(dir);
}

void test_batch_facade_normalizes_tombstones_and_preserves_duplicates() {
	const std::string dir = "/tmp/facade_test_batch_visibility";
	std::filesystem::remove_all(dir);
	const std::string binary("a\0b", 3);
	const std::vector<std::string> keys{"live", "empty", "gone", "missing", "live", binary};
	auto verify = [&](LSM &db) {
		const auto results = db.get_batch(keys);
		CHECK(results.size() == keys.size());
		for (std::size_t index = 0; index < keys.size(); ++index) {
			const auto single = db.get(keys[index]);
			CHECK(results[index]._value_type == single._value_type);
			CHECK(results[index]._value == single._value);
			CHECK(results[index]._value_type != TombStone);
		}
		CHECK(results[0]._value == "value" && results[4]._value == "value");
		CHECK(results[1]._value_type == Value && results[1]._value.empty());
		CHECK(results[2]._value_type == NotFound && results[3]._value_type == NotFound);
		CHECK(results[5]._value == std::string("v\0x", 3));
		CHECK(db.get_batch({}).empty());
	};
	{
		LSM db(dir);
		CHECK(db.put("live", "value").has_value());
		CHECK(db.put("empty", "").has_value());
		CHECK(db.put("gone", "old").has_value());
		CHECK(db.put(binary, std::string("v\0x", 3)).has_value());
		db.flush();
		CHECK(db.remove("gone").has_value());
		verify(db);
		db.close();
	}
	{
		LSM reopened(dir);
		verify(reopened);
	}
	std::filesystem::remove_all(dir);
}

void test_transaction_facade_and_public_limits() {
	const std::string dir = "/tmp/facade_test_transaction_entry";
	std::filesystem::remove_all(dir);
	{
		LSM db(dir);
		CHECK(db.get_max_key_size() == 65527);
		CHECK(db.get_max_value_size() == 65535);
		auto initial = db.begin_transaction(IsolationLevel::ReadCommited);
		CHECK(initial->put("k", "old"));
		CHECK(initial->commit().status == CommitResult::Status::Committed);
		auto snapshot = db.begin_transaction(IsolationLevel::ReadRepeated);
		CHECK(db.put("k", "new").has_value());
		CHECK(snapshot->get("k") == "old");
		CHECK(snapshot->put("k", "conflict"));
		CHECK(snapshot->commit().status == CommitResult::Status::Conflict);
		CHECK(db.get("k")._value == "new");
	}
	std::filesystem::remove_all(dir);
}

// ─── 测试注册 ───────────────────────────────────────────────────

struct TestCase {
	const char *name;
	void (*run)();
};

} // namespace

int main() {
	const TestCase tests[] = {
		{"basic put get remove", test_basic_put_get_remove},
		{"read your writes", test_read_your_writes},
		{"range half open", test_range_half_open},
		{"range reconcile with scan", test_range_reconcile_with_scan},
		{"full scan", test_scan},
		{"close semantics", test_close_semantics},
		{"flush and reopen", test_flush_and_reopen},
		{"watermark across instances", test_watermark_across_instances},
		{"batch facade handles tombstones, duplicates and binary data", test_batch_facade_normalizes_tombstones_and_preserves_duplicates},
		{"transaction facade and public size limits", test_transaction_facade_and_public_limits},
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
	std::cout << passed << "/" << sizeof(tests) / sizeof(tests[0])
			  << " tests passed\n";
	return passed == sizeof(tests) / sizeof(tests[0]) ? 0 : 1;
}
