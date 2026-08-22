#include "MemTable.h"
#include "Record.h"
#include "sst.h"

#include <cstdint>
#include <cstdio>
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

std::string tmp_path(const std::string &name) {
	return "/tmp/atlasdb_flush_last_test_" + name + ".sst";
}

void rm_tmp(const std::string &path) { std::remove(path.c_str()); }

struct FlushedEntry {
	std::string _key;
	std::string _value;
	std::uint64_t _seq;
	ValueType _type;
};

// 全量读出 SST 的所有 entry（跨 block，含同一 user_key 的全部版本）
//
// 注意: 不能用 SST::begin()/end() 做全量校验——SST::begin() 走
// get(get_first_key())，其迭代器带首 key 的 commit_sequence 过滤
// （clean_by_commit_sequence 清掉 seq 更大的条目），且
// BlockIterator::operator++ 在 keep_all_version=false 时会跳过
// 同 user_key 的旧版本。校验 flush 内容必须直接遍历 block
// （keep_all_version=true, 无 seq 过滤）。
std::vector<FlushedEntry> read_all_entries(const std::shared_ptr<SST> &sst) {
	std::vector<FlushedEntry> entries;
	for (std::size_t b = 0; b < sst->num_blocks(); b++) {
		std::shared_ptr<Block> block = sst->read_block(b);
		if (block == nullptr) {
			throw TestFailure("read_block returned nullptr");
		}
		for (BlockIterator it = block->begin(std::nullopt, true);
			 it != block->end(); ++it) {
			std::pair<std::string, std::string> kv = *it;
			InternalKey ik = InternalKey::decode(kv.first);
			entries.push_back({ik._key, kv.second, ik._commit_sequence,
							   ik._value_type});
		}
	}
	return entries;
}

// 每次 flush 用一个新的 SSTBuilder（builder 状态会在 build 后累积）
std::shared_ptr<SST> flush_once(MemTable &table, std::uint64_t sst_id,
								const std::string &path,
								std::vector<std::uint64_t> &flushed_seqs) {
	SSTBuilder builder;
	return table.flush_last(builder, path, sst_id, flushed_seqs);
}

// ─── tests ──────────────────────────────────────────────────────

void test_flush_empty_returns_nullptr() {
	MemTable table;
	std::vector<std::uint64_t> flushed_seqs;
	std::shared_ptr<SST> sst =
		flush_once(table, 1, tmp_path("empty"), flushed_seqs);
	CHECK(sst == nullptr);
	CHECK(flushed_seqs.empty());
}

void test_flush_active_only() {
	MemTable table;
	CHECK(table.put("alpha", "1", 1));
	CHECK(table.put("bravo", "2", 2));
	CHECK(table.put("charlie", "3", 3));

	std::string path = tmp_path("active");
	rm_tmp(path);
	std::vector<std::uint64_t> flushed_seqs;
	std::shared_ptr<SST> sst = flush_once(table, 7, path, flushed_seqs);
	CHECK(sst != nullptr);
	CHECK(sst->get_sst_id() == 7);

	// 迭代顺序 = InternalKey 序（user_key 升序）
	std::vector<FlushedEntry> entries = read_all_entries(sst);
	CHECK(entries.size() == 3);
	CHECK(entries[0]._key == "alpha" && entries[0]._value == "1" &&
		  entries[0]._seq == 1 && entries[0]._type == Value);
	CHECK(entries[1]._key == "bravo" && entries[1]._value == "2" &&
		  entries[1]._seq == 2 && entries[1]._type == Value);
	CHECK(entries[2]._key == "charlie" && entries[2]._value == "3" &&
		  entries[2]._seq == 3 && entries[2]._type == Value);

	// flushed_seqs 收集了全部 seq，顺序与迭代顺序一致
	CHECK(flushed_seqs.size() == 3);
	CHECK(flushed_seqs[0] == 1 && flushed_seqs[1] == 2 && flushed_seqs[2] == 3);

	// flush 后 memtable 已空
	CHECK(table.get("alpha", 100)._value_type == NotFound);
	CHECK(table.get("bravo", 100)._value_type == NotFound);
	CHECK(table.get("charlie", 100)._value_type == NotFound);

	// 第二次 flush → 空
	std::vector<std::uint64_t> flushed_seqs_2;
	std::shared_ptr<SST> sst_2 = flush_once(table, 8, path, flushed_seqs_2);
	CHECK(sst_2 == nullptr);
	CHECK(flushed_seqs_2.empty());
	rm_tmp(path);
}

