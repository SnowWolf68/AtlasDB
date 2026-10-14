#include "net/TcpServer.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <exception>
#include <fcntl.h>
#include <future>
#include <iostream>
#include <map>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
using namespace std::chrono_literals;
using Connection = std::shared_ptr<TcpConnection>;

void check(bool condition, const char *expression, int line) {
	if (condition) { return; }
	std::ostringstream message;
	message << "line " << line << ": check failed: " << expression;
	throw std::runtime_error(message.str());
}
#define CHECK(expression) check((expression), #expression, __LINE__)

// Test-only syscall observation keeps production interfaces unchanged. Bind
// to port 0 and discover the actual address without a probe/close/rebind race.
InetAddress bound_address;
int listener_fd = -1;
int accepted_count = 0;
thread_local std::function<void(int)> after_accept;
thread_local std::function<void(epoll_event *, int)> after_poll;

template <typename T> T await(std::future<T> &result) {
	CHECK(result.wait_for(3s) == std::future_status::ready);
	return result.get();
}

Socket connect_peer(const InetAddress &address) {
	Socket peer(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0));
	CHECK(peer.fd() >= 0);
	const int res = ::connect(peer.fd(),
		reinterpret_cast<const sockaddr *>(&address._sockaddr), sizeof(address._sockaddr));
	if (res == -1) {
		CHECK(errno == EINPROGRESS);
		pollfd watched{peer.fd(), POLLOUT, 0};
		CHECK(::poll(&watched, 1, 3000) > 0);
		int error = -1;
		socklen_t size = sizeof(error);
		CHECK(::getsockopt(peer.fd(), SOL_SOCKET, SO_ERROR, &error, &size) == 0);
		CHECK(error == 0);
	}
	return peer;
}

void wait_io(int fd, short events) {
	pollfd watched{fd, events, 0};
	int res;
	do { res = ::poll(&watched, 1, 3000); }
	while (res == -1 && errno == EINTR);
	CHECK(res > 0 && !(watched.revents & POLLNVAL));
}

void send_bytes(int fd, const std::vector<char> &bytes) {
	std::size_t offset = 0;
	while (offset < bytes.size()) {
		const auto n = ::send(fd, bytes.data() + offset, bytes.size() - offset, MSG_NOSIGNAL);
		if (n > 0) { offset += static_cast<std::size_t>(n); }
		else if (n == -1 && errno == EINTR) { continue; }
		else if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			wait_io(fd, POLLOUT);
		} else { CHECK(false); }
	}
}

std::vector<char> receive_bytes(int fd, std::size_t size) {
	std::vector<char> bytes(size);
	std::size_t offset = 0;
	while (offset < size) {
		const auto n = ::recv(fd, bytes.data() + offset, size - offset, 0);
		if (n > 0) { offset += static_cast<std::size_t>(n); }
		else if (n == -1 && errno == EINTR) { continue; }
		else if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			wait_io(fd, POLLIN);
		} else { CHECK(false); }
	}
	return bytes;
}

void expect_eof(int fd) {
	for (;;) {
		char byte;
		const auto n = ::recv(fd, &byte, 1, 0);
		if (n == 0) { return; }
		if (n == -1 && errno == EINTR) { continue; }
		CHECK(n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK));
		wait_io(fd, POLLIN);
	}
}

void echo(Connection conn, Buffer &buffer) {
	auto bytes = buffer.read(buffer.get_readable_size());
	CHECK(bytes.has_value());
	conn->send_in_loop(std::move(*bytes));
}

std::vector<char> payload(unsigned salt) {
	std::vector<char> bytes(8192);
	for (std::size_t i = 0; i < bytes.size(); ++i) {
		bytes[i] = static_cast<char>((i * 37 + salt) % 256);
	}
	return bytes;
}

void round_trip(Socket &peer, unsigned salt = 0) {
	const auto bytes = payload(salt);
	send_bytes(peer.fd(), bytes);
	CHECK(receive_bytes(peer.fd(), bytes.size()) == bytes);
}

