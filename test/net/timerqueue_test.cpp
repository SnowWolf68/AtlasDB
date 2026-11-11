#include "net/EventLoop.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <iostream>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

void check(bool condition, const char *expression, int line) {
	if (condition) { return; }
	std::ostringstream message;
	message << "line " << line << ": check failed: " << expression;
	throw std::runtime_error(message.str());
}

#define CHECK(expression) check((expression), #expression, __LINE__)

bool run_loop_with_timeout(EventLoop &loop,
					   std::chrono::milliseconds timeout =
						   std::chrono::milliseconds(1000)) {
	std::mutex mutex;
	std::condition_variable cv;
	bool finished = false;
	bool timed_out = false;
	std::thread watchdog([&] {
		std::unique_lock<std::mutex> lock(mutex);
		if (!cv.wait_for(lock, timeout, [&] { return finished; })) {
			timed_out = true;
			lock.unlock();
			loop.quit();
		}
	});

	try {
		loop.loop();
	} catch (...) {
		{
			std::lock_guard<std::mutex> lock(mutex);
			finished = true;
		}
		cv.notify_one();
		watchdog.join();
		throw;
	}
	{
		std::lock_guard<std::mutex> lock(mutex);
		finished = true;
	}
	cv.notify_one();
	watchdog.join();
	return !timed_out;
}

void test_one_shot_timers_run_in_deadline_order() {
	EventLoop loop;
	std::vector<int> order;
	const auto deadline = std::chrono::steady_clock::now() +
						  std::chrono::milliseconds(20);
	const auto first = loop.add_task_at(deadline, [&] { order.push_back(1); });
	const auto second = loop.add_task_at(deadline, [&] { order.push_back(2); });
	const auto third = loop.add_task_after_delay(std::chrono::milliseconds(50),
											   [&] {
												   order.push_back(3);
												   loop.quit();
											   });
	CHECK(first.has_value());
	CHECK(second.has_value());
	CHECK(third.has_value());
	CHECK(*first < *second);
	CHECK(run_loop_with_timeout(loop));
	CHECK((order == std::vector<int>{1, 2, 3}));
}

void test_cancel_one_shot_and_missing_ids() {
	EventLoop loop;
	int calls = 0;
	const auto cancelled = loop.add_task_after_delay(
		std::chrono::milliseconds(20), [&] { ++calls; });
	CHECK(cancelled.has_value());
	CHECK(loop.cancel(*cancelled));
	CHECK(loop.cancel(*cancelled));
	CHECK(loop.cancel(*cancelled + 1000));
	CHECK(loop.add_task_after_delay(std::chrono::milliseconds(60),
										  [&] { loop.quit(); })
			  .has_value());
	CHECK(run_loop_with_timeout(loop));
	CHECK(calls == 0);
}

void test_repeat_timer_can_cancel_itself() {
	EventLoop loop;
	int calls = 0;
	bool cancel_queued = false;
	std::optional<std::uint64_t> id;
	id = loop.add_task_repeat_after_delay(
		std::chrono::milliseconds(5), std::chrono::milliseconds(5), [&] {
			++calls;
			if (calls == 3) { cancel_queued = loop.cancel(*id); }
		});
	CHECK(id.has_value());
	CHECK(loop.add_task_after_delay(std::chrono::milliseconds(75),
										  [&] { loop.quit(); })
			  .has_value());
	CHECK(run_loop_with_timeout(loop));
	CHECK(cancel_queued);
	CHECK(calls == 3);
}

void test_cross_thread_add_then_cancel() {
	EventLoop loop;
	int calls = 0;
	std::optional<std::uint64_t> id;
	bool cancel_queued = false;
	std::exception_ptr worker_error;
	CHECK(loop.add_task_after_delay(std::chrono::milliseconds(160),
										  [&] { loop.quit(); })
			  .has_value());
	std::thread worker([&] {
		try {
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
			id = loop.add_task_after_delay(std::chrono::milliseconds(70),
										   [&] { ++calls; });
			if (id) { cancel_queued = loop.cancel(*id); }
		} catch (...) {
			worker_error = std::current_exception();
			try { loop.quit(); } catch (...) {}
		}
	});
	const bool completed = run_loop_with_timeout(loop);
	worker.join();
	if (worker_error) { std::rethrow_exception(worker_error); }
	CHECK(completed);
	CHECK(id.has_value());
	CHECK(cancel_queued);
	CHECK(calls == 0);
}

void test_shutdown_discards_future_timer() {
	int calls = 0;
	{
		EventLoop loop;
		CHECK(loop.add_task_after_delay(std::chrono::hours(1),
										  [&] { ++calls; })
				  .has_value());
		loop.quit();
		loop.loop();
	}
	CHECK(calls == 0);
}

void test_invalid_intervals_and_post_quit_requests_are_rejected() {
	EventLoop loop;
	int calls = 0;
	const auto callback = [&] { ++calls; };
	CHECK(!loop.add_task_repeat_after_delay(std::chrono::milliseconds(0),
		std::chrono::milliseconds(0), callback));
	CHECK(!loop.add_task_repeat_after_delay(std::chrono::milliseconds(0),
		std::chrono::milliseconds(-1), callback));
	loop.quit();
	CHECK(!loop.add_task_after_delay(std::chrono::milliseconds(1), callback));
	CHECK(!loop.add_task_at(std::chrono::steady_clock::now(), callback));
	CHECK(!loop.add_task_repeat_after_delay(std::chrono::milliseconds(1),
		std::chrono::milliseconds(1), callback));
	CHECK(!loop.cancel(1));
	loop.loop();
	CHECK(calls == 0);
}

