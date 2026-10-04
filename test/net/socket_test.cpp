#include "net/InetAddress.h"
#include "net/Socket.h"

#include <arpa/inet.h>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <netinet/in.h>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

#ifdef __linux__
#include <dirent.h>
#endif

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

// 连到 127.0.0.1:port 的裸客户端 fd（测试基建,不复用被测代码）
int connect_to(std::uint16_t port) {
	struct sockaddr_in server_address{};
	server_address.sin_family = AF_INET;
	server_address.sin_port = htons(port);
	if (::inet_pton(AF_INET, "127.0.0.1", &server_address.sin_addr) != 1) {
		throw TestFailure("test infra: inet_pton failed for 127.0.0.1");
	}

	int fd = ::socket(AF_INET, SOCK_STREAM, 0);
	if (fd == -1) {
		throw TestFailure("test infra: client socket failed");
	}
	if (::connect(fd, (struct sockaddr *)(&server_address),
				  sizeof(server_address)) == -1) {
		::close(fd);
		throw TestFailure("test infra: connect failed");
	}
	return fd;
}

void wait_readable(int fd) {
	struct pollfd watched{fd, POLLIN, 0};
	int ready;
	do {
		ready = ::poll(&watched, 1, 1000);
	} while (ready == -1 && errno == EINTR);
	if (ready <= 0) {
		throw TestFailure("test infra: timed out waiting for readable fd");
	}
}

int accept_eventually(Socket &server, InetAddress *peer) {
	const auto deadline = std::chrono::steady_clock::now() +
						  std::chrono::seconds(2);
	while (std::chrono::steady_clock::now() < deadline) {
		int fd = server.accept(peer);
		if (fd >= 0) { return fd; }
		wait_readable(server.fd());
	}
	throw TestFailure("test infra: timed out waiting for accepted connection");
}

std::string read_exactly(int fd, std::size_t length) {
	std::string result(length, '\0');
	std::size_t received = 0;
	while (received < length) {
		wait_readable(fd);
		ssize_t count = ::read(fd, &result[received], length - received);
		if (count > 0) {
			received += static_cast<std::size_t>(count);
		} else if (count == 0) {
			throw TestFailure("test infra: EOF before expected bytes");
		} else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
			throw TestFailure("test infra: read failed");
		}
	}
	return result;
}

// ─── createTcp 与 fd 生命周期 ─────────────────────────────────

void test_create_tcp_returns_valid_fd() {
	Socket socket = Socket::createTcp(AF_INET);
	CHECK(socket.fd() >= 0);
	CHECK((::fcntl(socket.fd(), F_GETFL) & O_NONBLOCK) != 0);
}

void test_accept_without_pending_connection_returns_minus_one() {
	Socket server = Socket::createTcp(AF_INET);
	server.bind_address(InetAddress(0));
	server.listen(SOMAXCONN);
	InetAddress peer("1.2.3.4", 1234);
	CHECK(server.accept(&peer) == -1);
	CHECK(peer._ip == "1.2.3.4");
	CHECK(peer._port == 1234);
}

void test_create_tcp_rejects_bogus_family() {
	bool threw = false;
	try {
		Socket socket = Socket::createTcp(9999);
	} catch (const std::runtime_error &) {
		threw = true;
	}
	if (!threw) {
		throw TestFailure("createTcp accepted bogus family 9999");
	}
}

#ifdef __linux__
std::size_t count_open_fds() {
	DIR *dir = opendir("/proc/self/fd");
	if (dir == nullptr) {
		throw TestFailure("test infra: opendir /proc/self/fd failed");
	}
	std::size_t count = 0;
	while (readdir(dir) != nullptr) {
		++count;
	}
	closedir(dir);
	return count;
}

// RAII 契约:Socket 析构必须 close,批量创建销毁后 fd 数不涨
void test_socket_raii_leaks_no_fd() {
	std::size_t before = count_open_fds();
	for (int i = 0; i < 20; ++i) {
		Socket socket = Socket::createTcp(AF_INET);
	}
	std::size_t after = count_open_fds();
	CHECK(before == after);
}
#endif

// ─── bind 临时端口 + 双地址查询 ───────────────────────────────

void test_bind_ephemeral_port_assigns_port() {
	Socket server = Socket::createTcp(AF_INET);
	server.set_reuse_addr(true);
	server.bind_address(InetAddress(0));
	server.listen(SOMAXCONN);

	// bind 端口 0 后,local_address 是得知内核分配了哪个端口的唯一途径
	InetAddress local = server.local_address();
	CHECK(local._port != 0);
	CHECK(local._ip == "0.0.0.0");
}

// ─── 端到端: connect → accept → 双向数据 → 半关闭 ─────────────

