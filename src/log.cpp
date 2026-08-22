#include "log.h"
#include <filesystem>
#include <iostream>


Log::Log(std::shared_ptr<Sink> sink, LogSink logsink)
	: _minimum_log_level(2), _sink(sink), _logsink(logsink) {}

Log::LogMessage Log::log(Log::LogLevel loglevel, std::string file, int line,
						 std::string func) {
	bool enabled = loglevel >= _minimum_log_level ? true : false;
	return Log::LogMessage(loglevel, file, line, func, _sink, enabled);
}

std::string get_curtime_str(std::tm *cur_tm) {
	char buffer[128] = {0};
	snprintf(buffer, sizeof(buffer), "%d-%02d-%02d %02d:%02d:%02d",
			 cur_tm->tm_year + 1900, cur_tm->tm_mon + 1, cur_tm->tm_mday,
			 cur_tm->tm_hour, cur_tm->tm_min, cur_tm->tm_sec);
	return std::string(buffer);
}

std::string get_loglevel_str(int level) {
	switch (level) {
		case 0: return "DEBUG"; break;
		case 1: return "INFO"; break;
		case 2: return "WARNING"; break;
		case 3: return "ERROR"; break;
		case 4: return "FATAL"; break;
		default: return "UNKNOWN";
	}
}

Log::LogMessage::LogMessage(Log::LogLevel loglevel, std::string file, int line,
							std::string func, std::shared_ptr<Sink> sink,
							bool enabled)
	: _sink(sink), _enabled(enabled) {
	if (enabled) {
		std::stringstream ss;
		ss << "[" << get_loglevel_str(loglevel) << "] ";
		std::time_t cur_time = std::time(nullptr);
		struct tm tm;
		struct tm *res = localtime_r(&cur_time, &tm);
		if (res == nullptr) {
			ss << "[unknown-time]";
		} else {
			ss << "[" << get_curtime_str(&tm) << "] ";
		}
		ss << "[pid = " << getpid() << "] ";
		ss << "[thread_id = " << std::this_thread::get_id() << "] ";
		ss << "[" << file << ":" << line << "] ";
		ss << "[" << func << "] ";
		_log_msg = ss.str();
	}
}

Log::LogMessage &Log::LogMessage::operator<<(std::string msg) {
	if (_enabled) { _log_msg += msg; }
	return *this;
}

Log::LogMessage::~LogMessage() {
	// std::cout << "~LogMessage(): " << _log_msg << std::endl;
	if (_enabled && _sink != nullptr) { _sink->output(_log_msg); }
}

void Log::set_minimum_log_level(int level) { _minimum_log_level = level; }

void Log::use_console_sink() {
	if (_logsink != CONSOLE) {
		_sink = std::make_shared<ConsoleSink>();
		_logsink = CONSOLE;
	}
}
bool Log::use_file_sink(std::string filename, std::string filepath) {
	if (_logsink != FILE) {
		try {
			_sink = std::make_shared<FileSink>(filename, filepath);
			_logsink = FILE;
		} catch (std::exception &error) {
			std::cerr << "error: " << error.what() << std::endl;
			return false;
		}
		return true;
	} else {
		bool res = change_log_output_file(filename, filepath);
		return res;
	}
}

bool Log::change_log_output_file(std::string filename, std::string filepath) {
	if (_logsink != FILE) {
		return false;
	} else {
		std::shared_ptr<FileSink> filesink =
			std::dynamic_pointer_cast<FileSink>(_sink);
		if (filesink == nullptr) {
			std::cerr << "dynamic_cast failed." << std::endl;
			return false;
		}
		bool res = filesink->set_output_file(filename, filepath);
		return res;
	}
}

FileSink::FileSink(std::string filename, std::string filepath)
	: _filename(filename), _filepath(filepath) {
	std::filesystem::path folder_path(ATLASDB_PROJECT_DIR + _filepath);
    std::cout << "folder_path: " << std::filesystem::absolute(folder_path) << std::endl;
	std::error_code err_code;
	std::filesystem::create_directories(folder_path, err_code);
	if (err_code) {
		throw std::filesystem::filesystem_error("create directory failed",
												_filepath, err_code);
	}
	std::filesystem::path path = folder_path / _filename;
	_ofs = std::ofstream(path, std::ios_base::app);
	if (!_ofs.is_open()) {
		throw std::runtime_error("file: " + path.string() + " open failed.");
	}
}

bool FileSink::set_output_file(std::string filename, std::string filepath) {
	std::filesystem::path folder_path(filepath);
	std::error_code err_code;
	std::filesystem::create_directories(folder_path, err_code);
	if (err_code) {
		std::cerr << "create directory: " << filepath << " failed."
				  << std::endl;
		return false;
	}
	std::filesystem::path path = folder_path / filename;
	std::ofstream ofs(path, std::ios_base::app);
	if (!ofs.is_open()) {
		std::cerr << "file: " << path.string() << " open failed." << std::endl;
		return false;
	}
	std::lock_guard<std::mutex> lguard(_mx);
	_ofs = std::move(ofs); // 文件流禁止复制，只允许移动。
	_filename = filename;
	_filepath = filepath;
	return true;
}

void FileSink::output(const std::string &log_msg) {
	std::lock_guard<std::mutex> lguard(_mx);
	_ofs << log_msg << "\n";
	if (!_ofs) { std::cerr << "write file failed." << std::endl; }
}

void ConsoleSink::output(const std::string &log_msg) {
	std::lock_guard<std::mutex> lguard(_mx);
	std::cout << log_msg << std::endl;
}