#include "lsm.h"
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
	if (condition) { return; }

	std::ostringstream message;
	message << "line " << line << ": check failed: " << expression;
	throw TestFailure(message.str());
}

#define CHECK(expression) check((expression), #expression, __LINE__)

// ─── helpers ────────────────────────────────────────────────────

// 用一个独立的 MemTable 造一个"全量"源流（无快照限制）。
// 源流按 begin 的语义已做：墓碑过滤 + 源内去重（keep_all 可选）。
std::shared_ptr<BaseIterator> make_mem_iter(bool keep_all) {
	MemTable table;
	std::shared_ptr<BaseIterator> it =
		std::make_shared<HeapIterator>(std::move(table.begin(std::nullopt, keep_all)));
	return it;
}

// 造一个含数据的 memtable 源流
std::shared_ptr<BaseIterator> make_mem_iter(
	std::vector<std::tuple<std::string, std::string, std::uint64_t>> kvs,
	bool keep_all) {
	MemTable table;
	for (auto &[key, value, seq] : kvs) {
		CHECK(table.put(key, value, seq));
	}
	return std::make_shared<HeapIterator>(
		std::move(table.begin(std::nullopt, keep_all)));
}

struct Entry {
	std::string key;
	std::string value;
	std::uint64_t seq;
	ValueType type;
};

// 遍历一个 TwoMergeIterator（用 is_valid() 判终止），收集所有条目
std::vector<Entry> collect(TwoMergeIterator &it) {
	std::vector<Entry> entries;
	while (it.is_valid()) {
		auto kv = *it;
		InternalKey ik = InternalKey::decode(kv.first);
		entries.push_back(
			Entry{ik._key, kv.second, ik._commit_sequence, ik._value_type});
		++it;
	}
	return entries;
}

// 防止死循环的兜底遍历（end 判等回归测试用）
std::vector<Entry> collect_with_end(TwoMergeIterator &it,
									const TwoMergeIterator &end) {
	std::vector<Entry> entries;
	int guard = 0;
	while (it != end) {
		CHECK(guard++ < 10000); // 死循环保护
		auto kv = *it;
		InternalKey ik = InternalKey::decode(kv.first);
		entries.push_back(
			Entry{ik._key, kv.second, ik._commit_sequence, ik._value_type});
		++it;
	}
	return entries;
}

// ─── 测试用例 ───────────────────────────────────────────────────

// 1. 基础归并：两源不同 key，整体按 user_key 升序输出
void test_basic_merge() {
	auto a = make_mem_iter({{"a", "1", 1}, {"c", "3", 3}}, false);
	auto b = make_mem_iter({{"b", "2", 2}, {"d", "4", 4}}, false);
	TwoMergeIterator it(a, b, false);

	auto entries = collect(it);
	CHECK(entries.size() == 4);
	CHECK(entries[0].key == "a" && entries[0].value == "1");
	CHECK(entries[1].key == "b" && entries[1].value == "2");
	CHECK(entries[2].key == "c" && entries[2].value == "3");
	CHECK(entries[3].key == "d" && entries[3].value == "4");
	// 整体 enc_key 有序
	for (std::size_t i = 1; i < entries.size(); i++) {
		InternalKeyComparator cmp;
		std::string prev_enc = InternalKey::encode(
			InternalKey(entries[i - 1].key, entries[i - 1].seq,
						entries[i - 1].type));
		std::string cur_enc = InternalKey::encode(
			InternalKey(entries[i].key, entries[i].seq, entries[i].type));
		CHECK(cmp(prev_enc, cur_enc) < 0);
	}
}

// 2. 查询模式去重：同 user_key 跨源两版本，只输出新版本
void test_query_mode_dedup_across_sources() {
	auto a = make_mem_iter({{"k", "v2", 10}}, false); // 新
	auto b = make_mem_iter({{"k", "v1", 5}}, false);  // 旧（SST 侧）
	TwoMergeIterator it(a, b, false);

	auto entries = collect(it);
	CHECK(entries.size() == 1); // k 只输出一次
	CHECK(entries[0].key == "k");
	CHECK(entries[0].value == "v2"); // 新版本胜出
	CHECK(entries[0].seq == 10);
}

