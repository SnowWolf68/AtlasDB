#include "Block.h"
#include "BlockMeta.h"
#include "Record.h"
#include "sst.h"
#include "binary_test_helpers.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <optional>
#include <exception>
#include <iostream>
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

// ─── helper functions ──────────────────────────────────────────

std::string enc(const std::string &user_key, std::uint64_t seq) {
	return InternalKey::encode(InternalKey(user_key, seq, Value));
}

std::string tmp_path(const std::string &name) {
	return "/tmp/atlasdb_test_sst_" + name;
}

void rm_tmp(const std::string &path) { std::remove(path.c_str()); }

std::shared_ptr<SST> build_sst(const std::string &path,
							   const std::vector<std::string> &keys,
							   const std::vector<std::string> &values,
							   std::uint64_t seq, std::uint64_t sst_id = 1) {
	SSTBuilder builder;
	for (size_t i = 0; i < keys.size(); i++) {
		CHECK(builder.add(keys[i], values[i], seq, Value));
	}
	auto sst = builder.build(sst_id, path);
	CHECK(sst != nullptr);
	return sst;
}

// ─── build / open round trip ───────────────────────────────────

void test_build_open_round_trip() {
	std::string path = tmp_path("round_trip");
	std::vector<std::string> keys = {"alpha", "bravo", "charlie"};
	std::vector<std::string> values = {"1", "2", "3"};

	auto built = build_sst(path, keys, values, 10, 42);

	auto opened = SST::open(path, 42);
	CHECK(opened != nullptr);
	CHECK(opened->get_sst_id() == 42);
	CHECK(opened->num_blocks() == built->num_blocks());
	CHECK(InternalKey::decode(opened->get_first_key())._key == "alpha");
	CHECK(InternalKey::decode(opened->get_last_key())._key == "charlie");
	rm_tmp(path);
}

void test_build_open_persisted_data() {
	std::string path = tmp_path("persist");
	{
		auto built = build_sst(path, {"a", "b"}, {"x", "y"}, 5);
		CHECK(built != nullptr);
	} // built 析构，文件仍在

	// 重新 open，数据必须完整
	auto opened = SST::open(path, 1);
	CHECK(opened != nullptr);
	auto it = opened->begin(std::nullopt, false);
	CHECK(it.is_valid());
	auto kv = *it;
	CHECK(InternalKey::decode(kv.first)._key == "a");
	CHECK(kv.second == "x");
	++it;
	kv = *it;
	CHECK(InternalKey::decode(kv.first)._key == "b");
	CHECK(kv.second == "y");
	rm_tmp(path);
}

// ─── get ────────────────────────────────────────────────────────

void test_get_existing_key() {
	std::string path = tmp_path("get_hit");
	auto sst = build_sst(path, {"apple", "banana", "cherry"},
						 {"1", "2", "3"}, 10);

	auto it_opt = sst->get(enc("banana", 10));
	CHECK(it_opt != std::nullopt);
	auto kv = *it_opt.value();
	CHECK(InternalKey::decode(kv.first)._key == "banana");
	CHECK(kv.second == "2");
	rm_tmp(path);
}

void test_get_missing_key() {
	std::string path = tmp_path("get_miss");
	auto sst = build_sst(path, {"apple", "cherry"}, {"1", "2"}, 10);

	// key 在范围内但不存在（apple < banana < cherry）
	auto it_opt = sst->get(enc("banana", 10));
	CHECK(it_opt == std::nullopt);
	rm_tmp(path);
}

void test_get_out_of_range() {
	std::string path = tmp_path("get_range");
	auto sst = build_sst(path, {"apple", "cherry"}, {"1", "2"}, 10);

	CHECK(sst->get(enc("a", 10)) == std::nullopt); // 小于 first_key
	CHECK(sst->get(enc("z", 10)) == std::nullopt); // 大于 last_key
	rm_tmp(path);
}

