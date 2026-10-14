#include "lsm.h"
#include "Record.h"
#include "sst.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
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

// 每个用例独立的临时目录（测试完保留，方便排错时人工检查）
std::string tmp_dir(const std::string &name) {
	return "/tmp/atlasdb_lsm_engine_test_" + name;
}

void rm_dir(const std::string &path) {
	std::filesystem::remove_all(path);
}

// 统计 data_dir 下的 SST 文件数（文件名以 sst_ 开头）
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

struct FlushedEntry {
	std::string _key;
	std::string _value;
	std::uint64_t _seq;
	ValueType _type;
};

// 全量读出 SST 的所有 entry（跨 block，含同一 user_key 的全部版本）
// 与 flush_last_test 的 read_all_entries 同款：
// 必须直接遍历 block（begin(nullopt, true)），不能用 SST::begin() 的全量语义
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
			entries.push_back(
				{ik._key, kv.second, ik._commit_sequence, ik._value_type});
		}
	}
	return entries;
}

// ─── 用例 ───────────────────────────────────────────────────────

// 1. 构造器：建目录（含嵌套路径）+ 空引擎 flush 返回 0、不产生文件
void test_constructor_creates_dir_and_empty_flush() {
	const std::string dir = tmp_dir("ctor");
	rm_dir(dir);
	CHECK(!std::filesystem::exists(dir));

	{
		LSMEngine engine(dir, 1024, 128);
		CHECK(std::filesystem::exists(dir)); // 构造即建目录

		// 空引擎 flush：返回 0，不产生任何 SST 文件
		CHECK(engine.flush() == 0);
		CHECK(count_sst_files(dir) == 0);
	}

	// 嵌套路径也能建（create_directories）
	const std::string nested = tmp_dir("ctor") + "/a/b";
	{
		LSMEngine engine(nested, 1024, 128);
		CHECK(std::filesystem::exists(nested));
	}

	rm_dir(dir);
}

// 2. 写满触发 flush：产生 sst_<32位id>.0 文件，且 put 返回值非 0
void test_put_triggers_flush_and_creates_file() {
	const std::string dir = tmp_dir("put_flush");
	rm_dir(dir);
	{
		// 大 active 阈值：不冻结，全在 active；小引擎阈值：快速触发 flush
		LSMEngine engine(dir, 1024, 1024 * 1024);

		bool saw_flush = false;
		std::size_t prev_files = 0;
		// 每条约 5+7+8=20B，60 条 ≈ 1200B > 1024 → 触发 flush
		for (int i = 0; i < 60; i++) {
			CHECK(engine.put("key-" + std::to_string(i),
					   "value-" + std::to_string(i))
					  .has_value());
			// put 返回语义已改为"成功=本次 seq",不再携带"是否刷盘"信息,
			// 用 SST 文件数增长判断
			std::size_t cur_files = count_sst_files(dir);
			if (cur_files > prev_files) { saw_flush = true; }
			prev_files = cur_files;
		}
		CHECK(saw_flush); // 至少一次触发刷盘

		// 命名规范：sst_<32位补零id>.0
		const std::string file0 = engine.get_sst_path(0, 0);
		CHECK(std::filesystem::exists(file0));
		CHECK(count_sst_files(dir) == 1);
	}
	rm_dir(dir);
}

// 3. flush 内容回环：SST 里读回的数据与写入完全一致
void test_flush_content_roundtrip() {
	const std::string dir = tmp_dir("roundtrip");
	rm_dir(dir);
	{
		// 大引擎阈值：写入过程不触发自动 flush，写完显式 flush，落盘内容确定
		LSMEngine engine(dir, 64 * 1024 * 1024, 1024 * 1024);

		const int n = 50;
		for (int i = 0; i < n; i++) {
			CHECK(engine.put("key-" + std::to_string(i),
					   "value-" + std::to_string(i))
					  .has_value());
		}

		// 显式 flush：active 直刷成 file0，返回值为表内最大 seq
		CHECK(engine.flush() == static_cast<std::uint64_t>(n));

		const std::string file0 = engine.get_sst_path(0, 0);
		std::shared_ptr<SST> sst = SST::open(file0, 0);
		CHECK(sst != nullptr);

		auto entries = read_all_entries(sst);
		CHECK(entries.size() == static_cast<std::size_t>(n));
		for (const auto &e : entries) {
			// 用 entry 自身 key 反推索引，校验 value 和 seq
			const std::string idx = e._key.substr(4);
			CHECK(e._value == "value-" + idx);
			CHECK(e._seq == static_cast<std::uint64_t>(std::stoull(idx) + 1));
			CHECK(e._type == Value);
		}
	}
	rm_dir(dir);
}

