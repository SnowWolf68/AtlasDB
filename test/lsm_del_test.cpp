#include "lsm.h"
#include "integration_support.h"

namespace {
using namespace integration;
using Result = LSMEngine::DelResult;
void semantics() {
	LSM db("./data");
	CHECK(db.del("absent") == Result::NotFound);
	const auto first_seq = db.put("key", "old");
	CHECK(first_seq.has_value());
	CHECK(*first_seq == 1); // A missing DEL does not consume a sequence number.
	CHECK(db.del("key") == Result::Success);
	CHECK(!db.get("key").is_found());
	CHECK(db.del("key") == Result::NotFound);
	const auto next_seq = db.put("key", "new");
	CHECK(next_seq.has_value());
	CHECK(*next_seq == *first_seq + 2); // Only the successful DEL consumes a sequence.
	CHECK(db.get("key")._value == "new");
	CHECK(db.del("key") == Result::Success);
	CHECK(db.put("", "").has_value());
	CHECK(db.del("") == Result::Success);
	CHECK(db.del("") == Result::NotFound);
	CHECK(db.del(std::string(db.get_max_key_size() + 1, 'x')) == Result::Error);
}
void closed_and_wal_error() {
	{
		LSM db("./closed");
		CHECK(db.put("key", "value").has_value());
		db.close();
		CHECK(db.del("key") == Result::Error);
		CHECK(db.del("missing") == Result::NotFound);
	}
	LSM db("./data");
	CHECK(db.put("key", "value").has_value());
	fail_wal_write = true;
	CHECK(db.del("key") == Result::Error);
	CHECK(injected_errors == 1);
	CHECK(db.get("key")._value == "value");
	CHECK(db.del("missing") == Result::NotFound);
	CHECK(db.del("key") == Result::Error);
}
void concurrent_deletes() {
	LSM db("./data");
	for (int round = 0; round < 30; ++round) {
		CHECK(db.put("shared", "value").has_value());
		std::promise<void> begin;
		auto gate = begin.get_future().share();
		std::vector<std::future<Result>> results;
		std::atomic<int> ready{0};
		for (int i = 0; i < 8; ++i) {
			results.push_back(std::async(std::launch::async, [&] {
				++ready;
				CHECK(gate.wait_for(5s) == std::future_status::ready);
				return db.del("shared");
			}));
		}
		while (ready != 8) { std::this_thread::yield(); }
		begin.set_value();
		int deleted = 0, missing = 0;
		for (auto &result : results) {
			auto value = await(result);
			deleted += value == Result::Success;
			missing += value == Result::NotFound;
		}
		CHECK(deleted == 1 && missing == 7);
		CHECK(!db.get("shared").is_found());
	}
}
void concurrent_put_delete() {
	LSM db("./data");
	for (int round = 0; round < 80; ++round) {
		const auto key = std::to_string(round);
		std::promise<void> begin;
		auto gate = begin.get_future().share();
		std::atomic<int> ready{0};
		auto writer = std::async(std::launch::async, [&] {
			++ready; CHECK(gate.wait_for(5s) == std::future_status::ready);
			CHECK(db.put(key, "value").has_value());
		});
		auto deleter = std::async(std::launch::async, [&] {
			++ready; CHECK(gate.wait_for(5s) == std::future_status::ready);
			return db.del(key);
		});
		while (ready != 2) { std::this_thread::yield(); }
		begin.set_value();
		await(writer);
		const auto deleted = await(deleter);
		const auto found = db.get(key);
		// These are the only two possible serial orderings of put and del.
		if (deleted == Result::Success) { CHECK(!found.is_found()); }
		else { CHECK(deleted == Result::NotFound && found.is_found() && found._value == "value"); }
	}
}
std::size_t sst_count() {
	std::size_t count = 0;
	for (const auto &entry : std::filesystem::directory_iterator("./data")) {
		count += entry.path().filename().string().rfind("sst_", 0) == 0;
	}
	return count;
}
void flush_and_reopen() {
	{
		// A tiny threshold forces del itself to call flush after releasing locks.
		LSMEngine db("./data", 1, 1024, 1000);
		CHECK(db.put("deleted", "old").has_value());
		CHECK(db.put("keep", "value").has_value());
		const auto before = sst_count();
		CHECK(db.del("deleted") == Result::Success);
		CHECK(sst_count() > before);
		const auto after = sst_count();
		CHECK(db.del("missing") == Result::NotFound);
		CHECK(sst_count() == after); // A missing DEL must not write another tombstone.
		CHECK(db.del("deleted") == Result::NotFound);
		CHECK(sst_count() == after); // An existing tombstone also needs no new write.
	}
	LSM db("./data");
	CHECK(!db.get("deleted").is_found());
	CHECK(db.get("keep")._value == "value");
	CHECK(db.del("deleted") == Result::NotFound);
}
void snapshot_and_wal_recovery() {
	// Exit without destructors so recovery must replay WAL, not graceful flush.
	const pid_t writer = ::fork();
	CHECK(writer >= 0);
	if (writer == 0) {
		try {
			LSMEngine db("./data");
			auto before = db.put("deleted", "old");
			CHECK(before.has_value());
			CHECK(db.del("deleted") == Result::Success);
			CHECK(db.get("deleted", *before)._value == "old");
			CHECK(db.get("deleted")._value_type == TombStone);
			CHECK(db.put("keep", "value").has_value());
			::_exit(0);
		} catch (...) { ::_exit(1); }
	}
	int status = 0;
	CHECK(::waitpid(writer, &status, 0) == writer);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	LSM db("./data");
	CHECK(!db.get("deleted").is_found());
	CHECK(db.del("deleted") == Result::NotFound);
	CHECK(db.get("keep")._value == "value");
}
} // namespace
int main(int argc, char **argv) {
	return integration::run({
		{"DEL result semantics", semantics},
		{"closed and WAL error gates", closed_and_wal_error},
		{"concurrent DEL reports exactly one success", concurrent_deletes},
		{"put and DEL serial order", concurrent_put_delete},
		{"DEL flush threshold and reopen", flush_and_reopen},
		{"snapshot visibility and WAL recovery", snapshot_and_wal_recovery},
	}, argc, argv);
}
