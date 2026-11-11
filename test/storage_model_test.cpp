#include "lsm.h"
#include "Record.h"
#include "TranContext.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
#define CHECK(expr) do { if (!(expr)) { throw std::runtime_error( \
	"line " + std::to_string(__LINE__) + ": " #expr); } } while (false)

struct TempDir {
	std::string path;
	TempDir() {
		char pattern[] = "/tmp/atlas-storage-model-XXXXXX";
		const char *created = ::mkdtemp(pattern);
		CHECK(created != nullptr);
		path = created;
	}
	~TempDir() {
		std::error_code error;
		std::filesystem::remove_all(path, error);
	}
};

struct Version {
	std::uint64_t seq;
	std::optional<std::string> value;
};
// Independent history oracle: no production comparator, merging, or lookup code.
using Model = std::map<std::string, std::vector<Version>>;
using Row = std::pair<std::string, std::string>;
constexpr auto latest = std::numeric_limits<std::uint64_t>::max();

const Version *visible(const Model &model, const std::string &key, std::uint64_t snapshot) {
	const auto found = model.find(key);
	if (found == model.end()) { return nullptr; }
	for (auto version = found->second.rbegin(); version != found->second.rend(); ++version) {
		if (version->seq <= snapshot) { return &*version; }
	}
	return nullptr;
}

void check_lookup(const LookUpResult &actual, const Version *expected) {
	if (!expected) { CHECK(actual._value_type == NotFound); return; }
	CHECK(actual._commit_sequence == expected->seq);
	CHECK(actual._value_type == (expected->value ? Value : TombStone));
	if (expected->value) { CHECK(actual._value == *expected->value); }
}

std::vector<Row> expected_rows(const Model &model, std::uint64_t snapshot,
	const std::optional<std::pair<std::string, std::string>> &range = std::nullopt) {
	std::vector<Row> rows;
	for (const auto &[key, history] : model) {
		(void)history;
		if (range && (key < range->first || key >= range->second)) { continue; }
		const auto *version = visible(model, key, snapshot);
		if (version && version->value) { rows.emplace_back(key, *version->value); }
	}
	return rows;
}

template<class Iterator>
std::vector<Row> collect(Iterator iterator, const Model &model, std::uint64_t snapshot) {
	std::vector<Row> rows;
	while (iterator.is_valid()) {
		CHECK(rows.size() <= model.size()); // also bounds a broken iterator that repeats
		const auto entry = *iterator;
		const auto key = InternalKey::decode(entry.first);
		const auto *version = visible(model, key._key, snapshot);
		CHECK(version && version->value);
		CHECK(key._value_type == Value && key._commit_sequence == version->seq);
		rows.emplace_back(key._key, entry.second);
		++iterator;
	}
	return rows;
}

void verify(LSMEngine &engine, const Model &model, const std::vector<std::string> &keys,
	std::uint64_t last_seq, std::mt19937 &random) {
	std::vector<std::string> query = keys;
	query.push_back("never-written");
	query.push_back(keys.front()); // batch ordering and duplicates are observable
	std::vector<std::uint64_t> snapshots;
	for (const auto &key : query) {
		const auto snapshot = random() % (last_seq + 1);
		snapshots.push_back(snapshot);
		check_lookup(engine.get(key), visible(model, key, latest));
		check_lookup(engine.get(key, snapshot), visible(model, key, snapshot));
	}
	const auto current = engine.get_batch(query);
	const auto historical = engine.get_batch(query, snapshots);
	CHECK(current.size() == query.size() && historical.size() == query.size());
	for (std::size_t i = 0; i < query.size(); ++i) {
		check_lookup(current[i], visible(model, query[i], latest));
		check_lookup(historical[i], visible(model, query[i], snapshots[i]));
	}
	for (auto snapshot : {std::uint64_t{0}, last_seq / 2, last_seq}) {
		CHECK(collect(engine.begin(snapshot, false, true), model, snapshot) ==
			expected_rows(model, snapshot));
	}
	const std::vector<std::pair<std::string, std::string>> ranges = {
		{"", "b"}, {"a", "ab"}, {"a", "a"}, {"b", "a"},
		{std::string(1, char(0x80)), std::string(2, char(0xff))}};
	for (const auto &range : ranges) {
		CHECK(collect(engine.range(range.first, range.second), model, latest) ==
			expected_rows(model, latest, range));
	}
}