// 4. 多次 flush：多个文件、id 从 0 递增、每个文件可打开
void test_multiple_flushes_increasing_ids() {
	const std::string dir = tmp_dir("multi_flush");
	rm_dir(dir);
	{
		// 小 active 阈值：每 ~7 条冻结一张表；小引擎阈值：频繁触发 flush
		// 4.5 起 flush 会在 L0 满 compact_threshold 时触发 compact（删 L0 文件）,
		// 本用例只验证 flush 的文件数/id 递增行为,传大阈值隔离 compact
		LSMEngine engine(dir, 1024, 128, 9999);

		std::size_t flush_count = 0;
		std::size_t prev_files = 0;
		for (int i = 0; i < 200; i++) {
			CHECK(engine.put("key-" + std::to_string(i),
					   "value-" + std::to_string(i))
					  .has_value());
			// put 返回语义已改为"成功=本次 seq",不再携带"是否刷盘"信息,
			// 改用 SST 文件数增长来数刷盘次数(threshold=9999 不触发 compact,文件只增不减)
			std::size_t cur_files = count_sst_files(dir);
			if (cur_files > prev_files) { flush_count++; }
			prev_files = cur_files;
		}
		CHECK(flush_count >= 2); // 至少两次刷盘

		// 文件数与刷盘次数一致，id 从 0 连续递增
		CHECK(count_sst_files(dir) == flush_count);
		for (std::size_t id = 0; id < flush_count; id++) {
			const std::string file = engine.get_sst_path(id, 0);
			CHECK(std::filesystem::exists(file));
			std::shared_ptr<SST> sst = SST::open(file, id);
			CHECK(sst != nullptr);
			CHECK(!read_all_entries(sst).empty()); // 每个文件都有数据
		}
	}
	rm_dir(dir);
}

// 5. remove 触发 flush：tombstone 落盘，且与旧版本共存
void test_remove_flushes_tombstone() {
	const std::string dir = tmp_dir("remove_flush");
	rm_dir(dir);
	{
		LSMEngine engine(dir, 64 * 1024 * 1024, 1024 * 1024);

		CHECK(engine.put("victim", "alive").has_value());
		CHECK(engine.remove("victim").has_value());
		// 填充数据（凑足一块 block 的内容）
		for (int i = 0; i < 60; i++) {
			CHECK(engine.put("pad-" + std::to_string(i), "x").has_value());
		}

		// 显式 flush，全部内容落盘到 file0
		CHECK(engine.flush() > 0);

		const std::string file0 = engine.get_sst_path(0, 0);
		std::shared_ptr<SST> sst = SST::open(file0, 0);
		CHECK(sst != nullptr);

		auto entries = read_all_entries(sst);
		bool found_value = false, found_tombstone = false;
		for (const auto &e : entries) {
			if (e._key == "victim") {
				if (e._type == Value && e._value == "alive" && e._seq == 1) {
					found_value = true;
				}
				if (e._type == TombStone && e._value.empty() && e._seq == 2) {
					found_tombstone = true;
				}
			}
		}
		CHECK(found_value);      // 旧版本还在
		CHECK(found_tombstone);  // tombstone 已落盘
	}
	rm_dir(dir);
}

