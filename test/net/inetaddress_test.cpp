#include "net/InetAddress.h"

#include <arpa/inet.h>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

class TestFailure : public std::runtime_error {
public:
	explicit TestFailure(const std::string &message)
		: std::runtime_error(message) {}
};

void check(bool condition, const char *expression, int line) {
	if (condition) {
		return;
	}

	std::ostringstream message;
	message << "line " << line << ": check failed: " << expression;
	throw TestFailure(message.str());
}

#define CHECK(expression) check((expression), #expression, __LINE__)

bool sin_zero_is_zero(const struct sockaddr_in &address) {
	for (unsigned char byte : address.sin_zero) {
		if (byte != 0) {
			return false;
		}
	}
	return true;
}

std::uint32_t expected_addr(const char *ip) {
	struct in_addr addr;
	if (::inet_pton(AF_INET, ip, &addr) != 1) {
		throw TestFailure(std::string("test infra: inet_pton failed for ") +
						  ip);
	}
	return addr.s_addr;
}

// ─── (ip, port) 构造：字符串地址 → 二进制 ─────────────────────

void test_ip_port_ctor_stores_fields() {
	InetAddress address("127.0.0.1", 1234);
	CHECK(address._ip == "127.0.0.1");
	CHECK(address._port == 1234);
	CHECK(address.to_string_ip_port() == "127.0.0.1:1234");
}

void test_ip_port_ctor_fills_sockaddr() {
	InetAddress address("192.168.1.10", 80);
	CHECK(address._sockaddr.sin_family == AF_INET);
	CHECK(ntohs(address._sockaddr.sin_port) == 80);
	CHECK(address._sockaddr.sin_addr.s_addr ==
		  expected_addr("192.168.1.10"));
	// memset 契约:填充字节必须确定,否则同地址两次构造 memcmp 不等
	CHECK(sin_zero_is_zero(address._sockaddr));
}

void test_ip_port_ctor_rejects_invalid_ip() {
	const char *bad_ips[] = {
		"999.999.1.1", "256.1.1.1", "1.2.3", "1.2.3.4.5", "abc", "",
	};
	for (const char *ip : bad_ips) {
		bool threw = false;
		try {
			InetAddress address(ip, 1);
		} catch (const std::runtime_error &) {
			threw = true;
		}
		if (!threw) {
			std::ostringstream message;
			message << "invalid ip accepted: \"" << ip << "\"";
			throw TestFailure(message.str());
		}
	}
}

// ─── (port) 监听构造:任意接口 ─────────────────────────────────

void test_listen_ctor_binds_any_address() {
	InetAddress address(5555);
	CHECK(address._port == 5555);
	CHECK(address._ip == "0.0.0.0");
	CHECK(address.to_string_ip_port() == "0.0.0.0:5555");
	CHECK(address._sockaddr.sin_family == AF_INET);
	CHECK(ntohs(address._sockaddr.sin_port) == 5555);
	// htonl(INADDR_ANY) == 0,直接断言网络序数值
	CHECK(address._sockaddr.sin_addr.s_addr == htonl(INADDR_ANY));
	CHECK(sin_zero_is_zero(address._sockaddr));
}

// ─── (sockaddr_in) 构造:内核回填地址的包装 ───────────────────

void test_sockaddr_ctor_derives_fields() {
	struct sockaddr_in raw;
	std::memset(&raw, 0, sizeof(raw));
	raw.sin_family = AF_INET;
	raw.sin_port = htons(4321);
	if (::inet_pton(AF_INET, "1.1.1.1", &raw.sin_addr) != 1) {
		throw TestFailure("test infra: inet_pton failed for 1.1.1.1");
	}

	InetAddress address(raw);
	CHECK(address._ip == "1.1.1.1");
	CHECK(address._port == 4321);
	CHECK(address.to_string_ip_port() == "1.1.1.1:4321");
	// ntohs 方向:从网络序还原出的端口必须等于原始主机序值
	CHECK(ntohs(address._sockaddr.sin_port) == 4321);
	CHECK(address._sockaddr.sin_addr.s_addr == expected_addr("1.1.1.1"));
	// 整体拷贝契约:调用方给的结构体字节原样保留
	CHECK(sin_zero_is_zero(address._sockaddr));
}

void test_sockaddr_ctor_high_port() {
	struct sockaddr_in raw;
	std::memset(&raw, 0, sizeof(raw));
	raw.sin_family = AF_INET;
	raw.sin_port = htons(65535);
	raw.sin_addr.s_addr = expected_addr("10.0.0.1");

	InetAddress address(raw);
	CHECK(address._port == 65535);
	CHECK(address._ip == "10.0.0.1");
}

// ─── 字节序往返 ────────────────────────────────────────────────

void test_port_byte_order_round_trip() {
	const std::uint16_t ports[] = {0, 1, 80, 8080, 65535};
	for (std::uint16_t port : ports) {
		InetAddress explicit_target("127.0.0.1", port);
		CHECK(ntohs(explicit_target._sockaddr.sin_port) == port);

		InetAddress listener(port);
		CHECK(ntohs(listener._sockaddr.sin_port) == port);
	}
}

void test_to_string_ip_port_format() {
	InetAddress address("10.20.30.40", 9999);
	CHECK(address.to_string_ip_port() == "10.20.30.40:9999");

	InetAddress listener(1);
	CHECK(listener.to_string_ip_port() == "0.0.0.0:1");
}

// ─── test runner ────────────────────────────────────────────────

struct TestCase {
	const char *name;
	void (*run)();
};

} // namespace

int main() {
	const TestCase tests[] = {
		{"ip_port ctor stores fields", test_ip_port_ctor_stores_fields},
		{"ip_port ctor fills sockaddr", test_ip_port_ctor_fills_sockaddr},
		{"ip_port ctor rejects invalid ip",
		 test_ip_port_ctor_rejects_invalid_ip},
		{"listen ctor binds any address",
		 test_listen_ctor_binds_any_address},
		{"sockaddr ctor derives fields", test_sockaddr_ctor_derives_fields},
		{"sockaddr ctor high port", test_sockaddr_ctor_high_port},
		{"port byte order round trip", test_port_byte_order_round_trip},
		{"to_string_ip_port format", test_to_string_ip_port_format},
	};

	std::size_t passed = 0;
	for (const TestCase &test : tests) {
		try {
			test.run();
			++passed;
			std::cout << "[PASS] " << test.name << '\n';
		} catch (const std::exception &error) {
			std::cerr << "[FAIL] " << test.name << ": " << error.what()
					  << '\n';
		}
	}

	std::cout << passed << '/' << (sizeof(tests) / sizeof(tests[0]))
			  << " tests passed\n";
	return passed == (sizeof(tests) / sizeof(tests[0])) ? 0 : 1;
}