void test_unstarted_stop_and_destruction() {
	EventLoop base;
	int notifications = 0;
	InetAddress address;
	{
		TcpServer server(&base, 2, InetAddress("127.0.0.1", 0), echo,
			[&] { ++notifications; }, [](Connection) {});
		address = bound_address;
		CHECK(server.stop());
		CHECK(server.stop());
	}
	// Never-started destruction must release the bound listener immediately.
	Socket replacement = Socket::createTcp(AF_INET);
	replacement.set_reuse_addr(true);
	replacement.bind_address(address);
	CHECK(base.queue_in_loop([&] { CHECK(notifications == 0); base.quit(); }));
	base.loop();
}

void empty_stop(std::size_t workers) {
	EventLoop base;
	const auto owner = std::this_thread::get_id();
	int notifications = 0;
	bool returned = false, sentinel = false;
	std::unique_ptr<TcpServer> server;
	server = std::make_unique<TcpServer>(&base, workers, InetAddress("127.0.0.1", 0),
		echo, [&] {
			CHECK(returned);
			CHECK(std::this_thread::get_id() == owner);
			CHECK(++notifications == 1);
			CHECK(server->stop());
			server.reset(); // Callback may release Server, including its callback member.
			CHECK(base.queue_in_loop([&] { sentinel = true; base.quit(); }));
		}, [](Connection) {});
	CHECK(server->start());
	CHECK(server->start());
	CHECK(server->stop());
	CHECK(notifications == 0);
	CHECK(!server->start()); // Includes the window after workers have joined.
	CHECK(server->stop());
	returned = true;
	base.loop();
	CHECK(notifications == 1 && sentinel && !server);
}

void test_empty_stop_zero_workers() { empty_stop(0); }
void test_empty_stop_multiple_workers() { empty_stop(3); }

void test_wrong_thread_management_rejected() {
	EventLoop base;
	int notifications = 0;
	TcpServer server(&base, 2, InetAddress("127.0.0.1", 0), echo,
		[&] { ++notifications; base.quit(); }, [](Connection) {});
	const auto address = bound_address;
	auto before = std::async(std::launch::async, [&] {
		CHECK(!server.start()); CHECK(!server.stop());
	});
	await(before);
	CHECK(server.start());
	auto client = std::async(std::launch::async, [&] {
		CHECK(!server.start()); CHECK(!server.stop());
		auto peer = connect_peer(address);
		round_trip(peer);
		CHECK(base.queue_in_loop([&] { CHECK(server.stop()); }));
		expect_eof(peer.fd());
	});
	base.loop();
	await(client);
	CHECK(notifications == 1);
}

struct SeenConnection {
	EventLoop *loop;
	std::thread::id owner;
	std::weak_ptr<TcpConnection> connection;
	std::weak_ptr<int> thread_token;
};

// Every observation is serialized through base. Token expiry verifies worker
// join without using a borrowed EventLoop after stop.
void active_connections(std::size_t workers) {
	EventLoop base;
	const auto owner = std::this_thread::get_id();
	std::map<std::uint16_t, SeenConnection> seen;
	int notifications = 0;
	std::unique_ptr<TcpServer> server;
	server = std::make_unique<TcpServer>(&base, workers, InetAddress("127.0.0.1", 0),
		[&](Connection conn, Buffer &buffer) {
			thread_local auto token = std::make_shared<int>(0);
			const auto peer_port = conn->get_peer_address()._port;
			SeenConnection observation{conn->get_event_loop(), std::this_thread::get_id(), conn, token};
			CHECK(base.queue_in_loop([&, peer_port, observation] { seen[peer_port] = observation; }));
			echo(conn, buffer);
		}, [&] {
			CHECK(std::this_thread::get_id() == owner);
			CHECK(++notifications == 1);
			CHECK(seen.size() == 6);
			std::map<std::thread::id, int> assignments;
			std::set<EventLoop *> loops;
			for (const auto &entry : seen) {
				const auto &observation = entry.second;
				CHECK(observation.connection.expired());
				if (workers == 0) {
					CHECK(observation.loop == &base && observation.owner == owner);
					CHECK(!observation.thread_token.expired());
				} else {
					CHECK(observation.loop != &base && observation.owner != owner);
					CHECK(observation.thread_token.expired());
				}
				++assignments[observation.owner];
				loops.insert(observation.loop);
			}
			CHECK(loops.size() == (workers == 0 ? 1 : workers));
			for (const auto &entry : assignments) {
				CHECK(entry.second == (workers == 0 ? 6 : 2));
			}
			server.reset();
			CHECK(base.queue_in_loop([&] { base.quit(); }));
		}, [](Connection) {});
	const auto address = bound_address;
	CHECK(server->start());
	auto client = std::async(std::launch::async, [&] {
		std::vector<Socket> peers;
		for (unsigned i = 0; i < 6; ++i) {
			peers.push_back(connect_peer(address));
			round_trip(peers.back(), i);
		}
		CHECK(base.queue_in_loop([&] {
			CHECK(server->stop()); CHECK(server->stop()); CHECK(!server->start());
		}));
		for (auto &peer : peers) { expect_eof(peer.fd()); }
	});
	base.loop();
	await(client);
	CHECK(notifications == 1 && !server);
}

