#include "net/EventLoopThread.h"
#include "net/channel.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <exception>
#include <fcntl.h>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
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

// The worker cannot exit until release() is called. This keeps the borrowed
// EventLoop pointer valid while the test observes an external stop request.
class BlockedTask {
public:
	BlockedTask() : _entered(_entered_promise.get_future()),
					_release(_release_promise.get_future().share()) {}

	void enter(EventLoop *loop) {
		CHECK(loop->queue_in_loop([this] {
			_entered_promise.set_value();
			_release.wait();
		}));
		await(_entered);
	}

	void release() { _release_promise.set_value(); }

private:
	std::promise<void> _entered_promise;
	std::future<void> _entered;
	std::promise<void> _release_promise;
	std::shared_future<void> _release;
};

void wait_until_quit_requested(EventLoop *loop) {
	const auto deadline = std::chrono::steady_clock::now() + 2s;
	while (loop->queue_in_loop([] {})) {
		CHECK(std::chrono::steady_clock::now() < deadline);
		std::this_thread::sleep_for(1ms);
	}
}

void test_unstarted_and_repeated_stop() {
	EventLoopThread thread;
	thread.stop();
	thread.stop();
}

void test_start_publishes_ready_loop_in_owner_thread() {
	EventLoopThread thread;
	EventLoop *loop = thread.start();
	CHECK(loop != nullptr);
	CHECK(thread.start() == loop);
	const auto caller = std::this_thread::get_id();
	std::thread::id owner;
	bool inline_called = false;
	int fds[2]{-1, -1};
	CHECK(::pipe2(fds, O_NONBLOCK | O_CLOEXEC) == 0);
	execute(loop, [&] {
		owner = std::this_thread::get_id();
		CHECK(owner != caller);
		CHECK(loop->run_or_queue_in_loop([&] { inline_called = true; }));
		CHECK(inline_called);
		// Channel registration rejects a loop constructed on the wrong thread.
		Channel channel(fds[0], Channel::TriggerMode::Level, loop);
		channel.enable_reading();
		channel.remove();
	});
	execute(loop, [&] { CHECK(std::this_thread::get_id() == owner); });
	thread.stop();
	thread.stop();
	CHECK(::close(fds[0]) == 0);
	CHECK(::close(fds[1]) == 0);
}

void test_destructor_stops_loop_and_discards_future_timer() {
	std::atomic<int> calls{0};
	{
		EventLoopThread thread;
		EventLoop *loop = thread.start();
		execute(loop, [&] { ++calls; });
		CHECK(loop->add_task_after_delay(1h, [&] { ++calls; }).has_value());
		// No explicit stop: destruction must quit, join, and destroy TimerQueue
		// in the worker thread without waiting for the future timer deadline.
	}
	CHECK(calls.load() == 1);
}

void test_loop_can_quit_before_stop() {
	EventLoopThread thread;
	EventLoop *loop = thread.start();
	std::promise<void> requested;
	auto result = requested.get_future();
	CHECK(loop->queue_in_loop([&] {
		loop->quit();
		requested.set_value();
	}));
	await(result);
	// The worker may already have destroyed its local loop, or may still be
	// finishing. Both schedules must permit safe thread reclamation.
	thread.stop();
	thread.stop();
}

void test_stop_waits_for_task_and_drains_accepted_queue() {
	EventLoopThread thread;
	EventLoop *loop = thread.start();
	BlockedTask task;
	task.enter(loop);
	int calls = 0;
	CHECK(loop->queue_in_loop([&] { ++calls; }));
	std::promise<void> stopped;
	auto result = stopped.get_future();
	std::thread stopper([&] {
		try {
			thread.stop();
			stopped.set_value();
		} catch (...) {
			stopped.set_exception(std::current_exception());
		}
	});
	wait_until_quit_requested(loop);
	CHECK(result.wait_for(0ms) == std::future_status::timeout);
	task.release();
	await(result);
	stopper.join();
	CHECK(calls == 1);
}

void test_concurrent_starts_share_one_ready_loop() {
	EventLoopThread thread;
	constexpr std::size_t count = 8;
	std::vector<EventLoop *> loops(count, nullptr);
	std::vector<std::exception_ptr> errors(count);
	std::vector<std::thread> callers;
	std::promise<void> go;
	auto gate = go.get_future().share();
	for (std::size_t i = 0; i < count; ++i) {
		callers.emplace_back([&, i] {
			gate.wait();
			try { loops[i] = thread.start(); }
			catch (...) { errors[i] = std::current_exception(); }
		});
	}
	go.set_value();
	for (auto &caller : callers) { caller.join(); }
	for (std::size_t i = 0; i < count; ++i) {
		if (errors[i]) { std::rethrow_exception(errors[i]); }
		CHECK(loops[i] != nullptr);
		CHECK(loops[i] == loops[0]);
	}
	execute(loops[0], [] {});
	thread.stop();
}

