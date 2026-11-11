#include "log.h"

#include <cerrno>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
namespace fs = std::filesystem;

void check(bool condition, const char *expression, int line) {
	if (!condition) {
		throw std::runtime_error("line " + std::to_string(line) + ": " + expression);
	}
}
#define CHECK(expression) check((expression), #expression, __LINE__)

struct Records {
	std::mutex mutex;
	std::vector<std::string> messages;
};

class RecordingSink : public Sink {
public:
	explicit RecordingSink(std::shared_ptr<Records> records) : records_(std::move(records)) {}
	void output(const std::string &message) override {
		std::lock_guard<std::mutex> lock(records_->mutex);
		records_->messages.push_back(message);
	}
private:
	std::shared_ptr<Records> records_;
};

class Capture {
public:
	explicit Capture(std::ostream &stream) : stream_(stream), previous_(stream.rdbuf(buffer_.rdbuf())) {}
	~Capture() { stream_.rdbuf(previous_); }
	std::string str() const { return buffer_.str(); }
private:
	std::ostream &stream_;
	std::ostringstream buffer_;
	std::streambuf *previous_;
};

struct TempDirectory {
	std::string path;
	TempDirectory() {
		char pattern[] = "/tmp/atlasdb-log-test-XXXXXX";
		CHECK(::mkdtemp(pattern) != nullptr);
		path = pattern;
	}
	~TempDirectory() {
		std::error_code ignored;
		fs::remove_all(path, ignored);
	}
};

std::string read_file(const std::string &path) {
	std::ifstream stream(path, std::ios::binary);
	CHECK(stream.is_open());
	return {std::istreambuf_iterator<char>(stream), {}};
}

void seed(const std::string &path, const std::string &contents) {
	std::ofstream stream(path, std::ios::binary);
	stream.exceptions(std::ios::badbit | std::ios::failbit);
	stream << contents;
}

void test_default_level_filters_below_warning(const std::string &) {
	auto records = std::make_shared<Records>();
	Log logger(std::make_shared<RecordingSink>(records));
	logger.log(Log::DEBUG, "file", 1, "function") << "debug";
	logger.log(Log::INFO, "file", 1, "function") << "info";
	logger.log(Log::WARNING, "file", 1, "function") << "warning";
	logger.log(Log::ERROR, "file", 1, "function") << "error";
	logger.log(Log::FATAL, "file", 1, "function") << "fatal";
	CHECK(records->messages.size() == 3);
	CHECK(records->messages[0].find("[WARNING]") != std::string::npos);
	CHECK(records->messages[1].find("[ERROR]") != std::string::npos);
	CHECK(records->messages[2].find("[FATAL]") != std::string::npos);
}

void test_each_threshold_includes_equal_level(const std::string &) {
	auto records = std::make_shared<Records>();
	Log logger(std::make_shared<RecordingSink>(records));
	for (int threshold = Log::DEBUG; threshold <= Log::FATAL; ++threshold) {
		records->messages.clear();
		logger.set_minimum_log_level(threshold);
		for (int level = Log::DEBUG; level <= Log::FATAL; ++level) {
			logger.log(static_cast<Log::LogLevel>(level), "file", 2, "function") << "message";
		}
		CHECK(records->messages.size() == static_cast<std::size_t>(Log::FATAL - threshold + 1));
	}
}

void test_message_emits_once_at_scope_end(const std::string &) {
	auto records = std::make_shared<Records>();
	Log logger(std::make_shared<RecordingSink>(records));
	const std::string payload("one\0two", 7);
	{
		auto message = logger.log(Log::ERROR, "source.cpp", 42, "scope_test");
		message << "prefix:" << payload;
		CHECK(records->messages.empty());
	}
	CHECK(records->messages.size() == 1);
	const auto &message = records->messages.front();
	CHECK(message.find("[ERROR]") == 0);
	CHECK(message.find("[source.cpp:42]") != std::string::npos);
	CHECK(message.find("[scope_test]") != std::string::npos);
	CHECK(message.find("[pid = " + std::to_string(::getpid()) + "]") != std::string::npos);
	CHECK(message.find("[thread_id = ") != std::string::npos);
	CHECK(message.substr(message.size() - 14) == "prefix:" + payload);
}

