//
// Created by test on 10/1/26.
//

#include "net/TimerQueue.h"

#include "log.h"
#include "net/EventLoop.h"

#include <sys/timerfd.h>

bool TimerQueue::TimerTaskCmp::operator()(const TimerTask &a,
										  const TimerTask &b) const {
	if (a._start_point != b._start_point) {
		return a._start_point < b._start_point;
	} else {
		return a._task_id < b._task_id;
	}
}

TimerQueue::TimerTask::TimerTask(
	std::uint64_t id, bool is_repeat,
	std::chrono::steady_clock::time_point start_point,
	std::chrono::steady_clock::duration interval, std::function<void()> task)
	: _task_id(id), _is_repeat(is_repeat), _start_point(start_point),
	  _interval(interval), _task(task) {}

std::uint64_t TimerQueue::get_next_task_id() {
	return _next_task_id.fetch_add(1);
}

Channel TimerQueue::create_timer_fd_channel(EventLoop *loop) {
	int timer_fd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
	if (timer_fd < 0) { throw_error("timerfd creation failed"); }
	return Channel(timer_fd, Channel::TriggerMode::Edge, loop);
}

TimerQueue::TimerQueue(EventLoop *loop)
	: _loop(loop), _next_task_id(1),
	  _timer_fd_channel(create_timer_fd_channel(loop)) {
	_timer_fd_channel.enable_reading();
	_timer_fd_channel.setReadCallback([this]() {
		std::uint64_t data;
		int n = -1;
		while (true) {
			n = ::read(_timer_fd_channel.fd(), &data, sizeof(data));
			if (n == -1) {
				if (errno == EAGAIN) {
					break;
				} else if (errno == EINTR) {
					continue;
				} else {
					throw_error("timerfd read error");
				}
			} else {
				break;
			}
		}

		if (_timer_tasks.empty()) {
			log_error("no timer tasks in this loop");
			return;
		}
		std::vector<TimerTask> rd_tasks;
		std::chrono::steady_clock::time_point now =
			std::chrono::steady_clock::now();
		while (!_timer_tasks.empty()) {
			TimerTask task = *_timer_tasks.begin();
			if (task._start_point > now) { break; }
			_timer_tasks.erase(task);
			_task_id_map.erase(task._task_id);
			rd_tasks.push_back(task);
		}

		for (auto task : rd_tasks) {
			try {
				task._task();
			} catch (const std::exception &e) {
				log_error(
					std::string("timer_task callback execution failed, e: ") +
					e.what());
			}
		}

		for (auto task : rd_tasks) {
			if (task._is_repeat) {
				task._start_point =
					std::chrono::steady_clock::now() + task._interval;
				auto [it, res] = _timer_tasks.insert(task);
				if (!res) { throw_error("timer tasks insert failed"); }
				_task_id_map[task._task_id] = it;
			}
		}

		refresh_timerfd();
	});
}

void TimerQueue::refresh_timerfd() {
	if (!_timer_tasks.empty()) {
		TimerTask first = *_timer_tasks.begin();
		std::chrono::steady_clock::time_point first_start = first._start_point;
		std::chrono::steady_clock::time_point now =
			std::chrono::steady_clock::now();
		std::chrono::steady_clock::duration interval = first_start - now;
		struct itimerspec interval_next{};
		interval_next.it_interval.tv_sec = 0;
		interval_next.it_interval.tv_nsec = 0;
		std::chrono::nanoseconds::rep ns =
			std::chrono::duration_cast<std::chrono::nanoseconds>(interval)
				.count();
		if (ns <= 0) {
			ns = 1;
		} // 如果下一次的时间间隔是0, 那么timefd会停止定时
		interval_next.it_value.tv_nsec = ns % 1000000000;
		interval_next.it_value.tv_sec = ns / 1000000000;
		int res = ::timerfd_settime(this->_timer_fd_channel.fd(), 0,
									&interval_next, nullptr);
		if (res < 0) { throw_error("timerfd_settime failed"); }
	}
}

std::optional<std::uint64_t>
TimerQueue::add_task_after_delay(std::chrono::steady_clock::duration delay,
								 const std::function<void()> &task) {
	TimerTask timer_task(get_next_task_id(), false,
						 std::chrono::steady_clock::now() + delay,
						 std::chrono::steady_clock::duration::zero(), task);
	bool res = _loop->queue_in_loop([this, timer_task]() {
		auto [it, res] = _timer_tasks.insert(timer_task);
		if (!res) { ERROR_LOG << "timer tasks insert failed"; }
		_task_id_map[timer_task._task_id] = it;
		refresh_timerfd();
	});
	if (!res) {
		ERROR_LOG << "queue in loop failed";
		return std::nullopt;
	}
	return timer_task._task_id;
}

std::optional<std::uint64_t>
TimerQueue::add_task_at(std::chrono::steady_clock::time_point timepoint,
						const std::function<void()> &task) {
	TimerTask timer_task(get_next_task_id(), false, timepoint,
						 std::chrono::steady_clock::duration::zero(), task);
	bool res = _loop->queue_in_loop([this, timer_task]() {
		auto [it, res] = _timer_tasks.insert(timer_task);
		if (!res) { ERROR_LOG << "timer tasks insert failed"; }
		_task_id_map[timer_task._task_id] = it;
		refresh_timerfd();
	});
	if (!res) {
		ERROR_LOG << "queue in loop failed";
		return std::nullopt;
	}
	return timer_task._task_id;
}

std::optional<std::uint64_t> TimerQueue::add_task_repeat_after_delay(
	std::chrono::steady_clock::duration delay,
	std::chrono::steady_clock::duration interval,
	const std::function<void()> &task) {
	if (interval <= std::chrono::nanoseconds::zero()) { return std::nullopt; }
	TimerTask timer_task(get_next_task_id(), true,
						 std::chrono::steady_clock::now() + delay, interval,
						 task);
	bool res = _loop->queue_in_loop([this, timer_task]() {
		auto [it, res] = _timer_tasks.insert(timer_task);
		if (!res) { ERROR_LOG << "timer tasks insert failed"; }
		_task_id_map[timer_task._task_id] = it;
		refresh_timerfd();
	});
	if (!res) {
		ERROR_LOG << "queue in loop failed";
		return std::nullopt;
	}
	return timer_task._task_id;
}

bool TimerQueue::cancel(std::uint64_t task_id) {
	// 当前由于add和cancel都是提交给loop的任务队列执行
	// 因此不会出现一个task在执行回调(此时id不在map,set中)时,
	// 触发cancel发现不存在这个task
	return _loop->queue_in_loop([this, task_id]() {
		if (_task_id_map.count(task_id) == 0) {
			// 如果任务执行完或者已经被取消过, 都会找不到
			// 因此这是正常现象
			return;
		}
		_timer_tasks.erase(_task_id_map[task_id]);
		_task_id_map.erase(task_id);
		if (_timer_tasks.empty()) {
			int timerfd = _timer_fd_channel.fd();
			struct itimerspec interval_next{};
			int res = ::timerfd_settime(timerfd, 0, &interval_next, nullptr);
			if (res == -1) { throw_error("timerfd settime failed"); }
		} else {
			refresh_timerfd();
		}
	});
}

TimerQueue::~TimerQueue() {
	if (_timer_fd_channel.fd() >= 0) {
		_timer_fd_channel.remove();
		int res = ::close(_timer_fd_channel.fd());
		if (res < 0) { log_error("timer fd close failed"); }
	}
}