// 6. put 返回值语义（新合同）：成功 = 本次写的 seq（读己之写快照点）；拒绝 = nullopt
void test_put_return_value_semantics() {
	const std::string dir = tmp_dir("retval");
	rm_dir(dir);
	{
		// 大阈值引擎：写入不触发 flush
		LSMEngine engine(dir, 64 * 1024 * 1024, 1024 * 1024);
		// 新库自动分配序号，成功返回本次分配的 seq。
		for (int i = 0; i < 10; i++) {
			CHECK(engine.put("k" + std::to_string(i), "v") ==
				  std::optional<std::uint64_t>(i + 1));
		}
		// 后续写入的 seq 严格单调递增。
		auto s1 = engine.put("a", "v");
		auto s2 = engine.put("b", "v");
		CHECK(s1.has_value() && s2.has_value() && *s2 > *s1);
		// batch：逐条返回 seq，与输入顺序对应且严格递增
		std::vector<std::pair<std::string, std::string>> kvs = {
			{"x", "1"}, {"y", "2"}, {"z", "3"}};
		auto sb = engine.put_batch(kvs);
		CHECK(sb.size() == kvs.size());
		std::uint64_t previous = *s2;
		for (const auto &seq : sb) {
			CHECK(seq.has_value() && *seq > previous);
			previous = *seq;
		}
		// 超限 value：单条拒绝
		CHECK(!engine.put("big", std::string(64 * 1024 + 1, 'v')).has_value());
	}
	rm_dir(dir);

	{
		// 小阈值引擎：触发 flush 的 put 仍返回本次 seq（不返回刷盘水位）
		LSMEngine engine(dir, 1024, 1024 * 1024);
		bool flushed = false;
		std::size_t prev_files = 0;
		for (int i = 0; i < 60 && !flushed; i++) {
			CHECK(engine.put("key-" + std::to_string(i),
							 "value-" + std::to_string(i)) ==
				  std::optional<std::uint64_t>(i + 1));
			std::size_t cur_files = count_sst_files(dir);
			if (cur_files > prev_files) { flushed = true; }
			prev_files = cur_files;
		}
		CHECK(flushed); // 确实发生过刷盘
	}
	rm_dir(dir);
}

// 7. get 读回：写→flush→引擎 get 命中；未写的 key → NotFound
void test_get_after_flush() {
	const std::string dir = tmp_dir("get_readback");
	rm_dir(dir);
	{
		LSMEngine engine(dir, 64 * 1024 * 1024, 1024 * 1024);

		const int n = 50;
		for (int i = 0; i < n; i++) {
			CHECK(engine.put("key-" + std::to_string(i),
					   "value-" + std::to_string(i))
					  .has_value());
		}
		CHECK(engine.flush() > 0);

		// 全部读回，值和 seq 正确
		for (int i = 0; i < n; i++) {
			LookUpResult res = engine.get("key-" + std::to_string(i), 100);
			CHECK(res._value_type == Value);
			CHECK(res._value == "value-" + std::to_string(i));
			CHECK(res._commit_sequence ==
				  static_cast<std::uint64_t>(i + 1));
		}
		// 未写过的 key → NotFound
		CHECK(engine.get("not-exist", 100)._value_type == NotFound);
	}
	rm_dir(dir);
}

// 8. memtable 墓碑短路：SST 有旧值，memtable 有墓碑 → 返回删除态，不读旧值
void test_memtable_tombstone_shortcircuit() {
	const std::string dir = tmp_dir("mem_tombstone");
	rm_dir(dir);
	{
		LSMEngine engine(dir, 64 * 1024 * 1024, 1024 * 1024);

		CHECK(engine.put("victim", "old-value").has_value());
		CHECK(engine.flush() > 0); // SST 里有 "victim"="old-value"

		// memtable 里写墓碑（不 flush）→ get 必须命中墓碑短路，
		// 不能穿透到 SST 读回旧值
		CHECK(engine.remove("victim").has_value());
		LookUpResult res = engine.get("victim", 100);
		CHECK(res._value_type == TombStone);
		CHECK(res._value.empty());
	}
	rm_dir(dir);
}

// 9. SST 墓碑短路：两个 SST，新的含墓碑，旧的有活值 → 返回删除态
void test_sst_tombstone_shortcircuit() {
	const std::string dir = tmp_dir("sst_tombstone");
	rm_dir(dir);
	{
		LSMEngine engine(dir, 64 * 1024 * 1024, 1024 * 1024);

		CHECK(engine.put("victim", "old-value").has_value());
		CHECK(engine.flush() > 0); // file0: victim@1 Value

		CHECK(engine.remove("victim").has_value());
		CHECK(engine.flush() > 0); // file1: victim@2 TombStone（更新）

		// L0 = [file1(新), file0(旧)]，先查 file1 命中墓碑 → 短路
		LookUpResult res = engine.get("victim", 100);
		CHECK(res._value_type == TombStone);
		CHECK(res._value.empty());
	}
	rm_dir(dir);
}