void test_get_multi_version() {
	std::string path = tmp_path("get_multi");
	SSTBuilder builder;
	// 同 user_key 不同 seq，internal key 顺序：seq 大的在前
	// 必须按 internal key 有序插入（builder 的假设）
	CHECK(builder.add("foo", "new", 100, Value));
	CHECK(builder.add("foo", "old", 50, Value));
	auto sst = builder.build(1, path);
	CHECK(sst != nullptr);

	// 查最新版本
	auto it_opt = sst->get(enc("foo", 100));
	CHECK(it_opt != std::nullopt);
	auto kv = *it_opt.value();
	CHECK(kv.second == "new");
	CHECK(InternalKey::decode(kv.first)._commit_sequence == 100);

	// 查旧版本
	it_opt = sst->get(enc("foo", 50));
	CHECK(it_opt != std::nullopt);
	kv = *it_opt.value();
	CHECK(kv.second == "old");
	rm_tmp(path);
}

// ─── iterator ───────────────────────────────────────────────────

void test_iterate_single_block() {
	std::string path = tmp_path("iter_single");
	auto sst = build_sst(path, {"a", "b", "c"}, {"1", "2", "3"}, 10);

	int count = 0;
	for (auto it = sst->begin(std::nullopt, false); it != sst->end(); ++it) {
		auto kv = *it;
		CHECK(kv.second == std::to_string(++count));
	}
	CHECK(count == 3);
	CHECK(sst->begin(std::nullopt, false) == sst->begin(std::nullopt, false));
	CHECK(sst->begin(std::nullopt, false) != sst->end());
	rm_tmp(path);
}

void test_iterate_multi_block() {
	std::string path = tmp_path("iter_multi");
	SSTBuilder builder;
	// 每条 ~1KB，共 50 条 ≈ 50KB > 32KB（DEFAULT_MAX_CAPACITY），必然跨 block
	const int N = 50;
	std::string big_value(1024, 'x');
	for (int i = 0; i < N; i++) {
		CHECK(builder.add("key" + std::to_string(i), big_value, 10, Value));
	}
	auto sst = builder.build(1, path);
	CHECK(sst != nullptr);
	CHECK(sst->num_blocks() >= 2); // 确实跨了 block

	int count = 0;
	for (auto it = sst->begin(std::nullopt, false); it != sst->end(); ++it) {
		auto kv = *it;
		CHECK(kv.second == big_value);
		count++;
	}
	CHECK(count == N);
	rm_tmp(path);
}

// ─── corrupt files ──────────────────────────────────────────────

void test_open_corrupt_magic() {
	std::string path = tmp_path("bad_magic");
	std::vector<uint8_t> data = {'h', 'e', 'l', 'l', 'o'};
	FileObj::create_and_write(path, data);

	auto sst = SST::open(path, 1);
	CHECK(sst == nullptr); // magic 不匹配
	rm_tmp(path);
}

void test_open_truncated_file() {
	std::string path = tmp_path("truncated");
	std::vector<uint8_t> data = {'a', 'b'};
	FileObj::create_and_write(path, data);

	auto sst = SST::open(path, 1);
	CHECK(sst == nullptr); // 文件太短（< 5 字节）
	rm_tmp(path);
}

class ScopedSstFixture {
public:
	ScopedSstFixture() {
		const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
		for (unsigned attempt = 0; attempt < 1000; ++attempt) {
			auto candidate = std::filesystem::temp_directory_path() /
				("atlasdb-sst-boundaries-" + std::to_string(stamp) + "-" + std::to_string(attempt));
			if (std::filesystem::create_directory(candidate)) {
				directory = candidate;
				path = (directory / "fixture.sst").string();
				return;
			}
		}
		throw std::runtime_error("cannot create SST fixture directory");
	}
	~ScopedSstFixture() {
		std::error_code error;
		std::filesystem::remove_all(directory, error);
	}
	ScopedSstFixture(const ScopedSstFixture &) = delete;
	ScopedSstFixture &operator=(const ScopedSstFixture &) = delete;