void model_history_through_flush_compaction_and_reopen(const std::string &path,
	std::uint32_t seed) {
	std::mt19937 random(seed);
	const std::vector<std::string> keys = {
		"", "a", "aa", "ab", "b", "z", std::string("a\0", 2),
		std::string("a\0x", 3), std::string("a\xff", 2),
		std::string(1, char(0x80)), std::string(1, char(0xff))};
	Model model;
	std::uint64_t last_seq = 0;
	auto open = [&] { return std::make_shared<LSMEngine>(path, 220, 90, 2, 100); };
	auto engine = open();
	auto record = [&](const std::string &key, std::optional<std::string> value,
		std::optional<std::uint64_t> seq) {
		CHECK(seq && *seq > last_seq);
		last_seq = *seq;
		model[key].push_back({*seq, std::move(value)});
	};
	// Guarantee every binary/prefix/empty key participates, independently of the seed.
	for (const auto &key : keys) { record(key, "initial", engine->put(key, "initial")); }
	verify(*engine, model, keys, last_seq, random);
	bool saw_sorted_level = false;
	for (unsigned step = 0; step < 120; ++step) {
		const auto &key = keys[random() % keys.size()];
		std::string value(random() % 32, '\0');
		for (char &byte : value) { byte = static_cast<char>(random() & 0xff); }
		switch (random() % 5) {
		case 0:
			record(key, std::nullopt, engine->remove(key));
			break;
		case 1: {
			auto tx = engine->begin_transaction(ReadCommited);
			std::map<std::string, std::optional<std::string>> pending;
			for (unsigned op = 0; op < 4; ++op) {
				const auto &target = keys[random() % keys.size()];
				if (random() % 3 == 0) {
					CHECK(tx->remove(target)); pending[target] = std::nullopt;
				} else {
					CHECK(tx->put(target, value)); pending[target] = value;
				}
			}
			const auto result = tx->commit();
			CHECK(result.status == CommitResult::Committed && result.commit_seq);
			CHECK(*result.commit_seq > last_seq);
			last_seq = *result.commit_seq;
			for (auto &[target, data] : pending) {
				model[target].push_back({last_seq, data});
			}
			break;
		}
		case 2: {
			const auto &other = keys[random() % keys.size()];
			const auto results = engine->put_batch({{key, value}, {other, ""}, {key, "last"}});
			CHECK(results.size() == 3);
			record(key, value, results[0]); record(other, "", results[1]);
			record(key, "last", results[2]);
			break;
		}
		default: record(key, value, engine->put(key, value)); break;
		}
		if (step % 7 == 0) { engine->flush(); }
		if (step % 13 == 0) { engine->compact(); }
		saw_sorted_level |= engine->get_cur_max_level() > 0;
		if (step % 10 == 0) { verify(*engine, model, keys, last_seq, random); }
		if (step == 39 || step == 79) {
			engine->close(); engine.reset(); engine = open();
			verify(*engine, model, keys, last_seq, random);
		}
	}
	CHECK(saw_sorted_level);
	engine->close(); engine.reset(); engine = open();
	verify(*engine, model, keys, last_seq, random);
	engine->close();
}

void rr_snapshot_survives_compaction(const std::string &path) {
	auto engine = std::make_shared<LSMEngine>(path, 180, 70, 2, 100);
	CHECK(engine->put("k", "before"));
	auto snapshot = engine->begin_transaction(ReadRepeated);
	for (int i = 0; i < 30; ++i) {
		CHECK(engine->put("k", "after-" + std::to_string(i)));
		CHECK(engine->put("filler-" + std::to_string(i), std::string(30, 'x')));
		if (i % 3 == 0) { engine->flush(); }
	}
	CHECK(engine->get_cur_max_level() > 0);
	CHECK(snapshot->get("k") == std::optional<std::string>("before"));
	CHECK(snapshot->put("k", "conflicting"));
	CHECK(snapshot->commit().status == CommitResult::Conflict);
	CHECK(engine->get("k")._value == "after-29");
	engine->close();
}
} // namespace

int main() {
	const std::vector<std::pair<std::string, std::function<void(const std::string &)>>> cases = {
		{"history model seed 17", [](const std::string &path) {
			model_history_through_flush_compaction_and_reopen(path, 17);
		}},
		{"history model seed 20261028", [](const std::string &path) {
			model_history_through_flush_compaction_and_reopen(path, 20261028);
		}},
		{"RR snapshot and conflict after compaction", rr_snapshot_survives_compaction}};
	std::size_t passed = 0;
	for (const auto &[name, run] : cases) {
		TempDir dir; // parent cleans data even on a child timeout or crash
		std::cout.flush(); std::cerr.flush();
		const auto child = ::fork();
		if (child < 0) { std::cerr << "fork failed\n"; return 1; }
		if (child == 0) {
			::alarm(20);
			try { run(dir.path); ::_exit(0); }
			catch (const std::exception &error) {
				std::cerr << "[FAIL] " << name << ": " << error.what() << std::endl;
				::_exit(1);
			}
		}
		int status = 0;
		pid_t waited;
		do { waited = ::waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
		if (waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
			++passed; std::cout << "[PASS] " << name << '\n';
		} else { std::cerr << "[FAIL] " << name << ": child status " << status << '\n'; }
	}
	std::cout << passed << '/' << cases.size() << " tests passed\n";
	return passed == cases.size() ? 0 : 1;
}