// 10. 多版本按 seq 过滤：同一 key 两个版本都在 SST，get 按 seq 取对应版本
void test_multi_version_seq_filter() {
	const std::string dir = tmp_dir("multi_version");
	rm_dir(dir);
	{
		LSMEngine engine(dir, 64 * 1024 * 1024, 1024 * 1024);

		CHECK(engine.put("key", "v1").has_value());
		CHECK(engine.put("key", "v2").has_value());
		CHECK(engine.flush() > 0); // 两个版本都在 file0

		LookUpResult r1 = engine.get("key", 1);   // 只看 seq<=1 → v1
		CHECK(r1._value_type == Value);
		CHECK(r1._value == "v1");
		CHECK(r1._commit_sequence == 1);

		LookUpResult r2 = engine.get("key", 2);   // seq<=2 → v2（更新）
		CHECK(r2._value_type == Value);
		CHECK(r2._value == "v2");
		CHECK(r2._commit_sequence == 2);

		LookUpResult r3 = engine.get("key", 100); // 无时间上限 → 最新 v2
		CHECK(r3._value == "v2");
	}
	rm_dir(dir);
}

// 11. 重启恢复：flush 后重建引擎 → 数据读回；next_sst_id 恢复不覆盖旧文件
void test_restart_recovery() {
	const std::string dir = tmp_dir("restart");
	rm_dir(dir);
	{
		LSMEngine engine(dir, 64 * 1024 * 1024, 1024 * 1024);
		CHECK(engine.put("a", "1").has_value());
		CHECK(engine.put("b", "2").has_value());
		CHECK(engine.flush() > 0); // file0
		CHECK(engine.put("c", "3").has_value());
		CHECK(engine.flush() > 0); // file1
	}
	{
		// 重启：同一目录重建引擎 → 扫目录恢复
		LSMEngine engine(dir, 64 * 1024 * 1024, 1024 * 1024);
		CHECK(engine.get("a", 100)._value == "1");
		CHECK(engine.get("b", 100)._value == "2");
		CHECK(engine.get("c", 100)._value == "3");
		CHECK(engine.get("zzz", 100)._value_type == NotFound);

		// next_sst_id 已恢复为 max+1=2：新 flush 用 id=2，不覆盖旧文件
		CHECK(engine.put("d", "4").has_value());
		CHECK(engine.flush() > 0);
		CHECK(std::filesystem::exists(engine.get_sst_path(2, 0)));
		// 旧数据仍在
		CHECK(engine.get("a", 100)._value == "1");
		CHECK(engine.get("d", 100)._value == "4");
	}
	rm_dir(dir);
}

// 12. L1+ 读路径：手动造 level=1 的 SST 文件 → 重启恢复 → get 走 L1+ 二分
void test_level1_read_path() {
	const std::string dir = tmp_dir("level1");
	rm_dir(dir);
	{
		// 手动构造两个 level=1 的 SST（key 段不重叠，模拟 compact 产物）
		// 目录要先建（引擎构造器才建目录，这里在构造引擎之前就要写文件）
		std::filesystem::create_directories(dir);
		{
			SSTBuilder builder;
			builder.add("a-key", "a-val", 1, Value);
			builder.add("b-key", "b-val", 2, Value);
			CHECK(builder.build(1, dir + "/sst_00000000000000000000000000000001.1") != nullptr);
		}
		{
			SSTBuilder builder;
			builder.add("c-key", "c-val", 3, Value);
			builder.add("d-key", "d-val", 4, Value);
			CHECK(builder.build(2, dir + "/sst_00000000000000000000000000000002.1") != nullptr);
		}

		// 重启恢复：扫目录应解析出 level=1 → _cur_max_level=1、_sst_id_map[1]
		LSMEngine engine(dir, 64 * 1024 * 1024, 1024 * 1024);

		// get 走 L1+ 分支（L0 为空）→ 二分定位 → 命中
		CHECK(engine.get("a-key", 100)._value == "a-val");
		CHECK(engine.get("b-key", 100)._value == "b-val");
		CHECK(engine.get("c-key", 100)._value == "c-val");
		CHECK(engine.get("d-key", 100)._value == "d-val");
		// 跨表边界、不存在的 key
		CHECK(engine.get("a-key", 100)._value_type == Value);
		CHECK(engine.get("zzz", 100)._value_type == NotFound);
		CHECK(engine.get("ab", 100)._value_type == NotFound);
	}
	rm_dir(dir);
}