	void write(const std::vector<std::uint8_t> &bytes) {
		FileObj::create_and_write(path, bytes);
	}
	std::string path;

private:
	std::filesystem::path directory;
};

std::vector<std::uint8_t> boundary_block_bytes() {
	Block block;
	CHECK(block.add_entry(enc("key", 10), "value", false));
	return block.encode(true);
}

std::vector<std::uint8_t> sst_fixture_bytes(
	const std::vector<std::uint8_t> &data, const std::vector<BlockMeta> &metas) {
	auto bytes = data;
	const auto metadata = BlockMeta::encode(metas);
	bytes.insert(bytes.end(), metadata.begin(), metadata.end());
	binary_test::append_u32(bytes, static_cast<std::uint32_t>(data.size()));
	bytes.push_back(SSTBuilder::MAGIC_NUM);
	return bytes;
}

void test_open_rejects_empty_metadata() {
	const auto empty_metadata = BlockMeta::decode(BlockMeta::encode({}));
	CHECK(empty_metadata.has_value() && empty_metadata->empty());
	for (const auto &data : std::vector<std::vector<std::uint8_t>>{{}, boundary_block_bytes()}) {
		ScopedSstFixture fixture;
		// A valid checksum and footer must not hide the empty-metadata case.
		fixture.write(sst_fixture_bytes(data, {}));
		CHECK(SST::open(fixture.path, 1) == nullptr);
	}
}

void test_open_rejects_metadata_offsets_past_footer() {
	const auto data = boundary_block_bytes();
	const auto key = enc("key", 10);
	const auto valid = sst_fixture_bytes(data, {BlockMeta(0, key, key)});
	const auto footer_start = valid.size() - 5;
	for (std::uint32_t offset : {static_cast<std::uint32_t>(footer_start + 1),
		static_cast<std::uint32_t>(valid.size()), 0xfffffffbu, 0xfffffffcu, 0xffffffffu}) {
		ScopedSstFixture fixture;
		auto malformed = valid;
		binary_test::overwrite_u32(malformed, footer_start, offset);
		fixture.write(malformed);
		CHECK(SST::open(fixture.path, 1) == nullptr);
	}
	ScopedSstFixture control;
	control.write(valid);
	auto sst = SST::open(control.path, 1);
	CHECK(sst != nullptr);
	auto block = sst->read_block(0);
	CHECK(block != nullptr);
	CHECK(*block->begin() == std::make_pair(key, std::string("value")));
}

void test_read_block_rejects_reversed_offsets() {
	const auto data = boundary_block_bytes();
	const auto end = static_cast<std::uint32_t>(data.size());
	const auto key = enc("key", 10);
	for (bool last_block : {false, true}) {
		const std::vector<BlockMeta> metas = last_block
			? std::vector<BlockMeta>{BlockMeta(0, key, key), BlockMeta(end + 1, key, key)}
			: std::vector<BlockMeta>{BlockMeta(end - 1, key, key), BlockMeta(0, key, key)};
		ScopedSstFixture fixture;
		fixture.write(sst_fixture_bytes(data, metas));
		auto sst = SST::open(fixture.path, 1);
		CHECK(sst != nullptr);
		CHECK(sst->read_block(last_block ? 1 : 0) == nullptr);
	}
}

void test_read_block_rejects_offsets_outside_file() {
	const auto data = boundary_block_bytes();
	const auto key = enc("key", 10);
	const auto normal_size = static_cast<std::uint32_t>(sst_fixture_bytes(data,
		{BlockMeta(0, key, key), BlockMeta(0, key, key)}).size());
	for (bool start_outside : {false, true}) {
		ScopedSstFixture fixture;
		const auto first = start_outside ? normal_size + 1 : 0;
		fixture.write(sst_fixture_bytes(data,
			{BlockMeta(first, key, key), BlockMeta(normal_size + 2, key, key)}));
		auto sst = SST::open(fixture.path, 1);
		CHECK(sst != nullptr);
		CHECK(sst->read_block(0) == nullptr);
	}
}

