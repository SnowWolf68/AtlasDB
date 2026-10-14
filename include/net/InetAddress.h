//
// Created by test on 9/28/26.
//

#ifndef ATLASDB_INETADDRESS_H
#define ATLASDB_INETADDRESS_H
#include <cstdint>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <sys/types.h> /* See NOTES */

class InetAddress {
public:
	InetAddress(const std::string &ip, std::uint16_t port);
	InetAddress(std::uint16_t port);
	InetAddress(struct sockaddr_in sockaddr);
	InetAddress();

	std::string to_string_ip_port() const;

	struct sockaddr_in _sockaddr;
	std::string _ip;
	std::uint16_t _port;

private:
	static std::string ip_n2p(struct sockaddr_in sockaddr);
	static struct in_addr ip_p2n(const std::string &ip);
};


#endif // ATLASDB_INETADDRESS_H
