//
// Created by test on 9/28/26.
//

#include "log.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include "net/InetAddress.h"

#define DEBUG_LOG LOG(Log::DEBUG)

int main() {
	Log::get_logger().set_minimum_log_level(Log::DEBUG);
	InetAddress addr1("127.0.0.1", 1234);
	struct sockaddr_in sock_addr;
	sock_addr.sin_family = AF_INET;
	sock_addr.sin_port = htons(4321);
	::inet_pton(AF_INET, "1.1.1.1", &sock_addr.sin_addr);
	InetAddress addr2(sock_addr);

	DEBUG_LOG << addr1._ip << " " << std::to_string(addr1._port);
	DEBUG_LOG << addr2._ip << " " << std::to_string(addr2._port);

}