void test_read_block_rejects_metadata_region() {
	const auto data = boundary_block_bytes();
	const auto key = enc("key", 10);
	const auto block_end = static_cast<std::uint32_t>(data.size());
	ScopedSstFixture fixture;
	fixture.write(sst_fixture_bytes(data, {BlockMeta(0, key, key)}));
	const std::vector<BlockMeta> metas = {BlockMeta(0, key, key), BlockMeta(block_end, key, key)};
	// Seed the public constructor so the referenced bytes form a CRC-valid
	// block, but its end crosses the declared metadata boundary by one byte.
	// Without that boundary check, CRC validation alone accepts this block.
	SST malformed(FileObj::open_read(fixture.path), metas, block_end - 1, 1, key, key);
	CHECK(malformed.read_block(0) == nullptr);
	SST control(FileObj::open_read(fixture.path), metas, block_end, 1, key, key);
	auto block = control.read_block(0);
	CHECK(block != nullptr);
	CHECK(*block->begin() == std::make_pair(key, std::string("value")));
}

void test_empty_builder_does_not_publish_sst() {
	ScopedSstFixture fixture;
	SSTBuilder builder;
	CHECK(builder.build(1, fixture.path) == nullptr);
	CHECK(!std::filesystem::exists(fixture.path));
}

// ─── tombstone ──────────────────────────────────────────────────

void test_tombstone_entry_readable() {
	std::string path = tmp_path("tombstone");
	SSTBuilder builder;
	CHECK(builder.add("dead", "", 10, TombStone));
	auto sst = builder.build(1, path);
	CHECK(sst != nullptr);

	// SST::get 的范围检查要求查询 key 落在 [_first_key, _last_key] 内，
	// 因此查询 key 必须与条目完全一致（同 seq 同 type）
	auto it_opt =
		sst->get(InternalKey::encode(InternalKey("dead", 10, TombStone)));
	CHECK(it_opt != std::nullopt);
	auto kv = *it_opt.value();
	CHECK(InternalKey::decode(kv.first)._key == "dead");
	CHECK(InternalKey::decode(kv.first)._value_type == TombStone);
	rm_tmp(path);
}


// ─── range query (Lab 3.7) ──────────────────────────────────────

// 单调谓词: 匹配 [lo, hi] 闭区间, 返回 -1 / 0 / +1
std::function<int(const std::string &)> pred_range(const std::string &lo,
												   const std::string &hi) {
	return [lo, hi](const std::string &k) -> int {
		if (k < lo) { return -1; }
		if (k > hi) { return 1; }
		return 0;
	};
}

// 收集区间内所有 user_key(range 为空时返回空)
std::vector<std::string> collect_keys(
	const std::optional<std::pair<SSTIterator, SSTIterator>> &range) {
	std::vector<std::string> keys;
	if (!range.has_value()) { return keys; }
	for (auto it = range->first; it != range->second; ++it) {
		keys.push_back(InternalKey::decode((*it).first)._key);
	}
	return keys;
}

// 用 1KB value 构造 n 条 key(k00..k(n-1))的 SST, 必然跨 block
std::shared_ptr<SST> build_multi_block_sst(const std::string &path, int n = 50) {
	SSTBuilder builder;
	std::string big_value(1024, 'x');
	for (int i = 0; i < n; i++) {
		std::string key = "k" + std::to_string(i / 10) + std::to_string(i % 10);
		CHECK(builder.add(key, big_value, 10, Value));
	}
	auto sst = builder.build(1, path);
	CHECK(sst != nullptr);
	CHECK(sst->num_blocks() >= 2);
	return sst;
}