void test_flush_oldest_frozen_first() {
	MemTable table;

	// A 批: put_batch 一次性超过 4MB 阈值 → 结束时触发一次冻结
	// 7(key) + 70(value) + 8 = 85B × 50000 ≈ 4.25MB
	const int A_COUNT = 50000;
	std::string big_value(70, 'v');
	std::vector<std::pair<std::string, std::string>> kvs_a;
	std::vector<std::uint64_t> seqs_a;
	for (int i = 0; i < A_COUNT; i++) {
		char buf[16];
		std::snprintf(buf, sizeof(buf), "a-%05d", i);
		kvs_a.push_back({std::string(buf), big_value});
		seqs_a.push_back(static_cast<std::uint64_t>(i + 1));
	}
	CHECK(table.put_batch(kvs_a, seqs_a));

	// B 批: 少量数据落在冻结后新建的 active 表
	CHECK(table.put("b-0", "B0", 50001));
	CHECK(table.put("b-1", "B1", 50002));
	CHECK(table.put("b-2", "B2", 50003));

	std::string path_1 = tmp_path("frozen_1"), path_2 = tmp_path("frozen_2");
	rm_tmp(path_1);
	rm_tmp(path_2);

	// 第一次 flush: 弹出最老的 frozen 表（A 批）
	std::vector<std::uint64_t> flushed_seqs_1;
	std::shared_ptr<SST> sst_1 = flush_once(table, 1, path_1, flushed_seqs_1);
	CHECK(sst_1 != nullptr);
	std::vector<FlushedEntry> entries_1 = read_all_entries(sst_1);
	CHECK(entries_1.size() == A_COUNT);
	for (const FlushedEntry &entry : entries_1) {
		CHECK(entry._key.compare(0, 2, "a-") == 0);
		CHECK(entry._value == big_value);
		CHECK(entry._type == Value);
	}
	// key 升序 → seq 升序（seqs_a[i] = i + 1）
	CHECK(entries_1.front()._seq == 1);
	CHECK(entries_1.back()._seq == A_COUNT);
	CHECK(flushed_seqs_1.size() == A_COUNT);

	// 第二次 flush: frozen 空 → 直接取 active（B 批）
	std::vector<std::uint64_t> flushed_seqs_2;
	std::shared_ptr<SST> sst_2 = flush_once(table, 2, path_2, flushed_seqs_2);
	CHECK(sst_2 != nullptr);
	std::vector<FlushedEntry> entries_2 = read_all_entries(sst_2);
	CHECK(entries_2.size() == 3);
	CHECK(entries_2[0]._key == "b-0" && entries_2[0]._value == "B0");
	CHECK(entries_2[1]._key == "b-1" && entries_2[1]._value == "B1");
	CHECK(entries_2[2]._key == "b-2" && entries_2[2]._value == "B2");
	CHECK(flushed_seqs_2.size() == 3);

	// 最老的先刷: sst_1 的最大 seq 早于 sst_2 的最小 seq
	CHECK(entries_1.back()._seq < entries_2.front()._seq);

	// 第三次 flush → 空
	std::vector<std::uint64_t> flushed_seqs_3;
	std::shared_ptr<SST> sst_3 = flush_once(table, 3, path_1, flushed_seqs_3);
	CHECK(sst_3 == nullptr);
	CHECK(flushed_seqs_3.empty());

	rm_tmp(path_1);
	rm_tmp(path_2);
}

void test_flush_keeps_tombstone() {
	MemTable table;
	CHECK(table.put("key", "value", 1));
	CHECK(table.remove("key", 2));

	std::string path = tmp_path("tombstone");
	rm_tmp(path);
	std::vector<std::uint64_t> flushed_seqs;
	std::shared_ptr<SST> sst = flush_once(table, 4, path, flushed_seqs);
	CHECK(sst != nullptr);

	// 同 user_key: seq 降序 → tombstone(2) 在前, value(1) 在后
	std::vector<FlushedEntry> entries = read_all_entries(sst);
	CHECK(entries.size() == 2);
	CHECK(entries[0]._key == "key" && entries[0]._type == TombStone &&
		  entries[0]._seq == 2 && entries[0]._value == "");
	CHECK(entries[1]._key == "key" && entries[1]._type == Value &&
		  entries[1]._seq == 1 && entries[1]._value == "value");
	// tombstone 的 seq 也必须收集（WAL checkpoint 用）
	CHECK(flushed_seqs.size() == 2);
	CHECK(flushed_seqs[0] == 2 && flushed_seqs[1] == 1);
	rm_tmp(path);
}

