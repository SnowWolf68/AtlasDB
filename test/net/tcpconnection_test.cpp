#include "net/TcpConnection.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <exception>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace {
using Connection = std::shared_ptr<TcpConnection>;

void check(bool condition, const char *expression, int line) {
	if (condition) { return; }
	std::ostringstream message;
	message << "line " << line << ": check failed: " << expression;
	throw std::runtime_error(message.str());
}
#define CHECK(expression) check((expression), #expression, __LINE__)

// Both endpoints use real IPv4 TCP; only the endpoint owned by Connection is
// nonblocking. Port 0 avoids a probe/close/rebind race between tests.
struct TcpPair {
	Socket server;
	Socket peer;
};

TcpPair connected_pair() {
	Socket listener(::socket(AF_INET, SOCK_STREAM, 0));
	CHECK(listener.fd() >= 0);
	listener.bind_address(InetAddress("127.0.0.1", 0));
	listener.listen(4);
	Socket peer(::socket(AF_INET, SOCK_STREAM, 0));
	CHECK(peer.fd() >= 0);
	const auto address = listener.local_address();
	CHECK(::connect(peer.fd(), reinterpret_cast<const sockaddr *>(&address._sockaddr),
					sizeof(address._sockaddr)) == 0);
	Socket server(::accept4(listener.fd(), nullptr, nullptr, SOCK_NONBLOCK));
	CHECK(server.fd() >= 0);
	return {std::move(server), std::move(peer)};
}

void wait_io(int fd, short events) {
	pollfd watched{fd, events, 0};
	int result;
	do { result = ::poll(&watched, 1, 3000); }
	while (result == -1 && errno == EINTR);
	CHECK(result > 0);
	CHECK(!(watched.revents & POLLNVAL));
}

void peer_send(int fd, const std::vector<char> &bytes) {
	std::size_t offset = 0;
	while (offset < bytes.size()) {
		const auto n = ::send(fd, bytes.data() + offset, bytes.size() - offset,
							 MSG_NOSIGNAL | MSG_DONTWAIT);
		if (n > 0) { offset += static_cast<std::size_t>(n); }
		else if (n == -1 && errno == EINTR) { continue; }
		else if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			wait_io(fd, POLLOUT);
		} else { CHECK(false); }
	}
}

std::vector<char> peer_receive(int fd, std::size_t size) {
	std::vector<char> result(size);
	std::size_t offset = 0;
	while (offset < size) {
		const auto n = ::recv(fd, result.data() + offset, size - offset, MSG_DONTWAIT);
		if (n > 0) { offset += static_cast<std::size_t>(n); }
		else if (n == -1 && errno == EINTR) { continue; }
		else if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			wait_io(fd, POLLIN);
		} else { CHECK(false); }
	}
	return result;
}

void expect_eof(int fd) {
	wait_io(fd, POLLIN);
	char byte;
	CHECK(::recv(fd, &byte, 1, MSG_DONTWAIT) == 0);
}

std::vector<char> pattern(std::size_t size, unsigned salt = 0) {
	std::vector<char> result(size);
	for (std::size_t i = 0; i < size; ++i) {
		result[i] = static_cast<char>((i * 37 + salt) % 256);
	}
	return result;
}

void test_fragmented_input_retains_unconsumed_bytes() {
	EventLoop loop;
	auto sockets = connected_pair();
	const auto first = pattern(6000);
	const auto second = pattern(2500, 17);
	std::vector<char> expected(first.begin() + 4000, first.end());
	expected.insert(expected.end(), second.begin(), second.end());
	int reads = 0, closes = 0;
	bool sent_second = false;
	Connection connection;
	connection = TcpConnection::create(&loop, std::move(sockets.server),
		InetAddress("127.0.0.1", 0), [&](Connection current, Buffer &buffer) {
			++reads;
			if (!sent_second) {
				if (buffer.peek().size() < first.size()) { return; }
				CHECK(buffer.peek().size() == first.size());
				CHECK(std::vector<char>(buffer.peek().begin(), buffer.peek().end()) == first);
				CHECK(buffer.erase(4000));
				sent_second = true;
				peer_send(sockets.peer.fd(), second);
			} else {
				if (buffer.peek().size() < expected.size()) { return; }
				CHECK(std::vector<char>(buffer.peek().begin(), buffer.peek().end()) == expected);
				current->close();
			}
		}, [&](Connection) { ++closes; connection.reset(); loop.quit(); });
	peer_send(sockets.peer.fd(), first);
	loop.loop();
	CHECK(reads >= 2);
	CHECK(closes == 1);
	expect_eof(sockets.peer.fd());
}