// 13. 多块 SST 读回：数据超过单块 32KB → 跨块定位（locate_block 多块路径）
void test_multi_block_read() {
	const std::string dir = tmp_dir("multiblock");
	rm_dir(dir);
	{
		LSMEngine engine(dir, 64 * 1024 * 1024, 1024 * 1024);

		// 每条 ~100B，1000 条 ≈ 100KB > 32KB → 必然跨多个 block
		const int n = 1000;
		const std::uint64_t snapshot = 100000; // 快照点大于所有 seq，全部可见
		for (int i = 0; i < n; i++) {
			std::string value(90, 'x'); // 90B 的 value
			CHECK(engine.put("key-" + std::to_string(i), value).has_value());
		}
		CHECK(engine.flush() > 0);

		// 抽查首/中/尾各 key，跨块读回正确
		for (int i = 0; i < n; i += 97) {
			LookUpResult res = engine.get("key-" + std::to_string(i), snapshot);
			CHECK(res._value_type == Value);
			CHECK(res._value == std::string(90, 'x'));
			CHECK(res._commit_sequence == static_cast<std::uint64_t>(i + 1));
		}
		CHECK(engine.get("key-0", snapshot)._value == std::string(90, 'x'));
		CHECK(engine.get("key-999", snapshot)._value == std::string(90, 'x'));
	}
	rm_dir(dir);
}

// 14. get_batch 全命中：批量读回与逐个 get 结果一致
void test_get_batch_all_hit() {
	const std::string dir = tmp_dir("getbatch_hit");
	rm_dir(dir);
	{
		LSMEngine engine(dir, 64 * 1024 * 1024, 1024 * 1024);
		const int n = 30;
		std::vector<std::string> keys;
		std::vector<std::uint64_t> seqs;
		for (int i = 0; i < n; i++) {
			CHECK(engine.put("key-" + std::to_string(i),
					   "value-" + std::to_string(i))
					  .has_value());
			keys.push_back("key-" + std::to_string(i));
			seqs.push_back(100);
		}
		CHECK(engine.flush() > 0); // 全部落盘

		auto results = engine.get_batch(keys, seqs);
		CHECK(results.size() == static_cast<std::size_t>(n));
		for (int i = 0; i < n; i++) {
			CHECK(results[i]._value_type == Value);
			CHECK(results[i]._value == "value-" + std::to_string(i));
			CHECK(results[i]._commit_sequence ==
				  static_cast<std::uint64_t>(i + 1));
		}
	}
	rm_dir(dir);
}

// 15. get_batch 混合场景：memtable 命中 + SST 命中 + 不存在，各归各位
void test_get_batch_mixed() {
	const std::string dir = tmp_dir("getbatch_mixed");
	rm_dir(dir);
	{
		LSMEngine engine(dir, 64 * 1024 * 1024, 1024 * 1024);

		// SST 里：sst-key（flush 落盘）
		CHECK(engine.put("sst-key", "sst-val").has_value());
		CHECK(engine.flush() > 0);
		// memtable 里：mem-key（不 flush）
		CHECK(engine.put("mem-key", "mem-val").has_value());

		std::vector<std::string> keys = {"sst-key", "mem-key", "no-key"};
		std::vector<std::uint64_t> seqs = {100, 100, 100};
		auto results = engine.get_batch(keys, seqs);
		CHECK(results[0]._value_type == Value);
		CHECK(results[0]._value == "sst-val"); // 从 SST 查到
		CHECK(results[1]._value_type == Value);
		CHECK(results[1]._value == "mem-val"); // 从 memtable 查到
		CHECK(results[2]._value_type == NotFound); // 真不存在
	}
	rm_dir(dir);
}

// 16. get_batch 墓碑短路：memtable 有墓碑的 key，不被 SST 旧值覆盖
void test_get_batch_tombstone_shortcircuit() {
	const std::string dir = tmp_dir("getbatch_tombstone");
	rm_dir(dir);
	{
		LSMEngine engine(dir, 64 * 1024 * 1024, 1024 * 1024);

		CHECK(engine.put("victim", "old").has_value());
		CHECK(engine.flush() > 0); // SST 里有旧值
		CHECK(engine.remove("victim").has_value()); // memtable 墓碑（不 flush）

		std::vector<std::string> keys = {"victim", "other"};
		std::vector<std::uint64_t> seqs = {100, 100};
		auto results = engine.get_batch(keys, seqs);
		CHECK(results[0]._value_type == TombStone); // 短路，不读 SST 旧值
		CHECK(results[0]._value.empty());
		CHECK(results[1]._value_type == NotFound);
	}
	rm_dir(dir);
}

