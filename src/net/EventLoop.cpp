//
// Created by test on 9/29/26.
//

#include "net/EventLoop.h"
#include "log.h"

#include <sys/epoll.h>
#include <sys/eventfd.h>

int EventLoop::create_event_fd() {
	int event_fd = ::eventfd(0, EFD_NONBLOCK);
	if (event_fd == -1) { throw_error("eventfd creation failed"); }
	return event_fd;
}

EventLoop::EventLoop()
	: _ch_event_fd(create_event_fd(), Channel::TriggerMode::Edge, this),
	  _is_quit(false), _thread_id(std::this_thread::get_id()),
	  _timer_queue(this) {
	_ch_event_fd.enable_reading();
	int eventfd = _ch_event_fd.fd();
	_ch_event_fd.setReadCallback([eventfd]() {
		std::uint64_t data = 0;
		int n = -1;
		while (true) {
			n = ::read(eventfd, &data, sizeof(data));
			if (n == -1) {
				if (errno == EINTR) {
					continue;
				} else {
					throw_error("event read failed");
				}
			} else {
				break;
			}
		}
	});
}

void EventLoop::quit() {
	{
		std::lock_guard<std::mutex> lk(_mutex);
		_is_quit = true;
	}
	wakeup();
}

void EventLoop::wakeup() {
	int event_fd = _ch_event_fd.fd();
	std::uint64_t data = 1;
	int res = ::write(event_fd, &data, sizeof(data));
	if (res == -1) { throw_error("eventfd write failed"); }
}

bool EventLoop::run_or_queue_in_loop(std::function<void()> task) {
	if (_thread_id == std::this_thread::get_id()) {
		// 当前线程, run
		{
			std::lock_guard<std::mutex> lk(_mutex);
			if (_is_quit) { return false; }
		}
		task();
	} else {
		// 其他线程, queue
		return queue_in_loop(task);
	}
	return true;
}
bool EventLoop::queue_in_loop(std::function<void()> task) {
	{
		std::lock_guard<std::mutex> lk(_mutex);
		if (_is_quit) { return false; }
		_queue.push(task);
	}
	wakeup();
	return true;
}

void EventLoop::update_channel(Channel *channel) {
	if (_thread_id != std::this_thread::get_id()) {
		throw_error("update_channel should be in the same thread");
	}
	_epoller.update_channel(channel);
}

void EventLoop::remove_channel(Channel *channel) {
	if (_thread_id != std::this_thread::get_id()) {
		throw_error("remove_channel should be in the same thread");
	}
	_epoller.remove_channel(channel);
}

void EventLoop::loop() {
	if (_thread_id != std::this_thread::get_id()) {
		throw_error("loop should be in the same thread");
	}
	while (!_is_quit) {
		std::queue<std::function<void()>> copy;
		{
			std::lock_guard<std::mutex> lk(_mutex);
			copy.swap(_queue);
		}
		while (!copy.empty()) {
			std::function<void()> task = copy.front();
			copy.pop();
			task();
		}
		if (_is_quit) { break; }
		std::vector<Channel *> rd_channels;
		_epoller.epoll_wait(rd_channels);
		for (Channel *channel : rd_channels) {
			channel->handle_event();
		}
	}
	while (!_queue.empty()) {
		std::function<void()> task = _queue.front();
		_queue.pop();
		task();
	}
}

std::optional<std::uint64_t>
EventLoop::add_task_after_delay(std::chrono::steady_clock::duration delay,
								const std::function<void()> &task) {
	return _timer_queue.add_task_after_delay(delay, task);
}

std::optional<std::uint64_t>
EventLoop::add_task_at(std::chrono::steady_clock::time_point timepoint,
					   const std::function<void()> &task) {
	return _timer_queue.add_task_at(timepoint, task);
}

std::optional<std::uint64_t> EventLoop::add_task_repeat_after_delay(
	std::chrono::steady_clock::duration delay,
	std::chrono::steady_clock::duration interval,
	const std::function<void()> &task) {
	return _timer_queue.add_task_repeat_after_delay(delay, interval, task);
}

bool EventLoop::cancel(std::uint64_t task_id) {
	return _timer_queue.cancel(task_id);
}

EventLoop::~EventLoop() {
	if (_ch_event_fd.fd() != -1) {
		_epoller.remove_channel(&_ch_event_fd);
		int res = ::close(_ch_event_fd.fd());
		if (res == -1) { log_error("event fd close failed"); }
	}
}