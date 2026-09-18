#include "lsm.h"
#include "wal.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

// Linux-only link-time fault injection: affects only WAL descriptors in this
// test's temporary directory. Production sources and private members stay intact.
#ifdef ATLASDB_WAL_FAULT_INJECTION
namespace fault {
enum class Mode { None, WriteError, ShortWrite, SyncError, EintrRetry, ZeroWrite };
Mode mode = Mode::None;
std::string prefix;
int remaining = 0;
int writes = 0;
int syncs = 0;

bool is_wal(int fd) {
    if (prefix.empty()) return false;
    char path[4096];
    const auto link = "/proc/self/fd/" + std::to_string(fd);
    const auto n = ::readlink(link.c_str(), path, sizeof(path));
    return n > 0 && std::string(path, static_cast<std::size_t>(n)).rfind(prefix, 0) == 0;
}
void arm(const std::string &dir, Mode selected, int nth) {
    prefix = dir + "/wal-";
    mode = selected;
    remaining = nth;
    writes = syncs = 0;
}
// Crash-at-Nth-fsync support: kills this process at a chosen durability
// point, scoped to descriptors under one directory (its files and itself).
std::string exit_dir;
int exit_nth = 0;
int exits_seen = 0;
bool is_exit_target(int fd) {
    if (exit_dir.empty()) return false;
    char path[4096];
    const auto link = "/proc/self/fd/" + std::to_string(fd);
    const auto n = ::readlink(link.c_str(), path, sizeof(path));
    if (n <= 0) return false;
    const std::string resolved(path, static_cast<std::size_t>(n));
    return resolved == exit_dir || resolved.rfind(exit_dir + "/", 0) == 0;
}
void arm_exit(const std::string &dir, int nth) {
    exit_dir = dir;
    exit_nth = nth;
    exits_seen = 0;
}

void reset() {
    prefix.clear();
    mode = Mode::None;
    exit_dir.clear();
    exit_nth = 0;
}
} // namespace fault
extern "C" ssize_t __real_write(int, const void *, size_t);
extern "C" int __real_fsync(int);
extern "C" ssize_t __wrap_write(int fd, const void *data, size_t size) {
    if (fault::is_wal(fd)) {
        ++fault::writes;
        if ((fault::mode == fault::Mode::WriteError ||
             fault::mode == fault::Mode::ShortWrite ||
             fault::mode == fault::Mode::EintrRetry ||
             fault::mode == fault::Mode::ZeroWrite) && --fault::remaining == 0) {
            const auto mode = fault::mode;
            fault::mode = fault::Mode::None; // following I/O could succeed
            if (mode == fault::Mode::ShortWrite)
                return __real_write(fd, data, size / 2);
            if (mode == fault::Mode::EintrRetry) {
                errno = EINTR; // interrupted before writing anything: retryable
                return -1;
            }
            if (mode == fault::Mode::ZeroWrite)
                return 0; // zero progress on a nonzero request
            errno = EIO;
            return -1;
        }
    }
    return __real_write(fd, data, size);
}
extern "C" int __wrap_fsync(int fd) {
    if (fault::is_exit_target(fd) && ++fault::exits_seen == fault::exit_nth) {
        ::_exit(0); // simulate power loss at this durability point
    }
    if (fault::is_wal(fd)) {
        ++fault::syncs;
        if (fault::mode == fault::Mode::SyncError && --fault::remaining == 0) {
            fault::mode = fault::Mode::None;
            errno = EIO;
            return -1;
        }
    }
    return __real_fsync(fd);
}
#endif