void test_data_before_eof_and_tie_lifetime() {
	EventLoop loop;
	auto sockets = connected_pair();
	std::vector<char> received;
	int closes = 0;
	Connection connection;
	std::weak_ptr<TcpConnection> weak;
	const auto bytes = pattern(8192);
	connection = TcpConnection::create(&loop, std::move(sockets.server),
		InetAddress("127.0.0.1", 0), [&](Connection, Buffer &buffer) {
			CHECK(closes == 0);
			received.insert(received.end(), buffer.peek().begin(), buffer.peek().end());
			CHECK(buffer.erase(buffer.get_readable_size()));
		}, [&](Connection) {
			++closes;
			CHECK(received == bytes);
			connection.reset();
			CHECK(!weak.expired());
			loop.quit();
		});
	weak = connection;
	peer_send(sockets.peer.fd(), bytes);
	CHECK(::shutdown(sockets.peer.fd(), SHUT_WR) == 0);
	loop.loop();
	CHECK(closes == 1);
	CHECK(received == bytes);
	CHECK(weak.expired());
	expect_eof(sockets.peer.fd());
}

void test_direct_send_empty_and_send_after_close() {
	EventLoop loop;
	auto sockets = connected_pair();
	int closes = 0;
	auto connection = TcpConnection::create(&loop, std::move(sockets.server),
		InetAddress("127.0.0.1", 0), [](Connection, Buffer &) {},
		[&](Connection) { ++closes; loop.quit(); });
	const auto bytes = pattern(10000);
	connection->send_in_loop({});
	connection->send_in_loop(bytes);
	CHECK(peer_receive(sockets.peer.fd(), bytes.size()) == bytes);
	connection->close();
	connection->close();
	connection->send_in_loop({'x'});
	CHECK(closes == 0);
	loop.loop();
	CHECK(closes == 1);
	// Current contract: close removes monitoring; the live object still owns fd.
	char byte;
	CHECK(::recv(sockets.peer.fd(), &byte, 1, MSG_DONTWAIT) == -1);
	CHECK(errno == EAGAIN || errno == EWOULDBLOCK);
	connection.reset();
	expect_eof(sockets.peer.fd());
}

void test_backpressure_preserves_order_and_resumes() {
	EventLoop loop;
	auto sockets = connected_pair();
	int send_size = 4096;
	CHECK(::setsockopt(sockets.server.fd(), SOL_SOCKET, SO_SNDBUF,
		&send_size, sizeof(send_size)) == 0);
	int acknowledgments = 0, closes = 0;
	Connection connection;
	connection = TcpConnection::create(&loop, std::move(sockets.server),
		InetAddress("127.0.0.1", 0), [&](Connection current, Buffer &buffer) {
			CHECK(buffer.peek() == "!");
			CHECK(buffer.erase(1));
			if (++acknowledgments == 1) { current->send_in_loop({'e', 'n', 'd'}); }
			else { current->close(); }
		}, [&](Connection) { ++closes; connection.reset(); loop.quit(); });
	auto expected = pattern(1024 * 1024);
	const auto tail = pattern(512 * 1024, 91);
	connection->send_in_loop(expected);
	connection->send_in_loop(tail);
	expected.insert(expected.end(), tail.begin(), tail.end());
	std::vector<char> peek(expected.size());
	const auto ready = ::recv(sockets.peer.fd(), peek.data(), peek.size(), MSG_PEEK | MSG_DONTWAIT);
	CHECK(ready > 0);
	CHECK(static_cast<std::size_t>(ready) < expected.size());
	std::exception_ptr reader_error;
	std::thread reader([&] {
		try {
			CHECK(peer_receive(sockets.peer.fd(), expected.size()) == expected);
			peer_send(sockets.peer.fd(), {'!'});
			CHECK((peer_receive(sockets.peer.fd(), 3) == std::vector<char>{'e', 'n', 'd'}));
			peer_send(sockets.peer.fd(), {'!'});
		} catch (...) { reader_error = std::current_exception(); loop.quit(); }
	});
	try { loop.loop(); }
	catch (...) { reader.join(); throw; }
	reader.join();
	if (reader_error) { std::rethrow_exception(reader_error); }
	CHECK(acknowledgments == 2);
	CHECK(closes == 1);
}

