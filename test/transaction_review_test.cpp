// Review regressions for the transaction path. Registered as a CTest target
// on Linux (test/CMakeLists.txt links it with -Wl,--wrap=fsync so the RC
// case can pause a committing transaction at its WAL rotation); non-zero
// exit means a regression is reproduced.
#include "TranContext.h"
#include "util.h"
#include "wal.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
std::atomic<bool> armed{false};
std::mutex gate;
std::condition_variable cv;
bool paused = false, resume = false;
struct TempDir {
    std::string path;
    TempDir() {
        char pattern[] = "/tmp/atlasdb-review-XXXXXX";
        auto p = mkdtemp(pattern);
        if (!p) throw std::runtime_error("mkdtemp");
        path = p;
    }
    ~TempDir() { std::filesystem::remove_all(path); }
};
int wait_child(pid_t pid) {
    if (pid < 0) throw std::runtime_error("fork");
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) throw std::runtime_error("waitpid");
    }
    return status;
}
auto engine(const std::string &path) {
    return std::make_shared<LSMEngine>(path, 128, 64, 100);
}
void fill(const std::shared_ptr<TranContext> &tx) {
    for (const auto key : {"a", "b", "c"}) {
        if (!tx->put(key, std::string(80, 'v'))) throw std::runtime_error("put");
    }
}
int count(const std::shared_ptr<LSMEngine> &db) {
    int n = 0;
    for (const auto key : {"a", "b", "c"}) n += db->get(key).is_found();
    return n;
}
}

extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd) {
    struct stat st {};
    if (armed.load() && fstat(fd, &st) == 0 && S_ISDIR(st.st_mode) &&
        armed.exchange(false)) {
        std::unique_lock<std::mutex> lock(gate);
        paused = true;
        cv.notify_all();
        cv.wait(lock, [] { return resume; });
    }
    return __real_fsync(fd);
}

int main() {
    int failures = 0;
    {
        TempDir dir;
        auto pid = fork();
        if (pid == 0) {
            auto db = engine(dir.path);
            auto tx = db->begin_transaction(ReadCommited);
            fill(tx);
            if (tx->commit().status != CommitResult::Committed || count(db) != 3)
                _exit(2);
            db->flush(); // Flush only the oldest generation, then crash.
            _exit(0);
        }
        auto status = wait_child(pid);
        if (!WIFEXITED(status) || WEXITSTATUS(status)) return 2;
        auto db = engine(dir.path);
        auto n = count(db);
        std::cout << "after commit + one flush + crash: recovered=" << n << "/3\n";
        failures += n != 3;
    }
    {
        TempDir dir;
        auto db = engine(dir.path);
        auto reader = db->begin_transaction(ReadCommited);
        auto writer = db->begin_transaction(ReadCommited);
        fill(writer);
        armed = true;
        std::thread thread([&] { writer->commit(); });
        bool reached;
        {
            std::unique_lock<std::mutex> lock(gate);
            reached = cv.wait_for(lock, std::chrono::seconds(5), [] { return paused; });
        }
        int n = 0;
        for (const auto key : {"a", "b", "c"}) n += reader->get(key).has_value();
        {
            std::lock_guard<std::mutex> lock(gate);
            resume = true;
        }
        cv.notify_all();
        thread.join();
        if (!reached) return 2;
        std::cout << "RC while commit paused at first rotation: visible=" << n << "/3\n";
        failures += n != 0 && n != 3;
        reader->abort();
        int ssts = 0;
        for (const auto &entry : std::filesystem::directory_iterator(dir.path))
            ssts += entry.path().filename().string().find("sst") != std::string::npos;
        std::cout << "transaction exceeds 128-byte memtable budget: SST files=" << ssts << '\n';
        failures += ssts == 0;
    }
    {
        TempDir dir;
        auto rec = WAL::encode_transaction(InternalKey::encode(InternalKey("k", 1, Value)),
                                           std::string(1024, 'v'));
        std::vector<std::uint8_t> bytes;
        while (bytes.size() < 12 * 1024 * 1024) bytes.insert(bytes.end(), rec.begin(), rec.end());
        FileObj::create_and_write(WAL::get_filepath(dir.path, 1), bytes);
        std::cout.flush();
        auto pid = fork();
        if (pid == 0) {
            struct rlimit core {0, 0};
            setrlimit(RLIMIT_CORE, &core);
            struct rlimit stack {};
            getrlimit(RLIMIT_STACK, &stack);
            stack.rlim_cur = 8 * 1024 * 1024;
            if (setrlimit(RLIMIT_STACK, &stack)) _exit(3);
            auto db = engine(dir.path);
            _exit(db->get("k").is_found() ? 0 : 4);
        }
        auto status = wait_child(pid);
        std::cout << "12 MiB valid WAL / 8 MiB stack: signal="
                  << (WIFSIGNALED(status) ? WTERMSIG(status) : 0)
                  << " exit=" << (WIFEXITED(status) ? WEXITSTATUS(status) : -1) << '\n';
        failures += !WIFEXITED(status) || WEXITSTATUS(status) != 0;
    }
    std::cout << failures << " review regressions reproduced\n";
    return failures ? 1 : 0;
}