void test_echo_and_active_stop_zero_workers() { active_connections(0); }
void test_round_robin_and_active_stop_workers() { active_connections(3); }

void test_peer_eof_then_new_connection() {
	EventLoop base;
	std::map<std::uint16_t, std::weak_ptr<TcpConnection>> seen;
	int notifications = 0;
	TcpServer server(&base, 2, InetAddress("127.0.0.1", 0),
		[&](Connection conn, Buffer &buffer) {
			const auto port = conn->get_peer_address()._port;
			CHECK(base.queue_in_loop([&, port, weak = std::weak_ptr<TcpConnection>(conn)] {
				seen[port] = weak;
			}));
			echo(conn, buffer);
		}, [&] { CHECK(++notifications == 1); base.quit(); }, [](Connection) {});
	const auto address = bound_address;
	CHECK(server.start());
	auto client = std::async(std::launch::async, [&] {
		for (unsigned i = 0; i < 2; ++i) {
			auto peer = connect_peer(address);
			round_trip(peer, i);
			CHECK(::shutdown(peer.fd(), SHUT_WR) == 0);
			expect_eof(peer.fd());
		}
		CHECK(base.queue_in_loop([&] {
			CHECK(notifications == 0 && seen.size() == 2);
			for (const auto &entry : seen) { CHECK(entry.second.expired()); }
			CHECK(server.stop());
		}));
	});
	base.loop();
	await(client);
	CHECK(notifications == 1);
}

void test_external_reference_survives_stop() {
	EventLoop base;
	Connection retained;
	std::weak_ptr<int> worker_token;
	std::promise<void> stopped;
	auto stopped_future = stopped.get_future();
	TcpServer server(&base, 1, InetAddress("127.0.0.1", 0),
		[&](Connection conn, Buffer &buffer) {
			thread_local auto token = std::make_shared<int>(0);
			CHECK(base.queue_in_loop([&, conn, weak = std::weak_ptr<int>(token)] {
				retained = conn; worker_token = weak;
			}));
			echo(conn, buffer);
		}, [&] {
			CHECK(retained && worker_token.expired());
			stopped.set_value();
			base.quit();
		}, [](Connection) {});
	const auto address = bound_address;
	CHECK(server.start());
	auto client = std::async(std::launch::async, [&] {
		auto peer = connect_peer(address);
		round_trip(peer);
		CHECK(base.queue_in_loop([&] { CHECK(server.stop()); }));
		await(stopped_future);
		// Channel cleanup is complete, while the retained object still owns fd.
		char byte;
		CHECK(::recv(peer.fd(), &byte, 1, 0) == -1);
		CHECK(errno == EAGAIN || errno == EWOULDBLOCK);
		retained.reset(); // Final release on another thread after worker join.
		expect_eof(peer.fd());
	});
	base.loop();
	await(client);
	CHECK(!retained);
}