void test_message_retains_sink_after_logger_is_destroyed(const std::string &) {
	auto records = std::make_shared<Records>();
	auto sink = std::make_shared<RecordingSink>(records);
	std::weak_ptr<Sink> weak = sink;
	auto logger = std::make_unique<Log>(sink);
	{
		auto message = logger->log(Log::ERROR, "file", 1, "function");
		message << "retained";
		logger.reset();
		sink.reset();
		CHECK(!weak.expired());
		CHECK(records->messages.empty());
	}
	CHECK(weak.expired());
	CHECK(records->messages.size() == 1);
	CHECK(records->messages.front().find("retained") != std::string::npos);
}

void test_console_sink_appends_newline(const std::string &) {
	Capture output(std::cout);
	ConsoleSink sink;
	sink.output("one");
	sink.output("two");
	CHECK(output.str() == "one\ntwo\n");
}

void test_file_sink_appends_and_switches(const std::string &dir) {
	Capture output(std::cout);
	CHECK(fs::create_directory(dir + "/first"));
	seed(dir + "/first/log.txt", "existing\n");
	{
		FileSink sink("log.txt", dir + "/first");
		sink.output("one");
		CHECK(sink.set_output_file("next.txt", dir + "/nested/second"));
		CHECK(read_file(dir + "/first/log.txt") == "existing\none\n");
		sink.output("two");
	}
	CHECK(read_file(dir + "/nested/second/next.txt") == "two\n");
}

void test_failed_file_switch_preserves_previous_stream(const std::string &dir) {
	Capture output(std::cout), errors(std::cerr);
	seed(dir + "/blocked", "regular file");
	CHECK(fs::create_directory(dir + "/directory-name"));
	{
		FileSink sink("log.txt", dir);
		sink.output("before");
		CHECK(!sink.set_output_file("bad.txt", dir + "/blocked/child"));
		CHECK(!sink.set_output_file("directory-name", dir));
		sink.output("after");
	}
	CHECK(read_file(dir + "/log.txt") == "before\nafter\n");
	CHECK(read_file(dir + "/blocked") == "regular file");
}

void test_logger_switches_between_file_and_console(const std::string &dir) {
	Capture output(std::cout);
	Log logger;
	CHECK(!logger.change_log_output_file("unused", dir));
	CHECK(logger.use_file_sink("a.log", dir));
	logger.log(Log::ERROR, "file", 1, "function") << "first-file-message";
	CHECK(logger.use_file_sink("b.log", dir));
	logger.log(Log::ERROR, "file", 1, "function") << "second-file-message";
	logger.use_console_sink();
	logger.log(Log::ERROR, "file", 1, "function") << "console-message";
	CHECK(read_file(dir + "/a.log").find("first-file-message") != std::string::npos);
	CHECK(read_file(dir + "/b.log").find("second-file-message") != std::string::npos);
	CHECK(output.str().find("console-message") != std::string::npos);
	CHECK(output.str().find("first-file-message") == std::string::npos);
	CHECK(output.str().find("second-file-message") == std::string::npos);
}

void test_failed_file_sink_creation_keeps_current_sink(const std::string &dir) {
	Capture output(std::cout), errors(std::cerr);
	seed(dir + "/blocked", "regular file");
	CHECK(fs::create_directory(dir + "/directory-name"));
	auto records = std::make_shared<Records>();
	Log logger(std::make_shared<RecordingSink>(records));
	CHECK(!logger.use_file_sink("log.txt", dir + "/blocked/child"));
	CHECK(!logger.use_file_sink("directory-name", dir));
	logger.log(Log::ERROR, "file", 1, "function") << "still-current";
	CHECK(records->messages.size() == 1);
	CHECK(records->messages.front().find("still-current") != std::string::npos);
}

