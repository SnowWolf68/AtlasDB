#include "AtlasServer.h"
#include "integration_support.h"

#include <condition_variable>
#include <mutex>
#include <poll.h>
#include <sys/socket.h>

namespace {
using namespace integration;
InetAddress bound_address;
std::atomic<int> accepted_fd{-1};
std::mutex drain_mutex;
std::condition_variable drain_cv;
unsigned drains = 0;

Socket connect_peer(const InetAddress &address) {
	Socket peer(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0));
	CHECK(peer.fd() >= 0);
	int result = ::connect(peer.fd(), reinterpret_cast<const sockaddr *>(&address._sockaddr),
		sizeof(address._sockaddr));
	if (result < 0) {
		CHECK(errno == EINPROGRESS);
		pollfd watched{peer.fd(), POLLOUT, 0};
		CHECK(::poll(&watched, 1, 5000) > 0);
		int error = -1;
		socklen_t size = sizeof(error);
		CHECK(::getsockopt(peer.fd(), SOL_SOCKET, SO_ERROR, &error, &size) == 0);
		CHECK(error == 0);
	}
	return peer;
}
void wait_io(int fd, short events) {
	pollfd watched{fd, events, 0};
	int result;
	do { result = ::poll(&watched, 1, 5000); } while (result < 0 && errno == EINTR);
	CHECK(result > 0 && !(watched.revents & POLLNVAL));
}
void send_bytes(int fd, const std::string &bytes) {
	std::size_t offset = 0;
	while (offset < bytes.size()) {
		auto n = ::send(fd, bytes.data() + offset, bytes.size() - offset, MSG_NOSIGNAL);
		if (n > 0) { offset += static_cast<std::size_t>(n); }
		else if (n < 0 && errno == EINTR) { continue; }
		else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { wait_io(fd, POLLOUT); }
		else { CHECK(false); }
	}
}
std::string receive(int fd, std::size_t size) {
	std::string bytes(size, '\0');
	std::size_t offset = 0;
	while (offset < size) {
		auto n = ::recv(fd, &bytes[offset], size - offset, 0);
		if (n > 0) { offset += static_cast<std::size_t>(n); }
		else if (n < 0 && errno == EINTR) { continue; }
		else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { wait_io(fd, POLLIN); }
		else { CHECK(false); }
	}
	return bytes;
}
void expect_eof(int fd) {
	for (;;) {
		char byte;
		auto n = ::recv(fd, &byte, 1, 0);
		if (n == 0) { return; }
		if (n < 0 && errno == EINTR) { continue; }
		CHECK(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
		wait_io(fd, POLLIN);
	}
}
std::string bulk(const std::string &s) { return "$" + std::to_string(s.size()) + "\r\n" + s + "\r\n"; }
std::string command(std::initializer_list<std::string> args) {
	std::string frame = "*" + std::to_string(args.size()) + "\r\n";
	for (const auto &arg : args) { frame += bulk(arg); }
	return frame;
}
void exchange(int fd, const std::string &request, const std::string &response) {
	send_bytes(fd, request);
	CHECK(receive(fd, response.size()) == response);
}
void expect_error(int fd, const std::string &request) {
	// Follow the failing command with PING to verify framing and connection reuse.
	send_bytes(fd, request + command({"PING"}));
	std::string line;
	while (line.size() < 1024) {
		line += receive(fd, 1);
		if (line.size() >= 2 && line.substr(line.size() - 2) == "\r\n") { break; }
	}
	CHECK(line.rfind("-ERR ", 0) == 0 && line.size() > 7);
	CHECK(line.substr(line.size() - 2) == "\r\n");
	CHECK(receive(fd, 7) == "+PONG\r\n");
}
using Stop = std::function<void()>;
void serve(std::size_t workers, const std::function<void(InetAddress, Stop)> &script) {
	EventLoop base;
	const auto owner = std::this_thread::get_id();
	int notifications = 0;
	AtlasServer server(&base, [&] {
		CHECK(std::this_thread::get_id() == owner);
		CHECK(++notifications == 1);
		base.quit();
	}, workers, InetAddress("127.0.0.1", 0));
	CHECK(server.start());
	CHECK(server.start());
	const auto address = bound_address;
	std::atomic<bool> requested{false};
	Stop stop = [&] {
		if (!requested.exchange(true)) {
			CHECK(base.queue_in_loop([&] { CHECK(server.stop()); }));
		}
	};
	auto client = std::async(std::launch::async, [&] {
		try {
			CHECK(!server.start()); CHECK(!server.stop());
			script(address, stop);
		} catch (...) { stop(); throw; }
		stop();
	});
	base.loop();
	await(client);
	CHECK(notifications == 1);
}
void basic(std::size_t workers) {
	serve(workers, [](InetAddress address, Stop) {
		auto peer = connect_peer(address); int fd = peer.fd();
		exchange(fd, command({"pInG"}), "+PONG\r\n");
		exchange(fd, command({"ping", "hello"}), bulk("hello"));
		exchange(fd, command({"PING", ""}), bulk(""));
		exchange(fd, command({"get", "missing"}), "$-1\r\n");
		exchange(fd, command({"sEt", "Key", "one"}), "+OK\r\n");
		exchange(fd, command({"GET", "key"}), "$-1\r\n");
		exchange(fd, command({"GET", "Key"}), bulk("one"));
		exchange(fd, command({"SET", "Key", "two"}), "+OK\r\n");
		exchange(fd, command({"GET", "Key"}), bulk("two"));
		exchange(fd, command({"dEl", "Key"}), ":1\r\n");
		exchange(fd, command({"DEL", "Key"}), ":0\r\n");
		exchange(fd, command({"GET", "Key"}), "$-1\r\n");
	});
}
void binary_and_empty() {
	serve(2, [](InetAddress address, Stop) {
		auto peer = connect_peer(address); int fd = peer.fd();
		std::string key("k\0\r\n", 4), value("v\0\r\n\xff", 5);
		exchange(fd, command({"SET", key, value}), "+OK\r\n");
		exchange(fd, command({"GET", key}), bulk(value));
		exchange(fd, command({"PING", value}), bulk(value));
		exchange(fd, command({"SET", "", ""}), "+OK\r\n");
		exchange(fd, command({"GET", ""}), "$0\r\n\r\n");
		exchange(fd, command({"DEL", ""}), ":1\r\n");
		exchange(fd, command({"GET", ""}), "$-1\r\n");
	});
}
void command_errors() {
	serve(2, [](InetAddress address, Stop) {
		auto peer = connect_peer(address);
		const std::vector<std::string> invalid = {
			"+PING\r\n", "*0\r\n", "*1\r\n:1\r\n", command({"UNKNOWN"}),
			command({"PING", "a", "b"}), "*2\r\n$4\r\nPING\r\n:1\r\n",
			command({"SET"}), command({"SET", "k"}), command({"SET", "k", "v", "extra"}),
			"*3\r\n$3\r\nSET\r\n:1\r\n$1\r\nv\r\n", "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n:1\r\n",
			command({"GET"}), command({"GET", "k", "extra"}), "*2\r\n$3\r\nGET\r\n:1\r\n",
			command({"DEL"}), command({"DEL", "k", "extra"}), "*2\r\n$3\r\nDEL\r\n:1\r\n"
		};
		for (const auto &request : invalid) { expect_error(peer.fd(), request); }
	});
}
void pipeline() {
	serve(3, [](InetAddress address, Stop) {
		auto peer = connect_peer(address);
		std::string requests, replies;
		for (int i = 0; i < 80; ++i) {
			std::string key = std::to_string(i), value(1024, static_cast<char>('a' + i % 26));
			requests += command({"SET", key, value}) + command({"GET", key}) + command({"DEL", key});
			replies += "+OK\r\n" + bulk(value) + ":1\r\n";
		}
		CHECK(requests.size() > 65536);
		exchange(peer.fd(), requests, replies);
	});
}
void fragmented_connections() {
	serve(2, [](InetAddress address, Stop) {
		auto first = connect_peer(address);
		exchange(first.fd(), command({"PING"}), "+PONG\r\n");
		// accept4 observation now refers to this first connection. Waiting for
		// read EAGAIN forces distinct server read callbacks, without sleeps.
		const auto request = command({"SET", "split", "value"});
		for (char c : request) {
			unsigned before;
			{ std::lock_guard<std::mutex> lock(drain_mutex); before = drains; }
			send_bytes(first.fd(), std::string(1, c));
			std::unique_lock<std::mutex> lock(drain_mutex);
			CHECK(drain_cv.wait_for(lock, 5s, [&] { return drains > before; }));
		}
		CHECK(receive(first.fd(), 5) == "+OK\r\n");
		send_bytes(first.fd(), "*2\r\n$3\r\nGET\r\n$5\r\nsp");
		auto second = connect_peer(address);
		exchange(second.fd(), command({"GET", "split"}), bulk("value"));
		exchange(first.fd(), "lit\r\n" + command({"PING"}), bulk("value") + "+PONG\r\n");
	});
}
void size_boundaries() {
	serve(1, [](InetAddress address, Stop) {
		auto peer = connect_peer(address); int fd = peer.fd();
		const std::string key(65527, 'k'), value(65535, 'v');
		exchange(fd, command({"SET", key, value}), "+OK\r\n");
		exchange(fd, command({"GET", key}), bulk(value));
		expect_error(fd, command({"SET", key + "x", "v"}));
		expect_error(fd, command({"SET", "large", value + "x"}));
		exchange(fd, command({"GET", "large"}), "$-1\r\n");
		exchange(fd, command({"PING", std::string(65536, 'p')}), bulk(std::string(65536, 'p')));
	});
}
void protocol_errors() {
	serve(2, [](InetAddress address, Stop) {
		for (const auto &bad : {"?bad\r\n", "*1\r\n$65537\r\n", "*1\r\n$-1\r\n", "$3\r\nabcXX"}) {
			auto peer = connect_peer(address);
			send_bytes(peer.fd(), bad);
			expect_eof(peer.fd());
		}
		auto healthy = connect_peer(address);
		exchange(healthy.fd(), command({"PING"}), "+PONG\r\n");
	});
}
void buffer_overflow() {
	serve(1, [](InetAddress address, Stop) {
		auto peer = connect_peer(address);
		exchange(peer.fd(), command({"PING"}), "+PONG\r\n");
		// No CRLF: remains NeedMore until the aggregate buffer limit is exceeded.
		send_bytes(peer.fd(), "+" + std::string(1024 * 1024, 'a'));
		expect_eof(peer.fd());
		auto healthy = connect_peer(address);
		exchange(healthy.fd(), command({"PING"}), "+PONG\r\n");
	});
}
void wal_failure() {
	serve(2, [](InetAddress address, Stop) {
		auto peer = connect_peer(address); int fd = peer.fd();
		exchange(fd, command({"SET", "keep", "old"}), "+OK\r\n");
		fail_wal_write = true;
		expect_error(fd, command({"SET", "keep", "new"}));
		CHECK(injected_errors == 1);
		exchange(fd, command({"GET", "keep"}), bulk("old"));
		expect_error(fd, command({"DEL", "keep"}));
		exchange(fd, command({"DEL", "missing"}), ":0\r\n");
	});
}
void concurrent_clients() {
	serve(3, [](InetAddress address, Stop) {
		std::vector<std::future<void>> clients;
		for (int i = 0; i < 6; ++i) {
			clients.push_back(std::async(std::launch::async, [=] {
				auto peer = connect_peer(address);
				for (int j = 0; j < 20; ++j) {
					const auto key = std::to_string(i) + ":" + std::to_string(j);
					exchange(peer.fd(), command({"SET", key, key}), "+OK\r\n");
					exchange(peer.fd(), command({"GET", key}), bulk(key));
					exchange(peer.fd(), command({"DEL", key}), ":1\r\n");
				}
			}));
		}
		for (auto &client : clients) { await(client); }
	});
}
void connection_cleanup_and_stop() {
	serve(3, [](InetAddress address, Stop stop) {
		for (int i = 0; i < 10; ++i) {
			auto peer = connect_peer(address);
			exchange(peer.fd(), command({"PING"}), "+PONG\r\n");
			CHECK(::shutdown(peer.fd(), SHUT_WR) == 0);
			// EOF requires release of the fd, detecting retained map ownership.
			expect_eof(peer.fd());
		}
		std::vector<Socket> peers;
		for (int i = 0; i < 6; ++i) {
			peers.push_back(connect_peer(address));
			exchange(peers.back().fd(), command({"PING"}), "+PONG\r\n");
		}
		stop();
		for (auto &peer : peers) { expect_eof(peer.fd()); }
	});
}
void reopen() {
	serve(1, [](InetAddress address, Stop) {
		auto peer = connect_peer(address);
		exchange(peer.fd(), command({"SET", "keep", "value"}), "+OK\r\n");
		exchange(peer.fd(), command({"SET", "deleted", "value"}), "+OK\r\n");
		exchange(peer.fd(), command({"DEL", "deleted"}), ":1\r\n");
	});
	serve(2, [](InetAddress address, Stop) {
		auto peer = connect_peer(address);
		exchange(peer.fd(), command({"GET", "keep"}), bulk("value"));
		exchange(peer.fd(), command({"GET", "deleted"}), "$-1\r\n");
		exchange(peer.fd(), command({"DEL", "deleted"}), ":0\r\n");
	});
}
} // namespace