void test_predicate_single_block() {
	std::string path = tmp_path("pred_single");
	auto sst = build_sst(path, {"a", "b", "c", "d", "e", "f", "g"},
						 {"1", "2", "3", "4", "5", "6", "7"}, 10);

	auto range = sst->iter_monotony_predicate(pred_range("c", "e"), std::nullopt);
	std::vector<std::string> keys = collect_keys(range);
	CHECK(keys.size() == 3);
	CHECK(keys[0] == "c");
	CHECK(keys[1] == "d");
	CHECK(keys[2] == "e");
	rm_tmp(path);
}

void test_predicate_no_match_gap() {
	std::string path = tmp_path("pred_gap");
	auto sst = build_sst(path, {"a", "c", "e", "g"}, {"1", "2", "3", "4"}, 10);

	auto range = sst->iter_monotony_predicate(pred_range("b", "b"), std::nullopt);
	CHECK(range == std::nullopt);
	rm_tmp(path);
}

void test_predicate_zone_before_sst() {
	std::string path = tmp_path("pred_before");
	auto sst = build_sst(path, {"a", "c", "e", "g"}, {"1", "2", "3", "4"}, 10);

	auto range =
		sst->iter_monotony_predicate(pred_range("00", "00"), std::nullopt);
	CHECK(range == std::nullopt);
	rm_tmp(path);
}

void test_predicate_multi_block_middle() {
	std::string path = tmp_path("pred_middle");
	auto sst = build_multi_block_sst(path);

	auto range = sst->iter_monotony_predicate(pred_range("k04", "k14"), std::nullopt);
	std::vector<std::string> expect;
	for (int i = 4; i <= 14; i++) {
		expect.push_back("k" + std::to_string(i / 10) + std::to_string(i % 10));
	}
	CHECK(collect_keys(range) == expect);
	rm_tmp(path);
}

void test_predicate_multi_block_tail() {
	std::string path = tmp_path("pred_tail");
	auto sst = build_multi_block_sst(path);

	auto range =
		sst->iter_monotony_predicate(pred_range("k40", "zzz"), std::nullopt);
	std::vector<std::string> expect;
	for (int i = 40; i <= 49; i++) {
		expect.push_back("k" + std::to_string(i / 10) + std::to_string(i % 10));
	}
	CHECK(collect_keys(range) == expect);
	rm_tmp(path);
}

void test_predicate_multi_block_front_aligned() {
	std::string path = tmp_path("pred_front");
	auto sst = build_multi_block_sst(path);

	auto range =
		sst->iter_monotony_predicate(pred_range("k31", "k40"), std::nullopt);
	std::vector<std::string> expect;
	for (int i = 31; i <= 40; i++) {
		expect.push_back("k" + std::to_string(i / 10) + std::to_string(i % 10));
	}
	CHECK(collect_keys(range) == expect);
	rm_tmp(path);
}

// 块0: k00..k30 @10; 块1: k50@90 / k51@90(全部太新)
void test_predicate_seq_filter_tail_block() {
	std::string path = tmp_path("pred_seq_tail");
	SSTBuilder builder;
	for (int i = 0; i < 31; i++) {
		std::string key = "k" + std::to_string(i / 10) + std::to_string(i % 10);
		CHECK(builder.add(key, std::string(1024, 'x'), 10, Value));
	}
	CHECK(builder.add("k50", std::string(900, 'y'), 90, Value));
	CHECK(builder.add("k51", "new", 90, Value));
	auto sst = builder.build(1, path);
	CHECK(sst != nullptr);
	CHECK(sst->num_blocks() == 2);

	auto range = sst->iter_monotony_predicate(pred_range("k00", "k45"),
											  std::optional<std::uint64_t>(10));
	std::vector<std::string> expect;
	for (int i = 0; i <= 30; i++) {
		expect.push_back("k" + std::to_string(i / 10) + std::to_string(i % 10));
	}
	CHECK(collect_keys(range) == expect);
	rm_tmp(path);
}