void test_end_to_end_connect_accept_and_half_close() {
	Socket server = Socket::createTcp(AF_INET);
	server.set_reuse_addr(true);
	server.bind_address(InetAddress(0));
	server.listen(SOMAXCONN);
	std::uint16_t listen_port = server.local_address()._port;
	CHECK(listen_port != 0);

	// 内核完成握手发生在 connect 返回前,accept 不会久等
	int client_fd = connect_to(listen_port);

	// 客户端本端端口 = 内核临时分配,用于核对 accept 报告的对端端口
	struct sockaddr_in client_local{};
	socklen_t client_local_len = sizeof(client_local);
	CHECK(::getsockname(client_fd, (struct sockaddr *)(&client_local),
						&client_local_len) == 0);
	std::uint16_t client_port = ntohs(client_local.sin_port);
	CHECK(client_port != 0);

	InetAddress peer(0);
	int conn_fd = accept_eventually(server, &peer);
	CHECK(conn_fd >= 0);
	CHECK((::fcntl(conn_fd, F_GETFL) & O_NONBLOCK) != 0);
	CHECK(peer._ip == "127.0.0.1");
	CHECK(peer._port == client_port);

	Socket conn(conn_fd);
	// 连接 fd 的本端: 地址按路由选了 loopback,端口仍是监听端口
	InetAddress conn_local = conn.local_address();
	CHECK(conn_local._ip == "127.0.0.1");
	CHECK(conn_local._port == listen_port);

	// 数据往返
	CHECK(::write(client_fd, "ping", 5) == 5);
	CHECK(read_exactly(conn_fd, 5) == std::string("ping", 5));

	CHECK(::write(conn_fd, "pong", 5) == 5);
	CHECK(read_exactly(client_fd, 5) == std::string("pong", 5));

	// 半关闭:服务端关写端发 FIN,客户端 read 返回 0(EOF)
	conn.shutdown_write();
	char eof_buffer[8]{};
	CHECK(::read(client_fd, eof_buffer, sizeof(eof_buffer)) == 0);

	CHECK(::close(client_fd) == 0);
}

void test_accept_multiple_connections_until_empty() {
	Socket server = Socket::createTcp(AF_INET);
	server.bind_address(InetAddress(0));
	server.listen(SOMAXCONN);
	const std::uint16_t port = server.local_address()._port;

	const int first_client = connect_to(port);
	const int second_client = connect_to(port);
	InetAddress first_peer(0);
	InetAddress second_peer(0);
	Socket first_connection(accept_eventually(server, &first_peer));
	Socket second_connection(accept_eventually(server, &second_peer));
	CHECK(first_connection.fd() >= 0);
	CHECK(second_connection.fd() >= 0);
	CHECK(first_connection.fd() != second_connection.fd());
	CHECK((::fcntl(first_connection.fd(), F_GETFL) & O_NONBLOCK) != 0);
	CHECK((::fcntl(second_connection.fd(), F_GETFL) & O_NONBLOCK) != 0);

	InetAddress unchanged("1.2.3.4", 1234);
	CHECK(server.accept(&unchanged) == -1);
	CHECK(unchanged._ip == "1.2.3.4");
	CHECK(unchanged._port == 1234);
	CHECK(::close(first_client) == 0);
	CHECK(::close(second_client) == 0);
}

// ─── SO_REUSEADDR: 连接周期后立刻重绑同一端口 ─────────────────

void test_rebind_same_port_after_connection_cycle() {
	std::uint16_t port = 0;
	{
		Socket server = Socket::createTcp(AF_INET);
		server.set_reuse_addr(true);
		server.bind_address(InetAddress(0));
		server.listen(SOMAXCONN);
		port = server.local_address()._port;
		CHECK(port != 0);

		// 跑一整笔连接再由服务端主动关闭,制造 TIME_WAIT
		int client_fd = connect_to(port);
		InetAddress peer(0);
		int conn_fd = accept_eventually(server, &peer);
		CHECK(conn_fd >= 0);
		CHECK(::close(conn_fd) == 0);
		CHECK(::close(client_fd) == 0);
	}

	// TIME_WAIT 尚在时重绑同一端口:SO_REUSEADDR 的回归钉(不抛即过)
	Socket reborn = Socket::createTcp(AF_INET);
	reborn.set_reuse_addr(true);
	reborn.bind_address(InetAddress(port));
	reborn.listen(SOMAXCONN);
}

// ─── test runner ────────────────────────────────────────────────

struct TestCase {
	const char *name;
	void (*run)();
};

} // namespace

int main() {
	const TestCase tests[] = {
		{"create_tcp returns valid fd", test_create_tcp_returns_valid_fd},
		{"accept without pending connection returns -1",
		 test_accept_without_pending_connection_returns_minus_one},
		{"create_tcp rejects bogus family",
		 test_create_tcp_rejects_bogus_family},
#ifdef __linux__
		{"socket raII leaks no fd", test_socket_raii_leaks_no_fd},
#endif
		{"bind ephemeral port assigns port",
		 test_bind_ephemeral_port_assigns_port},
		{"end to end connect accept and half close",
		 test_end_to_end_connect_accept_and_half_close},
		{"accept multiple connections until empty",
		 test_accept_multiple_connections_until_empty},
		{"rebind same port after connection cycle",
		 test_rebind_same_port_after_connection_cycle},
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
