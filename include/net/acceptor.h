//
// Created by test on 10/2/26.
//

#ifndef ATLASDB_ACCEPTOR_H
#define ATLASDB_ACCEPTOR_H

#include "net/EventLoop.h"
#include "net/InetAddress.h"
#include "net/Socket.h"
#include "net/channel.h"
#include <functional>

class Acceptor : public std::enable_shared_from_this<Acceptor> {
public:
	Acceptor(InetAddress server_address, EventLoop *loop,
			 const std::function<void(Socket connection_socket,
									  InetAddress peer_address)>
				 &new_connection_callback);

	bool listen();
	bool is_listening();
	void stop_listening();

	~Acceptor();

private:
	InetAddress _server_address;
	EventLoop *_loop;
	Socket _listen_socket;
	Channel _listen_channel;
	std::function<void(Socket connection_socket, InetAddress peer_address)>
		_new_connection_callback;
	bool _is_listening;
};

#endif // ATLASDB_ACCEPTOR_H
