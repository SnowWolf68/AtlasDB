#include "MemTable.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <mutex>
#include <optional>
#include <pthread.h>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

// Only this Linux executable wraps pthread calls. Capture lock addresses via
// get_total_size(), without exposing or changing production internals.
namespace {
enum class Role { Normal, Capture, Scan, Writer };
enum class Mode { CollectionBoundary, LockOrder };
thread_local Role role = Role::Normal;
thread_local std::size_t scan_requests = 0;
Mode mode = Mode::CollectionBoundary;
std::array<pthread_rwlock_t *, 2> locks{}; // active, frozen
std::size_t captured = 0;
pthread_rwlock_t *first_scan_lock = nullptr;

struct Gate {
	std::mutex mutex;
	std::condition_variable changed;
	bool scan_at_frozen = false;
	bool resume_scan = false;
	bool writer_attempted = false;
	bool writer_done = false;
	bool writer_holds_active = false;
	bool resume_writer = false;
	bool scan_attempted = false;
} gate;

void check(bool condition, const char *expression, int line) {
	if (!condition) {
		throw std::runtime_error("line " + std::to_string(line) + ": " + expression);
	}
}
#define CHECK(expression) check((expression), #expression, __LINE__)

void signal(bool Gate::*flag) {
	std::lock_guard<std::mutex> guard(gate.mutex);
	gate.*flag = true;
	gate.changed.notify_all();
}

bool wait_for(bool Gate::*flag) {
	std::unique_lock<std::mutex> guard(gate.mutex);
	return gate.changed.wait_for(guard, std::chrono::seconds(3),
		[flag] { return gate.*flag; });
}

class Job {
public:
	template <class F> Job(Role worker_role, F work)
		: thread([this, worker_role, work] {
			role = worker_role;
			try { work(); }
			catch (...) { error = std::current_exception(); }
		}) {}
	~Job() {
		if (thread.joinable()) {
			signal(&Gate::resume_scan);
			signal(&Gate::resume_writer);
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

using Entries = std::vector<std::pair<std::string, std::string>>;

std::pair<std::string, std::string> entry(const std::string &key,
		const std::string &value, std::uint64_t seq) {
	return {InternalKey::encode(InternalKey(key, seq, Value)), value};
}

Entries collect(HeapIterator it) {
	Entries result;
	while (it.is_valid()) {
		CHECK(result.size() < 16);
		result.push_back(*it);
		++it;
	}
	return result;
}

const std::string frozen_value(80, 'z');

void seed(MemTable &table) {
	CHECK(table.put("z", frozen_value, 1)._frozen_list_id == 0);
	CHECK(!table.put("a", "old", 2)._frozen_list_id);
	role = Role::Capture;
	table.get_total_size();
	role = Role::Normal;
	CHECK(captured == 2 && locks[0] != locks[1]);
}

enum class Operation { Write, Freeze, Flush };

template <Operation Op>
void collection_is_stable(const std::string &dir) {
	MemTable table(64);
	seed(table);
	std::optional<HeapIterator> snapshot;
	MemTable::PutResult put_result(false);
	MemTable::FlushLastResult flush_result(nullptr, std::nullopt, std::nullopt);
	Job scanning(Role::Scan, [&] {
		// Retain every version so duplicate collection cannot hide behind dedup.
		snapshot.emplace(table.begin(2, true, false));
	});
	CHECK(wait_for(&Gate::scan_at_frozen));
	// New implementation: active is still read-locked. Old split scopes:
	// active has been copied and unlocked, so the competing operation can run.
	int probe = ::pthread_rwlock_trywrlock(locks[0]);
	CHECK(probe == 0 || probe == EBUSY);
	const bool active_available = probe == 0;
	if (active_available) { CHECK(::pthread_rwlock_unlock(locks[0]) == 0); }
	Job writing(Role::Writer, [&] {
		if constexpr (Op == Operation::Flush) {
			SSTBuilder builder;
			std::vector<std::uint64_t> seqs;
			flush_result = table.flush_last(builder, dir + "/frozen.sst", 1, seqs);
		} else {
			put_result = table.put("b", Op == Operation::Freeze
				? std::string(60, 'b') : "new", 3);
		}
		signal(&Gate::writer_done);
	});
	CHECK(wait_for(&Gate::writer_attempted));
	// Force the old split-lock implementation to finish the write/freeze/flush
	// before it reads frozen. The fixed version blocks the writer until copied.
	if (active_available) { CHECK(wait_for(&Gate::writer_done)); }
	signal(&Gate::resume_scan);
	scanning.join();
	writing.join();
	CHECK(snapshot.has_value());
	const Entries expected{entry("a", "old", 2), entry("z", frozen_value, 1)};
	CHECK(collect(*snapshot) == expected);
	// seq=3 is outside this snapshot regardless of locking. This assertion
	// separately checks that collection keeps active protected across both sets.
	CHECK(!active_available);
	if constexpr (Op == Operation::Flush) {
		CHECK(flush_result._sst && flush_result._flush_id == 0);
		auto it = flush_result._sst->begin(std::nullopt, true);
		CHECK(it.is_valid() && *it == entry("z", frozen_value, 1));
		++it;
		CHECK(it == flush_result._sst->end());
		CHECK(collect(table.begin(2, true, false)) == Entries{entry("a", "old", 2)});
	} else {
		CHECK(put_result._is_add);
		if constexpr (Op == Operation::Freeze) {
			CHECK(put_result._frozen_list_id == 1 && put_result._new_active_list_id == 2);
		} else {
			CHECK(!put_result._frozen_list_id && !put_result._new_active_list_id);
		}
		const Entries after{entry("a", "old", 2),
			entry("b", Op == Operation::Freeze ? std::string(60, 'b') : "new", 3),
			entry("z", frozen_value, 1)};
		CHECK(collect(table.begin(3, true, false)) == after);
	}
}

void collection_and_freeze_follow_lock_order(const std::string &) {
	MemTable table(64);
	seed(table);
	mode = Mode::LockOrder;
	std::optional<HeapIterator> snapshot;
	Job writing(Role::Writer, [&] {
		auto result = table.put("b", std::string(60, 'b'), 3);
		CHECK(result._frozen_list_id == 1);
	});
	CHECK(wait_for(&Gate::writer_holds_active));
	Job scanning(Role::Scan, [&] { snapshot.emplace(table.begin(2, true, false)); });
	CHECK(wait_for(&Gate::scan_attempted));
	CHECK(first_scan_lock == locks[0]);
	signal(&Gate::resume_writer);
	writing.join();
	scanning.join();
	const Entries expected{entry("a", "old", 2), entry("z", frozen_value, 1)};
	CHECK(snapshot.has_value() && collect(*snapshot) == expected);
}
} // namespace

extern "C" int __real_pthread_rwlock_rdlock(pthread_rwlock_t *);
extern "C" int __real_pthread_rwlock_wrlock(pthread_rwlock_t *);

extern "C" int __wrap_pthread_rwlock_rdlock(pthread_rwlock_t *lock) {
	if (role == Role::Scan) {
		if (mode == Mode::LockOrder && scan_requests++ == 0) {
			first_scan_lock = lock;
			signal(&Gate::scan_attempted);
			// Reject reversed order deterministically instead of hanging the test
			// while a writer holds active and is about to request frozen.
			if (lock != locks[0]) { return EDEADLK; }
		}
		if (mode == Mode::CollectionBoundary && lock == locks[1]) {
			signal(&Gate::scan_at_frozen);
			if (!wait_for(&Gate::resume_scan)) { return ETIMEDOUT; }
		}
	}
	int result = __real_pthread_rwlock_rdlock(lock);
	if (result == 0 && role == Role::Capture && captured < locks.size()) {
		locks[captured++] = lock;
	}
	return result;
}

extern "C" int __wrap_pthread_rwlock_wrlock(pthread_rwlock_t *lock) {
	if (role == Role::Writer && lock == locks[0]) {
		signal(&Gate::writer_attempted);
	}
	int result = __real_pthread_rwlock_wrlock(lock);
	if (result == 0 && role == Role::Writer && mode == Mode::LockOrder && lock == locks[0]) {
		signal(&Gate::writer_holds_active);
		if (!wait_for(&Gate::resume_writer)) { ::_exit(3); }
	}
	return result;
}

int main() {
	struct Case { const char *name; void (*run)(const std::string &); };
	const Case cases[] = {
		{"collection excludes concurrent active write", collection_is_stable<Operation::Write>},
		{"concurrent freeze does not duplicate collected versions", collection_is_stable<Operation::Freeze>},
		{"concurrent flush does not remove frozen data before collection", collection_is_stable<Operation::Flush>},
		{"collection and freezing acquire active before frozen", collection_and_freeze_follow_lock_order},
	};
	int failures = 0;
	for (const auto &test : cases) {
		std::cout.flush();
		char directory[] = "/tmp/atlasdb-memtable-begin-XXXXXX";
		if (!::mkdtemp(directory)) { return 2; }
		pid_t child = ::fork();
		if (child < 0) {
			std::filesystem::remove_all(directory);
			return 2;
		}
		if (child == 0) {
			::alarm(8);
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
