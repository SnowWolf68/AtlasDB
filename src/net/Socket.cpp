//
// Created by test on 9/28/26.
//

#include "net/Socket.h"
#include "log.h"

#include <cstring>

Socket Socket::createTcp(int family) {
	int fd = ::socket(family, SOCK_STREAM | SOCK_NONBLOCK, 0);
	if (fd == -1) { throw_error("socket create failed"); }
	return Socket(fd);
}

Socket::Socket(int fd) : _fd(fd) {}

Socket::~Socket() {
	if (_fd != -1) {
		int res = ::close(_fd);
		if (res == -1) { log_error("socket close failed"); }
	}
}

void Socket::bind_address(const InetAddress &address) {
	int res = ::bind(_fd, (struct sockaddr *)(&address._sockaddr),
					 sizeof(address._sockaddr));
	if (res == -1) { throw_error("socket bind failed"); }
}

void Socket::listen(int backlog) {
	int res = ::listen(_fd, backlog);
	if (res == -1) { throw_error("socket listen failed"); }
}

int Socket::accept(InetAddress *peer_address) const {
	struct sockaddr_in address{};
	socklen_t address_len = sizeof(address);
	int fd = -1;
	while (true) {
		fd = ::accept4(_fd, (struct sockaddr *)(&address), &address_len,
					   SOCK_NONBLOCK);
		if (fd == -1) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				return -1;
			} else if (errno == EINTR || errno == ECONNABORTED) {
				continue;
			} else {
				throw_error("socket accept failed");
			}
		} else {
			break;
		}
	}
	*peer_address = address;
	return fd;
}

InetAddress Socket::local_address() const {
	struct sockaddr_in address{};
	socklen_t address_len = sizeof(address);
	int res = ::getsockname(_fd, (struct sockaddr *)(&address), &address_len);
	if (res == -1) { throw_error("socket getsockname failed"); }
	return InetAddress(address);
}
InetAddress Socket::peer_address() const {
	struct sockaddr_in address{};
	socklen_t address_len = sizeof(address);
	int res = ::getpeername(_fd, (struct sockaddr *)(&address), &address_len);
	if (res == -1) { throw_error("socket getpeername failed"); }
	return InetAddress(address);
}

void Socket::shutdown_write() {
	int res = ::shutdown(_fd, SHUT_WR);
	if (res == -1) { log_error("socket shutdown write failed"); }
}

void Socket::set_reuse_addr(bool on) {
	int integer = on ? 1 : 0;
	int res =
		::setsockopt(_fd, SOL_SOCKET, SO_REUSEADDR, &integer, sizeof(integer));
	if (res == -1) { throw_error("socket set_reuse_addr failed"); }
}

Socket::Socket(Socket &&other) : _fd(-1) { std::swap(_fd, other._fd); }

Socket &Socket::operator=(Socket &&other) {
	std::swap(_fd, other._fd);
	return *this;
}
