//
// Created by test on 10/3/26.
//

#include "net/TcpServer.h"

#include "log.h"

TcpServer::TcpServer(
	EventLoop *base_loop, std::size_t thread_num, InetAddress server_address,
	std::function<void(std::shared_ptr<TcpConnection>, Buffer &)>
		server_read_callback,
	std::function<void()> stop_finish_callback,
	std::function<void(std::shared_ptr<TcpConnection>)> server_close_callback)
	: _base_loop(base_loop), _thread_num(thread_num),
	  _server_address(server_address),
	  _acceptor(std::make_shared<Acceptor>(
		  server_address, base_loop,
		  [this](Socket connection_socket, InetAddress peer_address) {
			  new_connection_callback(std::move(connection_socket),
									  peer_address);
		  })),
	  _thread_pool(base_loop, thread_num), _state(ServerState::Stopped),
	  _server_read_callback(server_read_callback),
	  _base_loop_thread_id(std::this_thread::get_id()), _pending_conn_count(0),
	  _stop_finish_callback(stop_finish_callback),
	  _server_close_callback(server_close_callback) {}

void TcpServer::server_close_callback(std::shared_ptr<TcpConnection> conn) {
	_base_loop->queue_in_loop([this, conn]() {
		_connections.erase(conn);
		if (_state == ServerState::Stopping && _connections.empty() &&
			_pending_conn_count == 0) {
			_thread_pool.stop();
			// 执行到这里的时候, 有可能Acceptor里还有没有处理的连接
			// 如果直接析构server对象, 那么就会连着里面的Acceptor,
			// Channel一起销毁了 后面loop再执行channel的回调的时候,
			// 这里的channel对象已经销毁了, 此时就会有问题
			_base_loop->queue_in_loop([this]() {
				_state = ServerState::Stopped;
				std::function<void()> copy = _stop_finish_callback;
				copy();
			});
		}
		// 回调通知上层AtlasServer也删除连接以及对应的Resp
		_server_close_callback(conn);
	});
	DEBUG_LOG << "connection: " << conn->get_peer_address().to_string_ip_port()
			  << "close";
}

// 给Acceptor用的一个建立连接后的回调
void TcpServer::new_connection_callback(Socket connection_socket,
										InetAddress peer_address) {
	if (_state != ServerState::Running) { return; }
	EventLoop *work_loop = _thread_pool.get_next_loop();
	std::shared_ptr<Socket> sock_ptr =
		std::make_shared<Socket>(std::move(connection_socket));
	_pending_conn_count++;
	bool res = work_loop->queue_in_loop([this, work_loop, sock_ptr,
										 peer_address]() {
		std::shared_ptr<TcpConnection> conn = TcpConnection::create(
			work_loop, std::move(*sock_ptr), peer_address,
			_server_read_callback, [this](std::shared_ptr<TcpConnection> conn) {
				server_close_callback(conn);
			});
		_base_loop->run_or_queue_in_loop([this, conn]() {
			_connections.insert(conn);
			_pending_conn_count--;
			if (_state == ServerState::Stopping) { conn->close(); }
		});
	});
	if (!res) {
		_pending_conn_count--;
		ERROR_LOG << "create connection failed";
	}
}

bool TcpServer::is_in_base_thread() {
	return _base_loop_thread_id == std::this_thread::get_id();
}

bool TcpServer::start() {
	if (!is_in_base_thread()) { return false; }
	if (_state == ServerState::Stopping) { return false; }
	if (_state == ServerState::Running) { return true; }
	if (!_acceptor->listen()) { throw_error("acceptor listen failed"); }
	if (!_thread_pool.start()) { throw_error("start thread_pool failed"); }
	_state = ServerState::Running;
	return true;
}

// 只是发起关闭请求, 并不代表关闭完成
bool TcpServer::stop() {
	if (!is_in_base_thread()) { return false; }
	if (_state == ServerState::Stopped || _state == ServerState::Stopping) {
		return true;
	}
	_state = ServerState::Stopping;
	_acceptor->stop_listening();
	if (_connections.empty() && _pending_conn_count == 0) {
		_thread_pool.stop();
		// 执行到这里的时候, 有可能Acceptor里还有没有处理的连接
		// 如果直接析构server对象, 那么就会连着里面的Acceptor,
		// Channel一起销毁了 后面loop再执行channel的回调的时候,
		// 这里的channel对象已经销毁了, 此时就会有问题
		_base_loop->queue_in_loop([this]() {
			_state = ServerState::Stopped;
			std::function<void()> copy = _stop_finish_callback;
			copy();
		});
		return true;
	}
	for (auto &conn : _connections) {
		conn->close();
	}
	return true;
}

TcpServer::~TcpServer() {}