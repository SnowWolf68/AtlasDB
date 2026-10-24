#include "MemTable.h"
#include "wal.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <pthread.h>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

// Linux test executable only: arrange real I/O/lock interleavings without
// exposing MemTable internals or changing production code.
namespace {
enum class Role { Normal, CaptureLocks, Flush, OtherOperation, SizeReader };
enum class IoMode { Normal, PauseFileSync, FailFileSync, FailDirectorySync };
thread_local Role role = Role::Normal;
thread_local bool fail_skiplist_construction = false;
bool construction_failed = false;
bool io_failure_injected = false;
IoMode io_mode = IoMode::Normal;
std::array<pthread_rwlock_t *, 2> table_locks{}; // captured via get_total_size()
std::size_t captured_locks = 0;

struct Gate {
	std::mutex mutex;
	std::condition_variable changed;
	bool build_paused = false;
	bool resume_build = false;
	bool operation_attempted = false;
	bool operation_done = false;
	bool reader_paused = false;
	bool resume_reader = false;
	bool flush_attempted_active = false;
} gate;

void check(bool condition, const char *expression, int line) {
	if (!condition) {
		throw std::runtime_error("line " + std::to_string(line) + ": " + expression);
	}
}
#define CHECK(expression) check((expression), #expression, __LINE__)

void signal(bool Gate::*flag) {
	std::lock_guard<std::mutex> lock(gate.mutex);
	gate.*flag = true;
	gate.changed.notify_all();
}

bool wait_for(bool Gate::*flag) {
	std::unique_lock<std::mutex> lock(gate.mutex);
	return gate.changed.wait_for(lock, std::chrono::seconds(3),
								[flag] { return gate.*flag; });
}

class ThreadJob {
public:
	template <class F> ThreadJob(Role worker_role, F work)
		: thread([this, worker_role, work] {
			role = worker_role;
			try { work(); }
			catch (...) { error = std::current_exception(); }
		}) {}
	~ThreadJob() {
		if (thread.joinable()) {
			signal(&Gate::resume_build);
			signal(&Gate::resume_reader);
			thread.join();
		}
	}
	void join() {
		thread.join();
		if (error) { std::rethrow_exception(error); }
	}
private:
	std::exception_ptr error;
	std::thread thread;
};

struct Output {
	MemTable::FlushLastResult result{nullptr, std::nullopt, std::nullopt};
	std::vector<std::uint64_t> seqs;
};

Output flush(MemTable &table, const std::string &path, std::uint64_t id) {
	SSTBuilder builder;
	Output output;
	output.result = table.flush_last(builder, path, id, output.seqs);
	return output;
}

void check_one_entry(const Output &output, const std::string &key,
					 const std::string &value, std::uint64_t seq) {
	CHECK(output.result._sst != nullptr);
	CHECK(output.seqs == std::vector<std::uint64_t>{seq});
	auto it = output.result._sst->begin(std::nullopt, true);
	CHECK(it.is_valid());
	auto entry = *it;
	auto decoded = InternalKey::decode(entry.first);
	CHECK(decoded._key == key && decoded._commit_sequence == seq);
	CHECK(decoded._value_type == Value && entry.second == value);
	++it;
	CHECK(it == output.result._sst->end());
}

void capture_table_locks(MemTable &table) {
	role = Role::CaptureLocks;
	table.get_total_size();
	role = Role::Normal;
	CHECK(captured_locks == 2 && table_locks[0] != table_locks[1]);
}

// If an older implementation releases its locks during build, allow the
// competing operation to finish before resuming build. If it keeps them,
// resume build once that operation has actually attempted its lock. This
// tests preserved data, not a required lock duration, and needs no sleeps.
bool table_locks_available() {
	int first = ::pthread_rwlock_trywrlock(table_locks[0]);
	CHECK(first == 0 || first == EBUSY);
	if (first != 0) { return false; }
	int second = ::pthread_rwlock_trywrlock(table_locks[1]);
	CHECK(second == 0 || second == EBUSY);
	if (second == 0) { CHECK(::pthread_rwlock_unlock(table_locks[1]) == 0); }
	CHECK(::pthread_rwlock_unlock(table_locks[0]) == 0);
	return second == 0;
}

template <bool Frozen, IoMode Failure>
void io_failure_preserves_table(const std::string &dir) {
	MemTable table(Frozen ? 20 : 1024);
	const std::string value(12, 'v'); // a + value + internal suffix = 21 bytes
	table.put("a", value, 1);
	if (Frozen) { table.put("b", "live", 2); }
	const auto before = table.get_total_size();
	io_mode = Failure;
	role = Role::Flush;
	bool threw = false;
	try { flush(table, dir + "/failed.sst", 1); }
	catch (const std::runtime_error &) { threw = true; }
	role = Role::Normal;
	io_mode = IoMode::Normal;
	CHECK(threw && io_failure_injected);
	CHECK(table.get_total_size() == before);
	CHECK(table.get("a", 100)._value == value);
	if (Frozen) { CHECK(table.get("b", 100)._value == "live"); }

	auto retry = flush(table, dir + "/retry.sst", 2);
	check_one_entry(retry, "a", value, 1);
	CHECK(retry.result._flush_id == 0);
	CHECK(retry.result._new_active_id ==
		  (Frozen ? std::optional<std::uint64_t>{} : std::optional<std::uint64_t>{1}));
	CHECK(table.get_total_size() == before - 21);
	CHECK(table.get("a", 100)._value_type == NotFound);
	if (Frozen) {
		auto next = flush(table, dir + "/next.sst", 3);
		check_one_entry(next, "b", "live", 2);
		CHECK(next.result._flush_id == 1 && next.result._new_active_id == 2);
	}
	CHECK(table.get_total_size() == 0);
	auto empty = flush(table, dir + "/empty.sst", 4);
	CHECK(!empty.result._sst && !empty.result._flush_id && !empty.result._new_active_id);
}

template <bool Frozen, bool OversizedKey>
void oversized_record_aborts_flush_without_losing_data(const std::string &dir) {
	MemTable table(1024);
	const std::string invalid_key = OversizedKey ? std::string(65536 - 8, 'z') : "z";
	const std::string invalid_value = OversizedKey ? "oversized-key" : std::string(65536, 'v');
	const std::string after_key(1, static_cast<char>(0xff));
	// Accumulate one table without automatic freezing. Its size exceeds the
	// threshold, so the explicit froze() below must actually move it.
	CHECK(table.put("a", "before", 1, false)._is_add);
	CHECK(table.put(invalid_key, invalid_value, 2, false)._is_add);
	CHECK(table.put(after_key, "after", 3, false)._is_add);
	if (Frozen) {
		const auto frozen = table.froze();
		CHECK(frozen._frozen_list_id == 0 && frozen._new_active_list_id == 1);
		CHECK(table.put("live", "active", 4)._is_add);
	}
	const auto before = table.get_total_size();
	for (int attempt = 0; attempt < 2; ++attempt) {
		const auto path = dir + "/rejected-" + std::to_string(attempt) + ".sst";
		bool threw = false;
		// flush() creates a new builder and output vector for each attempt.
		// Partial builder contents are discarded when this operation fails.
		try { flush(table, path, static_cast<std::uint64_t>(attempt + 1)); }
		catch (const std::length_error &) { threw = true; }
		catch (const std::runtime_error &) { threw = true; }
		CHECK(threw);
		CHECK(!std::filesystem::exists(path));
		CHECK(table.get_total_size() == before);
		const auto first = table.get("a", 100);
		const auto invalid = table.get(invalid_key, 100);
		const auto last = table.get(after_key, 100);
		CHECK(first._value_type == Value && first._value == "before" && first._commit_sequence == 1);
		CHECK(invalid._value_type == Value && invalid._value == invalid_value && invalid._commit_sequence == 2);
		CHECK(last._value_type == Value && last._value == "after" && last._commit_sequence == 3);
		if (Frozen) {
			const auto active = table.get("live", 100);
			CHECK(active._value_type == Value && active._value == "active" && active._commit_sequence == 4);
		}
	}
	// A new write and a freeze also demonstrate that exception unwinding has
	// released both locks and that failed flushing did not advance the table ID.
	CHECK(table.put("new", "still-writable", 5, false)._is_add);
	CHECK(table.put("padding", std::string(1024, 'p'), 6, false)._is_add);
	const auto after_writes = table.get_total_size();
	const auto frozen = table.froze();
	CHECK(frozen._frozen_list_id == (Frozen ? 1 : 0));
	CHECK(frozen._new_active_list_id == (Frozen ? 2 : 1));
	CHECK(table.get_total_size() == after_writes);
	CHECK(table.get("new", 100)._value == "still-writable");
	CHECK(table.get(invalid_key, 100)._value == invalid_value);
}

template <bool Frozen, bool FreezeWriter>
void concurrent_write_survives_flush(const std::string &dir) {
	MemTable table(Frozen ? 20 : 40);
	const std::string old_value(12, 'v');
	const std::string new_value(FreezeWriter ? 50 : 3, 'w');
	table.put("a", old_value, 1);
	capture_table_locks(table);
	Output first;
	MemTable::PutResult inserted(false);
	io_mode = IoMode::PauseFileSync;
	ThreadJob flushing(Role::Flush, [&] { first = flush(table, dir + "/first.sst", 1); });
	CHECK(wait_for(&Gate::build_paused));
	bool can_finish_while_building = table_locks_available();
	ThreadJob writing(Role::OtherOperation, [&] {
		inserted = table.put("b", new_value, 2);
		signal(&Gate::operation_done);
	});
	CHECK(wait_for(&Gate::operation_attempted));
	if (can_finish_while_building) { CHECK(wait_for(&Gate::operation_done)); }
	signal(&Gate::resume_build);
	flushing.join();
	writing.join();
	io_mode = IoMode::Normal;

	check_one_entry(first, "a", old_value, 1);
	CHECK(first.result._flush_id == 0);
	CHECK(first.result._new_active_id ==
		  (Frozen ? std::optional<std::uint64_t>{} : std::optional<std::uint64_t>{1}));
	CHECK(inserted._is_add);
	if (FreezeWriter) {
		CHECK(inserted._frozen_list_id == 1 && inserted._new_active_list_id == 2);
	} else {
		CHECK(!inserted._frozen_list_id && !inserted._new_active_list_id);
	}
	CHECK(table.get("a", 100)._value_type == NotFound);
	CHECK(table.get("b", 100)._value == new_value);
	CHECK(table.get_total_size() == 1 + new_value.size() + 8);
	auto next = flush(table, dir + "/next.sst", 2);
	check_one_entry(next, "b", new_value, 2);
	CHECK(next.result._flush_id == 1);
	CHECK(table.get_total_size() == 0);
}

void concurrent_flushes_keep_distinct_tables(const std::string &dir) {
	MemTable table(20);
	const std::string value(12, 'v');
	table.put("a", value, 1); // frozen 0
	table.put("b", value, 2); // frozen 1
	table.put("c", "live", 3); // active 2
	capture_table_locks(table);
	Output first, second;
	io_mode = IoMode::PauseFileSync;
	ThreadJob flushing(Role::Flush, [&] { first = flush(table, dir + "/first.sst", 1); });
	CHECK(wait_for(&Gate::build_paused));
	bool can_finish_while_building = table_locks_available();
	ThreadJob another(Role::OtherOperation, [&] {
		second = flush(table, dir + "/second.sst", 2);
		signal(&Gate::operation_done);
	});
	CHECK(wait_for(&Gate::operation_attempted));
	if (can_finish_while_building) { CHECK(wait_for(&Gate::operation_done)); }
	signal(&Gate::resume_build);
	flushing.join();
	another.join();
	io_mode = IoMode::Normal;
	check_one_entry(first, "a", value, 1);
	check_one_entry(second, "b", value, 2);
	CHECK(first.result._flush_id == 0 && second.result._flush_id == 1);
	CHECK(!first.result._new_active_id && !second.result._new_active_id);
	CHECK(table.get_total_size() == 13);
	CHECK(table.get("a", 100)._value_type == NotFound);
	CHECK(table.get("b", 100)._value_type == NotFound);
	CHECK(table.get("c", 100)._value == "live");
	auto last = flush(table, dir + "/last.sst", 3);
	check_one_entry(last, "c", "live", 3);
	CHECK(last.result._flush_id == 2 && last.result._new_active_id == 3);
	CHECK(table.get_total_size() == 0);
}

void size_query_and_flush_do_not_deadlock(const std::string &dir) {
	MemTable table;
	table.put("a", "value", 1);
	capture_table_locks(table);
	std::size_t observed = 0;
	Output output;
	// Reader holds active shared, then flush attempts active exclusive.
	// Reader must still be able to obtain frozen shared and finish.
	ThreadJob reader(Role::SizeReader, [&] { observed = table.get_total_size(); });
	CHECK(wait_for(&Gate::reader_paused));
	ThreadJob flushing(Role::Flush, [&] { output = flush(table, dir + "/data.sst", 1); });
	CHECK(wait_for(&Gate::flush_attempted_active));
	signal(&Gate::resume_reader);
	reader.join();
	flushing.join();
	CHECK(observed == 14);
	check_one_entry(output, "a", "value", 1);
	CHECK(table.get_total_size() == 0);
}

void new_active_failure_preserves_state(const std::string &dir) {
	MemTable table;
	table.put("a", "value", 1);
	const auto before = table.get_total_size();
	fail_skiplist_construction = true;
	bool threw = false;
	try { flush(table, dir + "/failed.sst", 1); }
	catch (const std::bad_alloc &) { threw = true; }
	fail_skiplist_construction = false;
	CHECK(threw && construction_failed);
	CHECK(table.get_total_size() == before);
	CHECK(table.get("a", 100)._value == "value");
	auto retry = flush(table, dir + "/retry.sst", 2);
	check_one_entry(retry, "a", "value", 1);
	CHECK(retry.result._flush_id == 0 && retry.result._new_active_id == 1);
	CHECK(table.get_total_size() == 0);
}

void restore_active_id(WAL &wal, MemTable &table, const std::string &dir,
					   std::uint64_t active_id) {
	// Public recovery API provides the boundary state; no private-field hacks.
	auto seed = FileObj::create_rw(WAL::get_filepath(dir, active_id - 1));
	CHECK(seed.is_valid() && seed.close());
	CHECK(wal.load_wal(dir, table) == 0);
	CHECK(std::filesystem::exists(WAL::get_filepath(dir, active_id)));
}

void exhausted_id_preserves_state(const std::string &dir) {
	const auto maximum = std::numeric_limits<std::uint64_t>::max();
	MemTable table(20);
	WAL wal;
	restore_active_id(wal, table, dir, maximum);
	const std::string value(12, 'v');
	table.put("a", value, 1, false);
	for (int attempt = 0; attempt < 2; ++attempt) {
		bool threw = false;
		try { flush(table, dir + "/rejected.sst", attempt + 1); }
		catch (const std::runtime_error &) { threw = true; }
		CHECK(threw);
		CHECK(table.get_total_size() == 21);
		CHECK(table.get("a", 100)._value == value);
	}
	// Freezing must still reject MAX: failed flush must not have advanced it.
	bool threw = false;
	try { table.froze(); }
	catch (const std::runtime_error &) { threw = true; }
	CHECK(threw);
	CHECK(table.get_total_size() == 21);
	CHECK(table.get("a", 100)._value == value);
}

void last_id_transition_does_not_wrap(const std::string &dir) {
	const auto maximum = std::numeric_limits<std::uint64_t>::max();
	MemTable table;
	WAL wal;
	restore_active_id(wal, table, dir, maximum - 1);
	table.put("a", "old", 1);
	auto output = flush(table, dir + "/last.sst", 1);
	check_one_entry(output, "a", "old", 1);
	CHECK(output.result._flush_id == maximum - 1);
	CHECK(output.result._new_active_id == maximum);
	CHECK(table.get_total_size() == 0);
	table.put("b", "new", 2);
	bool threw = false;
	try { flush(table, dir + "/rejected.sst", 2); }
	catch (const std::runtime_error &) { threw = true; }
	CHECK(threw);
	CHECK(table.get_total_size() == 12);
	CHECK(table.get("b", 100)._value == "new");
}
} // namespace

