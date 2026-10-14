//
// Created by test on 10/3/26.
//

#ifndef ATLASDB_TCPSERVER_H
#define ATLASDB_TCPSERVER_H

#include "net/EventLoop.h"
#include "net/EventLoopThreadPool.h"
#include "net/InetAddress.h"
#include "net/TcpConnection.h"
#include "net/acceptor.h"

class TcpServer {
public:
	enum class ServerState { Running, Stopping, Stopped };

	TcpServer(EventLoop *base_loop, std::size_t thread_num,
			  InetAddress server_address,
			  std::function<void(std::shared_ptr<TcpConnection>, Buffer &)>
				  server_read_callback,
			  std::function<void()> stop_finish_callback,
			  std::function<void(std::shared_ptr<TcpConnection>)>
				  server_close_callback);

	bool start();

	// 只是发起关闭请求, 并不代表关闭完成
	bool stop();


	~TcpServer();

private:
	void server_close_callback(std::shared_ptr<TcpConnection> conn);
	bool is_in_base_thread();

	// 给Acceptor用的一个建立连接后的回调
	void new_connection_callback(Socket connection_socket,
								 InetAddress peer_address);

	EventLoop *_base_loop;
	std::size_t _thread_num;
	InetAddress _server_address;
	std::shared_ptr<Acceptor> _acceptor;
	EventLoopThreadPool _thread_pool;
	ServerState _state;
	std::set<std::shared_ptr<TcpConnection>> _connections;
	std::function<void(std::shared_ptr<TcpConnection>, Buffer &)>
		_server_read_callback;
	std::thread::id _base_loop_thread_id;
	std::size_t _pending_conn_count;
	std::function<void()>
		_stop_finish_callback; // 异步stop停止完成后触发, 上层负责析构server对象
	std::function<void(std::shared_ptr<TcpConnection>)> _server_close_callback;
};

#endif // ATLASDB_TCPSERVER_H