// 17. get_batch 与逐个 get 一致性（随机抽查对比）
void test_get_batch_consistent_with_get() {
	const std::string dir = tmp_dir("getbatch_consist");
	rm_dir(dir);
	{
		LSMEngine engine(dir, 64 * 1024 * 1024, 1024 * 1024);
		for (int i = 0; i < 40; i++) {
			CHECK(engine.put("k" + std::to_string(i), "v" + std::to_string(i))
					  .has_value());
		}
		CHECK(engine.flush() > 0);
		CHECK(engine.remove("k5").has_value()); // 墓碑（memtable）
		CHECK(engine.put("k6", "v6-new").has_value()); // 覆盖旧版本（memtable）

		std::vector<std::string> keys;
		std::vector<std::uint64_t> seqs;
		for (int i = 0; i < 40; i++) {
			keys.push_back("k" + std::to_string(i));
			seqs.push_back(100);
		}
		keys.push_back("missing");
		seqs.push_back(100);

		auto batch = engine.get_batch(keys, seqs);
		for (std::size_t i = 0; i < keys.size(); i++) {
			LookUpResult single = engine.get(keys[i], seqs[i]);
			CHECK(batch[i]._value_type == single._value_type);
			CHECK(batch[i]._value == single._value);
			CHECK(batch[i]._commit_sequence == single._commit_sequence);
		}
	}
	rm_dir(dir);
}

// 18. ConcactIterator 基础遍历：多表串联、顺序、end 终止
void test_concat_iterator_traversal() {
	const std::string dir = tmp_dir("concat_traverse");
	rm_dir(dir);
	std::filesystem::create_directories(dir);
	// 两个 level=1 的 SST，key 段不重叠（a~d, e~h）
	{
		SSTBuilder builder;
		builder.add("a-key", "a-val", 1, Value);
		builder.add("b-key", "b-val", 2, Value);
		CHECK(builder.build(1, dir + "/sst_00000000000000000000000000000001.1") != nullptr);
	}
	{
		SSTBuilder builder;
		builder.add("e-key", "e-val", 3, Value);
		builder.add("f-key", "f-val", 4, Value);
		CHECK(builder.build(2, dir + "/sst_00000000000000000000000000000002.1") != nullptr);
	}
	{
		LSMEngine engine(dir, 64 * 1024 * 1024, 1024 * 1024);
		auto it = engine.level_begin_without_lock(1, 100, false);
		auto end = engine.level_end(1);

		// 非空层 begin != end
		CHECK(!(it == end));

		// 依次遍历全部 4 个 key（跨表串联 + 顺序正确）
		const char *expect_keys[] = {"a-key", "b-key", "e-key", "f-key"};
		const char *expect_vals[] = {"a-val", "b-val", "e-val", "f-val"};
		int n = 0;
		while (it != end) {
			auto kv = *it;
			CHECK(InternalKey::decode(kv.first)._key == expect_keys[n]);
			CHECK(kv.second == expect_vals[n]);
			++it;
			n++;
		}
		CHECK(n == 4); // 遍历完且正确终止（end 相等性）
	}
	rm_dir(dir);
}

// 19. ConcactIterator 空层：begin == end，遍历 0 条
void test_concat_iterator_empty_level() {
	const std::string dir = tmp_dir("concat_empty");
	rm_dir(dir);
	{
		LSMEngine engine(dir, 64 * 1024 * 1024, 1024 * 1024);
		// 不存在的层（5）
		auto it = engine.level_begin_without_lock(5, 100, false);
		auto end = engine.level_end(5);
		CHECK(it == end); // 空层 begin==end
		int n = 0;
		while (it != end) { ++it; n++; }
		CHECK(n == 0);
	}
	rm_dir(dir);
}