void test_stop_requested_by_worker_callback() {
	EventLoop base;
	const auto owner = std::this_thread::get_id();
	int notifications = 0;
	std::atomic<bool> requested{false};
	TcpServer *server_ptr = nullptr;
	TcpServer server(&base, 1, InetAddress("127.0.0.1", 0),
		[&](Connection, Buffer &buffer) {
			CHECK(std::this_thread::get_id() != owner);
			CHECK(buffer.erase(buffer.get_readable_size()));
			CHECK(!server_ptr->stop());
			if (!requested.exchange(true)) {
				CHECK(base.queue_in_loop([&] { CHECK(server_ptr->stop()); }));
			}
		}, [&] { CHECK(++notifications == 1); base.quit(); }, [](Connection) {});
	server_ptr = &server;
	const auto address = bound_address;
	CHECK(server.start());
	auto client = std::async(std::launch::async, [&] {
		auto peer = connect_peer(address);
		send_bytes(peer.fd(), {'x'});
		expect_eof(peer.fd());
	});
	base.loop();
	await(client);
	CHECK(requested && notifications == 1);
}

void test_stop_waits_for_inflight_callback() {
	EventLoop base;
	std::promise<void> entered, stop_returned, release;
	auto entered_future = entered.get_future();
	auto returned_future = stop_returned.get_future();
	auto gate = release.get_future().share();
	std::atomic<int> notifications{0};
	std::weak_ptr<int> worker_token;
	TcpServer server(&base, 1, InetAddress("127.0.0.1", 0),
		[&](Connection, Buffer &buffer) {
			thread_local auto token = std::make_shared<int>(0);
			worker_token = token;
			entered.set_value();
			CHECK(gate.wait_for(3s) == std::future_status::ready);
			CHECK(buffer.erase(buffer.get_readable_size()));
		}, [&] {
			CHECK(worker_token.expired());
			CHECK(++notifications == 1);
			base.quit();
		}, [](Connection) {});
	const auto address = bound_address;
	CHECK(server.start());
	auto client = std::async(std::launch::async, [&] {
		auto peer = connect_peer(address);
		send_bytes(peer.fd(), {'x'});
		await(entered_future);
		CHECK(base.queue_in_loop([&] {
			CHECK(server.stop());
			CHECK(!server.start());
			CHECK(!worker_token.expired());
			stop_returned.set_value();
		}));
		// Stop must return while the callback is blocked; completion must wait
		// until it returns and the worker has processed delayed Channel cleanup.
		await(returned_future);
		CHECK(notifications.load() == 0);
		release.set_value();
		expect_eof(peer.fd());
	});
	base.loop();
	await(client);
	CHECK(notifications.load() == 1);
}

void test_restart_inside_finish_callback() {
	EventLoop base;
	int notifications = 0;
	std::promise<void> restart_ready[2];
	std::future<void> restart_future[] = {restart_ready[0].get_future(), restart_ready[1].get_future()};
	std::unique_ptr<TcpServer> server;
	server = std::make_unique<TcpServer>(&base, 2, InetAddress("127.0.0.1", 0), echo,
		[&] {
			++notifications;
			CHECK(server->stop());
			if (notifications < 3) {
				CHECK(server->start());
				CHECK(server->start());
				restart_ready[notifications - 1].set_value();
			} else {
				server.reset();
				base.quit();
			}
		}, [](Connection) {});
	const auto address = bound_address;
	CHECK(server->start());
	auto client = std::async(std::launch::async, [&] {
		for (unsigned cycle = 0; cycle < 3; ++cycle) {
			auto peer = connect_peer(address);
			round_trip(peer, cycle);
			CHECK(base.queue_in_loop([&] {
				CHECK(server->stop()); CHECK(!server->start());
			}));
			expect_eof(peer.fd());
			if (cycle < 2) { await(restart_future[cycle]); }
		}
	});
	base.loop();
	await(client);
	CHECK(notifications == 3 && !server);
}

void pending_creation_stop(std::size_t workers) {
	EventLoop base;
	int notifications = 0, accepted_fd = -1;
	bool stop_requested = false;
	TcpServer server(&base, workers, InetAddress("127.0.0.1", 0),
		[](Connection, Buffer &) { CHECK(false); }, [&] {
			CHECK(stop_requested && accepted_count == 1);
			CHECK(++notifications == 1);
			CHECK(::fcntl(accepted_fd, F_GETFD) == -1 && errno == EBADF);
			base.quit();
		}, [](Connection) {});
	const auto address = bound_address;
	CHECK(server.start());
	// Queue stop before Server queues creation/registration. It executes after
	// accept returns and pending is incremented, with no scheduling sleeps.
	after_accept = [&](int fd) {
		accepted_fd = fd;
		CHECK(base.queue_in_loop([&] {
			stop_requested = true;
			CHECK(server.stop()); CHECK(!server.start()); CHECK(notifications == 0);
		}));
	};
	auto peer = connect_peer(address);
	base.loop();
	after_accept = {};
	expect_eof(peer.fd());
	CHECK(notifications == 1);
}