extern "C" int __real_fsync(int);
extern "C" int __real_pthread_rwlock_rdlock(pthread_rwlock_t *);
extern "C" int __real_pthread_rwlock_wrlock(pthread_rwlock_t *);

extern "C" int __wrap_pthread_rwlock_rdlock(pthread_rwlock_t *lock) {
	int result = __real_pthread_rwlock_rdlock(lock);
	if (result == 0 && role == Role::CaptureLocks && captured_locks < table_locks.size()) {
		table_locks[captured_locks++] = lock;
	}
	if (result == 0 && role == Role::SizeReader && lock == table_locks[0]) {
		signal(&Gate::reader_paused);
		if (!wait_for(&Gate::resume_reader)) { ::_exit(3); }
	}
	return result;
}

extern "C" int __wrap_pthread_rwlock_wrlock(pthread_rwlock_t *lock) {
	if (role == Role::OtherOperation) { signal(&Gate::operation_attempted); }
	if (role == Role::Flush && lock == table_locks[0]) {
		signal(&Gate::flush_attempted_active);
	}
	return __real_pthread_rwlock_wrlock(lock);
}

extern "C" int __wrap_fsync(int fd) {
	if (role == Role::Flush) {
		struct stat info{};
		if (::fstat(fd, &info) == 0) {
			if (io_mode == IoMode::PauseFileSync && S_ISREG(info.st_mode)) {
				signal(&Gate::build_paused);
				if (!wait_for(&Gate::resume_build)) { errno = ETIMEDOUT; return -1; }
			} else if ((io_mode == IoMode::FailFileSync && S_ISREG(info.st_mode)) ||
					   (io_mode == IoMode::FailDirectorySync && S_ISDIR(info.st_mode))) {
				io_failure_injected = true;
				io_mode = IoMode::Normal;
				errno = EIO;
				return -1;
			}
		}
	}
	return __real_fsync(fd);
}

