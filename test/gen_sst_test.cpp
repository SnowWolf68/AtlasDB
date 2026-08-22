#include "lsm.h"
#include "MemTable.h"

#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
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

// 造一个含数据的 memtable 源流（keep_all=true，全版本）
std::shared_ptr<BaseIterator> make_mem_iter(
	std::vector<std::tuple<std::string, std::string, std::uint64_t>> kvs) {
	MemTable table;
	for (auto &[key, value, seq] : kvs) {
		CHECK(table.put(key, value, seq));
	}
	return std::make_shared<HeapIterator>(
		std::move(table.begin(std::nullopt, true)));
}

// 遍历生成的 SST，把所有条目收集回来（验证内容完整性）
std::vector<std::tuple<std::string, std::string, std::uint64_t>>
read_all_entries(const std::shared_ptr<SST> &sst) {
	std::vector<std::tuple<std::string, std::string, std::uint64_t>> entries;
	auto it = sst->begin(std::nullopt, true);
	while (it.is_valid()) {
		InternalKey ik = InternalKey::decode((*it).first);
		entries.push_back(
			std::make_tuple(ik._key, (*it).second, ik._commit_sequence));
		++it;
	}
	return entries;
}

// ─── 测试用例 ───────────────────────────────────────────────────

// 1. 少量数据（不足一张表）：全部进一张表，内容完整
void test_small_data_single_sst() {
	std::string dir = "/tmp/gensst_test_1";
	std::filesystem::remove_all(dir);

	LSMEngine engine(dir, 64 * 1024 * 1024, 4 * 1024 * 1024);
	auto stream = make_mem_iter(
		{{"a", "1", 1}, {"b", "2", 2}, {"c", "3", 3}});
	TwoMergeIterator two(stream, make_mem_iter({}), true);

	auto ssts = engine.generate_sst_from_bast_iter(two, 4 * 1024 * 1024, 1);
	CHECK(ssts.size() == 1); // 不足一张表 → 只有尾部兜底的一张
	CHECK(ssts[0] != nullptr);

	auto entries = read_all_entries(ssts[0]);
	CHECK(entries.size() == 3);
	CHECK(std::get<0>(entries[0]) == "a" && std::get<1>(entries[0]) == "1");
	CHECK(std::get<0>(entries[1]) == "b" && std::get<1>(entries[1]) == "2");
	CHECK(std::get<0>(entries[2]) == "c" && std::get<1>(entries[2]) == "3");
}

// 2. 大数据量切多张表：表间 key 段有序且不重叠
void test_multi_sst_key_ranges() {
	std::string dir = "/tmp/gensst_test_2";
	std::filesystem::remove_all(dir);

	// 100 个 key，每个 value 足够大，保证切出多张表
	LSMEngine engine(dir, 64 * 1024 * 1024, 4 * 1024 * 1024);
	std::vector<std::tuple<std::string, std::string, std::uint64_t>> kvs;
	std::string big_value(50 * 1024, 'x'); // 50KB/条
	for (int i = 0; i < 100; i++) {
		char buf[16];
		std::snprintf(buf, sizeof(buf), "key-%03d", i);
		kvs.push_back({std::string(buf), big_value, static_cast<std::uint64_t>(i + 1)});
	}
	auto stream = make_mem_iter(kvs);
	TwoMergeIterator two(stream, make_mem_iter({}), true);

	auto ssts = engine.generate_sst_from_bast_iter(two, 4 * 1024 * 1024, 1);
	CHECK(ssts.size() >= 2); // 100 × 50KB = 5MB > 4MB → 至少 2 张

	// 表间 key 段不重叠且有序
	std::string prev_last;
	for (auto &sst : ssts) {
		CHECK(sst != nullptr);
		CHECK(sst->get_first_key() > prev_last); // first_key 大于上一张的 last_key
		prev_last = sst->get_last_key();
	}

	// 内容完整性：100 条全回来，且不重复不遗漏
	std::vector<std::string> seen;
	for (auto &sst : ssts) {
		for (auto &e : read_all_entries(sst)) {
			seen.push_back(std::get<0>(e));
		}
	}
	CHECK(seen.size() == 100);
	for (int i = 0; i < 100; i++) {
		char buf[16];
		std::snprintf(buf, sizeof(buf), "key-%03d", i);
		CHECK(seen[i] == std::string(buf)); // 有序且无遗漏
	}
}

