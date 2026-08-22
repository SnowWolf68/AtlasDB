#include "lsm.h"
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
	std::uint64_t seq;
	ValueType type;
};

// [lo, hi) 半开区间的三值单调谓词
std::function<int(const std::string &)> range_pred(const std::string &lo,
												   const std::string &hi) {
	return [lo, hi](const std::string &key) -> int {
		if (key < lo) { return -1; }
		if (key >= hi) { return 1; }
		return 0;
	};
}

// 谓词结果收集: 约定的 is_valid 驱动循环(先查、再用、再推进)
std::vector<Entry> collect_pred(HeapIterator it) {
	std::vector<Entry> entries;
	while (it.is_valid()) {
		InternalKey ik = InternalKey::decode((*it).first);
		entries.push_back(
			{ik._key, (*it).second, ik._commit_sequence, ik._value_type});
		++it;
	}
	return entries;
}

// begin 到 end 全量收集(对账用)
std::vector<Entry> collect_scan(LSMEngineIterator it,
								const LSMEngineIterator &end) {
	std::vector<Entry> entries;
	while (it != end) {
		InternalKey ik = InternalKey::decode((*it).first);
		entries.push_back(
			{ik._key, (*it).second, ik._commit_sequence, ik._value_type});
		++it;
	}
	return entries;
}

// threshold=100: flush 永不自动 compact, L0 状态可控
std::shared_ptr<LSMEngine> make_engine(const std::string &dir) {
	std::filesystem::remove_all(dir);
	return std::make_shared<LSMEngine>(dir, 64 * 1024 * 1024,
									   4 * 1024 * 1024, 100, 4 * 1024 * 1024);
}

std::size_t count_level_files(const std::string &dir, const std::string &ext) {
	std::size_t count = 0;
	for (const auto &entry : std::filesystem::directory_iterator(dir)) {
		if (entry.path().extension() == ext) { count++; }
	}
	return count;
}

// ─── 1. 纯 memtable 命中, 半开边界 ─────────────────────────────

void test_memtable_only() {
	auto engine = make_engine("/tmp/predicate_test_memtable_only");
	engine->put("alpha", "1", 10);
	engine->put("bravo", "2", 20);
	engine->put("charlie", "3", 30);
	engine->put("delta", "4", 40);

	auto entries = collect_pred(
		engine->iter_monotony_predicate(range_pred("bravo", "delta"),
										std::nullopt));
	CHECK(entries.size() == 2); // bravo 含, delta 不含(半开)
	CHECK(entries[0].key == "bravo" && entries[0].value == "2");
	CHECK(entries[1].key == "charlie" && entries[1].value == "3");
}

// ─── 2. memtable + SST 跨源归并 ─────────────────────────────────

void test_memtable_and_sst() {
	auto engine = make_engine("/tmp/predicate_test_mem_sst");
	engine->put("alpha", "a-sst", 1);
	engine->put("bravo", "b-sst", 2);
	engine->put("charlie", "c-sst", 3);
	engine->flush(); // → L0
	engine->put("delta", "d-mem", 4);
	engine->put("echo", "e-mem", 5);

	auto entries = collect_pred(
		engine->iter_monotony_predicate(range_pred("bravo", "echo"),
										std::nullopt));
	CHECK(entries.size() == 3);
	CHECK(entries[0].key == "bravo" && entries[0].value == "b-sst");
	CHECK(entries[1].key == "charlie" && entries[1].value == "c-sst");
	CHECK(entries[2].key == "delta" && entries[2].value == "d-mem");
}

// ─── 3. 无命中 → 空堆 ───────────────────────────────────────────

void test_no_match() {
	auto engine = make_engine("/tmp/predicate_test_no_match");
	engine->put("alpha", "1", 10);
	engine->put("bravo", "2", 20);

	auto it = engine->iter_monotony_predicate(range_pred("x", "z"),
											  std::nullopt);
	CHECK(!it.is_valid());
}

// ─── 4. 空引擎 ──────────────────────────────────────────────────

void test_empty_engine() {
	auto engine = make_engine("/tmp/predicate_test_empty");
	auto it = engine->iter_monotony_predicate(range_pred("a", "z"),
											  std::nullopt);
	CHECK(!it.is_valid());
}

