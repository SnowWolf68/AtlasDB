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

// begin 到 end 全量收集（用 != 判等驱动, 验证扫描循环）
std::vector<Entry> collect(LSMEngineIterator it, const LSMEngineIterator &end) {
	std::vector<Entry> entries;
	while (it != end) {
		InternalKey ik = InternalKey::decode((*it).first);
		entries.push_back(
			{ik._key, (*it).second, ik._commit_sequence, ik._value_type});
		++it;
	}
	return entries;
}

// begin() 用 shared_from_this, 引擎必须堆分配
std::shared_ptr<LSMEngine> make_engine(const std::string &dir) {
	std::filesystem::remove_all(dir);
	return std::make_shared<LSMEngine>(dir, 64 * 1024 * 1024,
									   4 * 1024 * 1024);
}

// ─── 测试用例 ───────────────────────────────────────────────────

// 1. 基本有序遍历（仅 memtable, 不 flush）：按 user_key 升序输出
void test_basic_ordered() {
	auto engine = make_engine("/tmp/scan_test_basic");
	CHECK(engine->put("b", "vb").has_value());
	CHECK(engine->put("a", "va").has_value());
	CHECK(engine->put("c", "vc").has_value());

	auto entries = collect(engine->begin(std::nullopt, false), engine->end());
	CHECK(entries.size() == 3);
	CHECK(entries[0].key == "a" && entries[0].value == "va");
	CHECK(entries[1].key == "b" && entries[1].value == "vb");
	CHECK(entries[2].key == "c" && entries[2].value == "vc");
}

// 2. 跨源去重：同 key 在 memtable（新）和 L0（旧）, 只输出最新版本
void test_cross_source_dedup() {
	auto engine = make_engine("/tmp/scan_test_dedup");
	CHECK(engine->put("k", "v1").has_value());
	CHECK(engine->put("a", "va").has_value());
	engine->flush(); // k@1, a@2 → L0
	CHECK(engine->put("k", "v2").has_value()); // k@3(新) → memtable

	auto entries = collect(engine->begin(std::nullopt, false), engine->end());
	CHECK(entries.size() == 2);
	CHECK(entries[0].key == "a");
	CHECK(entries[1].key == "k" && entries[1].value == "v2" &&
		  entries[1].seq == 3); // 只出最新的 v2
}

// 3. 跨源墓碑抑制：墓碑在 memtable、活值在 L0, k 完全消失
void test_tombstone_suppressed() {
	auto engine = make_engine("/tmp/scan_test_tombstone");
	CHECK(engine->put("k", "v1").has_value());
	CHECK(engine->put("a", "va").has_value());
	engine->flush(); // k@1, a@2 → L0
	CHECK(engine->remove("k").has_value()); // k@3 墓碑 → memtable

	auto entries = collect(engine->begin(std::nullopt, false), engine->end());
	CHECK(entries.size() == 1); // 只有 a, k 整 key 消失
	CHECK(entries[0].key == "a");
}

// 4. keep_all=true：同 key 多版本全输出（seq 降序）
void test_keep_all_versions() {
	auto engine = make_engine("/tmp/scan_test_keepall");
	CHECK(engine->put("k", "v1").has_value());
	CHECK(engine->put("k", "v2").has_value());

	auto entries =
		collect(engine->begin(std::nullopt, true, false), engine->end());
	CHECK(entries.size() == 2);
	CHECK(entries[0].key == "k" && entries[0].seq == 2 &&
		  entries[0].value == "v2");
	CHECK(entries[1].key == "k" && entries[1].seq == 1 &&
		  entries[1].value == "v1");
}

// 5. 快照：begin(seq) 只看到 seq ≤ 快照点的版本
void test_snapshot() {
	auto engine = make_engine("/tmp/scan_test_snapshot");
	auto old_seq = engine->put("k", "v1");
	auto new_seq = engine->put("k", "v2");
	CHECK(old_seq && new_seq && *new_seq > *old_seq);

	// 以旧版本的实际返回序号建立快照：旧值可见，新值不可见。
	auto entries =
		collect(engine->begin(old_seq, false),
				engine->end());
	CHECK(entries.size() == 1);
	CHECK(entries[0].key == "k" && entries[0].value == "v1" &&
		  entries[0].seq == *old_seq);
}

// 6. 空库：begin == end, 遍历零条
void test_empty_engine() {
	auto engine = make_engine("/tmp/scan_test_empty");
	CHECK(engine->begin(std::nullopt, false) == engine->end());
}

// 7. 遍历完后 it == end（扫描循环退出条件）
void test_end_after_exhaustion() {
	auto engine = make_engine("/tmp/scan_test_exhausted");
	CHECK(engine->put("a", "va").has_value());
	CHECK(engine->put("b", "vb").has_value());

	auto it = engine->begin(std::nullopt, false);
	auto end = engine->end();
	std::size_t count = 0;
	while (it != end) {
		count++;
		++it;
	}
	CHECK(count == 2);
	CHECK(it == end); // 耗尽后与 end 判等
}

// 8. 与 get() 对账：扫描结果 = 逐 key get(最新快照) 按序排
void test_consistent_with_get() {
	auto engine = make_engine("/tmp/scan_test_consistency");
	CHECK(engine->put("b", "vb").has_value());
	CHECK(engine->put("a", "va").has_value());
	engine->flush(); // a@2, b@1 → L0
	CHECK(engine->put("b", "vb2").has_value()); // b@3(新) → memtable

	auto entries = collect(engine->begin(std::nullopt, false), engine->end());
	CHECK(entries.size() == 2);
	for (const Entry &e : entries) {
		auto res = engine->get(e.key, 999999); // 最新快照
		CHECK(res.is_found());
		CHECK(res._value == e.value);
	}
	CHECK(entries[0].key == "a" && entries[0].value == "va");
	CHECK(entries[1].key == "b" && entries[1].value == "vb2");
}

// ─── 测试注册 ───────────────────────────────────────────────────

struct TestCase {
	const char *name;
	void (*run)();
};

} // namespace

int main() {
	const TestCase tests[] = {
		{"basic ordered scan", test_basic_ordered},
		{"cross source dedup", test_cross_source_dedup},
		{"tombstone suppressed", test_tombstone_suppressed},
		{"keep all versions", test_keep_all_versions},
		{"snapshot filter", test_snapshot},
		{"empty engine", test_empty_engine},
		{"end after exhaustion", test_end_after_exhaustion},
		{"consistent with get", test_consistent_with_get},
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