// 3. keep_all 模式：同 user_key 跨源两版本，全部输出且 seq 降序
void test_keep_all_versions_across_sources() {
	auto a = make_mem_iter({{"k", "v2", 10}}, true);
	auto b = make_mem_iter({{"k", "v1", 5}}, true);
	TwoMergeIterator it(a, b, true);

	auto entries = collect(it);
	CHECK(entries.size() == 2);
	CHECK(entries[0].key == "k" && entries[0].value == "v2" &&
		  entries[0].seq == 10); // 新先出
	CHECK(entries[1].key == "k" && entries[1].value == "v1" &&
		  entries[1].seq == 5);
}

// 4. 平局：两源有完全相同的 enc_key 副本，只输出一次
void test_duplicate_enc_key_single_output() {
	auto a = make_mem_iter({{"k", "v", 10}}, false);
	auto b = make_mem_iter({{"k", "v", 10}}, false); // 完全相同的副本
	TwoMergeIterator it(a, b, false);

	auto entries = collect(it);
	CHECK(entries.size() == 1);
	CHECK(entries[0].key == "k" && entries[0].value == "v" &&
		  entries[0].seq == 10);
}

// 5. 一边空流：空源不影响输出
void test_one_side_empty() {
	auto a = make_mem_iter({{"a", "1", 1}}, false);
	auto b = make_mem_iter(false); // 空流
	TwoMergeIterator it(a, b, false);

	auto entries = collect(it);
	CHECK(entries.size() == 1);
	CHECK(entries[0].key == "a" && entries[0].value == "1");
}

// 6. 双空流 + end 判等：遍历循环能正确终止（死循环回归）
void test_end_equality_and_empty() {
	auto empty1 = make_mem_iter(false);
	auto empty2 = make_mem_iter(false);
	TwoMergeIterator empty_it(empty1, empty2, false);

	CHECK(!empty_it.is_valid()); // 双空流 = 无效

	// 两个"空"TwoMerge 判等
	TwoMergeIterator end_it(empty1, empty2, false);
	CHECK(empty_it == end_it);
	CHECK(!(empty_it != end_it));

	// 遍历完的迭代器 vs 另一个空迭代器：也应判等
	auto a = make_mem_iter({{"a", "1", 1}}, false);
	auto b = make_mem_iter({{"b", "2", 2}}, false);
	TwoMergeIterator it(a, b, false);
	auto entries = collect_with_end(it, end_it); // 用 != end 终止
	CHECK(entries.size() == 2);
	CHECK(entries[0].key == "a");
	CHECK(entries[1].key == "b");
	CHECK(it == end_it); // 遍历完后 == end
}

// 7. 墓碑：源流已滤墓碑（begin 默认 skip_tombstone），归并层不再重复输出
void test_tombstone_filtered_by_source() {
	MemTable table;
	CHECK(table.put("k", "v", 5));
	CHECK(table.remove("k", 10)); // 墓碑覆盖
	// begin() 默认 skip_tombstone=true：墓碑及其旧版本都被源流滤掉 → 空流
	auto source = std::make_shared<HeapIterator>(
		std::move(table.begin(std::nullopt, false)));
	CHECK(!source->is_valid());

	auto empty = make_mem_iter(false);
	TwoMergeIterator it(source, empty, false);
	CHECK(!it.is_valid()); // 墓碑已被源流滤掉，归并层输出空
}

// ─── 测试注册 ───────────────────────────────────────────────────

struct TestCase {
	const char *name;
	void (*run)();
};

} // namespace

int main() {
	const TestCase tests[] = {
		{"basic merge", test_basic_merge},
		{"query mode dedup across sources",
		 test_query_mode_dedup_across_sources},
		{"keep all versions across sources",
		 test_keep_all_versions_across_sources},
		{"duplicate enc_key single output", test_duplicate_enc_key_single_output},
		{"one side empty", test_one_side_empty},
		{"end equality and empty", test_end_equality_and_empty},
		{"tombstone filtered by source", test_tombstone_filtered_by_source},
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
