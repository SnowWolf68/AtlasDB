#include "net/EventLoopThreadPool.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <exception>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using namespace std::chrono_literals;

void check(bool condition, const char *expression, int line) {
	if (condition) { return; }
	std::ostringstream message;
	message << "line " << line << ": check failed: " << expression;
	throw std::runtime_error(message.str());
}

#define CHECK(expression) check((expression), #expression, __LINE__)

template <typename T> T await(std::future<T> &result) {
	CHECK(result.wait_for(2s) == std::future_status::ready);
	return result.get();
}

void execute(EventLoop *loop, std::function<void()> task) {
	CHECK(loop != nullptr);
	auto completed = std::make_shared<std::promise<void>>();
	auto result = completed->get_future();
	CHECK(loop->queue_in_loop([completed, task = std::move(task)] {
		try {
			task();
			completed->set_value();
		} catch (...) {
			completed->set_exception(std::current_exception());
		}
	}));
	await(result);
}

// A weak token becomes expired only when the owning worker thread exits.
// This checks join/lifetime without dereferencing a stopped EventLoop.
std::weak_ptr<int> exit_token(EventLoop *loop) {
	std::weak_ptr<int> token;
	execute(loop, [&] {
		thread_local auto owned = std::make_shared<int>(0);
		token = owned;
	});
	CHECK(!token.expired());
	return token;
}

void check_base_still_runs(EventLoop &base) {
	bool called = false;
	CHECK(base.queue_in_loop([&] {
		called = true;
		base.quit();
	}));
	base.loop();
	CHECK(called);
}

void test_null_base_rejected() {
	for (std::size_t count : {0u, 2u}) {
		bool rejected = false;
		try { EventLoopThreadPool pool(nullptr, count); }
		catch (const std::exception &) { rejected = true; }
		CHECK(rejected);
	}
}

void test_unstarted_and_repeated_stop() {
	EventLoop base;
	for (std::size_t count : {0u, 3u}) {
		EventLoopThreadPool pool(&base, count);
		CHECK(pool.get_next_loop() == nullptr);
		CHECK(pool.stop());
		CHECK(pool.stop());
		CHECK(pool.get_next_loop() == nullptr);
	}
	check_base_still_runs(base);
}

void test_zero_workers_use_borrowed_base() {
	EventLoop base;
	{
		EventLoopThreadPool pool(&base, 0);
		for (int cycle = 0; cycle < 3; ++cycle) {
			CHECK(pool.start());
			CHECK(pool.start());
			for (int i = 0; i < 10; ++i) {
				CHECK(pool.get_next_loop() == &base);
			}
			CHECK(pool.stop());
			CHECK(pool.get_next_loop() == nullptr);
		}
	}
	check_base_still_runs(base);
}

void test_single_worker_wraps() {
	EventLoop base;
	EventLoopThreadPool pool(&base, 1);
	CHECK(pool.start());
	EventLoop *worker = pool.get_next_loop();
	CHECK(worker != nullptr && worker != &base);
	for (int i = 0; i < 20; ++i) { CHECK(pool.get_next_loop() == worker); }
	const auto caller = std::this_thread::get_id();
	execute(worker, [&] { CHECK(std::this_thread::get_id() != caller); });
	CHECK(pool.stop());
	CHECK(pool.get_next_loop() == nullptr);
}

void test_round_robin_and_repeated_start() {
	EventLoop base;
	EventLoopThreadPool pool(&base, 3);
	CHECK(pool.start());
	std::vector<EventLoop *> loops;
	std::set<std::thread::id> owners;
	const auto caller = std::this_thread::get_id();
	for (int i = 0; i < 3; ++i) {
		auto *loop = pool.get_next_loop();
		CHECK(loop != nullptr && loop != &base);
		loops.push_back(loop);
		execute(loop, [&] {
			CHECK(std::this_thread::get_id() != caller);
			owners.insert(std::this_thread::get_id());
		});
	}
	CHECK(std::set<EventLoop *>(loops.begin(), loops.end()).size() == 3);
	CHECK(owners.size() == 3);
	for (std::size_t i = 0; i < 31; ++i) {
		CHECK(pool.get_next_loop() == loops[i % loops.size()]);
		// Repeated start must preserve both threads and round-robin position.
		CHECK(pool.start());
	}
	CHECK(pool.stop());
}

