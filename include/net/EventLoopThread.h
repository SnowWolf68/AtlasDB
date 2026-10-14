//
// Created by test on 10/2/26.
//

#ifndef ATLASDB_EVENTLOOPTHREAD_H
#define ATLASDB_EVENTLOOPTHREAD_H

#include "net/EventLoop.h"
#include <condition_variable>
#include <mutex>

class EventLoopThread {
public:
	EventLoopThread();
	EventLoop *start();
	void stop();
	~EventLoopThread();

private:
	std::mutex _mutex; // 多线程访问当前成员变量的互斥, 以及条件变量也用这个
	std::mutex _start_stop_mutex; // 多线程调用start, stop的互斥
	std::condition_variable _cond;
	bool _is_create;
	bool _start;
	EventLoop *_loop;
	std::thread _thread;
};

#endif // ATLASDB_EVENTLOOPTHREAD_H
