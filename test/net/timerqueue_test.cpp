#include "net/EventLoop.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <iostream>
#include <mutex>
#include <optional>
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
