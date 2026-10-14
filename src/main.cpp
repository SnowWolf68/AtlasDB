#include <iostream>

#include "AtlasServer.h"
#include "log.h"

#include <cstring>
#include <signal.h>
#include <sys/signalfd.h>
int main() {
	Log::get_logger().set_minimum_log_level(Log::LogLevel::DEBUG);
	EventLoop loop;
	sigset_t set{};
	int sigemptyset_res = ::sigemptyset(&set);
	if (sigemptyset_res == -1) {
		ERROR_LOG << "sigemptyset failed, errno: " << strerror(errno);
		return 1;
	}
	int sigaddset_res = ::sigaddset(&set, SIGINT);
	if (sigaddset_res == -1) {
		ERROR_LOG << "sigaddset failed, errno: " << strerror(errno);
		return 1;
	}
	int sigmask_res = ::pthread_sigmask(SIG_BLOCK, &set, nullptr);
	if (sigmask_res != 0) {
		ERROR_LOG << "pthread_sigmask failed, errno: " << strerror(sigmask_res);
		return 1;
	}
	int signal_fd = ::signalfd(-1, &set, SFD_NONBLOCK | SFD_CLOEXEC);
	if (signal_fd == -1) {
		ERROR_LOG << "signalfd failed, errno: " << strerror(errno);
		return 1;
	}
	Channel sig_channel(signal_fd, Channel::TriggerMode::Edge, &loop);
	AtlasServer server(
		&loop,
		[&loop]() {
			DEBUG_LOG << "server close...";
			loop.quit();
		},
		4, InetAddress("127.0.0.1", 6379));
	sig_channel.setReadCallback([&server, signal_fd]() {
		signalfd_siginfo info{};
		while (true) {
			int res = ::read(signal_fd, &info, sizeof(info));
			if (res == sizeof(info)) {
				if (info.ssi_signo == SIGINT) {
					DEBUG_LOG << "SIGINT received, server stopping...";
					server.stop();
				} else {
					break;
				}
			} else {
				if (errno == EINTR) {
					continue;
				} else if (errno == EAGAIN) {
					break;
				} else {
					DEBUG_LOG << "read signalfd failed, errno: "
							  << strerror(errno);
					break;
				}
			}
		}
	});
	sig_channel.enable_reading();
	server.start();
	DEBUG_LOG << "server start...";
	// loop.add_task_after_delay(std::chrono::seconds(4),
	// 						  [&server]() { server.stop(); });
	loop.loop();
	sig_channel.remove();
	::close(signal_fd);

	return 0;
}