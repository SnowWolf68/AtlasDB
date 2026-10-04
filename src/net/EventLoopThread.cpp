//
// Created by test on 10/2/26.
//

#include "net/EventLoopThread.h"

#include "log.h"

EventLoopThread::EventLoopThread()
	: _is_create(false), _start(false), _loop(nullptr) {}
EventLoop *EventLoopThread::start() {
	std::lock_guard<std::mutex> start_stop_lock(_start_stop_mutex);
	std::unique_lock<std::mutex> lk(_mutex);
	if (_start) { return _loop; }
	_thread = std::thread([this]() {
		EventLoop lp;
		{
			std::lock_guard<std::mutex> lk(_mutex);
			_loop = &lp;
			_is_create = true;
			_cond.notify_all();
		}
		try {
			_loop->loop();
		} catch (const std::exception &e) {
			ERROR_LOG << "work thread loop error: " << e.what();
		}
		{
			std::lock_guard<std::mutex> lk(_mutex);
			_loop = nullptr;
		}
	});
	_start = true;
	_cond.wait(lk, [this] { return _is_create; });
	return _loop;
}
void EventLoopThread::stop() {
	std::lock_guard<std::mutex> start_stop_lock(_start_stop_mutex);
	{
		std::lock_guard<std::mutex> lk(_mutex);
		if (!_start) { return; }
		_start = false;
		if (_loop != nullptr) _loop->quit();
		_is_create = false;
	}
	_thread.join();
}
EventLoopThread::~EventLoopThread() { stop(); }
