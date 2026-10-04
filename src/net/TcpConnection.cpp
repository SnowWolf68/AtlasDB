//
// Created by test on 10/2/26.
//

#include "net/TcpConnection.h"

#include "log.h"

#include <cstring>
#include <unistd.h>

TcpConnection::TcpConnection(
	EventLoop *loop, Socket socket, InetAddress peer_address,
	std::function<void(std::shared_ptr<TcpConnection>, Buffer &)>
		server_read_callback,
	std::function<void(std::shared_ptr<TcpConnection>)> server_close_callback)
	: _loop(loop), _socket(std::move(socket)), _peer_address(peer_address),
	  _channel(create_channel(loop, _socket.fd())), _is_registered2epoll(false),
	  _is_close(false), _server_read_callback(server_read_callback),
	  _server_close_callback(server_close_callback) {
	_channel.setReadCallback([this]() {
		if (_is_close) { return; }
		int sockfd = _socket.fd();
		int tot_read_size = 0;
		char buf[1024] = {0};
		bool eof = false;
		while (true) {
			memset(buf, 0, sizeof(buf));
			int n = ::read(sockfd, buf, sizeof(buf));
			if (n == -1) {
				if (errno == EAGAIN) {
					// 没有数据可读
					break;
				} else if (errno == EINTR) {
					continue;
				} else {
					connection_error_callback(errno);
					return;
				}
			} else if (n == 0) {
				// 对方关闭了连接
				eof = true;
				break;
			} else {
				// 正常得到数据
				_read_buffer.append(buf, n);
				tot_read_size += n;
			}
		}
		if (tot_read_size > 0) {
			_server_read_callback(shared_from_this(), _read_buffer);
		}
		if (eof && !_is_close) { close(); }
	});
	_channel.setWriteCallback([this]() {
		if (_is_close) { return; } // 可写表示socket缓冲区有空, 不一定非要写东西
		int tot_write_size = 0;
		while (true) {
			int n = ::send(
				_socket.fd(), _write_buffer.peek().data() + tot_write_size,
				_write_buffer.peek().size() - tot_write_size, MSG_NOSIGNAL);
			if (n == -1) {
				if (errno == EINTR) {
					continue;
				} else if (errno == EAGAIN) {
					break;
				} else {
					connection_error_callback(errno);
					return;
				}
			} else {
				tot_write_size += n;
				if (tot_write_size == _write_buffer.peek().size()) { break; }
			}
		}
		if (tot_write_size < _write_buffer.peek().size()) {
			_write_buffer.erase(tot_write_size);
			_channel.enable_writing();
			_is_registered2epoll = true;
		} else {
			_write_buffer.erase(tot_write_size);
			_channel.disable_writing();
		}
	});
	_channel.setCloseCallback([this]() { close(); });
	_channel.setErrorCallback([this]() { connection_error_callback(); });
}

void TcpConnection::connection_error_callback(int errno_code, int get_res) {
	if (get_res == -1) {
		// 日志展开的时候有可能也有错误, 可能把之前的errno覆盖
		int errno_backup = errno;
		ERROR_LOG << "get socket error failed: " << strerror(errno_backup);
	} else {
		if (errno_code == 0) {
			return;
		} else {
			ERROR_LOG << "socket error: " << strerror(errno_code);
		}
	}
	close();
}

void TcpConnection::connection_error_callback() {
	int errno_code = 0;
	socklen_t len = sizeof(errno_code);
	int get_res =
		::getsockopt(_socket.fd(), SOL_SOCKET, SO_ERROR, &errno_code, &len);
	connection_error_callback(errno_code, get_res);
}


Channel TcpConnection::create_channel(EventLoop *loop, int socket_fd) {
	return Channel(socket_fd, Channel::TriggerMode::Edge, loop);
}

void TcpConnection::enable_reading_and_tie() {
	_channel.enable_reading();
	_channel.tie(shared_from_this());
}
std::shared_ptr<TcpConnection> TcpConnection::create(
	EventLoop *loop, Socket socket, InetAddress peer_address,
	std::function<void(std::shared_ptr<TcpConnection>, Buffer &)>
		server_read_callback,
	std::function<void(std::shared_ptr<TcpConnection>)> server_close_callback) {
	std::shared_ptr<TcpConnection> conn_ptr(
		new TcpConnection(loop, std::move(socket), peer_address,
						  server_read_callback, server_close_callback));
	conn_ptr->enable_reading_and_tie();
	conn_ptr->_is_registered2epoll = true;
	return conn_ptr;
}
void TcpConnection::send_in_loop(std::vector<char> data) {
	std::shared_ptr<TcpConnection> ptr = shared_from_this();
	_loop->run_or_queue_in_loop([ptr, data]() { ptr->send(data); });
}

InetAddress TcpConnection::get_peer_address() { return _peer_address; }

EventLoop *TcpConnection::get_event_loop() { return _loop; }

bool TcpConnection::send(std::vector<char> data) {
	if (_is_close) { return false; }
	int tot_write_size = 0;
	if (_write_buffer.peek().empty()) {
		// 先往socket里写, 写不下的再放到buffer, 然后enablewriting
		while (true) {
			int n = ::send(_socket.fd(), data.data() + tot_write_size,
						   data.size() - tot_write_size, MSG_NOSIGNAL);
			if (n == -1) {
				if (errno == EINTR) {
					continue;
				} else if (errno == EAGAIN) {
					// 发送缓冲区已满, 剩下的放到buffer
					break;
				} else {
					connection_error_callback(errno);
					return false;
				}
			} else {
				tot_write_size += n;
				if (tot_write_size == data.size()) { break; }
			}
		}
		if (tot_write_size == data.size()) {
			// 全都放到socket缓冲区了
			return true;
		}
	}
	// 剩下的都往buffer中写
	_write_buffer.append(data.data() + tot_write_size,
						 data.size() - tot_write_size);
	_channel.enable_writing();
	_is_registered2epoll = true;
	return true;
}
void TcpConnection::close_in_loop() {
	if (_is_registered2epoll) { _channel.remove(); }
	_is_registered2epoll = false;
	_server_close_callback(shared_from_this());
}

void TcpConnection::close() {
	std::shared_ptr<TcpConnection> conn = shared_from_this();
	_loop->run_or_queue_in_loop([conn]() {
		if (conn->_is_close) { return; }
		conn->_is_close = true;
		conn->_loop->queue_in_loop([conn]() { conn->close_in_loop(); });
	});
}
TcpConnection::~TcpConnection() {
	// 需要保证析构TcpConnectioon的线程和TcpConnection所属的eventloop是同一个线程
	// 也就是说,
	// 需要保证持有connection的shared_ptr的一定是connection所属的loop对应的线程
	// 但是这样很难保证,
	// 因为connection可能会涉及到其他线程跨线程投递send(data)的情况
	// acceptor那边不会有跨线程的问题,
	// 但是connection就很容易出现跨线程访问的情况

	// 因此这里将channel.remove让上层loop通过显式close进行调用
}