//
// Created by test on 10/27/26.
//

#include "io/FileSystem.h"

#include "log.h"

#include <cstring>
#include <fcntl.h>
namespace atlas::io {
	FileObj FileSystem::open(const std::string &path, OpenOptions options) {
		int oflag = O_CLOEXEC;
		if (options._create_mode == CreateMode::OpenOrCreate) {
			oflag |= O_CREAT; // O_CREAT: 文件不存在时，允许 open() 创建它.
							  // 如果存在了也不会报错
		} else if (options._create_mode == CreateMode::CreateNew) {
			oflag |= O_CREAT | O_EXCL; // O_EXCL: 如果文件已经存在就报错
		}

		if (options._access_mode == AccessMode::ReadOnly) {
			oflag |= O_RDONLY;
		} else if (options._access_mode == AccessMode::WriteOnly) {
			oflag |= O_WRONLY;
		} else if (options._access_mode == AccessMode::ReadWrite) {
			oflag |= O_RDWR;
		}

		if (options._truncate) {
			if (options._access_mode == AccessMode::ReadOnly) {
				ERROR_LOG << "file: " << path << " truncate failed, read only";
				throw std::runtime_error("file: " + path +
										 " truncate failed, read only");
			}
			oflag |= O_TRUNC;
		}

		int fd = ::open(path.c_str(), oflag, options._permissions);
		if (fd == -1) {
			ERROR_LOG << "file: " << path
					  << " open failed, error: " << strerror(errno);
			throw std::runtime_error("file: " + path + " open failed");
		}
		return FileObj(fd, path, options._access_mode);
	}
	bool FileSystem::exists(const std::string &path) {
		return std::filesystem::exists(path);
	}
	bool FileSystem::create_directory(const std::string &path) {
		return std::filesystem::create_directory(path);
	}
	bool FileSystem::create_directories(const std::string &path) {
		return std::filesystem::create_directories(path);
	}
	std::vector<std::filesystem::directory_entry>
	FileSystem::list_directory(const std::string &path) {
		std::vector<std::filesystem::directory_entry> result;
		for (auto it : std::filesystem::directory_iterator(path)) {
			result.push_back(it);
		}
		return result;
	}
	void FileSystem::rename(const std::string &path,
							const std::string &new_path) {
		std::filesystem::rename(path, new_path);
	}
	bool FileSystem::remove_file(const std::string &path) {
		int res = ::unlink(path.c_str());
		if (res == 0) {
			return true;
		} else {
			if (errno == ENOENT) {	// 文件不存在
				return false;
			} else {
				ERROR_LOG << "unlink file failed, file: " << path
						  << " , error: " << strerror(errno);
				throw std::runtime_error("file: " + path + " unlink failed");
			}
		}
	}
	void FileSystem::sync_dir(const std::string &path) {
		int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		if (fd == -1) {
			ERROR_LOG << "file: " << path << " open failed";
			throw std::runtime_error("file: " + path + " open failed");
		}
		int res = ::fsync(fd);
		if (res == -1) {
			int fsync_errno = errno;
			close(fd);
			ERROR_LOG << "file: " << path
					  << " fsync failed, error: " << strerror(fsync_errno);
			throw std::runtime_error("file: " + path + " fsync failed");
		}
		close(fd);
	}
	void FileSystem::close(int fd) {
		int close_res = ::close(fd);
		if (close_res == -1) {
			ERROR_LOG << "close file failed, fd: " << std::to_string(fd)
					  << " error: " << strerror(errno);
			throw std::runtime_error("close file failed, fd: " +
									 std::to_string(fd));
		}
	}
} // namespace atlas::io
