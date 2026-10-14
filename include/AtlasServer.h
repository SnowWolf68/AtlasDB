//
// Created by test on 10/4/26.
//

#ifndef ATLASDB_ATLASSERVER_H
#define ATLASDB_ATLASSERVER_H
#include "lsm.h"
#include "net/TcpServer.h"
#include "resp.h"

class AtlasServer {
public:
	inline static const std::string SEP = "\r\n";

	AtlasServer(EventLoop *loop, std::function<void()> stop_finish_callback,
				std::size_t thread_num, InetAddress server_address);

	bool start();
	bool stop();

private:
	void server_close_callback(std::shared_ptr<TcpConnection> conn);
	void server_read_callback(std::shared_ptr<TcpConnection> conn,
							  Buffer &buffer);
	bool service(std::shared_ptr<TcpConnection> conn,
				 Resp::DecodeResult dec_res);
	void build_and_send_simple_string(const std::string &simple_string,
									  std::shared_ptr<TcpConnection> conn);

	void build_and_send_bulk_string(const std::string &bulk_string,
								  std::shared_ptr<TcpConnection> conn, bool is_null = false);
	void build_and_send_error(const std::string &error_string, std::shared_ptr<TcpConnection> conn);
	void build_and_send_integer(int integer, std::shared_ptr<TcpConnection> conn);


	EventLoop *_loop;
	std::function<void()> _stop_finish_callback;
	TcpServer _server;
	std::mutex
		_mutex; // 因为这里的connections可能会涉及到并发访问, 因此加锁控制
	std::unordered_map<std::shared_ptr<TcpConnection>, std::shared_ptr<Resp>>
		_connecions;
	LSM _lsm;
};

#endif // ATLASDB_ATLASSERVER_H