void test_concurrent_stops_reclaim_once() {
	EventLoopThread thread;
	execute(thread.start(), [] {});
	constexpr std::size_t count = 8;
	std::vector<std::exception_ptr> errors(count);
	std::vector<std::thread> callers;
	std::promise<void> go;
	auto gate = go.get_future().share();
	for (std::size_t i = 0; i < count; ++i) {
		callers.emplace_back([&, i] {
			gate.wait();
			try { thread.stop(); }
			catch (...) { errors[i] = std::current_exception(); }
		});
	}
	go.set_value();
	for (auto &caller : callers) { caller.join(); }
	for (const auto &error : errors) {
		if (error) { std::rethrow_exception(error); }
	}
	thread.stop();
	execute(thread.start(), [] {});
	thread.stop();
}

void test_start_waits_until_previous_stop_finishes() {
	EventLoopThread thread;
	EventLoop *old_loop = thread.start();
	BlockedTask task;
	task.enter(old_loop);
	std::promise<void> stopped;
	auto stop_result = stopped.get_future();
	std::thread stopper([&] {
		try { thread.stop(); stopped.set_value(); }
		catch (...) { stopped.set_exception(std::current_exception()); }
	});
	wait_until_quit_requested(old_loop);

	std::promise<void> entering_start;
	auto entering = entering_start.get_future();
	std::promise<EventLoop *> started;
	auto start_result = started.get_future();
	std::thread starter([&] {
		entering_start.set_value();
		try { started.set_value(thread.start()); }
		catch (...) { started.set_exception(std::current_exception()); }
	});
	await(entering);
	CHECK(start_result.wait_for(30ms) == std::future_status::timeout);
	task.release();
	await(stop_result);
	EventLoop *new_loop = await(start_result);
	stopper.join();
	starter.join();
	// A new stack-local loop can reuse the same address; pointer inequality
	// would not be a valid restart assertion.
	execute(new_loop, [] {});
	thread.stop();
}

void test_restart_after_completed_stop() {
	EventLoopThread thread;
	int calls = 0;
	for (int iteration = 0; iteration < 3; ++iteration) {
		EventLoop *loop = thread.start();
		execute(loop, [&] { ++calls; });
		thread.stop();
		thread.stop();
	}
	CHECK(calls == 3);
}

void test_runtime_exception_still_allows_cleanup_and_restart() {
	EventLoopThread thread;
	EventLoop *loop = thread.start();
	std::promise<void> entered;
	auto result = entered.get_future();
	CHECK(loop->queue_in_loop([&] {
		entered.set_value();
		throw std::runtime_error("expected eventloopthread test failure");
	}));
	await(result);
	thread.stop();
	execute(thread.start(), [] {});
	thread.stop();
}

struct TestCase {
	const char *name;
	void (*run)();
};

} // namespace

int main(int argc, char **argv) {
	const TestCase tests[] = {
		{"unstarted and repeated stop", test_unstarted_and_repeated_stop},
		{"start publishes ready loop in owner thread",
		 test_start_publishes_ready_loop_in_owner_thread},
		{"destructor stops loop and discards future timer",
		 test_destructor_stops_loop_and_discards_future_timer},
		{"loop can quit before stop", test_loop_can_quit_before_stop},
		{"stop waits for task and drains accepted queue",
		 test_stop_waits_for_task_and_drains_accepted_queue},
		{"concurrent starts share one ready loop",
		 test_concurrent_starts_share_one_ready_loop},
		{"concurrent stops reclaim once", test_concurrent_stops_reclaim_once},
		{"start waits until previous stop finishes",
		 test_start_waits_until_previous_stop_finishes},
		{"restart after completed stop", test_restart_after_completed_stop},
		{"runtime exception allows cleanup and restart",
		 test_runtime_exception_still_allows_cleanup_and_restart},
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
				std::cerr << "[FAIL] " << test.name << ": " << error.what()
						  << std::endl;
				::_exit(1);
			} catch (...) {
				std::cerr << "[FAIL] " << test.name << ": unknown exception"
						  << std::endl;
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
			std::cerr << "[FAIL] " << test.name << ": signal "
					  << WTERMSIG(status) << '\n';
		} else if (waited != child) {
			std::cerr << "[FAIL] " << test.name << ": waitpid failed\n";
		}
	}
	std::cout << passed << '/' << selected << " tests passed\n";
	return selected != 0 && passed == selected ? 0 : 1;
}
