#include "lsm.h"
#include "Record.h"
#include "sst.h"

#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
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

struct KV {
	std::string key;
	std::string value;
	std::uint64_t seq;
	ValueType type = Value;
};

// 与 LSMEngine::get_sst_path 相同的命名格式: sst_<32位补零id>.<level>
std::string make_sst_path(const std::string &dir, std::size_t id,
						  std::size_t level) {
	std::string id_str = std::to_string(id);
	std::string padded(32 - id_str.size(), '0');
	return dir + "/sst_" + padded + id_str + "." + std::to_string(level);
}

// 直接用 SSTBuilder 写一张 SST 文件到磁盘（引擎构造器扫目录时恢复它）
void write_sst(const std::string &dir, std::size_t id, std::size_t level,
			   const std::vector<KV> &kvs) {
	SSTBuilder builder;
	for (const KV &kv : kvs) {
		CHECK(builder.add(kv.key, kv.value, kv.seq, kv.type));
	}
	std::string path = make_sst_path(dir, id, level);
	CHECK(builder.build(id, path) != nullptr);
}

// 遍历一张 SST 的全部条目（keep_all=true，不过滤版本）
std::vector<Entry> read_all(const std::shared_ptr<SST> &sst) {
	std::vector<Entry> entries;
	auto it = sst->begin(std::nullopt, true);
	while (it.is_valid()) {
		InternalKey ik = InternalKey::decode((*it).first);
		entries.push_back(
			{ik._key, (*it).second, ik._commit_sequence, ik._value_type});
		++it;
	}
	return entries;
}

// 摊平一组 SST 的所有条目（按表序）
std::vector<Entry> read_all_ssts(
	const std::vector<std::shared_ptr<SST>> &ssts) {
	std::vector<Entry> entries;
	for (auto &sst : ssts) {
		for (auto &e : read_all(sst)) { entries.push_back(e); }
	}
	return entries;
}

// ─── compact_l0_l1 ──────────────────────────────────────────────

// 1. 只有 L0：两张 user_key 重叠的表 → 全版本合并、按 (key 升序, seq 降序)；
//    新表落在 level 1、id 接续高水位、旧文件未被删（删除是主流程的职责）
void test_l0_only_merge() {
	std::string dir = "/tmp/compact_test_l0_only";
	std::filesystem::remove_all(dir);
	std::filesystem::create_directories(dir);

	// id 越大越新；两张表 user_key 重叠（L0 语义）
	write_sst(dir, 1, 0, {{"a", "1", 1, Value}, {"b", "2", 2, Value}});
	write_sst(dir, 2, 0, {{"a", "3", 3, Value}, {"c", "4", 4, Value}});

	LSMEngine engine(dir, 64 * 1024 * 1024, 4 * 1024 * 1024);
	auto ssts = engine.compact_l0_l1(4 * 1024 * 1024);

	CHECK(ssts.size() == 1);
	auto entries = read_all(ssts[0]);
	// a@3, a@1, b@2, c@4
	CHECK(entries.size() == 4);
	CHECK(entries[0].key == "a" && entries[0].seq == 3 && entries[0].value == "3");
	CHECK(entries[1].key == "a" && entries[1].seq == 1 && entries[1].value == "1");
	CHECK(entries[2].key == "b" && entries[2].seq == 2 && entries[2].value == "2");
	CHECK(entries[3].key == "c" && entries[3].seq == 4 && entries[3].value == "4");

	// id 接续：恢复后高水位 = max(1,2)+1 = 3，新表应取 3
	CHECK(ssts[0]->get_sst_id() == 3);
	// 新文件写到 level 1
	CHECK(std::filesystem::exists(engine.get_sst_path(3, 1)));
	// 旧文件仍在（compact 只生成，删除是主流程的事）
	CHECK(std::filesystem::exists(make_sst_path(dir, 1, 0)));
	CHECK(std::filesystem::exists(make_sst_path(dir, 2, 0)));
}

// 2. L0 + L1 跨层合并：同 user_key 跨层多版本全部保留（keep_all 语义）
void test_l0_l1_cross_level() {
	std::string dir = "/tmp/compact_test_l0_l1";
	std::filesystem::remove_all(dir);
	std::filesystem::create_directories(dir);

	// L1 单表（层内有序）
	write_sst(dir, 1, 1,
			  {{"a", "1", 1, Value}, {"b", "2", 2, Value}, {"c", "3", 3, Value}});
	// L0 单表（与 L1 的 b 重叠）
	write_sst(dir, 2, 0, {{"b", "5", 5, Value}, {"d", "6", 6, Value}});

	LSMEngine engine(dir, 64 * 1024 * 1024, 4 * 1024 * 1024);
	auto ssts = engine.compact_l0_l1(4 * 1024 * 1024);

	CHECK(ssts.size() == 1);
	auto entries = read_all(ssts[0]);
	// a@1, b@5, b@2, c@3, d@6 —— b 的两个版本都保留
	CHECK(entries.size() == 5);
	CHECK(entries[0].key == "a" && entries[0].seq == 1 && entries[0].value == "1");
	CHECK(entries[1].key == "b" && entries[1].seq == 5 && entries[1].value == "5");
	CHECK(entries[2].key == "b" && entries[2].seq == 2 && entries[2].value == "2");
	CHECK(entries[3].key == "c" && entries[3].seq == 3 && entries[3].value == "3");
	CHECK(entries[4].key == "d" && entries[4].seq == 6 && entries[4].value == "6");
}