void check_wrong_thread_rejected(EventLoopThreadPool &pool) {
	CHECK(!pool.start());
	CHECK(pool.get_next_loop() == nullptr);
	CHECK(!pool.stop());
}

void test_wrong_thread_calls_do_not_change_pool() {
	EventLoop base;
	EventLoopThreadPool pool(&base, 2);
	auto before = std::async(std::launch::async, [&] {
		check_wrong_thread_rejected(pool);
	});
	await(before);
	CHECK(pool.get_next_loop() == nullptr);
	CHECK(pool.start());
	auto *first = pool.get_next_loop();
	auto *second = pool.get_next_loop();
	execute(first, [&] { check_wrong_thread_rejected(pool); });
	CHECK(pool.get_next_loop() == first);
	execute(second, [] {});
	CHECK(pool.stop());
	auto after = std::async(std::launch::async, [&] {
		check_wrong_thread_rejected(pool);
	});
	await(after);
	CHECK(pool.get_next_loop() == nullptr);
}

void test_workers_run_independently() {
	EventLoop base;
	EventLoopThreadPool pool(&base, 3);
	CHECK(pool.start());
	std::vector<std::future<void>> entered;
	auto release = std::make_shared<std::promise<void>>();
	auto gate = release->get_future().share();
	for (int i = 0; i < 3; ++i) {
		auto ready = std::make_shared<std::promise<void>>();
		entered.push_back(ready->get_future());
		CHECK(pool.get_next_loop()->queue_in_loop([ready, gate] {
			ready->set_value();
			// Bound the wait so a failed independence assertion can unwind.
			gate.wait_for(3s);
		}));
	}
	for (auto &ready : entered) { await(ready); }
	release->set_value();
	CHECK(pool.stop());
}

void test_stop_waits_and_drains_all_workers() {
	EventLoop base;
	auto calls = std::make_shared<std::atomic<int>>(0);
	EventLoopThreadPool pool(&base, 3);
	CHECK(pool.start());
	std::vector<EventLoop *> loops;
	std::vector<std::weak_ptr<int>> tokens;
	for (int i = 0; i < 3; ++i) {
		loops.push_back(pool.get_next_loop());
		tokens.push_back(exit_token(loops.back()));
	}
	auto entered = std::make_shared<std::promise<void>>();
	auto ready = entered->get_future();
	auto release = std::make_shared<std::promise<void>>();
	auto gate = release->get_future().share();
	CHECK(loops[0]->queue_in_loop([entered, gate] {
		entered->set_value();
		gate.wait();
	}));
	await(ready);
	for (auto *loop : loops) {
		for (int i = 0; i < 20; ++i) {
			CHECK(loop->queue_in_loop([calls] { ++*calls; }));
		}
	}
	// Pool::stop must run on the owner thread. The observer only uses a loop
	// held alive by the blocked task, and stops touching it before release.
	auto observer = std::async(std::launch::async, [&, release] {
		try {
			const auto deadline = std::chrono::steady_clock::now() + 2s;
			while (loops[0]->queue_in_loop([] {})) {
				CHECK(std::chrono::steady_clock::now() < deadline);
				std::this_thread::sleep_for(1ms);
			}
			CHECK(!tokens[0].expired());
		} catch (...) {
			release->set_value();
			throw;
		}
		release->set_value();
	});
	CHECK(pool.stop());
	await(observer);
	CHECK(calls->load() == 60);
	for (const auto &token : tokens) { CHECK(token.expired()); }
	CHECK(pool.get_next_loop() == nullptr);
	CHECK(pool.stop());
	check_base_still_runs(base);
}