// 20. ConcactIterator 快照 + keep_all_version：版本过滤语义
void test_concat_iterator_snapshot_and_keepall() {
	const std::string dir = tmp_dir("concat_snapshot");
	rm_dir(dir);
	std::filesystem::create_directories(dir);
	{
		// 单表：key-1@10、key-2@20、key-2@15（key-2 两个版本）
		SSTBuilder builder;
		builder.add("key-1", "v1", 10, Value);
		builder.add("key-2", "v2", 20, Value);
		builder.add("key-2", "v2-old", 15, Value);
		CHECK(builder.build(1, dir + "/sst_00000000000000000000000000000001.1") != nullptr);
	}
	{
		LSMEngine engine(dir, 64 * 1024 * 1024, 1024 * 1024);

		// keep_all=false + 快照 25：key-2 只出最新版 v2
		{
			auto it = engine.level_begin_without_lock(1, 25, false);
			auto end = engine.level_end(1);
			std::vector<std::pair<std::string, std::string>> got;
			while (it != end) { auto kv = *it; got.push_back(kv); ++it; }
			CHECK(got.size() == 2);
			CHECK(InternalKey::decode(got[1].first)._key == "key-2");
			CHECK(got[1].second == "v2"); // 最新版本
		}
		// keep_all=false + 快照 18：key-2 的可见版本是 v2-old
		{
			auto it = engine.level_begin_without_lock(1, 18, false);
			auto end = engine.level_end(1);
			std::vector<std::pair<std::string, std::string>> got;
			while (it != end) { auto kv = *it; got.push_back(kv); ++it; }
			CHECK(got.size() == 2);
			CHECK(got[1].second == "v2-old"); // ≤18 的最新版本
		}
		// keep_all=true + 快照 25：key-2 全部版本都出
		{
			auto it = engine.level_begin_without_lock(1, 25, true);
			auto end = engine.level_end(1);
			std::vector<std::pair<std::string, std::string>> got;
			while (it != end) { auto kv = *it; got.push_back(kv); ++it; }
			CHECK(got.size() == 3); // key-1@10, key-2@20, key-2@15
		}
		// 快照 5：所有版本都不可见 → begin == end
		{
			auto it = engine.level_begin_without_lock(1, 5, false);
			auto end = engine.level_end(1);
			CHECK(it == end);
		}
	}
	rm_dir(dir);
}

// Leading tables may be completely invisible at a snapshot while later
// tables still contain visible keys. Check both one and several skipped tables.
void test_concat_iterator_skips_filtered_leading_tables() {
	const std::string dir = tmp_dir("concat_filtered_leading_tables");
	rm_dir(dir);
	std::filesystem::create_directories(dir);
	const char *keys[] = {"a", "b", "z"};
	const std::uint64_t sequences[] = {30, 20, 1};
	for (std::size_t i = 0; i < 3; ++i) {
		SSTBuilder builder;
		CHECK(builder.add(keys[i], keys[i], sequences[i], Value));
		const std::string id = std::to_string(i + 1);
		CHECK(builder.build(i + 1, dir + "/sst_" +
			std::string(32 - id.size(), '0') + id + ".1") != nullptr);
	}
	{
		LSMEngine engine(dir);
		CHECK(engine.get("z", 10)._value == "z");
		for (bool keep_all : {false, true}) {
			// Snapshot 25 skips a; snapshot 10 skips a and b.
			for (std::uint64_t snapshot : {std::uint64_t{25}, std::uint64_t{10}}) {
				std::vector<std::string> expected = snapshot == 25
					? std::vector<std::string>{"b", "z"}
					: std::vector<std::string>{"z"};
				auto it = engine.level_begin_without_lock(1, snapshot, keep_all);
				auto end = engine.level_end(1);
				for (const auto &key : expected) {
					CHECK(it.is_valid());
					CHECK(it != end);
					CHECK(InternalKey::decode((*it).first)._key == key);
					CHECK((*it).second == key);
					++it;
				}
				CHECK(!it.is_valid());
				CHECK(it == end);
			}
		}
	}
	rm_dir(dir);
}

// 21. ConcactIterator 与引擎 get 一致性：遍历结果 == 逐 key get
void test_concat_iterator_consistent_with_get() {
	const std::string dir = tmp_dir("concat_consistent");
	rm_dir(dir);
	std::filesystem::create_directories(dir);
	{
		SSTBuilder builder;
		builder.add("apple", "1", 10, Value);
		builder.add("banana", "2", 20, Value);
		builder.add("cherry", "3", 30, Value);
		CHECK(builder.build(1, dir + "/sst_00000000000000000000000000000001.1") != nullptr);
	}
	{
		LSMEngine engine(dir, 64 * 1024 * 1024, 1024 * 1024);
		auto it = engine.level_begin_without_lock(1, 100, false);
		auto end = engine.level_end(1);
		while (it != end) {
			auto kv = *it;
			InternalKey ik = InternalKey::decode(kv.first);
			LookUpResult res = engine.get(ik._key, 100);
			CHECK(res._value_type == Value);
			CHECK(res._value == kv.second);
			++it;
		}
	}
	rm_dir(dir);
}

} // namespace