void test_flush_keeps_multiple_versions() {
	MemTable table;
	CHECK(table.put("k", "old", 1));
	CHECK(table.put("k", "new", 3));

	std::string path = tmp_path("versions");
	rm_tmp(path);
	std::vector<std::uint64_t> flushed_seqs;
	std::shared_ptr<SST> sst = flush_once(table, 5, path, flushed_seqs);
	CHECK(sst != nullptr);

	std::vector<FlushedEntry> entries = read_all_entries(sst);
	CHECK(entries.size() == 2);
	CHECK(entries[0]._seq == 3 && entries[0]._value == "new" &&
		  entries[0]._type == Value);
	CHECK(entries[1]._seq == 1 && entries[1]._value == "old" &&
		  entries[1]._type == Value);
	CHECK(flushed_seqs.size() == 2);
	CHECK(flushed_seqs[0] == 3 && flushed_seqs[1] == 1);
	rm_tmp(path);
}

void test_reopen_sst_from_disk() {
	MemTable table;
	CHECK(table.put("alpha", "hello", 10));
	CHECK(table.put("bravo", "world", 20));

	std::string path = tmp_path("reopen");
	rm_tmp(path);
	std::vector<std::uint64_t> flushed_seqs;
	std::shared_ptr<SST> sst = flush_once(table, 7, path, flushed_seqs);
	CHECK(sst != nullptr);

	// 从磁盘重新打开, 内容应与构建时一致
	std::shared_ptr<SST> reopened = SST::open(path, 7);
	CHECK(reopened != nullptr);
	CHECK(reopened->get_sst_id() == 7);
	std::vector<FlushedEntry> entries = read_all_entries(reopened);
	CHECK(entries.size() == 2);
	CHECK(entries[0]._key == "alpha" && entries[0]._value == "hello" &&
		  entries[0]._seq == 10);
	CHECK(entries[1]._key == "bravo" && entries[1]._value == "world" &&
		  entries[1]._seq == 20);
	rm_tmp(path);
}

void test_memtable_usable_after_flush() {
	MemTable table;
	CHECK(table.put("old-key", "old", 1));

	std::string path = tmp_path("usable");
	rm_tmp(path);
	std::vector<std::uint64_t> flushed_seqs;
	std::shared_ptr<SST> sst = flush_once(table, 6, path, flushed_seqs);
	CHECK(sst != nullptr);

	// 已刷走的数据在 memtable 中不可见
	CHECK(table.get("old-key", 100)._value_type == NotFound);

	// flush 后继续写读
	CHECK(table.put("new-key", "new", 2));
	LookUpResult result = table.get("new-key", 2);
	CHECK(result._value_type == Value);
	CHECK(result._value == "new");
	CHECK(result._commit_sequence == 2);

	// 新数据也能被刷走
	std::vector<std::uint64_t> flushed_seqs_2;
	std::shared_ptr<SST> sst_2 = flush_once(table, 9, path, flushed_seqs_2);
	CHECK(sst_2 != nullptr);
	std::vector<FlushedEntry> entries = read_all_entries(sst_2);
	CHECK(entries.size() == 1);
	CHECK(entries[0]._key == "new-key" && entries[0]._seq == 2);
	rm_tmp(path);
}

} // namespace

int main() {
	struct TestCase {
		const char *name;
		void (*run)();
	};

	const TestCase tests[] = {
		{"flush empty returns nullptr", test_flush_empty_returns_nullptr},
		{"flush active-only data", test_flush_active_only},
		{"flush oldest frozen first", test_flush_oldest_frozen_first},
		{"flush keeps tombstone", test_flush_keeps_tombstone},
		{"flush keeps multiple versions", test_flush_keeps_multiple_versions},
		{"reopen sst from disk", test_reopen_sst_from_disk},
		{"memtable usable after flush", test_memtable_usable_after_flush},
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
