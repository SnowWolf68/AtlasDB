//
// Created by test on 10/2/26.
//

#include "net/acceptor.h"

#include "log.h"

#include "log.h"
#include <cstring>

Acceptor::Acceptor(InetAddress server_address, EventLoop *loop,
				   const std::function<void(Socket connection_socket,
											InetAddress peer_address)>
					   &new_connection_callback)
	: _server_address(server_address), _loop(loop),
	  _listen_socket(Socket::createTcp(AF_INET)),
	  _listen_channel(_listen_socket.fd(), Channel::TriggerMode::Edge, loop),
	  _new_connection_callback(new_connection_callback), _is_listening(false) {
	_listen_socket.set_reuse_addr(true);
	_listen_socket.bind_address(InetAddress(server_address));
	_listen_channel.setReadCallback([this]() {
		if (!_is_listening) { return; }
		InetAddress peer_address;
		int fd = -1;
		while (_is_listening) {
			fd = _listen_socket.accept(&peer_address);
			if (fd != -1) {
				try {
					Socket socket(fd);
					_new_connection_callback(std::move(socket), peer_address);
				} catch (std::exception &e) {
					ERROR_LOG << "new connection callback error: " << e.what();
				}
			} else {
				break;
			}
		}
	});
}
bool Acceptor::listen() {
	_listen_channel.tie(shared_from_this());
	if (_is_listening) {
		DEBUG_LOG << "acceptor already listening";
		return false;
	}
	_listen_socket.listen(128);
	_listen_channel.enable_reading();
	_is_listening = true;
	return true;
}
bool Acceptor::is_listening() { return _is_listening; }

void Acceptor::stop_listening() {
	if (!_is_listening) { return; }
	_is_listening = false;
	_listen_channel.remove();
}

Acceptor::~Acceptor() {
	// 移除的时候一定要保证当前这个sockfd已经被注册到epoll中
	// 否则没注册的直接移除会抛异常

	// 这里同样也会有和TcpConnection类似的问题
	// 也就是持有acceptor的shared_ptr的线程必须是acceptor所属的loop对应的线程
	// 但是对于acceptor来说, 它只会被其对应的loop操作, 并不涉及到跨线程的问题
	// 因此这里这样一定能保证最后remove的线程就是对应的loop对应的线程
	if (_is_listening) { _listen_channel.remove(); }
}