// 3. 墓碑保留：compact 搬运所有数据，tombstone 也是数据
void test_tombstone_preserved() {
	std::string dir = "/tmp/compact_test_tombstone";
	std::filesystem::remove_all(dir);
	std::filesystem::create_directories(dir);

	write_sst(dir, 1, 0, {{"a", "1", 1, Value},
						  {"b", "", 2, TombStone},
						  {"c", "3", 3, Value}});

	LSMEngine engine(dir, 64 * 1024 * 1024, 4 * 1024 * 1024);
	auto ssts = engine.compact_l0_l1(4 * 1024 * 1024);

	CHECK(ssts.size() == 1);
	auto entries = read_all(ssts[0]);
	CHECK(entries.size() == 3);
	CHECK(entries[0].key == "a" && entries[0].type == Value);
	CHECK(entries[1].key == "b" && entries[1].type == TombStone &&
		  entries[1].value.empty());
	CHECK(entries[2].key == "c" && entries[2].type == Value);
}

// 4. 只有 L1（L0 空）：L0 侧空流，结果 = L1 内容
void test_l1_only() {
	std::string dir = "/tmp/compact_test_l1_only";
	std::filesystem::remove_all(dir);
	std::filesystem::create_directories(dir);

	write_sst(dir, 1, 1, {{"a", "1", 1, Value}, {"b", "2", 2, Value}});

	LSMEngine engine(dir, 64 * 1024 * 1024, 4 * 1024 * 1024);
	auto ssts = engine.compact_l0_l1(4 * 1024 * 1024);

	CHECK(ssts.size() == 1);
	auto entries = read_all(ssts[0]);
	CHECK(entries.size() == 2);
	CHECK(entries[0].key == "a" && entries[0].value == "1");
	CHECK(entries[1].key == "b" && entries[1].value == "2");
}

// 5. 空引擎（无任何表）：不崩，返回空
void test_empty_engine() {
	std::string dir = "/tmp/compact_test_empty_engine";
	std::filesystem::remove_all(dir);

	LSMEngine engine(dir, 64 * 1024 * 1024, 4 * 1024 * 1024);
	auto ssts = engine.compact_l0_l1(4 * 1024 * 1024);
	CHECK(ssts.empty());
}

// ─── compact_near_sorted_level ──────────────────────────────────

// 6. L1 + L2 基本归并：结果全量有序，文件写到目标层 2
void test_near_sorted_basic_merge() {
	std::string dir = "/tmp/compact_test_near_basic";
	std::filesystem::remove_all(dir);
	std::filesystem::create_directories(dir);

	write_sst(dir, 1, 1, {{"a", "1", 10, Value}, {"b", "2", 20, Value}});
	write_sst(dir, 2, 2, {{"c", "3", 30, Value}, {"d", "4", 40, Value}});

	LSMEngine engine(dir, 64 * 1024 * 1024, 4 * 1024 * 1024);
	auto ssts = engine.compact_near_sorted_level(1, 2, 2, 4 * 1024 * 1024);

	CHECK(ssts.size() == 1);
	auto entries = read_all(ssts[0]);
	CHECK(entries.size() == 4);
	CHECK(entries[0].key == "a" && entries[1].key == "b" && entries[2].key == "c" &&
		  entries[3].key == "d");
	CHECK(entries[0].value == "1" && entries[3].value == "4");
	// 新文件写到目标层 2
	CHECK(std::filesystem::exists(
		engine.get_sst_path(ssts[0]->get_sst_id(), 2)));
}

