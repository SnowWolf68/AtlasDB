#include "TranContext.h"
#include "lsm.h"
#include "Record.h"
#include "util.h"
#include "wal.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
#define CHECK(expr)                                                           \
	do {                                                                       \
		if (!(expr))                                                           \
			throw std::runtime_error("line " + std::to_string(__LINE__) +      \
									 ": " #expr);                             \
	} while (false)

struct TempDir {
	std::string path;
	TempDir() {
		std::string pattern = (std::filesystem::temp_directory_path() /
							   "atlasdb-tran-test-XXXXXX")
								  .string();
		std::vector<char> bytes(pattern.begin(), pattern.end());
		bytes.push_back('\0');
		auto result = ::mkdtemp(bytes.data());
		if (!result) throw std::runtime_error("mkdtemp failed");
		path = result;
	}
	~TempDir() {
		std::error_code ec;
		std::filesystem::remove_all(path, ec);
	}
};

void wait_child(pid_t pid) {
	int status = 0;
	pid_t result;
	do {
		result = ::waitpid(pid, &status, 0);
	} while (result < 0 && errno == EINTR);
	CHECK(result == pid);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

// begin_transaction() needs shared_from_this(), so the child engine must be
// created through make_shared. _exit skips close()/destructors: this tests
// process termination, not power loss.
void tx_without_close(
	const std::string &dir,
	const std::function<void(std::shared_ptr<LSMEngine> &)> &action) {
	const auto pid = ::fork();
	CHECK(pid >= 0);
	if (pid == 0) {
		try {
			auto engine = std::make_shared<LSMEngine>(dir, 1024 * 1024, 64, 100);
			action(engine);
			::_exit(0);
		} catch (const std::exception &e) {
			std::cerr << "child: " << e.what() << '\n';
			::_exit(1);
		}
	}
	wait_child(pid);
}

std::vector<std::uint8_t>
single_record(const std::string &key, const std::string &value,
			  std::uint64_t seq, ValueType type = Value) {
	return WAL::encode_transaction(
		InternalKey::encode(InternalKey(key, seq, type)), value);
}

void write_segment(const std::string &dir, std::uint64_t id,
				   const std::vector<std::uint8_t> &bytes) {
	FileObj::create_and_write(WAL::get_filepath(dir, id), bytes);
}

bool throws_runtime_error(const std::function<void()> &fn) {
	try {
		fn();
	} catch (const std::runtime_error &) {
		return true;
	}
	return false;
}

// ─── tests ──────────────────────────────────────────────────────

void test_basic_commit_and_visibility() {
	TempDir dir;
	auto engine = std::make_shared<LSMEngine>(dir.path);
	auto tx = engine->begin_transaction(IsolationLevel::ReadCommited);
	CHECK(tx->put("a", "1"));
	CHECK(tx->put("b", "2"));
	CHECK(tx->put("c", "3"));
	auto res = tx->commit();
	CHECK(res.status == CommitResult::Status::Committed);
	CHECK(res.commit_seq && *res.commit_seq == 1);
	CHECK(engine->get("a")._value == "1");
	CHECK(engine->get("b")._value == "2");
	CHECK(engine->get("c")._value == "3");
	CHECK(engine->get("a")._commit_sequence == 1);
	CHECK(engine->get("b")._commit_sequence == 1);
	CHECK(engine->get("c")._commit_sequence == 1);
	// one commit = one seq: the next bare put continues after it
	CHECK(engine->put("d", "4") == 2);
}

void test_read_own_writes_and_write_set_folding() {
	TempDir dir;
	auto engine = std::make_shared<LSMEngine>(dir.path);
	auto tx = engine->begin_transaction(IsolationLevel::ReadCommited);
	CHECK(tx->put("k", "v1"));
	auto v = tx->get("k");
	CHECK(v && *v == "v1");
	CHECK(tx->put("k", "v2"));
	v = tx->get("k");
	CHECK(v && *v == "v2");
	// remove folds over the pending put
	CHECK(tx->remove("k"));
	CHECK(!tx->get("k").has_value());
	CHECK(tx->put("k2", "w"));
	auto res = tx->commit();
	CHECK(res.status == CommitResult::Status::Committed);
	// only the folded result of the write set reaches the engine; the engine
	// layer passes tombstones through (the LSM facade is what translates
	// them to NotFound)
	CHECK(engine->get("k")._value_type == TombStone);
	CHECK(engine->get("k2")._value == "w");
}

void test_uncommitted_writes_invisible() {
	TempDir dir;
	auto engine = std::make_shared<LSMEngine>(dir.path);
	auto tx1 = engine->begin_transaction(IsolationLevel::ReadCommited);
	CHECK(tx1->put("k", "v"));
	CHECK(engine->get("k")._value_type == NotFound);
	auto tx2 = engine->begin_transaction(IsolationLevel::ReadRepeated);
	CHECK(!tx2->get("k").has_value());
	auto res = tx1->commit();
	CHECK(res.status == CommitResult::Status::Committed);
	CHECK(engine->get("k")._value == "v");
	// tx2 was begun before the commit with a fixed snapshot: still blind
	CHECK(!tx2->get("k").has_value());
	// a fresh RC transaction sees the committed value
	auto tx3 = engine->begin_transaction(IsolationLevel::ReadCommited);
	auto v = tx3->get("k");
	CHECK(v && *v == "v");
}

void test_rr_reads_fixed_snapshot() {
	TempDir dir;
	auto engine = std::make_shared<LSMEngine>(dir.path);
	CHECK(engine->put("k", "v1") == 1);
	CHECK(engine->put("gone", "g1") == 2);
	auto tx = engine->begin_transaction(IsolationLevel::ReadRepeated);
	// commits after the snapshot must be invisible: newer value and tombstone
	CHECK(engine->put("k", "v2") == 3);
	CHECK(engine->remove("gone") == 4);
	auto v = tx->get("k");
	CHECK(v && *v == "v1");
	v = tx->get("gone");
	CHECK(v && *v == "g1");
	CHECK(engine->get("k")._value == "v2");
	// a transaction begun after those commits sees them
	auto tx2 = engine->begin_transaction(IsolationLevel::ReadRepeated);
	CHECK(tx2->get("k") == std::optional<std::string>("v2"));
	CHECK(!tx2->get("gone").has_value());
}

void test_rc_sees_later_commits() {
	TempDir dir;
	auto engine = std::make_shared<LSMEngine>(dir.path);
	auto tx = engine->begin_transaction(IsolationLevel::ReadCommited);
	CHECK(engine->put("k", "v1") == 1);
	auto v = tx->get("k");
	CHECK(v && *v == "v1");
	CHECK(engine->put("k", "v2") == 2);
	v = tx->get("k");
	CHECK(v && *v == "v2");
}

void test_rr_write_conflict_aborts() {
	TempDir dir;
	auto engine = std::make_shared<LSMEngine>(dir.path);
	CHECK(engine->put("k", "v0") == 1);
	auto tx = engine->begin_transaction(IsolationLevel::ReadRepeated);
	CHECK(engine->put("k", "other") == 2);
	CHECK(tx->put("k", "mine"));
	// write set wins over the snapshot read before commit
	auto v = tx->get("k");
	CHECK(v && *v == "mine");
	auto res = tx->commit();
	CHECK(res.status == CommitResult::Status::Conflict);
	CHECK(!res.commit_seq.has_value());
	// the conflicting write set is discarded, the other commit survives
	CHECK(engine->get("k")._value == "other");
	// conflict only aborts the transaction, the engine stays writable; the
	// conflict path returns before the watermark advances, so no seq burns
	auto next = engine->put("k2", "v");
	CHECK(next && *next == 3);
	// after the conflict the context is dead
	CHECK(!tx->put("k", "again"));
	CHECK(throws_runtime_error([&] { tx->get("k"); }));
}

void test_rr_different_keys_commit() {
	TempDir dir;
	auto engine = std::make_shared<LSMEngine>(dir.path);
	auto tx = engine->begin_transaction(IsolationLevel::ReadRepeated);
	CHECK(engine->put("other_key", "x") == 1);
	CHECK(tx->put("k", "v"));
	auto res = tx->commit();
	CHECK(res.status == CommitResult::Status::Committed);
	CHECK(res.commit_seq && *res.commit_seq == 2);
}

void test_rc_never_conflicts() {
	TempDir dir;
	auto engine = std::make_shared<LSMEngine>(dir.path);
	CHECK(engine->put("k", "v0") == 1);
	auto tx = engine->begin_transaction(IsolationLevel::ReadCommited);
	CHECK(engine->put("k", "other") == 2);
	CHECK(tx->put("k", "mine"));
	auto res = tx->commit();
	CHECK(res.status == CommitResult::Status::Committed);
	CHECK(res.commit_seq && *res.commit_seq == 3);
	CHECK(engine->get("k")._value == "mine");
}

void test_rr_snapshot_boundary_no_false_conflict() {
	// the version sitting exactly at the snapshot must not count as conflict
	TempDir dir;
	auto engine = std::make_shared<LSMEngine>(dir.path);
	CHECK(engine->put("k", "v1") == 1);
	auto tx = engine->begin_transaction(IsolationLevel::ReadRepeated);
	CHECK(tx->put("k", "v2"));
	auto res = tx->commit();
	CHECK(res.status == CommitResult::Status::Committed);
	CHECK(res.commit_seq && *res.commit_seq == 2);
	CHECK(engine->get("k")._value == "v2");
}

void test_abort_discards_and_state_machine() {
	TempDir dir;
	auto engine = std::make_shared<LSMEngine>(dir.path);
	auto tx = engine->begin_transaction(IsolationLevel::ReadCommited);
	CHECK(tx->put("k", "v"));
	tx->abort();
	CHECK(engine->get("k")._value_type == NotFound);
	// abort is idempotent
	tx->abort();
	// a dead transaction refuses everything
	CHECK(!tx->put("k", "again"));
	CHECK(throws_runtime_error([&] { tx->get("k"); }));
	CHECK(throws_runtime_error([&] { tx->remove("k"); }));
	CHECK(throws_runtime_error([&] { tx->commit(); }));
}

void test_commit_state_transitions() {
	TempDir dir;
	auto engine = std::make_shared<LSMEngine>(dir.path);
	auto tx = engine->begin_transaction(IsolationLevel::ReadCommited);
	CHECK(tx->put("k", "v"));
	auto res = tx->commit();
	CHECK(res.status == CommitResult::Status::Committed);
	CHECK(!tx->put("k", "again"));
	CHECK(throws_runtime_error([&] { tx->get("k"); }));
	CHECK(throws_runtime_error([&] { tx->remove("k"); }));
	CHECK(throws_runtime_error([&] { tx->commit(); }));
	CHECK(throws_runtime_error([&] { tx->abort(); }));
}

void test_empty_transaction_ends_without_consuming_seq() {
	TempDir dir;
	{
		auto engine = std::make_shared<LSMEngine>(dir.path);
		for (auto level : {IsolationLevel::ReadCommited,
						   IsolationLevel::ReadRepeated}) {
			auto tx = engine->begin_transaction(level);
			auto res = tx->commit();
			CHECK(res.status == CommitResult::Status::Empty);
			CHECK(!res.commit_seq.has_value());
			// Empty commits end the context just like commits with writes.
			CHECK(!tx->put("k", "again"));
			CHECK(throws_runtime_error([&] { tx->get("k"); }));
			CHECK(throws_runtime_error([&] { tx->remove("k"); }));
			CHECK(throws_runtime_error([&] { tx->commit(); }));
			CHECK(throws_runtime_error([&] { tx->abort(); }));
		}
		CHECK(engine->put("k", "v") == 1);
		engine->close();
	}
	// Empty commits consume no sequence, including across a normal reopen.
	LSMEngine db(dir.path);
	CHECK(db.get("k")._value == "v");
	CHECK(db.put("k2", "v") == 2);
}

void test_empty_transactions_do_not_consume_seq_across_process_exit() {
	TempDir dir;
	tx_without_close(dir.path, [&](std::shared_ptr<LSMEngine> &engine) {
		CHECK(engine->put("base", "value") == 1);
		for (auto level : {IsolationLevel::ReadCommited,
						   IsolationLevel::ReadRepeated}) {
			auto tx = engine->begin_transaction(level);
			// Read-only transactions also have an empty write set.
			CHECK(tx->get("base") == std::optional<std::string>("value"));
			auto committed = tx->commit();
			CHECK(committed.status == CommitResult::Status::Empty);
			CHECK(!committed.commit_seq.has_value());
		}
	});
	LSMEngine reopened(dir.path);
	CHECK(reopened.get("base")._value == "value");
	CHECK(reopened.get("base")._commit_sequence == 1);
	const auto next = reopened.put("next", "value");
	CHECK(next && *next == 2);
}

void test_transaction_length_boundaries_preserve_write_set() {
	TempDir dir;
	const std::string maximum_key(65527, 'k');
	const std::string maximum_value(65535, 'v');
	{
		auto engine = std::make_shared<LSMEngine>(dir.path);
		auto tx = engine->begin_transaction(IsolationLevel::ReadCommited);
		CHECK(tx->put("retained", "valid"));
		CHECK(!tx->put("retained", maximum_value + "x"));
		CHECK(tx->get("retained") == "valid");
		CHECK(!tx->put(maximum_key + "x", "invalid"));
		CHECK(!tx->remove(maximum_key + "x"));
		CHECK(tx->put(maximum_key, "maximum-key"));
		CHECK(tx->put("", maximum_value));
		const auto committed = tx->commit();
		CHECK(committed.status == CommitResult::Status::Committed);
		CHECK(engine->get("retained")._value == "valid");
		CHECK(engine->get(maximum_key)._value == "maximum-key");
		CHECK(engine->get("")._value == maximum_value);
		CHECK(engine->get(maximum_key + "x")._value_type == NotFound);
		engine->close();
	}
	LSMEngine reopened(dir.path);
	CHECK(reopened.get("retained")._value == "valid");
	CHECK(reopened.get(maximum_key)._value == "maximum-key");
	CHECK(reopened.get("")._value == maximum_value);
}

void test_whole_transaction_survives_process_exit() {
	TempDir dir;
	tx_without_close(dir.path, [&](std::shared_ptr<LSMEngine> &engine) {
		CHECK(engine->put("base", "b") == 1);
		auto tx = engine->begin_transaction(IsolationLevel::ReadCommited);
		CHECK(tx->put("k1", "v1"));
		CHECK(tx->put("k2", "v2"));
		CHECK(tx->put("k3", "v3"));
		auto res = tx->commit();
		CHECK(res.status == CommitResult::Status::Committed);
		CHECK(res.commit_seq && *res.commit_seq == 2);
	});
	LSMEngine db(dir.path);
	CHECK(db.get("base")._value == "b");
	CHECK(db.get("k1")._value == "v1");
	CHECK(db.get("k2")._value == "v2");
	CHECK(db.get("k3")._value == "v3");
	CHECK(db.get("k1")._commit_sequence == 2);
	CHECK(db.get("k2")._commit_sequence == 2);
	CHECK(db.get("k3")._commit_sequence == 2);
	CHECK(db.put("next", "v") == 3);
}

void test_torn_multi_op_transaction_recovers_all_or_nothing() {
	std::unordered_map<std::string, std::optional<std::string>> map = {
		{"t1", std::optional<std::string>("v1")},
		{"t2", std::optional<std::string>("v2")},
		{"t3", std::optional<std::string>("v3")},
	};
	const auto first = single_record("base", "b", 1);
	const auto tx_bytes = WAL::encode_transaction(map, 2);
	for (std::size_t cut = 0; cut < tx_bytes.size(); ++cut) {
		TempDir dir;
		auto bytes = first;
		bytes.insert(bytes.end(), tx_bytes.begin(), tx_bytes.begin() + cut);
		write_segment(dir.path, 1, bytes);
		LSMEngine db(dir.path);
		// the earlier single-op record survives intact
		CHECK(db.get("base")._value == "b");
		// no operation of the torn transaction may surface, at any cut point
		CHECK(db.get("t1")._value_type == NotFound);
		CHECK(db.get("t2")._value_type == NotFound);
		CHECK(db.get("t3")._value_type == NotFound);
		// the dropped seq is not reused
		CHECK(db.put("next", "v") == 2);
	}
}

void test_concurrent_transactions_unique_sequences() {
	TempDir dir;
	auto engine = std::make_shared<LSMEngine>(dir.path);
	constexpr int kThreads = 4;
	constexpr int kPerThread = 25;
	std::vector<std::vector<std::uint64_t>> seqs(kThreads);
	std::vector<std::string> failures(kThreads);
	std::atomic<bool> ok{true};
	std::vector<std::thread> threads;
	for (int t = 0; t < kThreads; ++t) {
		threads.emplace_back([&, t] {
			try {
				for (int i = 0; i < kPerThread; ++i) {
					auto tx = engine->begin_transaction(
						i % 2 ? IsolationLevel::ReadCommited
							  : IsolationLevel::ReadRepeated);
					auto key =
						"k" + std::to_string(t) + "_" + std::to_string(i);
					CHECK(tx->put(key, "v"));
					auto res = tx->commit();
					CHECK(res.status == CommitResult::Status::Committed);
					CHECK(res.commit_seq.has_value());
					seqs[t].push_back(*res.commit_seq);
				}
			} catch (const std::exception &e) {
				ok = false;
				failures[t] = e.what();
			}
		});
	}
	for (auto &thread : threads) { thread.join(); }
	if (!ok) {
		for (const auto &failure : failures) {
			if (!failure.empty()) std::cerr << "thread: " << failure << '\n';
		}
	}
	CHECK(ok);
	std::vector<std::uint64_t> all;
	for (const auto &part : seqs) {
		all.insert(all.end(), part.begin(), part.end());
	}
	CHECK(all.size() == kThreads * kPerThread);
	std::sort(all.begin(), all.end());
	CHECK(std::adjacent_find(all.begin(), all.end()) == all.end());
	for (int t = 0; t < kThreads; ++t) {
		for (int i = 0; i < kPerThread; ++i) {
			auto key = "k" + std::to_string(t) + "_" + std::to_string(i);
			CHECK(engine->get(key)._value == "v");
		}
	}
	// watermark above every confirmed commit
	CHECK(engine->put("sentinel", "v") == kThreads * kPerThread + 1);
}

void test_transaction_triggers_threshold_flush() {
	TempDir dir;
	{
		auto engine = std::make_shared<LSMEngine>(dir.path, 128, 64, 100);
		auto tx = engine->begin_transaction(IsolationLevel::ReadCommited);
		for (int i = 0; i < 3; ++i) {
			CHECK(tx->put("k" + std::to_string(i), std::string(80, 'v')));
		}
		auto res = tx->commit();
		CHECK(res.status == CommitResult::Status::Committed);
		// the write set far exceeds the 128-byte budget: the commit path
		// itself must flush to SST instead of accumulating frozen tables
		std::size_t ssts = 0;
		for (const auto &entry : std::filesystem::directory_iterator(dir.path)) {
			const auto name = entry.path().filename().string();
			if (entry.is_regular_file() && name.rfind("sst_", 0) == 0) {
				++ssts;
			}
		}
		CHECK(ssts >= 1);
		for (int i = 0; i < 3; ++i) {
			CHECK(engine->get("k" + std::to_string(i))._value ==
				  std::string(80, 'v'));
		}
	}
	LSMEngine db(dir.path);
	for (int i = 0; i < 3; ++i) {
		CHECK(db.get("k" + std::to_string(i))._value == std::string(80, 'v'));
	}
}

void test_get_batch_one_view_per_call() {
	// One get_batch call must sample the published watermark once: the three
	// keys of one transaction share a seq, so a single call sees them all
	// (same round) or none. Correct code never violates this; the old
	// per-key-load version breaks it only under races, so this is a canary
	// rather than a deterministic pin.
	TempDir dir;
	auto engine = std::make_shared<LSMEngine>(dir.path);
	constexpr int kRounds = 1000;
	std::atomic<bool> failed{false};
	std::atomic<bool> stop{false};
	std::thread writer([&] {
		try {
			for (int r = 0; r < kRounds; ++r) {
				auto tx =
					engine->begin_transaction(IsolationLevel::ReadCommited);
				for (int k = 0; k < 3; ++k) {
					CHECK(tx->put("t" + std::to_string(k),
								  "r" + std::to_string(r)));
				}
				if (tx->commit().status != CommitResult::Status::Committed) {
					failed = true;
				}
			}
		} catch (const std::exception &) {
			failed = true;
		}
	});
	std::thread reader([&] {
		while (!stop) {
			auto results = engine->get_batch({"t0", "t1", "t2"});
			std::size_t visible = 0;
			std::string round;
			for (const auto &res : results) {
				if (res._value_type == ValueType::Value) {
					++visible;
					round = res._value;
				}
			}
			if (visible != 0 && visible != 3) { failed = true; }
			if (visible == 3) {
				for (const auto &res : results) {
					if (res._value != round) { failed = true; }
				}
			}
		}
	});
	writer.join();
	stop = true;
	reader.join();
	CHECK(!failed);
	auto results = engine->get_batch({"t0", "t1", "t2"});
	for (const auto &res : results) {
		CHECK(res._value_type == ValueType::Value);
		CHECK(res._value == "r" + std::to_string(kRounds - 1));
	}
}

void test_begin_after_close_reports_closed_at_commit() {
	TempDir dir;
	auto engine = std::make_shared<LSMEngine>(dir.path);
	CHECK(engine->put("k", "v") == 1);
	engine->close();
	// begin stays legal on a closed engine by design; the commit gate answers
	auto tx = engine->begin_transaction(IsolationLevel::ReadRepeated);
	CHECK(tx->put("k", "v2"));
	auto res = tx->commit();
	CHECK(res.status == CommitResult::Status::Closed);
	CHECK(!res.commit_seq.has_value());
	CHECK(engine->get("k")._value == "v");
}

} // namespace

int main() {
	const std::vector<std::pair<std::string, void (*)()>> tests = {
		{"basic commit and visibility", test_basic_commit_and_visibility},
		{"read own writes and write set folding",
		 test_read_own_writes_and_write_set_folding},
		{"uncommitted writes invisible", test_uncommitted_writes_invisible},
		{"RR reads fixed snapshot", test_rr_reads_fixed_snapshot},
		{"RC sees later commits", test_rc_sees_later_commits},
		{"RR write conflict aborts", test_rr_write_conflict_aborts},
		{"RR different keys commit", test_rr_different_keys_commit},
		{"RC never conflicts", test_rc_never_conflicts},
		{"RR snapshot boundary no false conflict",
		 test_rr_snapshot_boundary_no_false_conflict},
		{"abort discards and state machine",
		 test_abort_discards_and_state_machine},
		{"commit state transitions", test_commit_state_transitions},
		{"empty transaction ends without consuming seq",
		 test_empty_transaction_ends_without_consuming_seq},
		{"empty transactions do not consume seq across process exit",
		 test_empty_transactions_do_not_consume_seq_across_process_exit},
		{"transaction length boundaries preserve prior write set",
		 test_transaction_length_boundaries_preserve_write_set},
		{"whole transaction survives process exit",
		 test_whole_transaction_survives_process_exit},
		{"torn multi-op transaction all or nothing",
		 test_torn_multi_op_transaction_recovers_all_or_nothing},
		{"concurrent transactions unique sequences",
		 test_concurrent_transactions_unique_sequences},
		{"transaction triggers threshold flush",
		 test_transaction_triggers_threshold_flush},
		{"get_batch sees one transaction all or nothing",
		 test_get_batch_one_view_per_call},
		{"begin after close reports closed at commit",
		 test_begin_after_close_reports_closed_at_commit},
	};

	std::size_t passed = 0;
	for (const auto &test : tests) {
		try {
			test.second();
			++passed;
			std::cout << "[PASS] " << test.first << '\n';
		} catch (const std::exception &error) {
			std::cerr << "[FAIL] " << test.first << ": " << error.what()
					  << '\n';
		}
	}
	std::cout << passed << "/" << tests.size() << " tests passed\n";
	return passed == tests.size() ? 0 : 1;
}