void test_past_deadline_and_negative_delay_run_without_waiting_for_watchdog() {
	EventLoop loop;
	std::vector<int> calls;
	CHECK(loop.add_task_at(std::chrono::steady_clock::now() - std::chrono::hours(1), [&] {
		calls.push_back(1);
		if (calls.size() == 2) { loop.quit(); }
	}).has_value());
	CHECK(loop.add_task_after_delay(std::chrono::milliseconds(-10), [&] {
		calls.push_back(2);
		if (calls.size() == 2) { loop.quit(); }
	}).has_value());
	CHECK(run_loop_with_timeout(loop));
	CHECK((calls == std::vector<int>{1, 2}));
}

void test_throwing_callback_does_not_discard_other_expired_tasks() {
	EventLoop loop;
	std::vector<int> calls;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(5);
	CHECK(loop.add_task_at(deadline, [&] {
		calls.push_back(1);
		throw std::runtime_error("intentional timer callback failure");
	}).has_value());
	CHECK(loop.add_task_at(deadline, [&] { calls.push_back(2); loop.quit(); }).has_value());
	CHECK(run_loop_with_timeout(loop));
	CHECK((calls == std::vector<int>{1, 2}));
}

void test_throwing_repeat_callback_can_run_again_and_cancel() {
	EventLoop loop;
	int calls = 0;
	std::optional<std::uint64_t> id;
	id = loop.add_task_repeat_after_delay(std::chrono::milliseconds(0),
		std::chrono::milliseconds(5), [&] {
			if (++calls == 1) { throw std::runtime_error("intentional repeat callback failure"); }
			if (calls == 3) { CHECK(loop.cancel(*id)); loop.quit(); }
		});
	CHECK(id.has_value());
	CHECK(run_loop_with_timeout(loop));
	CHECK(calls == 3);
}

void test_concurrent_timer_creation_has_unique_ids_and_owner_callbacks() {
	EventLoop loop;
	const auto owner = std::this_thread::get_id();
	std::vector<std::vector<std::uint64_t>> ids(4);
	std::vector<std::exception_ptr> errors(4);
	std::vector<std::thread> producers;
	int calls = 0;
	for (int producer = 0; producer < 4; ++producer) {
		producers.emplace_back([&, producer] {
			try {
				for (int item = 0; item < 8; ++item) {
					auto id = loop.add_task_after_delay(std::chrono::milliseconds(0), [&] {
						CHECK(std::this_thread::get_id() == owner);
						if (++calls == 32) { loop.quit(); }
					});
					CHECK(id.has_value());
					ids[producer].push_back(*id);
				}
			} catch (...) { errors[producer] = std::current_exception(); }
		});
	}
	for (auto &producer : producers) { producer.join(); }
	for (auto error : errors) { if (error) { std::rethrow_exception(error); } }
	std::set<std::uint64_t> unique;
	for (const auto &producer_ids : ids) {
		for (auto id : producer_ids) { CHECK(unique.insert(id).second); }
	}
	CHECK(unique.size() == 32);
	CHECK(run_loop_with_timeout(loop));
	CHECK(calls == 32);
}

struct TestCase {
	const char *name;
	void (*run)();
};

} // namespace

int main() {
	const TestCase tests[] = {
		{"one-shot timers run in deadline order",
		 test_one_shot_timers_run_in_deadline_order},
		{"cancel one-shot and missing IDs",
		 test_cancel_one_shot_and_missing_ids},
		{"repeat timer can cancel itself",
		 test_repeat_timer_can_cancel_itself},
		{"cross-thread add then cancel", test_cross_thread_add_then_cancel},
		{"shutdown discards future timer", test_shutdown_discards_future_timer},
		{"invalid intervals and post-quit requests are rejected", test_invalid_intervals_and_post_quit_requests_are_rejected},
		{"past deadlines and negative delays run promptly", test_past_deadline_and_negative_delay_run_without_waiting_for_watchdog},
		{"throwing callback preserves other expired tasks", test_throwing_callback_does_not_discard_other_expired_tasks},
		{"throwing repeat callback can run again and cancel", test_throwing_repeat_callback_can_run_again_and_cancel},
		{"concurrent timers have unique IDs and owner callbacks", test_concurrent_timer_creation_has_unique_ids_and_owner_callbacks},
	};

	std::size_t passed = 0;
	for (const auto &test : tests) {
		try {
			test.run();
			++passed;
			std::cout << "[PASS] " << test.name << '\n';
		} catch (const std::exception &error) {
			std::cerr << "[FAIL] " << test.name << ": " << error.what()
					  << '\n';
		}
	}
	std::cout << passed << '/' << (sizeof(tests) / sizeof(tests[0]))
			  << " tests passed\n";
	return passed == (sizeof(tests) / sizeof(tests[0])) ? 0 : 1;
}
