//
// Created by test on 10/27/26.
//

#include "io/FileObj.h"

#include "log.h"

#include <cstring>
#include <sys/stat.h>
#include <unistd.h>
namespace atlas::io {
	FileObj::FileObj()
		: _fd(-1), _filepath(""), _access_mode(AccessMode::ReadOnly) {}

	FileObj::FileObj(int fd, std::string filepath, AccessMode mode)
		: _fd(fd), _filepath(filepath), _access_mode(mode) {}

	void FileObj::swap(FileObj &&other) {
		std::swap(_fd, other._fd);
		std::swap(_filepath, other._filepath);
		std::swap(_access_mode, other._access_mode);
	}

	FileObj::FileObj(FileObj &&other)
		: _fd(-1), _filepath(""), _access_mode(AccessMode::ReadOnly) {
		swap(std::move(other));
	}

	FileObj &FileObj::operator=(FileObj &&other) {
		swap(std::move(other));
		return *this;
	}

	std::size_t FileObj::read_at(std::size_t offset,
								 std::vector<std::uint8_t> &buffer) const {
		check_is_open();
		std::size_t read_size = 0;
		while (read_size < buffer.size()) {
			ssize_t res =
				::pread(_fd, buffer.data() + read_size,
						buffer.size() - read_size, offset + read_size);
			if (res == -1) {
				if (errno == EAGAIN) {
					break;
				} else if (errno == EINTR) {
					continue;
				} else {
					ERROR_LOG << "read failed, file: " << _filepath
							  << ", error: " << strerror(errno);
					throw std::runtime_error("read failed");
				}
			} else if (res == 0) {
				break;
			} else {
				read_size += res;
			}
		}
		return read_size;
	}
	std::size_t FileObj::write_at(std::size_t offset,
								  const std::vector<std::uint8_t> &data) {
		check_is_open();
		std::size_t write_size = 0;
		while (write_size < data.size()) {
			ssize_t res =
				::pwrite(_fd, data.data() + write_size,
						 data.size() - write_size, offset + write_size);
			if (res == -1) {
				if (errno == EAGAIN) {
					break;
				} else if (errno == EINTR) {
					continue;
				} else {
					ERROR_LOG << "write failed, file: " << _filepath
							  << ", error: " << strerror(errno);
					throw std::runtime_error("write failed");
				}
			} else if (res == 0) {
				ERROR_LOG << "write failed, file: " << _filepath;
				throw std::runtime_error("write failed, file: " + _filepath);
			} else {
				write_size += res;
			}
		}
		return write_size;
	}
	std::size_t FileObj::append(const std::vector<std::uint8_t> &data) {
		check_is_open();
		std::size_t write_size = 0;
		std::size_t sz = size();
		while (write_size < data.size()) {
			ssize_t res = ::pwrite(_fd, data.data() + write_size,
								   data.size() - write_size, sz + write_size);
			if (res == -1) {
				if (errno == EAGAIN) {
					break;
				} else if (errno == EINTR) {
					continue;
				} else {
					ERROR_LOG << "write failed, file: " << _filepath
							  << ", error: " << strerror(errno);
					throw std::runtime_error("write failed, file: " +
											 _filepath);
				}
			} else if (res == 0) {
				ERROR_LOG << "write failed, file: " << _filepath;
				throw std::runtime_error("write failed, file: " + _filepath);
			} else {
				write_size += res;
			}
		}
		return write_size;
	}
	void FileObj::check_is_open() const {
		if (!is_open()) {
			ERROR_LOG << "file: " << _filepath << " is not open";
			throw std::runtime_error("file: " + _filepath + " is not open");
		}
	}
	bool FileObj::truncate(std::size_t length) {
		check_is_open();
		int res = ::ftruncate(_fd, length);
		if (res == -1) {
			ERROR_LOG << "ftruncate failed, file: " << _filepath
					  << ", error: " << strerror(errno);
			throw std::runtime_error("ftruncate failed: " + _filepath);
		}
		return true;
	}
	std::size_t FileObj::size() const {
		check_is_open();
		struct stat statbuf{};
		int res = ::fstat(_fd, &statbuf);
		if (res == -1) {
			ERROR_LOG << "fstat failed, file: " << _filepath
					  << ", error: " << strerror(errno);
			throw std::runtime_error("fstat failed: " + _filepath);
		}
		return statbuf.st_size;
	}
	void FileObj::sync() {
		check_is_open();
		int res = ::fsync(_fd);
		if (res == -1) {
			ERROR_LOG << "fsync failed, file: " << _filepath
					  << ", error: " << strerror(errno);
			throw std::runtime_error("fsync failed: " + _filepath);
		}
	}

	bool FileObj::is_open() const { return _fd != -1; }

	void FileObj::close() {
		if (_fd != -1) {
			int fd = _fd;
			_fd = -1;
			int res = ::close(fd);
			if (res == -1) {
				ERROR_LOG << "file: " << _filepath
						  << " close failed, error: " << strerror(errno);
				throw std::runtime_error("file: " + _filepath +
										 " close failed");
			}
		}
	}

	std::string FileObj::get_file_path() { return _filepath; }

	FileObj::~FileObj() {
		try {
			close();
		} catch (const std::exception &e) { ERROR_LOG << e.what(); }
	}

} // namespace atlas::io
