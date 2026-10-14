//
// Created by test on 10/4/26.
//

#include "AtlasServer.h"

#include "log.h"

AtlasServer::AtlasServer(EventLoop *loop,
						 std::function<void()> stop_finish_callback,
						 std::size_t thread_num, InetAddress server_address)
	: _loop(loop), _stop_finish_callback(stop_finish_callback),
	  _server(
		  loop, thread_num, server_address,
		  [this](std::shared_ptr<TcpConnection> conn, Buffer &buffer) {
			  server_read_callback(conn, buffer);
		  },
		  stop_finish_callback,
		  [this](std::shared_ptr<TcpConnection> conn) {
			  server_close_callback(conn);
		  }),
	  _lsm("./data") {}

bool AtlasServer::start() { return _server.start(); }

bool AtlasServer::stop() { return _server.stop(); }

void AtlasServer::server_close_callback(std::shared_ptr<TcpConnection> conn) {
	std::lock_guard<std::mutex> lock(_mutex);
	_connecions.erase(conn);
}

void AtlasServer::build_and_send_simple_string(
	const std::string &simple_string, std::shared_ptr<TcpConnection> conn) {
	std::stringstream ss;
	ss << "+" << simple_string << SEP;
	std::string s = ss.str();
	std::vector<char> data(s.begin(), s.end());
	conn->send_in_loop(data);
}

void AtlasServer::build_and_send_bulk_string(
	const std::string &bulk_string, std::shared_ptr<TcpConnection> conn,
	bool is_null) {
	std::stringstream ss;
	if (is_null) {
		ss << "$-1" << SEP;
	} else {
		ss << "$" << bulk_string.size() << SEP;
		ss << bulk_string << SEP;
	}
	std::string s = ss.str();
	std::vector<char> data(s.begin(), s.end());
	conn->send_in_loop(data);
}

void AtlasServer::build_and_send_error(const std::string &error_string,
									   std::shared_ptr<TcpConnection> conn) {
	std::stringstream ss;
	ss << "-ERR " << error_string << SEP;
	std::string s = ss.str();
	std::vector<char> data(s.begin(), s.end());
	conn->send_in_loop(data);
}

void AtlasServer::build_and_send_integer(int integer,
										 std::shared_ptr<TcpConnection> conn) {
	std::stringstream ss;
	ss << ":" << integer << SEP;
	std::string s = ss.str();
	std::vector<char> data(s.begin(), s.end());
	conn->send_in_loop(data);
}

