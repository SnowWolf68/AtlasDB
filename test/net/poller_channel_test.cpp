#include "net/channel.h"
#include "net/EPoller.h"
#include "net/EventLoop.h"

#include <cerrno>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace fault {
enum class Wait { Normal, Interrupted, Error, UnknownFd };
Wait wait_mode = Wait::Normal;
bool fail_create = false;
}
extern "C" int __real_epoll_create(int);
extern "C" int __real_epoll_wait(int, epoll_event *, int, int);
extern "C" int __wrap_epoll_create(int size) {
	if (std::exchange(fault::fail_create, false)) { errno = EMFILE; return -1; }
	return __real_epoll_create(size);
}
extern "C" int __wrap_epoll_wait(int fd, epoll_event *events, int count, int timeout) {
	const auto mode = std::exchange(fault::wait_mode, fault::Wait::Normal);
	if (mode == fault::Wait::Interrupted) { errno = EINTR; return -1; }
	if (mode == fault::Wait::Error) { errno = EIO; return -1; }
	if (mode == fault::Wait::UnknownFd) {
		events[0] = {}; events[0].events = EPOLLIN; events[0].data.fd = -123;
		return 1;
	}
	return __real_epoll_wait(fd, events, count, timeout);
}

namespace {
#define CHECK(expr) do { if (!(expr)) { throw std::runtime_error( \
	"line " + std::to_string(__LINE__) + ": " #expr); } } while (false)

template<class Action> void must_throw(Action action) {
	bool threw = false;
	try { action(); } catch (const std::runtime_error &) { threw = true; }
	CHECK(threw);
}
struct EventFd {
	int fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	EventFd() { CHECK(fd >= 0); }
	~EventFd() { ::close(fd); }
	void ready() { const std::uint64_t value = 1; CHECK(::write(fd, &value, sizeof(value)) == sizeof(value)); }
	void drain() { std::uint64_t value = 0; CHECK(::read(fd, &value, sizeof(value)) == sizeof(value)); }
};

void dispatches_only_ready_callbacks_and_preserves_read_before_hup() {
	Channel channel(-1, Channel::TriggerMode::Level, nullptr);
	std::vector<int> calls;
	channel.setReadCallback([&] { calls.push_back(1); });
	channel.setWriteCallback([&] { calls.push_back(2); });
	channel.setErrorCallback([&] { calls.push_back(3); });
	channel.setCloseCallback([&] { calls.push_back(4); });
	channel.setRevents(EPOLLIN | EPOLLOUT | EPOLLERR | EPOLLHUP);
	channel.handle_event();
	CHECK((calls == std::vector<int>{1, 2, 3}));
	CHECK(channel.revents() == (EPOLLIN | EPOLLOUT | EPOLLERR | EPOLLHUP));
	calls.clear(); channel.setRevents(EPOLLHUP | EPOLLERR); channel.handle_event();
	CHECK((calls == std::vector<int>{3, 4}));
	calls.clear(); channel.setRevents(0); channel.handle_event(); CHECK(calls.empty());
}

void absent_callbacks_and_expired_owner_are_safe() {
	Channel channel(-1, Channel::TriggerMode::Edge, nullptr);
	channel.setRevents(EPOLLIN | EPOLLOUT | EPOLLERR | EPOLLHUP);
	channel.handle_event(); // empty std::function objects must never be invoked
	int calls = 0;
	channel.setReadCallback([&] { ++calls; });
	channel.setErrorCallback([&] { ++calls; });
	{
		auto owner = std::make_shared<int>(1);
		channel.tie(owner);
		channel.handle_event(); CHECK(calls == 2);
	}
	channel.handle_event(); CHECK(calls == 2);
}

void tied_owner_survives_all_callbacks_and_is_released_after_dispatch() {
	struct Owner { Channel channel{-1, Channel::TriggerMode::Level, nullptr}; };
	auto owner = std::make_shared<Owner>();
	std::weak_ptr<Owner> weak = owner;
	owner->channel.tie(owner);
	std::vector<int> calls;
	owner->channel.setReadCallback([&] { owner.reset(); CHECK(!weak.expired()); calls.push_back(1); });
	owner->channel.setWriteCallback([&] { CHECK(!weak.expired()); calls.push_back(2); });
	owner->channel.setErrorCallback([&] { CHECK(!weak.expired()); calls.push_back(3); });
	owner->channel.setRevents(EPOLLIN | EPOLLOUT | EPOLLERR);
	Channel *channel = &owner->channel;
	channel->handle_event();
	CHECK((calls == std::vector<int>{1, 2, 3}));
	CHECK(weak.expired());
}

void interest_masks_modes_and_remove_reregister_work() {
	EventFd fd;
	EventLoop loop;
	Channel channel(fd.fd, Channel::TriggerMode::Level, &loop);
	CHECK(channel.fd() == fd.fd && channel.events() == 0);
	channel.enable_reading(); CHECK(channel.events() == EPOLLIN);
	channel.enable_writing(); CHECK(channel.events() == (EPOLLIN | EPOLLOUT));
	channel.disable_reading(); CHECK(channel.events() == EPOLLOUT);
	channel.setTriggerMode(Channel::TriggerMode::Edge);
	CHECK(channel.getTriggerMode() == Channel::TriggerMode::Edge);
	CHECK(channel.events() == (EPOLLOUT | EPOLLET));
	channel.disable_writing(); CHECK(channel.events() == EPOLLET);
	channel.enable_reading(); channel.disable_all(); CHECK(channel.events() == EPOLLET);
	channel.setTriggerMode(Channel::TriggerMode::Level);
	CHECK(channel.getTriggerMode() == Channel::TriggerMode::Level && channel.events() == 0);
	channel.remove();
	channel.enable_reading(); // after DEL, this must register with ADD again
	int calls = 0;
	channel.setReadCallback([&] { fd.drain(); ++calls; loop.quit(); });
	fd.ready(); loop.loop(); CHECK(calls == 1);
	channel.disable_all(); channel.remove();
}

void poller_rejects_duplicate_fd_identity_and_unknown_removal() {
	EventFd fd;
	EventLoop loop;
	Channel original(fd.fd, Channel::TriggerMode::Level, &loop);
	Channel impostor(fd.fd, Channel::TriggerMode::Level, &loop);
	EPoller poller;
	poller.update_channel(&original);
	must_throw([&] { poller.update_channel(&impostor); });
	must_throw([&] { poller.remove_channel(&impostor); });
	poller.update_channel(&original); // rejected operations preserve the original mapping
	poller.remove_channel(&original);
	must_throw([&] { poller.remove_channel(&original); });
	poller.update_channel(&impostor); poller.remove_channel(&impostor);
}

void failed_add_and_modify_do_not_remove_other_registrations() {
	EventFd fd;
	EventLoop loop;
	Channel channel(fd.fd, Channel::TriggerMode::Level, &loop);
	Channel invalid(-1, Channel::TriggerMode::Level, &loop);
	channel.enable_reading();
	EPoller poller;
	must_throw([&] { poller.update_channel(&invalid); });
	poller.update_channel(&channel);
	// A separate descriptor supplies real EBADF for MOD and DEL while the
	// valid channel and all EventLoop-owned descriptors remain open.
	const int stale_fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC); CHECK(stale_fd >= 0);
	Channel stale(stale_fd, Channel::TriggerMode::Level, nullptr);
	poller.update_channel(&stale); CHECK(::close(stale_fd) == 0);
	must_throw([&] { poller.update_channel(&stale); });
	must_throw([&] { poller.remove_channel(&stale); });
	fd.ready();
	std::vector<Channel *> ready;
	CHECK(poller.epoll_wait(ready) == 1 && ready.front() == &channel);
	CHECK((ready.front()->revents() & EPOLLIN) != 0);
	fd.drain(); poller.remove_channel(&channel); channel.remove();
}

