//
// Created by test on 10/3/26.
//

#include "net/EventLoopThreadPool.h"

#include "log.h"

EventLoopThreadPool::EventLoopThreadPool(EventLoop *base_loop,
										 std::size_t thread_num)
	: _base_loop(base_loop), _base_loop_thread_id(std::this_thread::get_id()),
	  _thread_num(thread_num), _event_loop_threads(thread_num),
	  _event_loops(thread_num, nullptr), _start(false),
	  _next_idx(_thread_num == 0 ? -1 : 0) {
	if (_base_loop == nullptr) {
		throw_error(
			"eventloop thread pool create failed: base loop is nullptr");
	}
}

bool EventLoopThreadPool::start() {
	if (!is_in_base_thread()) { return false; }
	if (_start) return true;
	_next_idx = _thread_num == 0 ? -1 : 0;
	for (std::size_t i = 0; i < _thread_num; i++) {
		EventLoopThread &thread = _event_loop_threads[i];
		_event_loops[i] = thread.start();
	}
	_start = true;
	return true;
}

EventLoop *EventLoopThreadPool::get_next_loop() {
	if (!is_in_base_thread()) { return nullptr; }
	if (!_start) { return nullptr; }
	if (_next_idx == -1) {
		return _base_loop;
	} else {
		EventLoop *return_loop = _event_loops[_next_idx];
		_next_idx = (_next_idx + 1) % _thread_num;
		return return_loop;
	}
}

bool EventLoopThreadPool::stop() {
	if (!is_in_base_thread()) { return false; }
	if (!_start) return true;
	for (EventLoopThread &thread : _event_loop_threads) {
		thread.stop();
	}
	_start = false;
	return true;
}

bool EventLoopThreadPool::is_in_base_thread() {
	return std::this_thread::get_id() == _base_loop_thread_id;
}

EventLoopThreadPool::~EventLoopThreadPool() { stop(); }