bool AtlasServer::service(std::shared_ptr<TcpConnection> conn,
						  Resp::DecodeResult dec_res) {
	if (dec_res._result_value._value_type != Resp::RespValueType::Arrays) {
		// 客户端的请求类型一定是数组类型,
		// 里面每一个命令字符串都是一个BulkString
		ERROR_LOG << "command resp type is not arrays";
		build_and_send_error("command resp type is not arrays", conn);
		return true;
	}
	std::vector<Resp::RespValue> array = dec_res._result_value._array;
	if (array.size() == 0) {
		ERROR_LOG << "array is empty";
		build_and_send_error("array is empty", conn);
		return true;
	}
	if (array[0]._value_type != Resp::RespValueType::BulkString) {
		ERROR_LOG << "command resp type is not BulkString";
		build_and_send_error("command resp type is not BulkString", conn);
		return true;
	}

	// 命令忽略大小写
	std::string &command = array[0]._bulk_string;
	std::transform(
		command.begin(), command.end(), command.begin(),
		[](unsigned char c) { return static_cast<char>(std::toupper(c)); });

	if (array[0]._bulk_string == "PING") {
		if (array.size() > 2) {
			ERROR_LOG << "wrong number of arguments for 'PING' command";
			build_and_send_error("wrong number of arguments for 'PING' command",
								 conn);
			return true;
		} else if (array.size() == 1) {
			build_and_send_simple_string("PONG", conn);
		} else {
			if (array[1]._value_type != Resp::RespValueType::BulkString) {
				ERROR_LOG << "PING command arg1 must be bulk string";
				build_and_send_error("PING command arg1 must be bulk string",
									 conn);
				return true;
			}
			build_and_send_bulk_string(array[1]._bulk_string, conn);
		}
	} else if (array[0]._bulk_string == "SET") {
		if (array.size() != 3 ||
			array[1]._value_type != Resp::RespValueType::BulkString ||
			array[2]._value_type != Resp::RespValueType::BulkString) {
			ERROR_LOG << "set command requires 3 bulk string parameters";
			build_and_send_error(
				"set command requires 3 bulk string parameters", conn);
			return true;
		}
		std::string key = array[1]._bulk_string, value = array[2]._bulk_string;
		if (key.size() > _lsm.get_max_key_size()) {
			ERROR_LOG << "key size exceeds max key size";
			build_and_send_error("key size exceeds max key size", conn);
			return true;
		}
		if (value.size() > _lsm.get_max_value_size()) {
			ERROR_LOG << "value size exceeds max value size";
			build_and_send_error("value size exceeds max value size", conn);
			return true;
		}
		std::optional<std::uint64_t> put_res = _lsm.put(key, value);
		if (put_res == std::nullopt) {
			ERROR_LOG << "set command execute failed";
			build_and_send_error("set command execute failed", conn);
			return true;
		}
		// +OK\r\n
		build_and_send_simple_string("OK", conn);
	} else if (array[0]._bulk_string == "GET") {
		if (array.size() != 2 ||
			array[1]._value_type != Resp::RespValueType::BulkString) {
			ERROR_LOG << "get command requires 2 bulk string arguments";
			build_and_send_error("get command requires 2 bulk string arguments",
								 conn);
			return true;
		}
		std::string key = array[1]._bulk_string;
		LookUpResult get_res = _lsm.get(key);
		if (get_res._value_type == NotFound) {
			build_and_send_bulk_string("", conn, true);
		} else {
			// Value
			build_and_send_bulk_string(get_res._value, conn);
		}
	} else if (array[0]._bulk_string == "DEL") {
		if (array.size() != 2 ||
			array[1]._value_type != Resp::RespValueType::BulkString) {
			ERROR_LOG << "del command requires 2 bulk string arguments";
			build_and_send_error("del command requires 2 bulk string arguments",
								 conn);
			return true;
		}
		std::string key = array[1]._bulk_string;
		LSMEngine::DelResult del_result = _lsm.del(key);
		if (del_result == LSMEngine::DelResult::NotFound) {
			build_and_send_integer(0, conn);
		} else if (del_result == LSMEngine::DelResult::Error) {
			ERROR_LOG << "del command execute failed, try again";
			build_and_send_error("del command execute failed, try again", conn);
		} else {
			build_and_send_integer(1, conn);
		}
	} else {
		ERROR_LOG << "command not supported";
		build_and_send_error("command not supported", conn);
		return true;
	}
	return true;
}

void AtlasServer::server_read_callback(std::shared_ptr<TcpConnection> conn,
									   Buffer &buffer) {
	// 看一下是不是新连接, 然后获取到对应的resp对象
	std::shared_ptr<Resp> resp = nullptr;
	{
		std::lock_guard<std::mutex> lock(_mutex);
		if (_connecions.find(conn) == _connecions.end()) {
			// 新连接
			std::shared_ptr<Resp> new_resp = std::make_shared<Resp>();
			_connecions[conn] = new_resp;
			resp = new_resp;
		} else {
			// 旧连接
			resp = _connecions[conn];
		}
	}
	// 新连接就可以读里面的数据并进行处理
	bool append_res = resp->append(std::string(buffer.peek()));
	if (!append_res) {
		ERROR_LOG << "resp append command failed";
		conn->close();
		return;
	}
	buffer.erase(buffer.get_readable_size());
	Resp::DecodeResult dec_res = resp->decode();
	bool is_close = false;
	while (dec_res._state == Resp::DecodeState::Complete && !is_close) {
		bool res = service(conn, dec_res);
		if (!res) {
			// 服务端生成响应有问题
			ERROR_LOG << "server process failed, close connection";
			conn->close();
			is_close = true;
		}
		dec_res = resp->decode();
	}
	if (dec_res._state == Resp::DecodeState::Error) {
		ERROR_LOG << "resp decode failed, close connection";
		conn->close();
	}
	// NeedMore不用管, 直接等待下一次数据发来再处理
}