// 块0 尾部的 k30@90 被过滤, 可见版本在块1
void test_predicate_seq_visible_in_later_block() {
	std::string path = tmp_path("pred_seq_later");
	SSTBuilder builder;
	for (int i = 0; i < 30; i++) {
		std::string key = "k" + std::to_string(i / 10) + std::to_string(i % 10);
		CHECK(builder.add(key, std::string(1024, 'x'), 10, Value));
	}
	CHECK(builder.add("k30", std::string(1024, 'x'), 90, Value));
	CHECK(builder.add("k31", std::string(1024, 'x'), 10, Value));
	CHECK(builder.add("k32", "small", 10, Value));
	auto sst = builder.build(1, path);
	CHECK(sst != nullptr);
	CHECK(sst->num_blocks() == 2);

	auto range = sst->iter_monotony_predicate(pred_range("k30", "k32"),
											  std::optional<std::uint64_t>(10));
	std::vector<std::string> keys = collect_keys(range);
	CHECK(keys.size() == 2);
	CHECK(keys[0] == "k31");
	CHECK(keys[1] == "k32");
	rm_tmp(path);
}

// 同 key 多版本在同一块: 只返回 <= 上界的版本
void test_predicate_seq_same_key_versions() {
	std::string path = tmp_path("pred_seq_versions");
	SSTBuilder builder;
	CHECK(builder.add("k5", "new", 90, Value));
	CHECK(builder.add("k5", "old", 10, Value));
	CHECK(builder.add("k6", "v", 10, Value));
	auto sst = builder.build(1, path);
	CHECK(sst != nullptr);

	auto range = sst->iter_monotony_predicate(pred_range("k5", "k5"),
											  std::optional<std::uint64_t>(10));
	CHECK(range.has_value());
	int count = 0;
	for (auto it = range->first; it != range->second; ++it) {
		CHECK(InternalKey::decode((*it).first)._key == "k5");
		CHECK((*it).second == "old");
		count++;
	}
	CHECK(count == 1);
	rm_tmp(path);
}

// 换块后新块全被 commit_sequence 过滤: 两种遍历风格都应正常终止
void test_iterate_skip_filtered_block() {
	std::string path = tmp_path("iter_skip_filtered");
	SSTBuilder builder;
	for (int i = 0; i < 31; i++) {
		std::string key = "k" + std::to_string(i / 10) + std::to_string(i % 10);
		CHECK(builder.add(key, std::string(1024, 'x'), 10, Value));
	}
	CHECK(builder.add("k50", std::string(900, 'y'), 90, Value));
	CHECK(builder.add("k51", "new", 90, Value));
	auto sst = builder.build(1, path);
	CHECK(sst != nullptr);
	CHECK(sst->num_blocks() == 2);

	int count = 0;
	auto it = sst->begin(10, false);
	while (it.is_valid()) {
		count++;
		++it;
	}
	CHECK(count == 31);

	count = 0;
	for (auto it2 = sst->begin(10, false); it2 != sst->end(); ++it2) { count++; }
	CHECK(count == 31);
	rm_tmp(path);
}

