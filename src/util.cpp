#include "util.h"
#include "log.h"
#include <algorithm>
#include <asm-generic/errno-base.h>
#include <cerrno>
#include <cstddef>
#include <fcntl.h>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string.h>
#include <string>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <utility>
#include <vector>

std::vector<uint8_t> get_bytes_of_uint16t(std::uint16_t num) {
	std::vector<uint8_t> ret;
	for (int i = 0; i < 2; i++) {
		ret.push_back((num >> (8 - i * 8)) & static_cast<std::uint16_t>(0xff));
	}
	return ret;
}

std::vector<uint8_t> get_bytes_of_uint32t(std::uint32_t num) {
	std::vector<uint8_t> ret;
	for (int i = 0; i < 4; i++) {
		ret.push_back((num >> (8 * 3 - i * 8)) &
					  static_cast<std::uint16_t>(0xff));
	}
	return ret;
}

std::vector<uint8_t> get_bytes_of_uint64t(std::uint64_t num) {
	std::vector<uint8_t> ret;
	for (int i = 0; i < 8; i++) {
		ret.push_back((num >> (8 * 7 - i * 8)) &
					  static_cast<std::uint8_t>(0xff));
	}
	return ret;
}

FileObj::FileObj() : _fd(-1) {}

FileObj::FileObj(const std::string &path) : _fd(-1), _path(path) {}

FileObj::FileObj(const std::string &path, int fd) : _path(path), _fd(fd) {}

FileObj::FileObj(FileObj &&other) : _fd(other._fd), _path(other._path) {
	other._fd = -1;
}
void FileObj::swap(FileObj &&other) {
	std::swap(_fd, other._fd);
	std::swap(_path, other._path);
}


FileObj &FileObj::operator=(FileObj &&other) {
	swap(std::move(other));
	return *this;
}

FileObj::~FileObj() {
	if (_fd != -1) { ::close(_fd); }
}


FileObj FileObj::open_read(const std::string &path) {
	int fd = ::open(path.data(), O_RDONLY);
	if (fd == -1) {
		LOG(Log::ERROR) << "file: " << path
						<< " open failed, errno: " << strerror(errno);
		return FileObj(path, -1);
	}
	return FileObj(path, fd);
}