// 7. 跨层同 key 多版本：L1 的 k@10/k@8 + L2 的 k@5，全部保留
void test_near_sorted_same_key_across_levels() {
	std::string dir = "/tmp/compact_test_near_same_key";
	std::filesystem::remove_all(dir);
	std::filesystem::create_directories(dir);

	write_sst(dir, 1, 1, {{"k", "10", 10, Value}, {"k", "8", 8, Value}});
	write_sst(dir, 2, 2, {{"j", "1", 1, Value}, {"k", "5", 5, Value}});

	LSMEngine engine(dir, 64 * 1024 * 1024, 4 * 1024 * 1024);
	auto ssts = engine.compact_near_sorted_level(1, 2, 2, 4 * 1024 * 1024);

	CHECK(ssts.size() == 1);
	auto entries = read_all(ssts[0]);
	// j@1, k@10, k@8, k@5
	CHECK(entries.size() == 4);
	CHECK(entries[0].key == "j" && entries[0].seq == 1);
	CHECK(entries[1].key == "k" && entries[1].seq == 10);
	CHECK(entries[2].key == "k" && entries[2].seq == 8);
	CHECK(entries[3].key == "k" && entries[3].seq == 5);
}

// 8. second 层缺失（second_iter = nullptr 路径）：结果 = first 层内容
void test_near_sorted_second_missing() {
	std::string dir = "/tmp/compact_test_near_second_missing";
	std::filesystem::remove_all(dir);
	std::filesystem::create_directories(dir);

	write_sst(dir, 1, 1, {{"a", "1", 1, Value}, {"b", "2", 2, Value}});

	LSMEngine engine(dir, 64 * 1024 * 1024, 4 * 1024 * 1024);
	auto ssts = engine.compact_near_sorted_level(1, 2, 2, 4 * 1024 * 1024);

	CHECK(ssts.size() == 1);
	auto entries = read_all(ssts[0]);
	CHECK(entries.size() == 2);
	CHECK(entries[0].key == "a" && entries[1].key == "b");
}

// 9. first 层缺失（first_iter = nullptr 路径）：结果 = second 层内容
void test_near_sorted_first_missing() {
	std::string dir = "/tmp/compact_test_near_first_missing";
	std::filesystem::remove_all(dir);
	std::filesystem::create_directories(dir);

	write_sst(dir, 2, 2, {{"a", "1", 1, Value}, {"b", "2", 2, Value}});

	LSMEngine engine(dir, 64 * 1024 * 1024, 4 * 1024 * 1024);
	auto ssts = engine.compact_near_sorted_level(1, 2, 2, 4 * 1024 * 1024);

	CHECK(ssts.size() == 1);
	auto entries = read_all(ssts[0]);
	CHECK(entries.size() == 2);
	CHECK(entries[0].key == "a" && entries[1].key == "b");
}

// 10. 两层都缺失：不崩，返回空
void test_near_sorted_both_missing() {
	std::string dir = "/tmp/compact_test_near_both_missing";
	std::filesystem::remove_all(dir);

	LSMEngine engine(dir, 64 * 1024 * 1024, 4 * 1024 * 1024);
	auto ssts = engine.compact_near_sorted_level(1, 2, 2, 4 * 1024 * 1024);
	CHECK(ssts.empty());
}

// 11. 大数据切多张表：表间 key 段有序且不重叠，内容无遗漏
void test_near_sorted_multi_sst() {
	std::string dir = "/tmp/compact_test_near_multi";
	std::filesystem::remove_all(dir);
	std::filesystem::create_directories(dir);

	std::string big_value(50 * 1024, 'x'); // 50KB/条（<64KB 格式上限）
	std::vector<KV> kvs;
	for (int i = 0; i < 30; i++) {
		char buf[16];
		std::snprintf(buf, sizeof(buf), "key-%02d", i);
		kvs.push_back({std::string(buf), big_value,
					   static_cast<std::uint64_t>(i + 1), Value});
	}
	write_sst(dir, 1, 1, kvs); // 30×50KB = 1.5MB

	LSMEngine engine(dir, 64 * 1024 * 1024, 4 * 1024 * 1024);
	// 阈值 100KB → 必然切多张
	auto ssts = engine.compact_near_sorted_level(1, 2, 2, 100 * 1024);

	CHECK(ssts.size() >= 2);
	std::string prev_last;
	for (auto &sst : ssts) {
		CHECK(sst->get_first_key() > prev_last); // 表间 key 段有序不重叠
		prev_last = sst->get_last_key();
	}
	auto entries = read_all_ssts(ssts);
	CHECK(entries.size() == 30);
	for (int i = 0; i < 30; i++) {
		char buf[16];
		std::snprintf(buf, sizeof(buf), "key-%02d", i);
		CHECK(entries[i].key == std::string(buf)); // 有序且无遗漏
	}
}

// ─── compact 主流程（flush 触发）────────────────────────────────

