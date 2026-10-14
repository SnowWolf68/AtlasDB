//
// Created by test on 10/3/26.
//

#ifndef ATLASDB_EVENTLOOPTHREADPOOL_H
#define ATLASDB_EVENTLOOPTHREADPOOL_H

#include "EventLoopThread.h"
#include "net/EventLoop.h"

class EventLoopThreadPool {
public:
	EventLoopThreadPool(EventLoop *base_loop, std::size_t thread_num);

	bool start();
	EventLoop *get_next_loop();
	bool stop();

	~EventLoopThreadPool();

private:
	bool is_in_base_thread();

	EventLoop *_base_loop;
	std::thread::id _base_loop_thread_id;
	std::size_t _thread_num;
	std::vector<EventLoopThread> _event_loop_threads;
	std::vector<EventLoop *> _event_loops;
	bool _start;
	int _next_idx; // -1表示没有工作线程, 只有主线程
};

#endif // ATLASDB_EVENTLOOPTHREADPOOL_H