void test_pending_creation_zero_workers() { pending_creation_stop(0); }
void test_pending_registration_workers() { pending_creation_stop(2); }

void test_stale_listener_event_after_stop() {
	EventLoop base;
	Socket control(::eventfd(0, EFD_NONBLOCK));
	CHECK(control.fd() >= 0);
	Channel channel(control.fd(), Channel::TriggerMode::Edge, &base);
	bool stopped = false, shared_batch = false;
	int notifications = 0;
	std::unique_ptr<TcpServer> server;
	server = std::make_unique<TcpServer>(&base, 1, InetAddress("127.0.0.1", 0), echo,
		[&] {
			CHECK(stopped && shared_batch && accepted_count == 0);
			CHECK(++notifications == 1);
			server.reset();
			base.quit();
		}, [](Connection) {});
	const auto address = bound_address;
	const int listen_fd = listener_fd;
	CHECK(server->start());
	channel.setReadCallback([&] {
		CHECK(shared_batch && !stopped);
		channel.remove();
		CHECK(server->stop());
		CHECK(notifications == 0);
		stopped = true;
	});
	channel.enable_reading();
	after_poll = [&](epoll_event *events, int count) {
		if (shared_batch) { return; }
		bool has_control = false, has_listener = false;
		for (int i = 0; i < count; ++i) {
			has_control |= events[i].data.fd == control.fd();
			has_listener |= events[i].data.fd == listen_fd;
		}
		CHECK(has_control && has_listener);
		// Force stop before the already-returned listener event is dispatched.
		std::stable_partition(events, events + count, [&](const epoll_event &event) {
			return event.data.fd == control.fd();
		});
		shared_batch = true;
	};
	auto peer = connect_peer(address);
	// Client-side connect completion can precede listener readiness. Wait for
	// the actual accept queue before making the control event ready as well.
	wait_io(listen_fd, POLLIN);
	const std::uint64_t value = 1;
	CHECK(::write(control.fd(), &value, sizeof(value)) == sizeof(value));
	base.loop();
	after_poll = {};
	// The peer was never accepted; destroying the listener resets its queued TCP.
	CHECK(stopped && shared_batch && notifications == 1 && !server);
}

// Application close notifications must run on base, exactly once, and before
// stop completion. Cover peer EOF, explicit connection close, and server stop.
void application_close_notification(int mode) {
	EventLoop base;
	const auto owner = std::this_thread::get_id();
	int closed = 0, finished = 0;
	std::weak_ptr<TcpConnection> observed;
	std::promise<void> notified;
	auto notification = notified.get_future();
	TcpServer server(&base, 2, InetAddress("127.0.0.1", 0),
		[&](Connection conn, Buffer &buffer) {
			if (mode == 1) {
				CHECK(buffer.erase(buffer.get_readable_size()));
				conn->close(); conn->close();
			} else { echo(conn, buffer); }
		}, [&] {
			CHECK(closed == 1 && observed.expired());
			CHECK(++finished == 1);
			base.quit();
		}, [&](Connection conn) {
			CHECK(std::this_thread::get_id() == owner);
			CHECK(++closed == 1);
			CHECK(finished == 0);
			observed = conn;
			notified.set_value();
		});
	const auto address = bound_address;
	CHECK(server.start());
	auto client = std::async(std::launch::async, [&] {
		auto peer = connect_peer(address);
		if (mode == 1) { send_bytes(peer.fd(), {'x'}); }
		else {
			round_trip(peer);
			if (mode == 0) { CHECK(::shutdown(peer.fd(), SHUT_WR) == 0); }
			else { CHECK(base.queue_in_loop([&] { CHECK(server.stop()); })); }
		}
		await(notification);
		expect_eof(peer.fd());
		if (mode != 2) { CHECK(base.queue_in_loop([&] { CHECK(server.stop()); })); }
	});
	base.loop();
	await(client);
	CHECK(closed == 1 && finished == 1);
}

