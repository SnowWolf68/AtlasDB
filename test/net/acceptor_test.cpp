#include "net/acceptor.h"

#include <arpa/inet.h>
#include <chrono>
#include <condition_variable>
#include <cerrno>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <mutex>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

void check(bool condition, const char *expression, int line) {
	if (condition) { return; }
	std::ostringstream message;
	message << "line " << line << ": check failed: " << expression;
	throw std::runtime_error(message.str());
}

#define CHECK(expression) check((expression), #expression, __LINE__)

std::uint16_t available_port() {
	Socket probe = Socket::createTcp(AF_INET);
	probe.bind_address(InetAddress("127.0.0.1", 0));
	return probe.local_address()._port;
}

Socket connect_to(std::uint16_t port) {
	Socket client(::socket(AF_INET, SOCK_STREAM, 0));
	CHECK(client.fd() >= 0);
	InetAddress address("127.0.0.1", port);
	CHECK(::connect(client.fd(),
					reinterpret_cast<const sockaddr *>(&address._sockaddr),
					sizeof(address._sockaddr)) == 0);
	return client;
}

bool run_loop_with_timeout(EventLoop &loop) {
	std::mutex mutex;
	std::condition_variable cv;
	bool finished = false;
	bool timed_out = false;
	std::thread watchdog([&] {
		std::unique_lock<std::mutex> lock(mutex);
		if (!cv.wait_for(lock, std::chrono::seconds(1),
						 [&] { return finished; })) {
			timed_out = true;
			lock.unlock();
			loop.quit();
		}
	});
	try {
		loop.loop();
	} catch (...) {
		{
			std::lock_guard<std::mutex> lock(mutex);
			finished = true;
		}
		cv.notify_one();
		watchdog.join();
		throw;
	}
	{
		std::lock_guard<std::mutex> lock(mutex);
		finished = true;
	}
	cv.notify_one();
	watchdog.join();
	return !timed_out;
}

void wait_readable(int fd) {
	pollfd watched{fd, POLLIN, 0};
	int result;
	do {
		result = ::poll(&watched, 1, 1000);
	} while (result == -1 && errno == EINTR);
	CHECK(result > 0);
}

void test_destroy_before_listen_releases_port() {
	const auto port = available_port();
	const InetAddress address("127.0.0.1", port);
	EventLoop loop;
	{
		auto acceptor = std::make_shared<Acceptor>(
			address, &loop, [](Socket, InetAddress) {});
		CHECK(!acceptor->is_listening());
	}
	Socket rebound = Socket::createTcp(AF_INET);
	rebound.bind_address(address);
}

void test_listen_drains_multiple_pending_connections() {
	const auto port = available_port();
	EventLoop loop;
	std::vector<std::uint16_t> peer_ports;
	bool all_nonblocking = true;
	auto acceptor = std::make_shared<Acceptor>(
		InetAddress("127.0.0.1", port), &loop,
		[&](Socket connection, InetAddress peer) {
						  all_nonblocking &=
							  (::fcntl(connection.fd(), F_GETFL) & O_NONBLOCK) != 0;
						  peer_ports.push_back(peer._port);
						  if (peer_ports.size() == 2) { loop.quit(); }
					  });
	CHECK(!acceptor->is_listening());
	CHECK(acceptor->listen());
	CHECK(acceptor->is_listening());
	CHECK(!acceptor->listen());
	Socket first = connect_to(port);
	Socket second = connect_to(port);
	CHECK(run_loop_with_timeout(loop));
	CHECK(peer_ports.size() == 2);
	CHECK(peer_ports[0] == first.local_address()._port);
	CHECK(peer_ports[1] == second.local_address()._port);
	CHECK(all_nonblocking);
}

void test_callback_exception_closes_connection() {
	const auto port = available_port();
	EventLoop loop;
	int callbacks = 0;
	auto acceptor = std::make_shared<Acceptor>(
		InetAddress("127.0.0.1", port), &loop,
		[&](Socket, InetAddress) {
						  ++callbacks;
						  loop.quit();
						  throw std::runtime_error("expected callback failure");
					  });
	CHECK(acceptor->listen());
	Socket client = connect_to(port);
	CHECK(run_loop_with_timeout(loop));
	CHECK(callbacks == 1);
	wait_readable(client.fd());
	char byte = '\0';
	CHECK(::read(client.fd(), &byte, 1) == 0);
}

void test_rebind_after_connection_cycle() {
	const auto port = available_port();
	const InetAddress address("127.0.0.1", port);
	{
		EventLoop loop;
		auto acceptor = std::make_shared<Acceptor>(
			address, &loop, [&](Socket, InetAddress) { loop.quit(); });
		CHECK(acceptor->listen());
		Socket client = connect_to(port);
		CHECK(run_loop_with_timeout(loop));
		wait_readable(client.fd());
		char byte = '\0';
		CHECK(::read(client.fd(), &byte, 1) == 0);
	}
	EventLoop next_loop;
	auto rebound = std::make_shared<Acceptor>(
		address, &next_loop, [](Socket, InetAddress) {});
	CHECK(rebound->listen());
}

struct TestCase {
	const char *name;
	void (*run)();
};

} // namespace

int main() {
	const TestCase tests[] = {
		{"destroy before listen releases port",
		 test_destroy_before_listen_releases_port},
		{"listen drains multiple pending connections",
		 test_listen_drains_multiple_pending_connections},
		{"callback exception closes connection",
		 test_callback_exception_closes_connection},
		{"rebind after connection cycle", test_rebind_after_connection_cycle},
	};
	std::size_t passed = 0;
	for (const auto &test : tests) {
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
