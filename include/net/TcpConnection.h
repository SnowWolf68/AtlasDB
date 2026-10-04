//
// Created by test on 10/2/26.
//

#ifndef ATLASDB_TCPCONNECTION_H
#define ATLASDB_TCPCONNECTION_H

#include "net/EventLoop.h"
#include "net/Socket.h"
#include "net/buffer.h"
#include "net/channel.h"

class TcpConnection : public std::enable_shared_from_this<TcpConnection> {
public:
	static std::shared_ptr<TcpConnection>
	create(EventLoop *loop, Socket socket, InetAddress peer_address,
		   std::function<void(std::shared_ptr<TcpConnection>, Buffer &)>
			   server_read_callback,
		   std::function<void(std::shared_ptr<TcpConnection>)>
			   server_close_callback);

	void send_in_loop(std::vector<char> data);
	InetAddress get_peer_address();
	EventLoop *get_event_loop();

	void close();

	~TcpConnection();

private:
	TcpConnection(EventLoop *loop, Socket socket, InetAddress peer_address,
				  std::function<void(std::shared_ptr<TcpConnection>, Buffer &)>
					  server_read_callback,
				  std::function<void(std::shared_ptr<TcpConnection>)>
					  server_close_callback);
	Channel create_channel(EventLoop *loop, int socket_fd);
	void connection_error_callback();
	void connection_error_callback(int errno_code, int get_res = 0);
	void enable_reading_and_tie();
	bool send(std::vector<char> data);
	void close_in_loop();

	EventLoop *_loop;
	Socket _socket;
	InetAddress _peer_address;
	Channel _channel;
	bool _is_registered2epoll;
	bool _is_close;

	Buffer _read_buffer;
	Buffer _write_buffer;

	std::function<void(std::shared_ptr<TcpConnection>, Buffer &)>
		_server_read_callback;
	std::function<void(std::shared_ptr<TcpConnection>)> _server_close_callback;
};

#endif // ATLASDB_TCPCONNECTION_H