FileObj FileObj::create_and_write(const std::string &path,
								  std::vector<std::uint8_t> data) {
	int fd = ::open(path.data(), O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (fd == -1) {
		LOG(Log::ERROR) << "file: " << path
						<< " open failed, errno: " << strerror(errno);
		throw std::runtime_error("file: " + path + " open failed");
	}
	FileObj file(path, fd);
	ssize_t write_size = 0, cur_write_size = 0;
	while (write_size < data.size()) {
		cur_write_size =
			::write(fd, data.data() + write_size, data.size() - write_size);
		if (cur_write_size == -1) {
			if (errno == EINTR) {
				continue;
			} else {
				throw std::runtime_error("file: " + path + " write failed");
			}
		} else if (cur_write_size > 0) {
			write_size += cur_write_size;
		} else {
			// cur_write_size == 0
			throw std::runtime_error("file: " + path + " write failed");
		}
	}
	if (!file.fsync()) {
		throw std::runtime_error("file: " + path + " fsync failed");
	}
	return file;
}

FileObj FileObj::create_rw(const std::string &path) {
	int fd = ::open(path.c_str(), O_CREAT | O_RDWR, 0644);
	return FileObj(path, fd);
}

FileObj FileObj::open_rw(const std::string &path) {
	int fd = ::open(path.c_str(), O_RDWR);
	return FileObj(path, fd);
}

bool FileObj::atomic_write(const std::string &file_path,
						   const std::vector<std::uint8_t> &data) {
	FileObj::create_and_write(file_path + ".tmp", data);
	std::filesystem::rename(file_path + ".tmp", file_path);
	std::string parent_path = std::filesystem::path(file_path).parent_path();
	int fd = ::open(parent_path.c_str(), O_RDONLY | O_DIRECTORY);
	if (fd == -1) {
		throw std::runtime_error(std::string("data_dir open failed, errno: ") +
								 strerror(errno));
	}
	int res = ::fsync(fd);
	if (res == -1) {
		::close(fd);
		throw std::runtime_error(std::string("data_dir fsync failed, errno: ") +
								 strerror(errno));
	}
	if (fd != -1) { ::close(fd); }
	return true;
}

bool FileObj::atomic_write(const std::string &file_path,
						   const std::string &str) {
	std::vector<std::uint8_t> data(str.begin(), str.end());
	return atomic_write(file_path, data);
}

void FileObj::fsync_parent_path(const std::string &file_path) {
	std::string parent_path = std::filesystem::path(file_path).parent_path();
	int fd = ::open(parent_path.c_str(), O_RDONLY | O_DIRECTORY);
	if (fd == -1) {
		throw std::runtime_error("parent dir: " + parent_path +
								 " open failed, errno: " + strerror(errno));
	}
	int res = ::fsync(fd);
	if (res == -1) {
		::close(fd);
		throw std::runtime_error(std::string("data_dir fsync failed, errno: ") +
								 strerror(errno));
	}
	if (fd != -1) { ::close(fd); }
}

std::vector<std::uint8_t> FileObj::read(std::size_t offset,
										std::size_t length) const {
	// char buf[length];
	std::vector<std::uint8_t> buf(length);
	// ::memset(buf, 0, length);
	off_t off = ::lseek(_fd, offset, SEEK_SET);
	if (off == -1) {
		throw std::runtime_error("lseek, fd: " + std::to_string(_fd) +
								 " , errno: " + strerror(errno));
	}
	ssize_t read_bytes = 0;
	while (read_bytes < length) {
		ssize_t cur_size =
			::read(_fd, buf.data() + read_bytes, length - read_bytes);
		if (cur_size == -1) {
			if (errno == EINTR) {
				continue;
			} else {
				throw std::runtime_error(
					"read failed, fd: " + std::to_string(_fd) +
					" , errno: " + strerror(errno));
			}
		} else if (cur_size == 0) {
			// 没有数据了
			break;
		} else {
			read_bytes += cur_size;
		}
	}
	return buf;
}

std::vector<std::uint8_t> FileObj::read_continue(std::size_t length) const {
	char buf[length];
	::memset(buf, 0, length);
	ssize_t read_bytes = 0, cur_read_bytes = 0;
	while (read_bytes < length) {
		cur_read_bytes = ::read(_fd, buf + read_bytes, length - read_bytes);
		if (cur_read_bytes > 0) {
			read_bytes += cur_read_bytes;
		} else if (cur_read_bytes == -1 && errno == EINTR) {
			continue;
		} else {
			break;
		}
	}
	if (cur_read_bytes == -1) {
		LOG(Log::ERROR) << "read file: " << _path
						<< " failed, errno: " << strerror(errno);
		throw std::runtime_error("read file failed.");
	}
	return std::vector<uint8_t>(buf, buf + read_bytes);
}

std::uint64_t FileObj::read_uint64(std::size_t offset) const {
	char buf[8] = {0};
	off_t off = ::lseek(_fd, offset, SEEK_SET);
	ssize_t read_bytes = ::read(_fd, buf, 8);
	if (read_bytes == -1) {
		LOG(Log::ERROR) << "read file: " << _path
						<< " failed, errno: " << strerror(errno);
		throw std::runtime_error("read file failed.");
	}
	std::uint64_t dec_data = decode_to_uint64(buf, buf + 8);
	return dec_data;
}

std::uint32_t FileObj::read_uint32(std::size_t offset) const {
	char buf[4] = {0};
	off_t off = ::lseek(_fd, offset, SEEK_SET);
	ssize_t read_bytes = ::read(_fd, buf, 4);
	if (read_bytes == -1) {
		LOG(Log::ERROR) << "read file: " << _path
						<< " failed, errno: " << strerror(errno);
		throw std::runtime_error("read file failed.");
	}
	std::uint32_t dec_data = decode_to_uint32(buf, buf + 4);
	return dec_data;
}

std::uint16_t FileObj::read_uint16(std::size_t offset) const {
	char buf[2] = {0};
	off_t off = ::lseek(_fd, offset, SEEK_SET);
	ssize_t read_bytes = ::read(_fd, buf, 2);
	if (read_bytes == -1) {
		LOG(Log::ERROR) << "read file: " << _path
						<< " failed, errno: " << strerror(errno);
		throw std::runtime_error("read file failed.");
	}
	std::uint16_t dec_data = decode_to_uint16(buf, buf + 2);
	return dec_data;
}

std::uint8_t FileObj::read_uint8(std::size_t offset) const {
	char buf = 0;
	off_t off = ::lseek(_fd, offset, SEEK_SET);
	ssize_t read_bytes = ::read(_fd, &buf, 1);
	if (read_bytes == -1) {
		LOG(Log::ERROR) << "read file: " << _path
						<< " failed, errno: " << strerror(errno);
		throw std::runtime_error("read file failed.");
	}
	return buf;
}

std::size_t FileObj::write_data(std::vector<std::uint8_t> data) {
	return ::write(_fd, data.data(), data.size());
}

std::size_t FileObj::write_uint64(std::uint64_t data) {
	return write_data(get_bytes_of_uint64(data));
}

std::vector<std::uint8_t> FileObj::get_bytes_of_uint64(std::uint64_t n) {
	std::vector<std::uint8_t> data;
	for (std::size_t i = 0; i < 8; i++) {
		data.push_back((n >> (8 * 7 - 8 * i)) & 0xff);
	}
	return data;
}

bool FileObj::append_data(std::vector<std::uint8_t> data) {
	::lseek(_fd, 0, SEEK_END);

	ssize_t write_size = 0;
	while (write_size < data.size()) {
		ssize_t cur =
			::write(_fd, data.data() + write_size, data.size() - write_size);
		if (cur == -1) {
			if (errno == EINTR) {
				continue;
			} else {
				LOG(Log::ERROR)
					<< "file write failed, fd: " << std::to_string(_fd);
				break;
			}
		} else if (cur == 0) {
			LOG(Log::ERROR) << "file write failed, fd: " << std::to_string(_fd);
			break;
		} else {
			write_size += cur;
		}
	}
	return write_size == data.size();
}

bool FileObj::clear() {
	int res = ::ftruncate(_fd, 0);
	if (res == 0) {
		return true;
	} else {
		LOG(Log::ERROR) << "clear file: " << _path
						<< "failed, errno: " << strerror(errno);
		return false;
	}
}

bool FileObj::fsync() {
	int res = ::fsync(_fd);
	if (res == 0) {
		return true;
	} else {
		LOG(Log::ERROR) << "fsync file: " << _path
						<< "failed, errno: " << strerror(errno);
		return false;
	}
}

bool FileObj::seek_begin() { return ::lseek(_fd, 0, SEEK_SET) != -1; }

bool FileObj::close() {
	int res = ::close(_fd);
	if (res == -1) {
		LOG(Log::ERROR) << "close file: " << _path
						<< " failed, errno: " << strerror(errno);
		return false;
	}
	_fd = -1;
	return true;
}
std::size_t FileObj::size() const {
	struct stat buf;
	int res = ::fstat(_fd, &buf);
	if (res == -1) {
		throw std::runtime_error("file fd: " + std::to_string(_fd) +
								 " fstat failed, errno: " + strerror(errno));
	}
	return buf.st_size;
}
bool FileObj::is_valid() const { return _fd != -1; }

bool FileObj::delete_() { return std::filesystem::remove(_path); }