struct TestCase { const char *name; void (*run)(); };
} // namespace

extern "C" int __real_bind(int, const sockaddr *, socklen_t);
extern "C" int __wrap_bind(int fd, const sockaddr *address, socklen_t length) {
	const int res = __real_bind(fd, address, length);
	const int saved_errno = errno;
	if (res == 0 && address->sa_family == AF_INET &&
		reinterpret_cast<const sockaddr_in *>(address)->sin_port == 0) {
		sockaddr_in actual{};
		socklen_t size = sizeof(actual);
		CHECK(::getsockname(fd, reinterpret_cast<sockaddr *>(&actual), &size) == 0);
		bound_address = InetAddress(actual);
		listener_fd = fd;
	}
	errno = saved_errno;
	return res;
}

extern "C" int __real_accept4(int, sockaddr *, socklen_t *, int);
extern "C" int __wrap_accept4(int fd, sockaddr *address, socklen_t *length, int flags) {
	const int res = __real_accept4(fd, address, length, flags);
	const int saved_errno = errno;
	if (res >= 0) { ++accepted_count; if (after_accept) { after_accept(res); } }
	errno = saved_errno;
	return res;
}

extern "C" int __real_epoll_wait(int, epoll_event *, int, int);
extern "C" int __wrap_epoll_wait(int fd, epoll_event *events, int capacity, int timeout) {
	const int res = __real_epoll_wait(fd, events, capacity, timeout);
	const int saved_errno = errno;
	if (res > 0 && after_poll) { after_poll(events, res); }
	errno = saved_errno;
	return res;
}

int main(int argc, char **argv) {
	const TestCase tests[] = {
		{"application notified on EOF", [] { application_close_notification(0); }},
		{"application notified on explicit close", [] { application_close_notification(1); }},
		{"application notified before stop finishes", [] { application_close_notification(2); }},
		{"unstarted stop and destruction", test_unstarted_stop_and_destruction},
		{"empty stop zero workers", test_empty_stop_zero_workers},
		{"empty stop multiple workers", test_empty_stop_multiple_workers},
		{"wrong thread management rejected", test_wrong_thread_management_rejected},
		{"echo and active stop zero workers", test_echo_and_active_stop_zero_workers},
		{"round robin and active stop workers", test_round_robin_and_active_stop_workers},
		{"peer EOF then new connection", test_peer_eof_then_new_connection},
		{"external reference survives stop", test_external_reference_survives_stop},
		{"stop requested by worker callback", test_stop_requested_by_worker_callback},
		{"stop waits for inflight callback", test_stop_waits_for_inflight_callback},
		{"restart inside finish callback", test_restart_inside_finish_callback},
		{"pending creation zero workers", test_pending_creation_zero_workers},
		{"pending registration workers", test_pending_registration_workers},
		{"stale listener event after stop", test_stale_listener_event_after_stop},
	};
	std::size_t selected = 0, passed = 0;
	for (const auto &test : tests) {
		if (argc > 1 && std::string(argv[1]) != test.name) { continue; }
		++selected;
		std::cout.flush(); std::cerr.flush();
		const pid_t child = ::fork();
		if (child == -1) { std::cerr << "[FAIL] fork failed\n"; continue; }
		if (child == 0) {
			::alarm(8);
			try {
				test.run();
				std::cout.flush(); std::cerr.flush(); ::_exit(0);
			} catch (const std::exception &error) {
				std::cerr << "[FAIL] " << test.name << ": " << error.what() << std::endl;
				::_exit(1);
			} catch (...) { ::_exit(1); }
		}
		int status = 0;
		pid_t waited;
		do { waited = ::waitpid(child, &status, 0); } while (waited == -1 && errno == EINTR);
		if (waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
			++passed;
			std::cout << "[PASS] " << test.name << '\n';
		} else if (waited == child && WIFSIGNALED(status)) {
			std::cerr << "[FAIL] " << test.name << ": signal " << WTERMSIG(status) << '\n';
		} else if (waited != child) { std::cerr << "[FAIL] waitpid failed\n"; }
	}
	std::cout << passed << '/' << selected << " tests passed\n";
	return selected != 0 && selected == passed ? 0 : 1;
}
