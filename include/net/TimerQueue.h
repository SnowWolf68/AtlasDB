//
// Created by test on 10/1/26.
//

#ifndef ATLASDB_TIMERQUEUE_H
#define ATLASDB_TIMERQUEUE_H

#include "net/channel.h"
#include <atomic>
#include <chrono>
#include <functional>
#include <optional>
#include <set>

class TimerQueue {
public:
	class TimerTask {
	public:
		TimerTask(std::uint64_t id, bool is_repeat,
				  std::chrono::steady_clock::time_point start_point,
				  std::chrono::steady_clock::duration interval,
				  std::function<void()> task);

		std::uint64_t _task_id;
		bool _is_repeat;
		std::chrono::steady_clock::time_point _start_point;
		std::chrono::steady_clock::duration _interval;
		std::function<void()> _task;
	};

	class TimerTaskCmp {
	public:
		bool operator()(const TimerTask &a, const TimerTask &b) const;
	};

	TimerQueue(EventLoop *loop);

	std::uint64_t get_next_task_id();

	std::optional<std::uint64_t>
	add_task_after_delay(std::chrono::steady_clock::duration delay,
						 const std::function<void()> &task);
	std::optional<std::uint64_t>
	add_task_at(std::chrono::steady_clock::time_point timepoint,
				const std::function<void()> &task);
	std::optional<std::uint64_t>
	add_task_repeat_after_delay(std::chrono::steady_clock::duration delay,
								std::chrono::steady_clock::duration interval,
								const std::function<void()> &task);

	bool cancel(std::uint64_t task_id);

	~TimerQueue();

private:
	static Channel create_timer_fd_channel(EventLoop *loop);
	void refresh_timerfd();

	EventLoop *_loop;
	std::atomic<std::uint64_t> _next_task_id;
	std::set<TimerTask, TimerTaskCmp> _timer_tasks;
	std::unordered_map<std::uint64_t,
					   std::set<TimerTask, TimerTaskCmp>::iterator>
		_task_id_map;
	Channel _timer_fd_channel;
};

#endif // ATLASDB_TIMERQUEUE_H