void test_cross_thread_send_close_and_task_ownership() {
	EventLoop loop;
	auto sockets = connected_pair();
	const auto owner = std::this_thread::get_id();
	int closes = 0;
	Connection connection;
	connection = TcpConnection::create(&loop, std::move(sockets.server),
		InetAddress("127.0.0.1", 0), [](Connection, Buffer &) {},
		[&](Connection) {
			CHECK(std::this_thread::get_id() == owner);
			++closes;
			connection.reset();
			loop.quit();
		});
	std::weak_ptr<TcpConnection> weak = connection;
	std::thread worker([copy = connection] {
		copy->send_in_loop({'a', '\0', 'b'});
		copy->send_in_loop({'c'});
		copy->close();
		copy->close();
		copy->send_in_loop({'x'});
	});
	worker.join();
	CHECK(closes == 0);
	CHECK(!weak.expired());
	loop.loop();
	CHECK(closes == 1);
	CHECK(weak.expired());
	CHECK((peer_receive(sockets.peer.fd(), 4) == std::vector<char>{'a', '\0', 'b', 'c'}));
	expect_eof(sockets.peer.fd());
}

void test_close_from_message_and_reentrant_close_notification() {
	EventLoop loop;
	auto sockets = connected_pair();
	int closes = 0;
	Connection connection;
	std::weak_ptr<TcpConnection> weak;
	connection = TcpConnection::create(&loop, std::move(sockets.server),
		InetAddress("127.0.0.1", 0), [&](Connection current, Buffer &) {
			current->close();
			current->send_in_loop({'x'});
		}, [&](Connection current) {
			++closes;
			connection.reset();
			CHECK(!weak.expired());
			current->close();
			loop.quit();
		});
	weak = connection;
	peer_send(sockets.peer.fd(), {'x'});
	CHECK(::shutdown(sockets.peer.fd(), SHUT_WR) == 0);
	loop.loop();
	CHECK(closes == 1);
	CHECK(weak.expired());
	expect_eof(sockets.peer.fd());
}

void test_peer_reset_closes_once() {
	EventLoop loop;
	auto sockets = connected_pair();
	int closes = 0, reads = 0;
	Connection connection;
	connection = TcpConnection::create(&loop, std::move(sockets.server),
		InetAddress("127.0.0.1", 0), [&](Connection, Buffer &) { ++reads; },
		[&](Connection) { ++closes; connection.reset(); loop.quit(); });
	{
		Socket peer(std::move(sockets.peer));
		linger reset{1, 0};
		CHECK(::setsockopt(peer.fd(), SOL_SOCKET, SO_LINGER, &reset, sizeof(reset)) == 0);
	}
	loop.loop();
	CHECK(reads == 0);
	CHECK(closes == 1);
}

void test_send_error_does_not_raise_sigpipe() {
	EventLoop loop;
	auto sockets = connected_pair();
	const int fd = sockets.server.fd();
	int closes = 0;
	auto connection = TcpConnection::create(&loop, std::move(sockets.server),
		InetAddress("127.0.0.1", 0), [](Connection, Buffer &) {},
		[&](Connection) { ++closes; loop.quit(); });
	// A shut-down write side deterministically produces EPIPE. Each test runs
	// in a child with SIGPIPE's default action, so omitting MSG_NOSIGNAL fails.
	CHECK(::shutdown(fd, SHUT_WR) == 0);
	connection->send_in_loop({'x'});
	connection->close();
	CHECK(closes == 0);
	loop.loop();
	CHECK(closes == 1);
}

void test_removal_precedes_throwing_close_callback() {
	EventLoop loop;
	auto sockets = connected_pair();
	const int fd = sockets.server.fd();
	auto connection = TcpConnection::create(&loop, std::move(sockets.server),
		InetAddress("127.0.0.1", 0), [](Connection, Buffer &) {},
		[](Connection) { throw std::runtime_error("expected close callback failure"); });
	bool caught = false;
	connection->close();
	try { loop.loop(); }
	catch (const std::runtime_error &error) {
		caught = std::string(error.what()) == "expected close callback failure";
	}
	CHECK(caught);
	// Registration of a different Channel for this still-live fd succeeds only
	// if the original was removed from both epoll and EPoller's map.
	Channel probe(fd, Channel::TriggerMode::Edge, &loop);
	probe.enable_reading();
	probe.remove();
	connection->close();
}

void test_final_reference_can_be_released_on_another_thread_after_close() {
	EventLoop loop;
	auto sockets = connected_pair();
	const int fd = sockets.server.fd();
	auto connection = TcpConnection::create(&loop, std::move(sockets.server),
		InetAddress("127.0.0.1", 0), [](Connection, Buffer &) {},
		[&](Connection) { loop.quit(); });
	std::weak_ptr<TcpConnection> weak = connection;
	connection->close();
	loop.loop();
	std::thread worker([copy = std::move(connection)]() mutable { copy.reset(); });
	worker.join();
	CHECK(weak.expired());
	CHECK(::fcntl(fd, F_GETFD) == -1 && errno == EBADF);
	expect_eof(sockets.peer.fd());
}