extern "C" int __real_bind(int, const sockaddr *, socklen_t);
extern "C" int __wrap_bind(int fd, const sockaddr *address, socklen_t size) {
	const int result = __real_bind(fd, address, size);
	if (result == 0 && address->sa_family == AF_INET) {
		sockaddr_in actual{}; socklen_t length = sizeof(actual);
		CHECK(::getsockname(fd, reinterpret_cast<sockaddr *>(&actual), &length) == 0);
		bound_address = InetAddress(actual);
	}
	return result;
}
extern "C" int __real_accept4(int, sockaddr *, socklen_t *, int);
extern "C" int __wrap_accept4(int fd, sockaddr *address, socklen_t *size, int flags) {
	const int result = __real_accept4(fd, address, size, flags);
	if (result >= 0) { accepted_fd = result; }
	return result;
}
extern "C" ssize_t __real_read(int, void *, size_t);
extern "C" ssize_t __wrap_read(int fd, void *data, size_t size) {
	const auto result = __real_read(fd, data, size);
	const int saved = errno;
	if (result < 0 && saved == EAGAIN && fd == accepted_fd.load()) {
		{ std::lock_guard<std::mutex> lock(drain_mutex); ++drains; }
		drain_cv.notify_all();
	}
	errno = saved;
	return result;
}
int main(int argc, char **argv) {
	return integration::run({
		{"basic commands zero workers", [] { basic(0); }},
		{"basic commands multiple workers", [] { basic(3); }},
		{"binary and empty strings", binary_and_empty},
		{"command errors preserve connection", command_errors},
		{"large pipeline ordered responses", pipeline},
		{"fragmented requests and independent parsers", fragmented_connections},
		{"protocol and storage size boundaries", size_boundaries},
		{"protocol errors close only offending connection", protocol_errors},
		{"aggregate buffer limit", buffer_overflow},
		{"WAL failure error responses", wal_failure},
		{"concurrent clients", concurrent_clients},
		{"EOF cleanup and active stop", connection_cleanup_and_stop},
		{"server recreation preserves data", reopen},
	}, argc, argv);
}
