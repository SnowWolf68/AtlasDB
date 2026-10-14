//
// Created by test on 9/29/26.
//

#include "net/EPoller.h"
#include "log.h"
#include <sys/epoll.h>

EPoller::EPoller() {
	_epfd = ::epoll_create(1);
	if (_epfd == -1) { throw_error("epoll_create failed"); }
}

void EPoller::update_channel(Channel *channel) {
	if (_channels.count(channel->fd()) != 0) {
		if (channel != _channels[channel->fd()]) {
			throw std::runtime_error(
				"the param channel does not match channel in _channels");
		}
		struct epoll_event event{};
		event.events = channel->events();
		event.data.fd = channel->fd();
		int res = ::epoll_ctl(_epfd, EPOLL_CTL_MOD, channel->fd(), &event);
		if (res == -1) { throw_error("epoll_ctl failed"); }
	} else {
		struct epoll_event event{};
		event.events = channel->events();
		event.data.fd = channel->fd();
		int res = ::epoll_ctl(_epfd, EPOLL_CTL_ADD, channel->fd(), &event);
		if (res == -1) { throw_error("epoll_ctl failed"); }
		_channels[channel->fd()] = channel;
	}
}

void EPoller::remove_channel(Channel *channel) {
	if (_channels.count(channel->fd()) == 0 ||
		channel != _channels[channel->fd()]) {
		throw std::runtime_error(
			"the param channel does not match channel in _channels");
	}
	int res = ::epoll_ctl(_epfd, EPOLL_CTL_DEL, channel->fd(), nullptr);
	if (res == -1) { throw_error("epoll_ctl failed"); }
	_channels.erase(channel->fd());
}

int EPoller::epoll_wait(std::vector<Channel *> &rd_channels) {
	rd_channels.clear();
	struct epoll_event events[MAX_EVENTS] = {};
	int n = -1;
	while (true) {
		n = ::epoll_wait(_epfd, events, MAX_EVENTS, -1);
		if (n == -1) {
			if (errno == EINTR) {
				continue;
			} else {
				throw_error("epoll_wait failed");
				break;
			}
		} else {
			break;
		}
	}
	if (n == -1) { return n; }
	for (int i = 0; i < n; i++) {
		if (_channels.count(events[i].data.fd) == 0) {
			throw std::runtime_error("epoll_wait for non-existing fd");
		}
		Channel *channel = _channels[events[i].data.fd];
		channel->setRevents(events[i].events);
		rd_channels.push_back(channel);
	}
	return n;
}

EPoller::~EPoller() {
	if (_epfd != -1) {
		int res = ::close(_epfd);
		if (res == -1) { log_error("close failed"); }
	}
}