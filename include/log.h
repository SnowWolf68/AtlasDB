#pragma once

#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <time.h>
#include <unistd.h>

#define ERROR_LOG LOG(Log::ERROR)
#define DEBUG_LOG LOG(Log::DEBUG)
void log_error(const std::string &message);
void throw_error(const std::string &message);

class Sink {
public:
	virtual void output(const std::string &log_msg) = 0;
	// 普通虚函数调用时，只会选择一个最终实现。
	// 析构函数执行时，继承链上的每一层析构函数都必须执行。
	virtual ~Sink() = default;
};

class FileSink : public Sink {
public:
	FileSink(std::string filename = "app.log",
			 std::string _filepath = "./logs");
	void output(const std::string &log_msg);

	bool set_output_file(std::string filename, std::string filepath);
	virtual ~FileSink() = default;

private:
	std::string _filename;
	std::string _filepath;
	std::mutex _mx;
	std::ofstream _ofs;
};

class ConsoleSink : public Sink {
public:
	void output(const std::string &log_msg);
	virtual ~ConsoleSink() = default;

private:
	std::mutex _mx;
};

/**
	#define LOG(level) Log::log(level, __FILE__, __LINE__, __func__)

	usage:
		LOG(INFO) << "aaa" << "bbbb";

	log_msg_format:
		[yyyy-mm-dd hh-mm-ss] [pid] [thread_id] [xxx.cpp] [x] ---- msg content
*/
class Log {
public:
	enum LogLevel { DEBUG, INFO, WARNING, ERROR, FATAL };
	enum LogSink { CONSOLE, FILE };
	class LogMessage {
	public:
		LogMessage(Log::LogLevel loglevel, std::string file, int line,
				   std::string func, std::shared_ptr<Sink> sink, bool enabled);

		LogMessage &operator<<(std::string msg);

		~LogMessage();

	private:
		std::string _log_msg;
		std::shared_ptr<Sink> _sink;
		bool _enabled;
	};

	Log(std::shared_ptr<Sink> sink = std::make_shared<ConsoleSink>(),
		LogSink logsink = CONSOLE);
	LogMessage log(Log::LogLevel, std::string file, int ling, std::string func);

	void set_minimum_log_level(int level);

	void use_console_sink();
	bool use_file_sink(std::string filename, std::string filepath);

	bool change_log_output_file(std::string filename, std::string filepath);

	static Log &get_logger() {
		static Log logger;
		return logger;
	}


private:
	int _minimum_log_level;
	std::shared_ptr<Sink> _sink;
	int _logsink;
};


#define LOG(level) Log::get_logger().log(level, __FILE__, __LINE__, __func__)