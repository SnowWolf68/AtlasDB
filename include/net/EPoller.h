//
// Created by test on 9/29/26.
//

#ifndef ATLASDB_EPOLLER_H
#define ATLASDB_EPOLLER_H

#include "net/channel.h"
#include <unordered_map>
#include <vector>

class EPoller {
public:
	static const int MAX_EVENTS = 16;

	EPoller();
	EPoller(EPoller const &) = delete;
	EPoller &operator=(EPoller const &) = delete;
	void update_channel(Channel *channel);
	void remove_channel(Channel *channel);
	int epoll_wait(std::vector<Channel *> &rd_channels);
	~EPoller();

private:
	int _epfd;
	std::unordered_map<int, Channel *> _channels;
};

#endif // ATLASDB_EPOLLER_H
