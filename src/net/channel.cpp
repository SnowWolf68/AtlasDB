//
// Created by test on 9/29/26.
//

#include "net/channel.h"
#include "log.h"
#include "net/EventLoop.h"
#include <sys/epoll.h>

Channel::Channel(int fd, TriggerMode mode, EventLoop *loop)
	: _fd(fd), _events(0), _revents(0), _trigger_mode(mode), _loop(loop),
	  _tied(false) {
	if (_trigger_mode == TriggerMode::Edge) { _events |= EPOLLET; }
}

Channel::TriggerMode Channel::getTriggerMode() { return _trigger_mode; }
void Channel::setTriggerMode(TriggerMode mode) {
	_trigger_mode = mode;
	if (_trigger_mode == TriggerMode::Edge) {
		_events |= EPOLLET;
	} else {
		_events &= ~EPOLLET;
	}
	_loop->update_channel(this);
}

int Channel::fd() const noexcept { return _fd; }
uint32_t Channel::events() const noexcept { return _events; }
uint32_t Channel::revents() const noexcept { return _revents; }
void Channel::setRevents(uint32_t events) noexcept { _revents = events; }

void Channel::setReadCallback(std::function<void()> cb) { _read_callback = cb; }
void Channel::setWriteCallback(std::function<void()> cb) {
	_write_callback = cb;
}
void Channel::setCloseCallback(std::function<void()> cb) {
	_close_callback = cb;
}
void Channel::setErrorCallback(std::function<void()> cb) {
	_error_callback = cb;
}

void Channel::enable_reading() {
	_events |= EPOLLIN;
	_loop->update_channel(this);
}
void Channel::enable_writing() {
	_events |= EPOLLOUT;
	_loop->update_channel(this);
}
void Channel::disable_writing() {
	_events &= ~EPOLLOUT;
	_loop->update_channel(this);
}
void Channel::disable_reading() {
	_events &= ~EPOLLIN;
	_loop->update_channel(this);
}
void Channel::disable_all() {
	_events = _trigger_mode == TriggerMode::Edge ? EPOLLET : 0;
	_loop->update_channel(this);
}
void Channel::tie(std::shared_ptr<void> ptr) {
	_weak_ptr = ptr;
	_tied = true;
}
void Channel::remove() { _loop->remove_channel(this); }
void Channel::handle_event() {
	// 为什么要有tie?
	//	因为这里的Channel是被一个上层对象持有
	//	那么在执行回调的过程中, 有可能会把上层对象析构掉,
	//	因此channel对象也坑被析构掉
	//	如果channel都被析构了, 下面再调用回调肯定就会有问题
	//	因此这里的tie就是为了保证在执行回调的过程中,
	//	持有channel的上层对象一直存活
	//	(channel在一次handle_event的时候可能多次执行回调)
	std::shared_ptr<void> ptr = nullptr;
	if (_tied) {
		// 获取一个上层指针, 比如TcpConnection,
		// 保证在执行handle的时候上层一直活着
		ptr = _weak_ptr.lock();
		if (ptr == nullptr) { return; }
	}
	if (_revents & EPOLLIN && _read_callback) { _read_callback(); }
	if (_revents & EPOLLOUT && _write_callback) { _write_callback(); }
	if (_revents & EPOLLERR && _error_callback) { _error_callback(); }
	if (_revents & EPOLLHUP && !(_revents & EPOLLIN) && _close_callback) {
		_close_callback();
	}
}