// 21. seq 水位跨重启不回退：同目录 reopen 后取号继续递增（读己之写不因重启断裂）
void test_seq_watermark_no_regression_on_reopen() {
	const std::string dir = tmp_dir("seq_watermark");
	rm_dir(dir);
	std::uint64_t max_seq = 0;
	{
		LSMEngine engine(dir, 64 * 1024 * 1024, 1024 * 1024);
		for (int i = 0; i < 20; i++) {
			auto ret = engine.put("k" + std::to_string(i), "v");
			CHECK(ret.has_value());
			max_seq = std::max(max_seq, *ret);
		}
		CHECK(max_seq >= 20); // 至少取了 20 个号
		engine.flush();       // flush 义务写路径
		// close 经析构自动触发（close 为 private,作用域结束即优雅关闭+水位落盘）
	}
	{
		LSMEngine engine(dir, 64 * 1024 * 1024, 1024 * 1024);
		// 数据经重启存活
		auto res = engine.get("k5", max_seq);
		CHECK(res.is_found() && res._value == "v");
		// 新取号严格大于上会话最大 → 水位恢复正确,不复用旧 seq
		auto ret = engine.put("fresh", "v");
		CHECK(ret.has_value());
		CHECK(*ret > max_seq);
		// 读己之写：返回的 seq 作快照点能看到自己的写
		auto res2 = engine.get("fresh", *ret);
		CHECK(res2.is_found() && res2._value == "v");
	}
	rm_dir(dir);
}

int main() {
	struct TestCase { const char *name; void (*run)(); };
	const TestCase cases[] = {
		{"test_constructor_creates_dir_and_empty_flush", test_constructor_creates_dir_and_empty_flush},
		{"test_put_triggers_flush_and_creates_file", test_put_triggers_flush_and_creates_file},
		{"test_flush_content_roundtrip", test_flush_content_roundtrip},
		{"test_multiple_flushes_increasing_ids", test_multiple_flushes_increasing_ids},
		{"test_remove_flushes_tombstone", test_remove_flushes_tombstone},
		{"test_put_return_value_semantics", test_put_return_value_semantics},
		{"test_get_after_flush", test_get_after_flush},
		{"test_memtable_tombstone_shortcircuit", test_memtable_tombstone_shortcircuit},
		{"test_sst_tombstone_shortcircuit", test_sst_tombstone_shortcircuit},
		{"test_multi_version_seq_filter", test_multi_version_seq_filter},
		{"test_restart_recovery", test_restart_recovery},
		{"test_level1_read_path", test_level1_read_path},
		{"test_multi_block_read", test_multi_block_read},
		{"test_get_batch_all_hit", test_get_batch_all_hit},
		{"test_get_batch_mixed", test_get_batch_mixed},
		{"test_get_batch_tombstone_shortcircuit", test_get_batch_tombstone_shortcircuit},
		{"test_get_batch_consistent_with_get", test_get_batch_consistent_with_get},
		{"test_concat_iterator_traversal", test_concat_iterator_traversal},
		{"test_concat_iterator_empty_level", test_concat_iterator_empty_level},
		{"test_concat_iterator_snapshot_and_keepall", test_concat_iterator_snapshot_and_keepall},
		{"test_concat_iterator_skips_filtered_leading_tables", test_concat_iterator_skips_filtered_leading_tables},
		{"test_concat_iterator_consistent_with_get", test_concat_iterator_consistent_with_get},
		{"test_seq_watermark_no_regression_on_reopen", test_seq_watermark_no_regression_on_reopen},
	};
	int passed = 0;
	const int total = sizeof(cases) / sizeof(cases[0]);
	// Keep running independent cases after a failure so a regression cannot
	// hide the remainder of the engine coverage.
	for (const auto &test : cases) {
		try {
			test.run();
			std::cout << "[PASS] " << test.name << "\n";
			++passed;
		} catch (const std::exception &e) {
			std::cerr << "[FAIL] " << test.name << ": " << e.what() << "\n";
		}
	}
	std::cout << passed << "/" << total << " tests passed\n";
	return passed == total ? 0 : 1;
}