// ─── 5. 墓碑压制(memtable 墓碑 vs SST 旧值) ─────────────────────

void test_tombstone_suppressed() {
	auto engine = make_engine("/tmp/predicate_test_tombstone");
	engine->put("k1", "v1", 1);
	engine->put("k2", "v2", 2);
	engine->put("k3", "v3", 3);
	engine->flush(); // → L0
	engine->remove("k2", 10); // memtable 墓碑必须压住 SST 的 k2 旧值

	auto entries = collect_pred(
		engine->iter_monotony_predicate(range_pred("k1", "k4"),
										std::nullopt));
	CHECK(entries.size() == 2);
	CHECK(entries[0].key == "k1" && entries[0].value == "v1");
	CHECK(entries[1].key == "k3" && entries[1].value == "v3");
}

// ─── 6. 范围首 key 恰是墓碑(收口顺序回归) ────────────────────────

void test_tombstone_at_range_start() {
	auto engine = make_engine("/tmp/predicate_test_tomb_start");
	engine->put("a", "va", 1);
	engine->put("b", "vb", 2);
	engine->put("c", "vc", 3);
	engine->flush();
	engine->remove("a", 10); // 范围内最小 key 被删除

	auto entries = collect_pred(
		engine->iter_monotony_predicate(range_pred("a", "c"), std::nullopt));
	// 首个 *it 不允许漏出 a 的墓碑条目
	CHECK(entries.size() == 1);
	CHECK(entries[0].key == "b" && entries[0].value == "vb");
	CHECK(entries[0].type == Value);
}

// ─── 7. 快照过滤(seq / nullopt 双口径) ──────────────────────────

void test_snapshot_filter() {
	auto engine = make_engine("/tmp/predicate_test_snapshot");
	engine->put("k", "v0", 5);
	engine->flush(); // SST: k@5
	engine->put("k", "v1", 50); // memtable: k@50

	// seq=20: 只见 SST 的 v0
	auto e20 = collect_pred(
		engine->iter_monotony_predicate(range_pred("k", "l"),
										std::optional<std::uint64_t>(20)));
	CHECK(e20.size() == 1);
	CHECK(e20[0].value == "v0" && e20[0].seq == 5);

	// nullopt: 见最新 v1
	auto elast = collect_pred(
		engine->iter_monotony_predicate(range_pred("k", "l"), std::nullopt));
	CHECK(elast.size() == 1);
	CHECK(elast[0].value == "v1" && elast[0].seq == 50);
}

// ─── 8. L0 重叠多表, 跨表最新版本胜 ─────────────────────────────

void test_l0_overlap_newest_wins() {
	auto engine = make_engine("/tmp/predicate_test_l0_overlap");
	engine->put("a", "1", 1);
	engine->flush(); // L0 表1: a@1
	engine->put("a", "2", 2);
	engine->put("b", "1", 3);
	engine->flush(); // L0 表2: a@2, b@1
	engine->put("b", "2", 4); // memtable: b@2

	auto entries = collect_pred(
		engine->iter_monotony_predicate(range_pred("a", "c"), std::nullopt));
	CHECK(entries.size() == 2);
	CHECK(entries[0].key == "a" && entries[0].value == "2"); // 来自表2
	CHECK(entries[1].key == "b" && entries[1].value == "2"); // 来自 memtable
}

// ─── 9. L1 层内二分(id 与位置错开 + 多表) ────────────────────────