void wait_retries_eintr_and_clears_previous_output() {
	EventFd fd;
	EventLoop loop;
	Channel channel(fd.fd, Channel::TriggerMode::Edge, &loop);
	channel.enable_reading();
	EPoller poller; poller.update_channel(&channel);
	std::vector<Channel *> ready{nullptr, nullptr};
	fd.ready(); fault::wait_mode = fault::Wait::Interrupted;
	CHECK(poller.epoll_wait(ready) == 1);
	CHECK(ready.size() == 1 && ready.front() == &channel);
	fd.drain(); poller.remove_channel(&channel); channel.remove();
}

void hard_wait_error_and_unknown_ready_fd_throw() {
	EPoller poller;
	std::vector<Channel *> ready{nullptr};
	fault::wait_mode = fault::Wait::Error;
	must_throw([&] { poller.epoll_wait(ready); }); CHECK(ready.empty());
	fault::wait_mode = fault::Wait::UnknownFd;
	must_throw([&] { poller.epoll_wait(ready); }); CHECK(ready.empty());
}

void failed_creation_does_not_prevent_a_subsequent_poller() {
	fault::fail_create = true;
	must_throw([] { EPoller poller; });
	EPoller poller;
}
} // namespace

int main() {
	::alarm(10);
	const std::vector<std::pair<const char *, std::function<void()>>> cases = {
		{"ready callback dispatch", dispatches_only_ready_callbacks_and_preserves_read_before_hup},
		{"absent callbacks and expired owner", absent_callbacks_and_expired_owner_are_safe},
		{"tied owner lifetime during callbacks", tied_owner_survives_all_callbacks_and_is_released_after_dispatch},
		{"interest masks modes and re-registration", interest_masks_modes_and_remove_reregister_work},
		{"duplicate fd identity and unknown removal", poller_rejects_duplicate_fd_identity_and_unknown_removal},
		{"failed registration preserves others", failed_add_and_modify_do_not_remove_other_registrations},
		{"wait retries EINTR and clears output", wait_retries_eintr_and_clears_previous_output},
		{"wait errors and unknown ready fd", hard_wait_error_and_unknown_ready_fd_throw},
		{"creation failure can be retried", failed_creation_does_not_prevent_a_subsequent_poller}};
	std::size_t passed = 0;
	for (const auto &[name, run] : cases) {
		try { run(); ++passed; std::cout << "[PASS] " << name << '\n'; }
		catch (const std::exception &error) {
			std::cerr << "[FAIL] " << name << ": " << error.what() << '\n';
		}
		fault::wait_mode = fault::Wait::Normal; fault::fail_create = false;
	}
	std::cout << passed << '/' << cases.size() << " tests passed\n";
	return passed == cases.size() ? 0 : 1;
}