// GNU/Clang Itanium ABI symbol used by this Linux-only test target. Throw at
// new active's constructor (including the bad_alloc path of its allocations).
using Cmp = std::shared_ptr<const Comparator<std::string>>;
extern "C" void real_skiplist_constructor(SkipList *, Cmp, int)
	asm("__real__ZN8SkipListC1ESt10shared_ptrIK10ComparatorINSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEEEi");
extern "C" void wrapped_skiplist_constructor(SkipList *, Cmp, int)
	asm("__wrap__ZN8SkipListC1ESt10shared_ptrIK10ComparatorINSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEEEi");
extern "C" void wrapped_skiplist_constructor(SkipList *self, Cmp cmp, int seed) {
	if (fail_skiplist_construction) {
		fail_skiplist_construction = false;
		construction_failed = true;
		throw std::bad_alloc();
	}
	real_skiplist_constructor(self, std::move(cmp), seed);
}

int main() {
	struct Case { const char *name; void (*run)(const std::string &); };
	const Case cases[] = {
		{"active file sync failure retains table and ID", io_failure_preserves_table<false, IoMode::FailFileSync>},
		{"frozen file sync failure retains table and ID", io_failure_preserves_table<true, IoMode::FailFileSync>},
		{"active directory sync failure retains table and ID", io_failure_preserves_table<false, IoMode::FailDirectorySync>},
		{"frozen directory sync failure retains table and ID", io_failure_preserves_table<true, IoMode::FailDirectorySync>},
		{"active oversized key aborts flush and retains data", oversized_record_aborts_flush_without_losing_data<false, true>},
		{"frozen oversized key aborts flush and retains data", oversized_record_aborts_flush_without_losing_data<true, true>},
		{"active oversized value aborts flush and retains data", oversized_record_aborts_flush_without_losing_data<false, false>},
		{"frozen oversized value aborts flush and retains data", oversized_record_aborts_flush_without_losing_data<true, false>},
		{"write during active build survives", concurrent_write_survives_flush<false, false>},
		{"freeze during active build preserves table IDs", concurrent_write_survives_flush<false, true>},
		{"write during frozen build survives", concurrent_write_survives_flush<true, false>},
		{"concurrent flushes remove distinct tables", concurrent_flushes_keep_distinct_tables},
		{"size query and flush do not deadlock", size_query_and_flush_do_not_deadlock},
		{"new active construction failure retains state", new_active_failure_preserves_state},
		{"exhausted ID retains table bytes and ID", exhausted_id_preserves_state},
		{"last valid ID transition does not wrap", last_id_transition_does_not_wrap},
	};
	int failures = 0;
	for (const auto &test : cases) {
		std::cout.flush();
		char directory[] = "/tmp/atlasdb-memtable-flush-XXXXXX";
		if (!::mkdtemp(directory)) { return 2; }
		pid_t child = ::fork();
		if (child < 0) {
			std::filesystem::remove_all(directory);
			return 2;
		}
		if (child == 0) {
			::alarm(8); // isolate deadlocks/UB so all regression cases still run
			try {
				test.run(directory);
				::_exit(0);
			} catch (const std::exception &error) {
				std::cerr << test.name << ": " << error.what() << '\n';
				::_exit(1);
			}
		}
		int status = 0;
		pid_t waited;
		do { waited = ::waitpid(child, &status, 0); }
		while (waited == -1 && errno == EINTR);
		std::filesystem::remove_all(directory);
		if (waited != child) { return 2; }
		bool passed = WIFEXITED(status) && WEXITSTATUS(status) == 0;
		std::cout << (passed ? "[PASS] " : "[FAIL] ") << test.name;
		if (WIFSIGNALED(status)) { std::cout << " (signal " << WTERMSIG(status) << ')'; }
		std::cout << '\n';
		if (!passed) { ++failures; }
	}
	std::cout << std::size(cases) - failures << '/' << std::size(cases) << " tests passed\n";
	return failures ? 1 : 0;
}