// 3. 同 key 多版本不跨表（红线）
void test_same_key_not_split_across_ssts() {
	std::string dir = "/tmp/gensst_test_3";
	std::filesystem::remove_all(dir);

	// k 的 5 个版本 + 20 个其他 key，max 很小，逼它跨过阈值——但 k 必须全在一张表里
	LSMEngine engine(dir, 64 * 1024 * 1024, 4 * 1024 * 1024);
	std::string big_value(50 * 1024, 'x'); // 50KB/条（<64KB 格式上限）
	std::vector<std::tuple<std::string, std::string, std::uint64_t>> kvs;
	// k 的 5 个版本（seq 降序排列，保持同 key 连续）
	kvs.push_back({"k", big_value, 10});
	kvs.push_back({"k", big_value, 8});
	kvs.push_back({"k", big_value, 5});
	kvs.push_back({"k", big_value, 3});
	kvs.push_back({"k", big_value, 1});
	// 20 个其他 key，每条约 50KB → 总共 25×50KB=1.25MB > 100KB 阈值 → 必然切多张
	for (int i = 0; i < 20; i++) {
		char buf[16];
		std::snprintf(buf, sizeof(buf), "key-%02d", i);
		kvs.push_back({std::string(buf), big_value,
					   static_cast<std::uint64_t>(100 + i)});
	}
	auto stream = make_mem_iter(kvs);
	TwoMergeIterator two(stream, make_mem_iter({}), true);

	// max_sst_size 调小到 100KB：1.25MB 本应切 10+ 张
	auto ssts = engine.generate_sst_from_bast_iter(two, 100 * 1024, 1);

	// k 的所有版本必须出现在同一张表（k 的 5 版本 250KB > 100KB 阈值，但同 key 不拆）
	// 验证：k 只出现在一张表里（未被拆开）
	int k_count_tables = 0;
	for (auto &sst : ssts) {
		for (auto &e : read_all_entries(sst)) {
			if (std::get<0>(e) == "k") { k_count_tables++; break; }
		}
	}
	CHECK(k_count_tables == 1); // k 只出现在一张表里（未被拆开）
	CHECK(ssts.size() >= 2);    // 确实切了多张（其他 key 触发切表）

	// 且 k 的 5 个版本全保留
	std::size_t k_versions = 0;
	for (auto &sst : ssts) {
		for (auto &e : read_all_entries(sst)) {
			if (std::get<0>(e) == "k") { k_versions++; }
		}
	}
	CHECK(k_versions == 5);
}

// 4. 空流：返回空（不 build nullptr）
void test_empty_stream() {
	std::string dir = "/tmp/gensst_test_4";
	std::filesystem::remove_all(dir);

	LSMEngine engine(dir, 64 * 1024 * 1024, 4 * 1024 * 1024);
	auto empty = make_mem_iter({});
	TwoMergeIterator two(empty, empty, true);

	auto ssts = engine.generate_sst_from_bast_iter(two, 4 * 1024 * 1024, 1);
	CHECK(ssts.empty()); // 空流 → 空结果
}

// 5. 生成的 SST id 递增（取号正确）
void test_sst_ids_increasing() {
	std::string dir = "/tmp/gensst_test_5";
	std::filesystem::remove_all(dir);

	LSMEngine engine(dir, 64 * 1024 * 1024, 4 * 1024 * 1024);
	std::string big_value(50 * 1024, 'x'); // 50KB/条（<64KB 格式上限）
	std::vector<std::tuple<std::string, std::string, std::uint64_t>> kvs;
	for (int i = 0; i < 100; i++) {
		kvs.push_back({"k" + std::to_string(i), big_value,
					   static_cast<std::uint64_t>(i + 1)});
	}
	auto stream = make_mem_iter(kvs);
	TwoMergeIterator two(stream, make_mem_iter({}), true);

	// 100×50KB = 5MB > 4MB → 应切出多张表
	auto ssts = engine.generate_sst_from_bast_iter(two, 4 * 1024 * 1024, 1);
	CHECK(ssts.size() >= 2);
	for (std::size_t i = 1; i < ssts.size(); i++) {
		CHECK(ssts[i]->get_sst_id() == ssts[i - 1]->get_sst_id() + 1); // 递增
	}
	// 文件确实写到了目标层
	std::string expect_path =
		engine.get_sst_path(ssts[0]->get_sst_id(), 1);
	CHECK(std::filesystem::exists(expect_path));
}

// ─── 测试注册 ───────────────────────────────────────────────────

struct TestCase {
	const char *name;
	void (*run)();
};

} // namespace

int main() {
	const TestCase tests[] = {
		{"small data single sst", test_small_data_single_sst},
		{"multi sst key ranges", test_multi_sst_key_ranges},
		{"same key not split across ssts", test_same_key_not_split_across_ssts},
		{"empty stream", test_empty_stream},
		{"sst ids increasing", test_sst_ids_increasing},
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