void check_close_in_current_event_batch(bool release_references) {
	EventLoop loop;
	auto first = connected_pair();
	auto second = connected_pair();
	const int first_fd = first.server.fd(), second_fd = second.server.fd();
	std::array<Connection, 2> connections;
	std::array<std::weak_ptr<TcpConnection>, 2> weak;
	int reads = 0, closes = 0;
	for (int i = 0; i < 2; ++i) {
		Socket &socket = i == 0 ? first.server : second.server;
		connections[i] = TcpConnection::create(&loop, std::move(socket),
			InetAddress("127.0.0.1", 0), [&, i](Connection current, Buffer &) {
				++reads;
				connections[1 - i]->close();
				current->close();
				CHECK(closes == 0);
				if (release_references) {
					connections[0].reset();
					connections[1].reset();
				}
				// Verify ownership before dispatch proceeds, so a broken keepalive
				// fails here rather than deliberately accessing a dangling Channel.
				CHECK(!weak[0].expired());
				CHECK(!weak[1].expired());
			}, [&](Connection) {
				if (++closes == 2) { loop.quit(); }
			});
		weak[i] = connections[i];
	}
	// Both are ready before epoll_wait. Whichever runs first closes the other.
	// Test both external ownership and ownership by deferred cleanup tasks.
	peer_send(first.peer.fd(), {'a'});
	peer_send(second.peer.fd(), {'b'});
	wait_io(first_fd, POLLIN);
	wait_io(second_fd, POLLIN);
	loop.loop();
	CHECK(closes == 2);
	CHECK(reads == 1);
	if (release_references) {
		CHECK(weak[0].expired());
		CHECK(weak[1].expired());
	}
}

void test_closed_connection_in_current_event_batch_is_not_dispatched() {
	check_close_in_current_event_batch(false);
}

void test_cleanup_tasks_keep_current_event_batch_alive() {
	check_close_in_current_event_batch(true);
}

struct TestCase { const char *name; void (*run)(); };
} // namespace

int main(int argc, char **argv) {
	const TestCase tests[] = {
		{"fragmented input retains unconsumed bytes", test_fragmented_input_retains_unconsumed_bytes},
		{"data before EOF and tie lifetime", test_data_before_eof_and_tie_lifetime},
		{"direct send, empty input and send after close", test_direct_send_empty_and_send_after_close},
		{"backpressure preserves order and resumes", test_backpressure_preserves_order_and_resumes},
		{"cross-thread send, close and task ownership", test_cross_thread_send_close_and_task_ownership},
		{"close from message and reentrant notification", test_close_from_message_and_reentrant_close_notification},
		{"peer reset closes once", test_peer_reset_closes_once},
		{"send error does not raise SIGPIPE", test_send_error_does_not_raise_sigpipe},
		{"removal precedes throwing close callback", test_removal_precedes_throwing_close_callback},
		{"final reference released on another thread after close", test_final_reference_can_be_released_on_another_thread_after_close},
		{"closed connection in current event batch is not dispatched", test_closed_connection_in_current_event_batch_is_not_dispatched},
		{"cleanup tasks keep current event batch alive", test_cleanup_tasks_keep_current_event_batch_alive},
	};
	std::size_t passed = 0, total = 0;
	for (const auto &test : tests) {
		if (argc > 1 && std::string(test.name) != argv[1]) { continue; }
		++total;
		std::cout.flush();
		std::cerr.flush();
		const pid_t child = ::fork();
		if (child == -1) { std::cerr << "fork failed\n"; return 1; }
		if (child == 0) {
			::signal(SIGPIPE, SIG_DFL);
			::alarm(7);
			try { test.run(); std::cout.flush(); std::cerr.flush(); ::_exit(0); }
			catch (const std::exception &error) {
				std::cerr << "  " << error.what() << std::endl;
				::_exit(1);
			}
		}
		int status = 0;
		pid_t result;
		do { result = ::waitpid(child, &status, 0); }
		while (result == -1 && errno == EINTR);
		if (result == child && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
			++passed;
			std::cout << "[PASS] " << test.name << '\n';
		} else {
			std::cerr << "[FAIL] " << test.name;
			if (result == child && WIFSIGNALED(status)) {
				std::cerr << " (signal " << WTERMSIG(status) << ')';
			}
			std::cerr << '\n';
		}
	}
	std::cout << passed << '/' << total << " tests passed\n";
	return total != 0 && passed == total ? 0 : 1;
}
