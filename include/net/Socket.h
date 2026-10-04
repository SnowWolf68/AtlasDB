//
// Created by test on 9/28/26.
//

#ifndef ATLASDB_SOCKET_H
#define ATLASDB_SOCKET_H

#include "net/InetAddress.h"
#include <sys/socket.h>
#include <sys/types.h> /* See NOTES */

/**
 * Socket是一个通用的socket对象, 里面的_fd不一定只是服务端的监听fd
 * 也可以是服务端的具体连接的fd, 也可以是客户端的连接fd
 */
class Socket {
public:
	// 接管已有 fd，例如 accept4() 返回的连接 fd
	Socket(int fd);

	// 内部调用 ::socket()
	static Socket createTcp(int family);

	~Socket();

	Socket(const Socket&) = delete;
	Socket& operator=(const Socket&) = delete;

	Socket(Socket &&other);
	Socket &operator=(Socket &&other);

	int fd() const noexcept { return _fd; }

	void bind_address(const InetAddress& address);
	void listen(int backlog);
	int accept(InetAddress* peer_address) const;

	InetAddress local_address() const;
	InetAddress peer_address() const;

	void shutdown_write();

	void set_reuse_addr(bool on);
	// void set_reuse_port(bool on);
	// void set_tcp_no_delay(bool on);
	// void set_keep_alive(bool on);

private:
	int _fd;
};

#endif // ATLASDB_SOCKET_H
