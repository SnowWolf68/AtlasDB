//
// Created by test on 9/29/26.
//

#ifndef ATLASDB_EVENTLOOP_H
#define ATLASDB_EVENTLOOP_H

#include "net/EPoller.h"
#include "net/channel.h"

#include "net/TimerQueue.h"
#include <atomic>
#include <mutex>
#include <queue>
#include <thread>

class EventLoop {
public:
	std::mutex _mutex;

	EventLoop();

	void update_channel(Channel *channel);
	void remove_channel(Channel *channel);

	void loop();
	void wakeup();

	// 对外提供的quit接口
	void quit();

	bool run_or_queue_in_loop(std::function<void()> task);
	bool queue_in_loop(std::function<void()> task);

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

	~EventLoop();

private:
	static int create_event_fd();

	EPoller _epoller;
	Channel _ch_event_fd;
	std::atomic<bool> _is_quit;
	std::thread::id _thread_id;
	std::queue<std::function<void()>> _queue;
	TimerQueue _timer_queue;
};

#endif // ATLASDB_EVENTLOOP_H