// 12. flush 触发 compact：L0 满阈值后自动压到 L1，数据完整可读
void test_compact_triggered() {
	std::string dir = "/tmp/compact_test_triggered";
	std::filesystem::remove_all(dir);

	// 阈值 2（少造表），L1 目标用默认大值 → 不会级联
	LSMEngine engine(dir, 64 * 1024 * 1024, 4 * 1024 * 1024, 2);

	// 第 1 轮：flush 后 L0 = 1（<2，不触发）
	for (int i = 0; i < 100; i++) {
		engine.put("k" + std::to_string(i), std::string(1024, 'a'), i + 1);
	}
	engine.flush();
	// 第 2 轮：flush 后 L0 = 2 → 触发 compact → L0 清空、压到 L1
	for (int i = 0; i < 100; i++) {
		engine.put("k" + std::to_string(100 + i), std::string(1024, 'b'),
				   1000 + i);
	}
	engine.flush();
	// 第 3 轮：又一张 L0（=1，不触发）
	for (int i = 0; i < 100; i++) {
		engine.put("k" + std::to_string(200 + i), std::string(1024, 'c'),
				   2000 + i);
	}
	engine.flush();

	// 数据完整性：300 个 key 全部可读，值正确（经过 flush + compact 不丢数据）
	for (int i = 0; i < 300; i++) {
		auto res = engine.get("k" + std::to_string(i), 999999);
		CHECK(res.is_found());
		char expect = 'a' + i / 100;
		CHECK(res._value == std::string(1024, expect));
	}

	// 磁盘上存在 level-1 文件（第 2 轮的 L0 被压上去了）
	bool has_l1 = false;
	for (auto &entry : std::filesystem::directory_iterator(dir)) {
		if (entry.path().filename().string().find(".1") != std::string::npos) {
			has_l1 = true;
		}
	}
	CHECK(has_l1);
}

// 13. 级联：L1 压完仍超阈值 → 继续压下一层,直到收敛成单表,数据完整
void test_compact_cascade() {
	std::string dir = "/tmp/compact_test_cascade";
	std::filesystem::remove_all(dir);

	// 阈值 2, L0 目标 25KB → 每层目标 ×2: L1=50KB, L2=100KB, L3=200KB, L4=400KB
	// ~204KB 数据 → L1 得 ~4 张(≥2,级联)→ L2 得 ~2 张(≥2,级联)→ L3 得 ~2 张
	// (级联)→ L4 得 1 张(<2,停)：级联必须终止且收敛
	LSMEngine engine(dir, 64 * 1024 * 1024, 4 * 1024 * 1024, 2, 25 * 1024);

	for (int i = 0; i < 100; i++) {
		engine.put("k" + std::to_string(i), std::string(1024, 'a'), i + 1);
	}
	engine.flush(); // L0 = 1
	for (int i = 0; i < 100; i++) {
		engine.put("k" + std::to_string(100 + i), std::string(1024, 'b'),
				   1000 + i);
	}
	engine.flush(); // L0 = 2 → compact → 级联

	// 数据完整性（经过多轮 compact 不丢数据）
	for (int i = 0; i < 200; i++) {
		auto res = engine.get("k" + std::to_string(i), 999999);
		CHECK(res.is_found());
		char expect = i < 100 ? 'a' : 'b';
		CHECK(res._value == std::string(1024, expect));
	}

	// 级联收敛：最终只剩 1 张表,且不在 L0/L1（全被清空压上去了）
	// 只数 SST 文件（目录里还有水位文件 seq.txt,不能计入）
	int file_count = 0;
	bool has_deep = false;
	for (auto &entry : std::filesystem::directory_iterator(dir)) {
		std::string name = entry.path().filename().string();
		if (name.rfind("sst_", 0) != 0) { continue; }
		file_count++;
		if (name.find(".0") == std::string::npos &&
			name.find(".1") == std::string::npos) {
			has_deep = true; // 数据落在 L2+
		}
	}
	CHECK(file_count == 1); // 级联收敛成单表（不是无限建层）
	CHECK(has_deep);        // 且不在 L0/L1
}

// ─── 测试注册 ───────────────────────────────────────────────────

struct TestCase {
	const char *name;
	void (*run)();
};

} // namespace

int main() {
	const TestCase tests[] = {
		{"l0 only merge", test_l0_only_merge},
		{"l0+l1 cross level", test_l0_l1_cross_level},
		{"tombstone preserved", test_tombstone_preserved},
		{"l1 only (l0 empty)", test_l1_only},
		{"empty engine", test_empty_engine},
		{"near sorted basic merge", test_near_sorted_basic_merge},
		{"near sorted same key across levels", test_near_sorted_same_key_across_levels},
		{"near sorted second level missing", test_near_sorted_second_missing},
		{"near sorted first level missing", test_near_sorted_first_missing},
		{"near sorted both missing", test_near_sorted_both_missing},
		{"near sorted multi sst", test_near_sorted_multi_sst},
		{"compact triggered by flush", test_compact_triggered},
		{"compact cascade to l2", test_compact_cascade},
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