void test_concurrent_messages_remain_whole(const std::string &) {
	auto records = std::make_shared<Records>();
	Log logger(std::make_shared<RecordingSink>(records));
	logger.set_minimum_log_level(Log::INFO);
	std::vector<std::thread> workers;
	for (int worker = 0; worker < 4; ++worker) {
		workers.emplace_back([&, worker] {
			for (int item = 0; item < 50; ++item) {
				logger.log(Log::INFO, "thread.cpp", 7, "worker")
					<< "entry=" << std::to_string(worker) + ":" + std::to_string(item);
			}
		});
	}
	for (auto &worker : workers) { worker.join(); }
	CHECK(records->messages.size() == 200);
	std::set<std::string> entries;
	for (const auto &message : records->messages) {
		const auto position = message.find("entry=");
		CHECK(position != std::string::npos);
		CHECK(entries.insert(message.substr(position + 6)).second);
	}
	for (int worker = 0; worker < 4; ++worker) {
		for (int item = 0; item < 50; ++item) {
			CHECK(entries.count(std::to_string(worker) + ":" + std::to_string(item)) == 1);
		}
	}
}

void test_concurrent_file_output_has_complete_lines(const std::string &dir) {
	Capture output(std::cout);
	{
		FileSink sink("threads.log", dir);
		std::vector<std::thread> workers;
		for (int worker = 0; worker < 4; ++worker) {
			workers.emplace_back([&, worker] {
				for (int item = 0; item < 50; ++item) {
					sink.output(std::to_string(worker) + ":" + std::to_string(item));
				}
			});
		}
		for (auto &worker : workers) { worker.join(); }
	}
	std::istringstream input(read_file(dir + "/threads.log"));
	std::set<std::string> lines;
	std::string line;
	while (std::getline(input, line)) { CHECK(lines.insert(line).second); }
	CHECK(lines.size() == 200);
	for (int worker = 0; worker < 4; ++worker) {
		for (int item = 0; item < 50; ++item) {
			CHECK(lines.count(std::to_string(worker) + ":" + std::to_string(item)) == 1);
		}
	}
}

void test_error_helpers_log_and_throw(const std::string &) {
	Capture output(std::cout);
	Log::get_logger().set_minimum_log_level(Log::WARNING);
	errno = ENOENT;
	log_error("log-helper-probe");
	bool caught = false;
	try { throw_error("throw-helper-probe"); }
	catch (const std::runtime_error &error) {
		caught = true;
		CHECK(std::string(error.what()).find("throw-helper-probe") != std::string::npos);
	}
	CHECK(caught);
	CHECK(output.str().find("log-helper-probe") != std::string::npos);
	CHECK(output.str().find("throw-helper-probe") != std::string::npos);
	CHECK(output.str().find("errno:") != std::string::npos);
}

struct Case { const char *name; void (*run)(const std::string &); };
} // namespace

int main() {
	const Case cases[] = {
		{"default threshold filters below warning", test_default_level_filters_below_warning},
		{"each threshold includes its own level", test_each_threshold_includes_equal_level},
		{"message emits once at scope end with binary and metadata", test_message_emits_once_at_scope_end},
		{"message retains sink after logger destruction", test_message_retains_sink_after_logger_is_destroyed},
		{"console sink appends newline", test_console_sink_appends_newline},
		{"file sink appends and switches destinations", test_file_sink_appends_and_switches},
		{"failed file switch preserves previous stream", test_failed_file_switch_preserves_previous_stream},
		{"logger switches between file and console", test_logger_switches_between_file_and_console},
		{"failed file sink creation keeps current sink", test_failed_file_sink_creation_keeps_current_sink},
		{"concurrent messages remain whole", test_concurrent_messages_remain_whole},
		{"concurrent file output preserves complete lines", test_concurrent_file_output_has_complete_lines},
		{"error helpers log and throw", test_error_helpers_log_and_throw},
	};
	std::size_t passed = 0;
	for (const auto &test : cases) {
		TempDirectory directory;
		std::cout.flush();
		std::cerr.flush();
		const pid_t child = ::fork();
		CHECK(child >= 0);
		if (child == 0) {
			::alarm(5);
			try { test.run(directory.path); ::_exit(0); }
			catch (const std::exception &error) { std::cerr << test.name << ": " << error.what() << '\n'; }
			::_exit(1);
		}
		int status = 0;
		pid_t waited;
		do { waited = ::waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
		CHECK(waited == child);
		const bool success = WIFEXITED(status) && WEXITSTATUS(status) == 0;
		std::cout << (success ? "[PASS] " : "[FAIL] ") << test.name << '\n';
		if (success) { ++passed; }
	}
	std::cout << passed << '/' << std::size(cases) << " tests passed\n";
	return passed == std::size(cases) ? 0 : 1;
}
