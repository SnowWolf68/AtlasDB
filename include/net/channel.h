//
// Created by test on 9/29/26.
//

#ifndef ATLASDB_CHANNEL_H
#define ATLASDB_CHANNEL_H

#include <cstdint>
#include <functional>
#include <memory>

class EventLoop;

class Channel {
public:
	enum class TriggerMode { Level, Edge };

	Channel(int fd, TriggerMode mode, EventLoop *loop);

	Channel(const Channel &) = delete;
	void operator=(const Channel &) = delete;

	TriggerMode getTriggerMode();
	void setTriggerMode(TriggerMode mode);

	int fd() const noexcept;
	uint32_t events() const noexcept;  // 希望关注的事件
	uint32_t revents() const noexcept; // epoll 实际返回的事件
	void setRevents(uint32_t events) noexcept;

	void setReadCallback(std::function<void()> cb);
	void setWriteCallback(std::function<void()> cb);
	void setCloseCallback(std::function<void()> cb);
	void setErrorCallback(std::function<void()> cb);

	void enable_reading();
	void enable_writing();
	void disable_writing();
	void disable_reading();

	// 只是把事件清除
	void disable_all();

	// 把channel从Epoller中移除
	void remove();

	void tie(std::shared_ptr<void> ptr);

	// 具体的channel执行事件对应的回调
	// 上层应该是EventLoop调用的Channel的handle_event
	void handle_event();

private:
	const int _fd;
	uint32_t _events;
	uint32_t _revents;
	TriggerMode _trigger_mode;
	EventLoop *_loop;
	std::weak_ptr<void> _weak_ptr;
	bool _tied;
	// 各事件回调……
	std::function<void()> _read_callback;
	std::function<void()> _write_callback;
	std::function<void()> _close_callback;
	std::function<void()> _error_callback;
};

#endif // ATLASDB_CHANNEL_H