namespace {
#define CHECK(expr) do { if (!(expr)) throw std::runtime_error( \
    "line " + std::to_string(__LINE__) + ": " #expr); } while (false)

struct TempDir {
    std::string path;
    TempDir() {
        std::string pattern = (std::filesystem::temp_directory_path() /
                               "atlasdb-wal-test-XXXXXX").string();
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

using Results = std::vector<std::optional<std::uint64_t>>;
void check_success(const Results &results, std::size_t count) {
    CHECK(results.size() == count);
    std::uint64_t previous = 0;
    for (const auto &seq : results) {
        CHECK(seq && *seq > previous);
        previous = *seq;
    }
}
void check_rejected(const Results &results, std::size_t count) {
    CHECK(results.size() == count);
    for (const auto &seq : results) CHECK(!seq);
}
std::vector<std::uint64_t> wal_ids(const std::string &dir) {
    std::vector<std::uint64_t> result;
    for (const auto &entry : std::filesystem::directory_iterator(dir)) {
        auto name = entry.path().filename().string();
        if (entry.is_regular_file() && name.rfind("wal-", 0) == 0)
            result.push_back(std::stoull(name.substr(4)));
    }
    std::sort(result.begin(), result.end());
    return result;
}
void write_segment(const std::string &dir, std::uint64_t id,
                   const std::vector<std::uint8_t> &bytes) {
    FileObj::create_and_write(WAL::get_filepath(dir, id), bytes);
}
std::vector<std::uint8_t> record(const std::string &key, const std::string &value,
                               std::uint64_t seq, ValueType type = Value) {
    return WAL::encode(InternalKey::encode(InternalKey(key, seq, type)), value);
}
void wait_child(pid_t pid) {
    int status = 0;
    pid_t result;
    do { result = ::waitpid(pid, &status, 0); } while (result < 0 && errno == EINTR);
    CHECK(result == pid);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
// Child uses _exit while engine is alive, so neither close nor destructor can
// flush its MemTable. This tests process termination, not power loss.
void write_without_close(const std::string &dir,
                         const std::function<void(LSMEngine &)> &action,
                         std::size_t active_limit = 64) {
    const auto pid = ::fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        try {
            LSMEngine engine(dir, 1024 * 1024, active_limit, 100);
            action(engine);
            ::_exit(0);
        } catch (const std::exception &e) {
            std::cerr << "child: " << e.what() << '\n';
            ::_exit(1);
        }
    }
    wait_child(pid);
}

void test_batch_results_and_versions() {
    TempDir dir;
    LSMEngine db(dir.path, 1024 * 1024, 1024, 100);
    const std::string binary_key("a\0b", 3), binary_value("v\0x", 3);
    auto seqs = db.put_batch({{"same", "old"}, {"same", "new"},
                              {binary_key, binary_value}, {"empty", ""}});
    check_success(seqs, 4);
    CHECK(db.get("same", *seqs[0])._value == "old");
    CHECK(db.get("same")._value == "new");
    CHECK(db.get(binary_key)._value == binary_value);
    CHECK(db.get("empty")._value_type == Value);
    auto deleted = db.remove_batch({"same", "absent", "same"});
    check_success(deleted, 3);
    CHECK(*deleted.front() > *seqs.back());
    CHECK(db.get("same")._value_type == TombStone);
    CHECK(db.get("absent")._value_type == TombStone);
    CHECK(db.get("same", *seqs[1])._value == "new");
    CHECK(db.put_batch({}).empty() && db.remove_batch({}).empty());
    CHECK(db.put_batch({{"reject", std::string(65536, 'x')},
                         {"not_attempted", "v"}}).empty());
    CHECK(db.get("reject")._value_type == NotFound);
    CHECK(db.get("not_attempted")._value_type == NotFound);
    db.close();
    check_rejected(db.put_batch({{"later", "v"}, {"later2", "v"}}), 2);
    check_rejected(db.remove_batch({"same", "empty"}), 2);
}

void test_batch_segment_recovery_and_reclamation() {
    TempDir dir;
    write_without_close(dir.path, [](LSMEngine &db) {
        std::vector<std::pair<std::string, std::string>> values;
        for (int i = 0; i < 8; ++i)
            values.push_back({"key" + std::to_string(i), std::string(20, 'a' + i)});
        auto put = db.put_batch(values);
        check_success(put, 8);
        CHECK(put.front() == 1 && put.back() == 8);
        check_success(db.remove_batch({"key0", "key2", "key4", "key6"}), 4);
    });
    const auto before = wal_ids(dir.path);
    CHECK(before.size() >= 4); // both put_batch and remove_batch crossed freezes
    for (const auto &entry : std::filesystem::directory_iterator(dir.path))
        CHECK(entry.path().filename().string().rfind("sst_", 0) != 0);
    LSMEngine reopened(dir.path, 1024 * 1024, 64, 100);
    for (int i = 0; i < 8; ++i) {
        auto value = reopened.get("key" + std::to_string(i));
        CHECK(value._value_type == (i % 2 == 0 ? TombStone : Value));
        if (i % 2) CHECK(value._value == std::string(20, 'a' + i));
    }
    auto seq = reopened.put("after", "reopen");
    CHECK(seq == 13); // seq.txt remained at 1 while WAL reached 12
    auto all_ids = wal_ids(dir.path);
    CHECK(all_ids.size() == before.size() + 1);
    CHECK(all_ids.back() == before.back() + 1);
    CHECK(reopened.flush() > 0);
    auto after = wal_ids(dir.path);
    CHECK(after.size() + 1 == all_ids.size());
    CHECK(!std::filesystem::exists(WAL::get_filepath(dir.path, before.front())));
    for (std::size_t i = 1; i < all_ids.size(); ++i)
        CHECK(std::filesystem::exists(WAL::get_filepath(dir.path, all_ids[i])));
    CHECK(reopened.get("key1")._value == std::string(20, 'b'));
    CHECK(reopened.get("key0")._value_type == TombStone);
}

void test_active_flush_then_crash() {
    TempDir dir;
    write_without_close(dir.path, [&](LSMEngine &db) {
        CHECK(db.put("before", "sst") == 1);
        CHECK(db.flush() == 1);
        CHECK(!std::filesystem::exists(WAL::get_filepath(dir.path, 1)));
        CHECK(std::filesystem::exists(WAL::get_filepath(dir.path, 2)));
        CHECK(db.put("after", "wal") == 2);
        CHECK(db.remove("before") == 3);
    }, 1024);
    LSMEngine db(dir.path);
    CHECK(db.get("after")._value == "wal");
    CHECK(db.get("before")._value_type == TombStone);
    CHECK(db.put("next", "v") == 4);
}

void test_concurrent_batches_recover_across_freezes() {
    TempDir dir;
    write_without_close(dir.path, [](LSMEngine &db) {
        std::vector<std::thread> writers;
        std::vector<std::exception_ptr> errors(4);
        for (int worker = 0; worker < 4; ++worker) {
            writers.emplace_back([&, worker] {
                try {
                    std::vector<std::pair<std::string, std::string>> values;
                    std::vector<std::string> deleted;
                    for (int i = 0; i < 20; ++i) {
                        auto key = std::to_string(worker) + ":" + std::to_string(i);
                        values.push_back({key, "value"});
                        if (i % 2 == 0) deleted.push_back(key);
                    }
                    check_success(db.put_batch(values), values.size());
                    check_success(db.remove_batch(deleted), deleted.size());
                } catch (...) { errors[worker] = std::current_exception(); }
            });
        }
        for (auto &writer : writers) writer.join();
        for (const auto &error : errors) if (error) std::rethrow_exception(error);
    }, 80);
    LSMEngine db(dir.path);
    for (int worker = 0; worker < 4; ++worker) {
        for (int i = 0; i < 20; ++i) {
            auto found = db.get(std::to_string(worker) + ":" + std::to_string(i));
            CHECK(found._value_type == (i % 2 == 0 ? TombStone : Value));
            if (i % 2) CHECK(found._value == "value");
        }
    }
    CHECK(db.put("next", "v") == 121);
}

void test_unordered_segments_and_byte_accounting() {
    TempDir dir;
    write_segment(dir.path, 9, record("same", "new", 20));
    write_segment(dir.path, 2, record("same", "old", 10));
    MemTable mem;
    WAL wal;
    CHECK(wal.load_wal(dir.path, mem) == 20);
    CHECK(mem.get("same", 20)._value == "new");
    CHECK(mem.get("same", 10)._value == "old");
    CHECK(mem.get_total_size() == 2 * (4 + 3 + 8));
    CHECK(std::filesystem::exists(WAL::get_filepath(dir.path, 10)));
    SSTBuilder builder;
    std::vector<std::uint64_t> seqs;
    auto result = mem.flush_last(builder, dir.path + "/old.sst", 1, seqs);
    CHECK(result._sst && result._flush_id == 2 && !result._new_active_id);
    CHECK(seqs == std::vector<std::uint64_t>{10});
    CHECK(mem.get_total_size() == 15);
    CHECK(mem.get("same", 20)._value == "new");
}

void test_every_truncated_tail_boundary() {
    const auto first = record("good", "prefix", 7);
    const auto last = record("tail", "payload", 100);
    for (std::size_t length = 0; length < last.size(); ++length) {
        TempDir dir;
        auto bytes = first;
        bytes.insert(bytes.end(), last.begin(), last.begin() + length);
        write_segment(dir.path, 1, bytes);
        LSMEngine db(dir.path);
        CHECK(db.get("good")._value == "prefix");
        CHECK(db.get("tail")._value_type == NotFound);
        CHECK(db.put("next", "v") == 8);
    }
}

void test_bad_final_crc_rejects_recovery() {
    TempDir dir;
    auto bytes = record("good", "v", 7);
    auto damaged = record("bad", "v", 100);
    damaged.back() ^= 1;
    bytes.insert(bytes.end(), damaged.begin(), damaged.end());
    write_segment(dir.path, 1, bytes);
    // Unlike a truncated tail, a complete record with a bad CRC rejects open.
    bool rejected = false;
    try {
        LSMEngine db(dir.path);
    } catch (const std::runtime_error &e) {
        CHECK(std::string(e.what()) == "crc check failed");
        rejected = true;
    }
    CHECK(rejected);
}

void test_watermarks() {
    for (const std::uint64_t stored : {std::uint64_t{0}, std::uint64_t{3},
                                       std::uint64_t{200}}) {
        TempDir dir;
        write_segment(dir.path, 3, record("value", "v", 40));
        write_segment(dir.path, 8, record("deleted", "", 90, TombStone));
        if (stored)
            FileObj::create_and_write(dir.path + "/seq.txt", get_bytes_of_uint64t(stored));
        {
            LSMEngine db(dir.path);
            CHECK(db.get("value")._value == "v");
            CHECK(db.get("deleted")._value_type == TombStone);
            CHECK(db.put("next", "v") == std::max(stored, std::uint64_t{91}));
        }
        LSMEngine reopened(dir.path);
        CHECK(reopened.put("again", "v") == std::max(stored, std::uint64_t{91}) + 1);
    }
    TempDir empty;
    LSMEngine db(empty.path);
    CHECK(db.put("first", "v") == 1);
}

void test_memtable_generation_results_and_clear() {
    TempDir dir;
    MemTable mem(20);
    auto put = mem.put("a", std::string(12, 'v'), 1); // 21B: freeze generation 0
    CHECK(put._is_add && put._frozen_list_id == 0 && put._new_active_list_id == 1);
    CHECK(!mem.remove("x", 2)._frozen_list_id); // 9B
    CHECK(!mem.remove("y", 3)._frozen_list_id); // 18B
    auto removed = mem.remove("z", 4); // 27B: freeze generation 1
    CHECK(removed._is_success && removed._frozen_list_id == 1 &&
          removed._new_active_list_id == 2);
    mem.clear();
    CHECK(mem.get_total_size() == 0);
    CHECK(mem.put("fresh", "v", 5)._is_add);
    SSTBuilder builder;
    std::vector<std::uint64_t> seqs;
    auto flushed = mem.flush_last(builder, dir.path + "/fresh.sst", 10, seqs);
    CHECK(flushed._sst && flushed._flush_id == 0 && flushed._new_active_id == 1);
    CHECK(seqs == std::vector<std::uint64_t>{5});
}

void test_freeze_rotation_open_failure_latches_error() {
    TempDir dir;
    LSMEngine db(dir.path, 1024 * 1024, 20, 100);
    // open_rw on this directory fails; no permissions/root assumptions needed.
    std::filesystem::create_directory(WAL::get_filepath(dir.path, 2));
    auto result = db.put_batch({{"a", std::string(12, 'v')}, {"b", "v"}});
    CHECK(result.size() == 2 && result[0] == 1 && !result[1]);
    CHECK(db.get("a")._value == std::string(12, 'v'));
    CHECK(db.get("b")._value_type == NotFound);
    std::filesystem::remove(WAL::get_filepath(dir.path, 2));
    CHECK(!db.put("later", "v")); // fixing the external cause must not reset state
    CHECK(!db.remove("a"));
    check_rejected(db.put_batch({{"later", "v"}}), 1);
    check_rejected(db.remove_batch({"a"}), 1);
}

void test_sst_build_failure_preserves_memtable() {
    for (bool frozen : {false, true}) {
        TempDir dir;
        MemTable mem(frozen ? 20 : 1024);
        const std::string value(12, 'v');
        auto inserted = mem.put("a", value, 1); // 21B
        CHECK(inserted._is_add);
        CHECK(inserted._frozen_list_id.has_value() == frozen);
        if (frozen) CHECK(mem.put("b", "v", 2)._is_add); // keep active populated too
        const auto bytes_before = mem.get_total_size();

        // Opening a directory as an SST output fails even when run as root.
        const auto blocked_path = dir.path + "/blocked.sst";
        std::filesystem::create_directory(blocked_path);
        bool threw = false;
        try {
            SSTBuilder builder;
            std::vector<std::uint64_t> seqs;
            mem.flush_last(builder, blocked_path, 1, seqs);
        } catch (const std::runtime_error &) { threw = true; }
        CHECK(threw);
        CHECK(mem.get_total_size() == bytes_before);
        CHECK(mem.get("a", 1)._value_type == Value);
        CHECK(mem.get("a", 1)._value == value);
        if (frozen) CHECK(mem.get("b", 2)._value == "v");

        // A fresh builder retries the same generation, without lost bytes or IDs.
        SSTBuilder retry_builder;
        std::vector<std::uint64_t> retry_seqs;
        auto result = mem.flush_last(retry_builder, dir.path + "/retry.sst", 2,
                                     retry_seqs);
        CHECK(result._sst && result._flush_id == 0);
        CHECK(retry_seqs == std::vector<std::uint64_t>{1});
        CHECK(mem.get_total_size() == bytes_before - 21);
        auto it = result._sst->begin(std::nullopt, true);
        CHECK(it.is_valid());
        CHECK(InternalKey::decode((*it).first)._key == "a");
        CHECK((*it).second == value);
        if (frozen) {
            CHECK(!result._new_active_id);
            CHECK(mem.get("b", 2)._value == "v");
        } else {
            CHECK(result._new_active_id == 1);
        }
        CHECK(mem.put("c", "v", 3)._is_add); // b + c is 20B: no extra freeze
        SSTBuilder next_builder;
        std::vector<std::uint64_t> next_seqs;
        auto next = mem.flush_last(next_builder, dir.path + "/next.sst", 3,
                                   next_seqs);
        CHECK(next._sst && next._flush_id == 1 && next._new_active_id == 2);
        CHECK(mem.get_total_size() == 0);
    }
}

void test_active_flush_rotation_open_failure_latches_error() {
    TempDir dir;
    {
        LSMEngine db(dir.path, 1024 * 1024, 1024, 100);
        CHECK(db.put("saved", "value") == 1);
        std::filesystem::create_directory(WAL::get_filepath(dir.path, 2));
        // The SST is already registered, but opening its replacement WAL fails.
        CHECK(db.flush() == 1);
        CHECK(!std::filesystem::exists(WAL::get_filepath(dir.path, 1)));
        CHECK(db.get("saved")._value == "value");
        std::filesystem::remove(WAL::get_filepath(dir.path, 2));
        CHECK(!db.put("lost_if_written", "value"));
        CHECK(!db.remove("saved"));
        check_rejected(db.put_batch({{"later", "v"}}), 1);
        check_rejected(db.remove_batch({"saved"}), 1);
        CHECK(db.get("lost_if_written")._value_type == NotFound);
    }
    LSMEngine reopened(dir.path);
    CHECK(reopened.get("saved")._value == "value");
    CHECK(reopened.get("lost_if_written")._value_type == NotFound);
}

void test_concurrent_flush_keeps_watermark_monotonic() {
    // Concurrent threshold flushes race on seq.txt: publishes are serialized
    // under the flush locks, so the file must never regress below any
    // confirmed seq, no publish (rename) may fail, and a reopen must hand
    // out seqs strictly above every confirmed one.
    TempDir dir;
    constexpr int kThreads = 4, kPuts = 40;
    std::uint64_t max_seq = 0;
    {
        auto engine = std::make_shared<LSMEngine>(dir.path, 512, 1024, 8);
        std::vector<std::thread> workers;
        std::vector<Results> seqs(
            kThreads, std::vector<std::optional<std::uint64_t>>(kPuts));
        for (int t = 0; t < kThreads; t++) {
            workers.emplace_back([&, t] {
                for (int i = 0; i < kPuts; i++) {
                    seqs[t][i] = engine->put("t" + std::to_string(t) + "-" +
                                                 std::to_string(i),
                                             "value-" + std::to_string(i));
                }
            });
        }
        for (auto &worker : workers) worker.join();
        for (const auto &results : seqs) {
            for (const auto &seq : results) {
                CHECK(seq);
                max_seq = std::max(max_seq, *seq);
            }
        }
        for (int t = 0; t < kThreads; t++) {
            for (int i = 0; i < kPuts; i++) {
                const auto found = engine->get("t" + std::to_string(t) + "-" +
                                               std::to_string(i));
                CHECK(found._value_type == Value);
                CHECK(found._value == "value-" + std::to_string(i));
            }
        }
    } // close(): drain + final publish
    {
        FileObj f = FileObj::open_read(dir.path + "/seq.txt");
        CHECK(f.size() == 8);
        CHECK(f.read_uint64(0) > max_seq);
        f.close();
    }
    {
        auto engine = std::make_shared<LSMEngine>(dir.path);
        const auto next = engine->put("after-reopen", "v");
        CHECK(next && *next == max_seq + 1);
        for (int t = 0; t < kThreads; t++) {
            for (int i = 0; i < kPuts; i++) {
                CHECK(engine->get("t" + std::to_string(t) + "-" +
                                  std::to_string(i))
                          ._value == "value-" + std::to_string(i));
            }
        }
    }
}

void test_leftover_tmp_files_are_ignored_on_recovery() {
    TempDir dir;
    {
        LSMEngine db(dir.path);
        CHECK(db.put("persisted", "v") == 1);
        CHECK(db.flush() == 1);
    }
    std::string sst_name;
    for (const auto &entry : std::filesystem::directory_iterator(dir.path)) {
        const auto name = entry.path().filename().string();
        if (name.rfind("sst_", 0) == 0) {
            sst_name = name;
            break;
        }
    }
    CHECK(!sst_name.empty());
    // Garbage left by a crash between publish steps must not be parsed as data.
    FileObj::create_and_write(dir.path + "/" + sst_name + ".tmp",
                              {'g', 'a', 'r', 'b', 'a', 'g', 'e'});
    FileObj::create_and_write(dir.path + "/seq.txt.tmp", {'j', 'u', 'n', 'k'});
    {
        LSMEngine db(dir.path);
        CHECK(db.decode_sst_path(dir.path + "/" + sst_name + ".tmp") ==
              std::nullopt);
        CHECK(db.get("persisted")._value == "v");
        CHECK(db.put("next", "v2") == 2);
    }
    {
        LSMEngine db(dir.path);
        CHECK(db.get("persisted")._value == "v");
        CHECK(db.get("next")._value == "v2");
    }
}

std::size_t open_fd_count() {
    // directory_iterator itself holds one descriptor; both counts include it.
    std::size_t count = 0;
    for (const auto &entry :
         std::filesystem::directory_iterator("/proc/self/fd")) {
        (void)entry;
        ++count;
    }
    return count;
}

void test_wal_segment_creation_leaks_no_descriptor() {
#if defined(__linux__)
    // Every new segment opens its file fd plus transient directory fds for
    // the creation fsyncs; none of them may survive the WAL object.
    TempDir dir;
    const std::size_t before = open_fd_count();
    for (int i = 0; i < 15; i++) {
        WAL flat(dir.path, i + 1); // create branch inside an existing dir
        WAL nested(dir.path + "/nested",
                   i + 1); // first round also runs create_directories + its
                           // parent-dir fsync
    }
    CHECK(open_fd_count() == before);
#else
    std::cout << "[SKIP] /proc/self/fd descriptor accounting unavailable\n";
#endif
}

void test_manifest_bootstrap_migration() {
    // A pre-manifest directory (manifest removed) must be bootstrapped: the
    // legacy scan result is written as the initial manifest, data survives,
    // and every later open goes through the manifest path.
    TempDir dir;
    const auto manifest_path = dir.path + "/manifest.txt";
    {
        LSMEngine db(dir.path, 1024 * 1024, 1024, 100);
        CHECK(db.put("legacy", "v") == 1);
        CHECK(db.flush() == 1);
    }
    CHECK(std::filesystem::exists(manifest_path));
    std::filesystem::remove(manifest_path); // simulate a pre-manifest version
    {
        LSMEngine db(dir.path);
        CHECK(db.get("legacy")._value == "v");
        CHECK(db.put("next", "v2").has_value());
    }
    CHECK(std::filesystem::exists(manifest_path)); // bootstrap rewrote it
    {
        LSMEngine db(dir.path);
        CHECK(db.get("legacy")._value == "v");
        CHECK(db.get("next")._value == "v2");
    }
}

void test_manifest_ignores_resurrected_sst() {
    // A stale SST file resurrected after a compaction deleted it must stay
    // invisible: recovery loads only manifest-referenced tables. Without the
    // manifest filter the resurrected L0 table would shadow the newer value.
    TempDir dir;
    const auto backup = dir.path + "/resurrect.bak";
    {
        LSMEngine db(dir.path, 256, 1024, 2);
        CHECK(db.put("a", "old") == 1);
        CHECK(db.flush() == 1); // sst_0 with a=old
        std::string first_sst;
        for (const auto &entry : std::filesystem::directory_iterator(dir.path)) {
            const auto name = entry.path().filename().string();
            if (name.rfind("sst_", 0) == 0) {
                first_sst = entry.path().string();
                break;
            }
        }
        CHECK(!first_sst.empty());
        std::filesystem::copy_file(first_sst, backup);
        CHECK(db.put("a", "new") == 2);
        CHECK(db.flush() == 2); // sst_1; L0 reaches threshold 2 -> compaction
        CHECK(db.get("a")._value == "new");
        CHECK(!std::filesystem::exists(first_sst)); // old file retired
    }
    // Crash-after-publish-before-delete residue: bring the stale file back
    // under its original name (id 0, level 0, 32-digit zero padding).
    const auto first_sst = dir.path + "/sst_" + std::string(32, '0') + ".0";
    std::filesystem::copy_file(backup, first_sst);
    {
        LSMEngine db(dir.path);
        CHECK(db.get("a")._value == "new"); // resurrected table must be ignored
        CHECK(db.put("post", "v").has_value());
    }
}

void test_manifest_corrupt_line_rejects_open() {
    TempDir dir;
    {
        LSMEngine db(dir.path, 1024 * 1024, 1024, 100);
        CHECK(db.put("k", "v").has_value());
        CHECK(db.flush() == 1);
    }
    FileObj::atomic_write(dir.path + "/manifest.txt",
                          {'g', 'a', 'r', 'b', 'a', 'g', 'e', '\n'});
    bool rejected = false;
    try {
        LSMEngine db(dir.path);
    } catch (const std::runtime_error &) {
        rejected = true;
    }
    CHECK(rejected);
}

std::string find_first_sst(const std::string &dir) {
    for (const auto &entry : std::filesystem::directory_iterator(dir)) {
        if (entry.path().filename().string().rfind("sst_", 0) == 0) {
            return entry.path().string();
        }
    }
    return "";
}

void test_manifest_missing_sst_rejects_open() {
    // The manifest references a table whose file is gone: silent skip would
    // lose data; the constructor must reject the open instead.
    TempDir dir;
    std::string sst_path;
    {
        LSMEngine db(dir.path, 1024 * 1024, 1024, 100);
        CHECK(db.put("k", "v") == 1);
        CHECK(db.flush() == 1);
        sst_path = find_first_sst(dir.path);
    }
    CHECK(!sst_path.empty());
    std::filesystem::remove(sst_path);
    bool rejected = false;
    try {
        LSMEngine db(dir.path);
    } catch (const std::runtime_error &) {
        rejected = true;
    }
    CHECK(rejected);
}

void test_manifest_corrupt_sst_rejects_open() {
    // The file exists and its name matches the manifest, but its bytes are
    // garbage: only counting successful SST::open results catches this.
    TempDir dir;
    std::string sst_path;
    {
        LSMEngine db(dir.path, 1024 * 1024, 1024, 100);
        CHECK(db.put("k", "v") == 1);
        CHECK(db.flush() == 1);
        sst_path = find_first_sst(dir.path);
    }
    CHECK(!sst_path.empty());
    FileObj::atomic_write(sst_path, {'g', 'a', 'r', 'b', 'a', 'g', 'e'});
    bool rejected = false;
    try {
        LSMEngine db(dir.path);
    } catch (const std::runtime_error &) {
        rejected = true;
    }
    CHECK(rejected);
}

void test_compact_delete_failure_continues() {
    // SST file deletion fails (its path is occupied by a non-empty directory):
    // the already-open fd keeps the old table readable through the merge, the
    // failed retire must be logged and skipped, compaction completes, the
    // in-memory index stays consistent, and the residue is shielded by the
    // manifest on the next open.
    TempDir dir;
    std::string sst0;
    {
        LSMEngine db(dir.path, 256, 1024, 2);
        CHECK(db.put("k1", "v1") == 1);
        CHECK(db.flush() == 1);
        sst0 = find_first_sst(dir.path);
        CHECK(!sst0.empty());
        std::filesystem::remove(sst0);
        std::filesystem::create_directories(sst0 + "/occupied");
        CHECK(db.put("k2", "v2") == 2);
        CHECK(db.flush() == 2); // L0 reaches 2 -> compaction, retire of 0 fails
        CHECK(db.get("k1")._value == "v1");
        CHECK(db.get("k2")._value == "v2");
        CHECK(db.put("after", "v") == 3); // write path untouched
    }
    {
        LSMEngine db(dir.path);
        CHECK(db.get("k1")._value == "v1");
        CHECK(db.get("k2")._value == "v2");
        CHECK(db.get("after")._value == "v");
        CHECK(std::filesystem::is_directory(sst0)); // residue stays, invisible
    }
}

#ifdef ATLASDB_WAL_FAULT_INJECTION
void test_batch_failure(fault::Mode mode, bool deleting) {
    TempDir dir;
    LSMEngine db(dir.path, 1024 * 1024, 1024, 100);
    if (deleting) check_success(db.put_batch({{"a", "old"}, {"b", "old"}, {"c", "old"}}), 3);
    const auto wal_path = WAL::get_filepath(dir.path, 1);
    const auto size_before = std::filesystem::file_size(wal_path);
    fault::arm(dir.path, mode, 2);
    auto result = deleting ? db.remove_batch({"a", "b", "c"})
                           : db.put_batch({{"a", "v"}, {"b", "v"}, {"c", "v"}});
    if (mode == fault::Mode::ShortWrite || mode == fault::Mode::EintrRetry) {
        // A short write is retried transparently; an EINTR is a retryable
        // interruption before any byte landed. Both must complete the record
        // from scratch or from the written prefix: the whole batch is
        // confirmed and the write path stays alive instead of latching.
        check_success(result, 3);
        const auto first = *result[0];
        const auto type = deleting ? TombStone : Value;
        const auto value = deleting ? "" : "v";
        const auto total = record("a", value, first, type).size() +
                           record("b", value, first + 1, type).size() +
                           record("c", value, first + 2, type).size();
        CHECK(std::filesystem::file_size(wal_path) == size_before + total);
        CHECK(db.get("a")._value_type == type);
        CHECK(db.get("b")._value_type == type);
        CHECK(db.get("c")._value_type == type);
        if (!deleting) {
            CHECK(db.get("b")._value == "v");
            CHECK(db.get("c")._value == "v");
        }
        CHECK(db.put("later", "v").has_value()); // no latch after retry
        fault::reset();
        return;
    }
    CHECK(result.size() == 3 && result[0] && !result[1] && !result[2]);
    CHECK(fault::writes == 2 && fault::syncs == 2);
    const auto first = *result[0];
    const auto type = deleting ? TombStone : Value;
    const auto value = deleting ? "" : "v";
    const auto first_size = record("a", value, first, type).size();
    const auto second_size = record("b", value, first + 1, type).size();
    const auto expected_tail = mode == fault::Mode::SyncError ? second_size : 0;
    CHECK(std::filesystem::file_size(wal_path) == size_before + first_size + expected_tail);
    CHECK(db.get("a")._value_type == type);
    if (!deleting) CHECK(db.get("a")._value == "v");
    for (const auto &key : {"b", "c"}) {
        const auto found = db.get(key);
        CHECK(found._value_type == (deleting ? Value : NotFound));
        if (deleting) CHECK(found._value == "old");
    }
    // The injected fault was one-shot, so further successful writes would be
    // possible at the OS level. The engine must refuse them before doing I/O.
    CHECK(!db.put("later", "v"));
    CHECK(!db.remove("a"));
    check_rejected(db.put_batch({{"later", "v"}, {"later2", "v"}}), 2);
    check_rejected(db.remove_batch({"a", "b"}), 2);
    CHECK(fault::writes == 2 && fault::syncs == 2);
    fault::reset();
}

void test_atomic_publish_crash_leaves_complete_file() {
    // Power loss at each fsync point of FileObj::atomic_write: the final name
    // must always hold one complete value, never an empty or torn file.
    // fsync #1 lands on seq.txt.tmp (rename not reached yet -> old value),
    // fsync #2 lands on the parent directory (rename done -> new value).
    const std::uint64_t old_value = 7, new_value = 9;
    for (int nth : {1, 2}) {
        TempDir dir;
        const auto seq_path = dir.path + "/seq.txt";
        FileObj::atomic_write(seq_path, get_bytes_of_uint64t(old_value));
        const pid_t pid = ::fork();
        CHECK(pid != -1);
        if (pid == 0) {
            fault::arm_exit(dir.path, nth);
            FileObj::atomic_write(seq_path, get_bytes_of_uint64t(new_value));
            ::_exit(0); // only reached if the exit point never fired
        }
        int status = 0;
        CHECK(::waitpid(pid, &status, 0) == pid);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
        FileObj f = FileObj::open_read(seq_path);
        CHECK(f.is_valid());
        CHECK(f.size() == 8); // never empty, never partial
        CHECK(f.read_uint64(0) == (nth == 1 ? old_value : new_value));
        f.close();
    }
}

void run_manifest_publish_child(const std::string &dir, int nth) {
    LSMEngine db(dir, 256, 1024, 2);
    for (int i = 0; i < 8; i++) {
        db.put("k" + std::to_string(i), "value-" + std::to_string(i));
    }
    // All puts are durable in the WAL (their fsyncs never match the prefix).
    // Arm only around the publish sequence we want to interrupt.
    fault::arm_exit(dir + "/manifest.txt", nth);
    db.flush();               // publish #1 (sst with all 8 keys)
    db.put("m1", "v1");
    db.flush();               // publish #2, then compaction publish #3
    fault::reset();
    ::_exit(0); // only reached when the exit point never fired
}

void test_manifest_publish_crash_preserves_data() {
    // Power loss at each manifest publish fsync (during flush and during
    // compaction): recovery must keep every confirmed write. Killed before
    // the rename, the manifest stays at the previous complete version while
    // the WAL segments and old SST files are all still intact.
    for (int nth : {1, 2, 3}) {
        TempDir dir;
        const pid_t pid = ::fork();
        CHECK(pid != -1);
        if (pid == 0) run_manifest_publish_child(dir.path, nth);
        int status = 0;
        CHECK(::waitpid(pid, &status, 0) == pid);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
        LSMEngine db(dir.path);
        for (int i = 0; i < 8; i++) {
            CHECK(db.get("k" + std::to_string(i))._value ==
                  "value-" + std::to_string(i));
        }
        CHECK(db.get("m1")._value_type == Value);
        CHECK(db.put("post-crash", "v").has_value());
    }
}
#endif
} // namespace

int main() {
    std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"batch results, versions, empty/oversize/closed inputs", test_batch_results_and_versions},
        {"batch freeze, process-exit recovery and WAL reclamation", test_batch_segment_recovery_and_reclamation},
        {"active flush then write and process-exit recovery", test_active_flush_then_crash},
        {"concurrent batches recover across freezes", test_concurrent_batches_recover_across_freezes},
        {"unordered WAL generations and byte accounting", test_unordered_segments_and_byte_accounting},
        {"every truncated final-record boundary", test_every_truncated_tail_boundary},
        {"bad final CRC rejects recovery", test_bad_final_crc_rejects_recovery},
        {"missing/stale/ahead watermark and tombstone maximum", test_watermarks},
        {"put/remove generation results and clear IDs", test_memtable_generation_results_and_clear},
        {"freeze rotation open failure latches write error", test_freeze_rotation_open_failure_latches_error},
        {"active flush rotation open failure latches write error", test_active_flush_rotation_open_failure_latches_error},
        {"SST build failure preserves active/frozen data, bytes and generation", test_sst_build_failure_preserves_memtable},
        {"concurrent flush keeps watermark monotonic", test_concurrent_flush_keeps_watermark_monotonic},
        {"leftover .tmp files ignored on recovery", test_leftover_tmp_files_are_ignored_on_recovery},
        {"WAL segment creation leaks no descriptor", test_wal_segment_creation_leaks_no_descriptor},
        {"manifest bootstrap migration", test_manifest_bootstrap_migration},
        {"manifest ignores resurrected sst", test_manifest_ignores_resurrected_sst},
        {"manifest corrupt line rejects open", test_manifest_corrupt_line_rejects_open},
        {"manifest missing sst rejects open", test_manifest_missing_sst_rejects_open},
        {"manifest corrupt sst rejects open", test_manifest_corrupt_sst_rejects_open},
        {"compact delete failure continues", test_compact_delete_failure_continues},
#ifdef ATLASDB_WAL_FAULT_INJECTION
        {"atomic publish crash leaves complete file", test_atomic_publish_crash_leaves_complete_file},
        {"manifest publish crash preserves data", test_manifest_publish_crash_preserves_data},
#endif
    };
#ifdef ATLASDB_WAL_FAULT_INJECTION
    for (auto mode : {fault::Mode::WriteError, fault::Mode::ShortWrite,
                      fault::Mode::SyncError, fault::Mode::EintrRetry,
                      fault::Mode::ZeroWrite}) {
        for (bool deleting : {false, true}) {
            tests.push_back({std::string(deleting ? "remove_batch" : "put_batch") +
                             " one-shot WAL fault " + std::to_string(static_cast<int>(mode)),
                             [=] { test_batch_failure(mode, deleting); }});
        }
    }
#else
    std::cout << "[SKIP] Linux linker-wrapped write/fsync fault injection unavailable\n";
#endif
    int failed = 0;
    for (const auto &[name, run] : tests) {
        try {
            run();
            std::cout << "[PASS] " << name << std::endl;
        } catch (const std::exception &e) {
            ++failed;
            std::cerr << "[FAIL] " << name << ": " << e.what() << std::endl;
        }
#ifdef ATLASDB_WAL_FAULT_INJECTION
        fault::reset();
#endif
    }
    std::cout << tests.size() - failed << "/" << tests.size() << " passed\n";
    return failed ? 1 : 0;
}