void test_restart_creates_usable_workers() {
	EventLoop base;
	EventLoopThreadPool pool(&base, 3);
	for (int cycle = 0; cycle < 3; ++cycle) {
		CHECK(pool.start());
		std::vector<EventLoop *> loops;
		std::vector<std::weak_ptr<int>> tokens;
		for (int i = 0; i < 3; ++i) {
			loops.push_back(pool.get_next_loop());
			tokens.push_back(exit_token(loops.back()));
		}
		CHECK(std::set<EventLoop *>(loops.begin(), loops.end()).size() == 3);
		for (std::size_t i = 0; i < 7; ++i) {
			CHECK(pool.get_next_loop() == loops[i % loops.size()]);
		}
		CHECK(pool.stop());
		for (const auto &token : tokens) { CHECK(token.expired()); }
		CHECK(pool.get_next_loop() == nullptr);
		// No comparison with an old generation's addresses: they may be reused.
	}
}

void test_destructor_joins_all_and_preserves_base() {
	EventLoop base;
	auto calls = std::make_shared<std::atomic<int>>(0);
	std::vector<std::weak_ptr<int>> tokens;
	{
		EventLoopThreadPool pool(&base, 3);
		CHECK(pool.start());
		for (int i = 0; i < 3; ++i) {
			auto *loop = pool.get_next_loop();
			tokens.push_back(exit_token(loop));
			CHECK(loop->add_task_after_delay(1h, [calls] { ++*calls; }).has_value());
			CHECK(loop->queue_in_loop([calls] { ++*calls; }));
		}
		// Deliberately omit explicit stop.
	}
	CHECK(calls->load() == 3);
	for (const auto &token : tokens) { CHECK(token.expired()); }
	check_base_still_runs(base);
}

struct TestCase {
	const char *name;
	void (*run)();
};

} // namespace

int main(int argc, char **argv) {
	const TestCase tests[] = {
		{"null base rejected", test_null_base_rejected},
		{"unstarted and repeated stop", test_unstarted_and_repeated_stop},
		{"zero workers use borrowed base", test_zero_workers_use_borrowed_base},
		{"single worker wraps", test_single_worker_wraps},
		{"round robin and repeated start", test_round_robin_and_repeated_start},
		{"wrong thread calls do not change pool", test_wrong_thread_calls_do_not_change_pool},
		{"workers run independently", test_workers_run_independently},
		{"stop waits and drains all workers", test_stop_waits_and_drains_all_workers},
		{"restart creates usable workers", test_restart_creates_usable_workers},
		{"destructor joins all and preserves base", test_destructor_joins_all_and_preserves_base},
	};
	std::size_t passed = 0;
	std::size_t selected = 0;
	for (const auto &test : tests) {
		if (argc > 1 && std::string(argv[1]) != test.name) { continue; }
		++selected;
		std::cout.flush();
		std::cerr.flush();
		const pid_t child = ::fork();
		if (child == -1) {
			std::cerr << "[FAIL] " << test.name << ": fork failed\n";
			continue;
		}
		if (child == 0) {
			::alarm(7);
			try {
				test.run();
				std::cout.flush();
				std::cerr.flush();
				::_exit(0);
			} catch (const std::exception &error) {
				std::cerr << "[FAIL] " << test.name << ": " << error.what() << std::endl;
				::_exit(1);
			} catch (...) {
				std::cerr << "[FAIL] " << test.name << ": unknown exception" << std::endl;
				::_exit(1);
			}
		}
		int status = 0;
		pid_t waited;
		do { waited = ::waitpid(child, &status, 0); }
		while (waited == -1 && errno == EINTR);
		if (waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
			++passed;
			std::cout << "[PASS] " << test.name << '\n';
		} else if (waited == child && WIFSIGNALED(status)) {
			std::cerr << "[FAIL] " << test.name << ": signal " << WTERMSIG(status) << '\n';
		} else if (waited != child) {
			std::cerr << "[FAIL] " << test.name << ": waitpid failed\n";
		}
	}
	std::cout << passed << '/' << selected << " tests passed\n";
	return selected != 0 && passed == selected ? 0 : 1;
}
