#include "net/EventLoop.h"
#include "net/channel.h"

#include <chrono>
#include <exception>
#include <fcntl.h>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

void check(bool condition, const char *expression, int line) {
	if (condition) { return; }
	std::ostringstream message;
	message << "line " << line << ": check failed: " << expression;
	throw std::runtime_error(message.str());
}

#define CHECK(expression) check((expression), #expression, __LINE__)

void test_quit_before_loop_drains_accepted_tasks() {
	EventLoop loop;
	int calls = 0;
	CHECK(loop.queue_in_loop([&] { ++calls; }));
	loop.quit();
	CHECK(!loop.queue_in_loop([&] { ++calls; }));
	CHECK(!loop.run_or_queue_in_loop([&] { ++calls; }));
	loop.loop();
	CHECK(calls == 1);
}

void test_same_thread_run_and_nested_queue() {
	EventLoop loop;
	std::vector<int> order;
	CHECK(loop.queue_in_loop([&] {
		order.push_back(1);
		CHECK(loop.run_or_queue_in_loop([&] { order.push_back(2); }));
		CHECK(loop.queue_in_loop([&] { order.push_back(4); }));
		order.push_back(3);
		loop.quit();
	}));
	loop.loop();
	CHECK((order == std::vector<int>{1, 2, 3, 4}));
}

void test_cross_thread_tasks_and_quit_wake_loop() {
	EventLoop loop;
	const std::thread::id loop_thread = std::this_thread::get_id();
	std::vector<int> order;
	std::vector<std::thread::id> execution_threads;
	bool queued = false;
	bool dispatched = false;
	bool rejected_after_quit = false;
	std::exception_ptr worker_error;

	std::thread worker([&] {
		try {
			// Give loop() a chance to enter epoll_wait; correctness does not
			// depend on the exact scheduling order.
			std::this_thread::sleep_for(std::chrono::milliseconds(30));
			queued = loop.queue_in_loop([&] {
				order.push_back(1);
				execution_threads.push_back(std::this_thread::get_id());
			});
			dispatched = loop.run_or_queue_in_loop([&] {
				order.push_back(2);
				execution_threads.push_back(std::this_thread::get_id());
			});
			loop.quit();
			rejected_after_quit = !loop.queue_in_loop([&] {
				order.push_back(3);
			});
		} catch (...) {
			worker_error = std::current_exception();
			try { loop.quit(); } catch (...) {}
		}
	});

	loop.loop();
	worker.join();
	if (worker_error) { std::rethrow_exception(worker_error); }
	CHECK(queued);
	CHECK(dispatched);
	CHECK(rejected_after_quit);
	CHECK((order == std::vector<int>{1, 2}));
	CHECK(execution_threads.size() == 2);
	for (const auto &id : execution_threads) { CHECK(id == loop_thread); }
}

void test_readable_channel_dispatches_callback() {
	int fds[2]{-1, -1};
	CHECK(::pipe2(fds, O_NONBLOCK | O_CLOEXEC) == 0);
	EventLoop loop;
	Channel channel(fds[0], Channel::TriggerMode::Level, &loop);
	int calls = 0;
	char received = '\0';
	channel.setReadCallback([&] {
		char byte = '\0';
		CHECK(::read(fds[0], &byte, 1) == 1);
		received = byte;
		++calls;
		channel.disable_all();
		channel.remove();
		loop.quit();
	});
	channel.enable_reading();
	CHECK(::write(fds[1], "x", 1) == 1);
	loop.loop();
	CHECK(calls == 1);
	CHECK(received == 'x');
	CHECK(::close(fds[0]) == 0);
	CHECK(::close(fds[1]) == 0);
}

void test_multiple_producers_drain_once_and_preserve_per_producer_order() {
	EventLoop loop;
	const auto owner = std::this_thread::get_id();
	std::vector<std::vector<int>> observed(4);
	std::vector<std::thread> producers;
	std::vector<std::exception_ptr> errors(4);
	for (int producer = 0; producer < 4; ++producer) {
		producers.emplace_back([&, producer] {
			try {
				for (int item = 0; item < 50; ++item) {
					CHECK(loop.queue_in_loop([&, producer, item] {
						CHECK(std::this_thread::get_id() == owner);
						observed[producer].push_back(item);
					}));
				}
			} catch (...) { errors[producer] = std::current_exception(); }
		});
	}
	for (auto &producer : producers) { producer.join(); }
	for (auto error : errors) { if (error) { std::rethrow_exception(error); } }
	loop.quit();
	loop.loop();
	for (const auto &items : observed) {
		CHECK(items.size() == 50);
		for (int item = 0; item < 50; ++item) { CHECK(items[item] == item); }
	}
}

void test_wrong_thread_management_is_rejected_without_disabling_owner() {
	int fds[2] {-1, -1};
	CHECK(::pipe2(fds, O_NONBLOCK | O_CLOEXEC) == 0);
	EventLoop loop;
	Channel channel(fds[0], Channel::TriggerMode::Level, &loop);
	int rejected = 0;
	std::thread worker([&] {
		try { loop.loop(); } catch (const std::exception &) { ++rejected; }
		try { loop.update_channel(&channel); } catch (const std::exception &) { ++rejected; }
		try { loop.remove_channel(&channel); } catch (const std::exception &) { ++rejected; }
	});
	worker.join();
	CHECK(rejected == 3);
	int calls = 0;
	channel.setReadCallback([&] {
		char byte;
		CHECK(::read(fds[0], &byte, 1) == 1 && byte == 'x');
		++calls;
		channel.disable_all();
		channel.remove();
		loop.quit();
	});
	channel.enable_reading();
	CHECK(::write(fds[1], "x", 1) == 1);
	loop.loop();
	CHECK(calls == 1);
	CHECK(::close(fds[0]) == 0 && ::close(fds[1]) == 0);
}

struct TestCase {
	const char *name;
	void (*run)();
};

} // namespace

int main() {
	const TestCase tests[] = {
		{"quit before loop drains accepted tasks",
		 test_quit_before_loop_drains_accepted_tasks},
		{"same-thread run and nested queue",
		 test_same_thread_run_and_nested_queue},
		{"cross-thread tasks and quit wake loop",
		 test_cross_thread_tasks_and_quit_wake_loop},
		{"readable channel dispatches callback",
		 test_readable_channel_dispatches_callback},
		{"multiple producers drain once in per-producer order", test_multiple_producers_drain_once_and_preserve_per_producer_order},
		{"wrong-thread management leaves owner loop usable", test_wrong_thread_management_is_rejected_without_disabling_owner},
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