void test_l1_binary_search() {
	// threshold=3: 第 3 次 flush 触发 compact; L1 得 2 表后 2 < 3 级联停止
	// max_l0_sst_size=2KB → L1 目标 = 2KB × 3 = 6KB; 总量 ~11KB → 切 2 表
	std::string dir = "/tmp/predicate_test_l1";
	std::filesystem::remove_all(dir);
	auto engine = std::make_shared<LSMEngine>(dir, 64 * 1024 * 1024,
											  4 * 1024 * 1024, 3, 2 * 1024);
	for (int round = 0; round < 3; round++) {
		for (int i = 0; i < 20; i++) {
			int n = round * 20 + i;
			std::string key = "k" + std::to_string(100 + n); // k100..k159
			engine->put(key, std::string(180, 'v'), n + 1);
		}
		engine->flush();
	}
	// compact 后 L1 应有 2 张表(id 从 3 起, 与位置 0/1 错开)
	CHECK(count_level_files(dir, ".1") == 2);
	CHECK(count_level_files(dir, ".0") == 0);

	// 只命中第二张表的区间: 二分必须落在 index 1
	auto second = collect_pred(
		engine->iter_monotony_predicate(range_pred("k145", "k150"),
										std::nullopt));
	CHECK(second.size() == 5);
	for (std::size_t i = 0; i < second.size(); i++) {
		CHECK(second[i].key == "k" + std::to_string(145 + i));
	}

	// 只命中第一张表的区间
	auto first = collect_pred(
		engine->iter_monotony_predicate(range_pred("k100", "k103"),
										std::nullopt));
	CHECK(first.size() == 3);
	for (std::size_t i = 0; i < first.size(); i++) {
		CHECK(first[i].key == "k" + std::to_string(100 + i));
	}

	// 全范围: 60 key 一条不少(跨表无丢失)
	auto all = collect_pred(
		engine->iter_monotony_predicate(range_pred("k100", "k160"),
										std::nullopt));
	CHECK(all.size() == 60);
}

// ─── 10. 与 begin()/end() 全扫描对账 ────────────────────────────

void test_reconcile_with_scan() {
	auto engine = make_engine("/tmp/predicate_test_reconcile");
	// 混合状态: 两轮 flush + 多版本 + 墓碑 + memtable 尾巴
	for (int i = 0; i < 8; i++) {
		engine->put("key" + std::to_string(i), "v1-" + std::to_string(i),
					i + 1);
	}
	engine->flush();
	for (int i = 4; i < 12; i++) {
		engine->put("key" + std::to_string(i), "v2-" + std::to_string(i),
					i + 21);
	}
	engine->flush();
	engine->remove("key6", 100);
	engine->put("key12", "v3-12", 101);
	engine->put("key3", "v3-3", 102);

	auto pred = range_pred("key2", "key9");
	std::vector<Entry> expected;
	for (const Entry &e :
		 collect_scan(engine->begin(std::nullopt, false), engine->end())) {
		if (pred(e.key) == 0) { expected.push_back(e); }
	}
	auto actual =
		collect_pred(engine->iter_monotony_predicate(pred, std::nullopt));

	CHECK(actual.size() == expected.size());
	for (std::size_t i = 0; i < actual.size() && i < expected.size(); i++) {
		CHECK(actual[i].key == expected[i].key);
		CHECK(actual[i].value == expected[i].value);
	}
	// 抽查语义: key6 被删, key3 取 v3-3
	bool saw_key6 = false, saw_key3 = false;
	for (const Entry &e : actual) {
		if (e.key == "key6") { saw_key6 = true; }
		if (e.key == "key3") { saw_key3 = (e.value == "v3-3"); }
	}
	CHECK(!saw_key6);
	CHECK(saw_key3);
}

// ─── 11. 谓词落在 key 间隙 ──────────────────────────────────────

void test_gap_predicate() {
	auto engine = make_engine("/tmp/predicate_test_gap");
	engine->put("a", "1", 1);
	engine->put("c", "3", 3);
	engine->put("e", "5", 5);

	// [b, d): 命中的只有 c
	auto entries = collect_pred(
		engine->iter_monotony_predicate(range_pred("b", "d"), std::nullopt));
	CHECK(entries.size() == 1);
	CHECK(entries[0].key == "c" && entries[0].value == "3");
}

// ─── 测试注册 ───────────────────────────────────────────────────

struct TestCase {
	const char *name;
	void (*run)();
};

} // namespace

int main() {
	const TestCase tests[] = {
		{"memtable only", test_memtable_only},
		{"memtable and sst", test_memtable_and_sst},
		{"no match", test_no_match},
		{"empty engine", test_empty_engine},
		{"tombstone suppressed", test_tombstone_suppressed},
		{"tombstone at range start", test_tombstone_at_range_start},
		{"snapshot filter", test_snapshot_filter},
		{"l0 overlap newest wins", test_l0_overlap_newest_wins},
		{"l1 binary search", test_l1_binary_search},
		{"reconcile with scan", test_reconcile_with_scan},
		{"gap predicate", test_gap_predicate},
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
