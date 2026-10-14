//
// Created by test on 9/28/26.
//
#include "net/InetAddress.h"

#include "log.h"

#include <arpa/inet.h>
#include <cstring>
#include <netinet/in.h>

InetAddress::InetAddress(const std::string &ip, std::uint16_t port)
	: _ip(ip), _port(port) {
	memset(&_sockaddr, 0, sizeof(_sockaddr));
	_sockaddr.sin_family = AF_INET;
	_sockaddr.sin_port = htons(port);
	_sockaddr.sin_addr = ip_p2n(_ip);
}

InetAddress::InetAddress(std::uint16_t port) : _port(port) {
	memset(&_sockaddr, 0, sizeof(_sockaddr));
	_sockaddr.sin_family = AF_INET;
	_sockaddr.sin_port = htons(port);
	_sockaddr.sin_addr.s_addr = htonl(INADDR_ANY);
	_ip = ip_n2p(_sockaddr);
}

InetAddress::InetAddress(struct sockaddr_in sockaddr) : _sockaddr(sockaddr) {
	_port = ntohs(sockaddr.sin_port);
	_ip = ip_n2p(_sockaddr);
}

InetAddress::InetAddress() {}

std::string InetAddress::ip_n2p(struct sockaddr_in sockaddr) {
	char ip[INET_ADDRSTRLEN]{};
	::inet_ntop(AF_INET, &sockaddr.sin_addr, ip, sizeof(ip));
	return ip;
}
struct in_addr InetAddress::ip_p2n(const std::string &ip) {
	struct in_addr addr;
	int result = ::inet_pton(AF_INET, ip.c_str(), &addr);
	if (result != 1) {
		LOG(Log::ERROR) << "InetAddress::InetAddress failed";
		throw std::runtime_error("InetAddress::InetAddress failed");
	}
	return addr;
}

std::string InetAddress::to_string_ip_port() const {
	return _ip + ":" + std::to_string(_port);
}