void test_begin_skips_filtered_leading_blocks() {
	const std::string path = tmp_path("begin_filtered_leading_blocks");
	SSTBuilder builder;
	const char *keys[] = {"a", "b", "z"};
	const std::uint64_t sequences[] = {30, 20, 1};
	for (std::size_t i = 0; i < 3; ++i) {
		CHECK(builder.add(keys[i], std::string(20000, keys[i][0]), sequences[i], Value));
	}
	auto sst = builder.build(1, path);
	CHECK(sst != nullptr);
	CHECK(sst->num_blocks() == 3);
	CHECK(sst->get(enc("z", 10)).has_value());
	for (bool keep_all : {false, true}) {
		for (std::uint64_t snapshot : {std::uint64_t{25}, std::uint64_t{10}, std::uint64_t{0}}) {
			const std::vector<std::string> expected = snapshot == 25
				? std::vector<std::string>{"b", "z"}
				: snapshot == 10 ? std::vector<std::string>{"z"}
								 : std::vector<std::string>{};
			auto it = sst->begin(snapshot, keep_all);
			std::size_t index = 0;
			while (it.is_valid()) {
				CHECK(index < expected.size());
				CHECK(InternalKey::decode((*it).first)._key == expected[index]);
				CHECK((*it).second == std::string(20000, expected[index][0]));
				++index;
				++it;
			}
			CHECK(index == expected.size());
			CHECK(it == sst->end());
			++it;
			CHECK(it == sst->end());
			index = 0;
			for (auto other = sst->begin(snapshot, keep_all); other != sst->end(); ++other) {
				CHECK(other.is_valid());
				CHECK(index < expected.size());
				CHECK(InternalKey::decode((*other).first)._key == expected[index]);
				++index;
			}
			CHECK(index == expected.size());
		}
	}
	rm_tmp(path);
}

// end() 哨兵上 ++ 应保持稳定(不漂移)
void test_end_sentinel_stable() {
	std::string path = tmp_path("end_stable");
	auto sst = build_sst(path, {"a", "b", "c"}, {"1", "2", "3"}, 10);

	auto e = sst->end();
	++e;
	CHECK(e == sst->end());
	++e;
	CHECK(e == sst->end());
	CHECK(!e.is_valid());
	rm_tmp(path);
}

// ─── test runner ────────────────────────────────────────────────

struct TestCase {
	const char *name;
	void (*run)();
};

int run_tests(const std::vector<TestCase> &tests) {
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

	std::cout << passed << '/' << tests.size() << " tests passed\n";
	return passed == tests.size() ? 0 : 1;
}

} // namespace

int main() {
	std::vector<TestCase> tests = {
		{"build open round trip", test_build_open_round_trip},
		{"build open persisted data", test_build_open_persisted_data},
		{"get existing key", test_get_existing_key},
		{"get missing key", test_get_missing_key},
		{"get out of range", test_get_out_of_range},
		{"get multi version", test_get_multi_version},
		{"iterate single block", test_iterate_single_block},
		{"iterate multi block", test_iterate_multi_block},
		{"open corrupt magic", test_open_corrupt_magic},
		{"open truncated file", test_open_truncated_file},
		{"open rejects empty metadata", test_open_rejects_empty_metadata},
		{"open rejects metadata offsets past footer", test_open_rejects_metadata_offsets_past_footer},
		{"read block rejects reversed offsets", test_read_block_rejects_reversed_offsets},
		{"read block rejects offsets outside file", test_read_block_rejects_offsets_outside_file},
		{"read block rejects metadata region", test_read_block_rejects_metadata_region},
		{"empty builder does not publish sst", test_empty_builder_does_not_publish_sst},
		{"tombstone entry readable", test_tombstone_entry_readable},
		{"predicate single block", test_predicate_single_block},
		{"predicate no match gap", test_predicate_no_match_gap},
		{"predicate zone before sst", test_predicate_zone_before_sst},
		{"predicate multi block middle", test_predicate_multi_block_middle},
		{"predicate multi block tail", test_predicate_multi_block_tail},
		{"predicate multi block front aligned",
		 test_predicate_multi_block_front_aligned},
		{"predicate seq filter tail block", test_predicate_seq_filter_tail_block},
		{"predicate seq visible in later block",
		 test_predicate_seq_visible_in_later_block},
		{"predicate seq same key versions",
		 test_predicate_seq_same_key_versions},
		{"iterate skip filtered block", test_iterate_skip_filtered_block},
		{"begin skips filtered leading blocks", test_begin_skips_filtered_leading_blocks},
		{"end sentinel stable", test_end_sentinel_stable},
	};
	return run_tests(tests);
}
