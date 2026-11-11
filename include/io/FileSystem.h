//
// Created by test on 10/27/26.
//

#ifndef ATLASDB_FILESYSTEM_H
#define ATLASDB_FILESYSTEM_H

#include "io/FileObj.h"

#include <filesystem>

namespace atlas::io {
	enum class CreateMode {
		OpenExisting, // 只打开已有文件, 不存在就报错
		OpenOrCreate, // 存在就打开, 不存在就创建
		CreateNew	  // 必须创建新文件, 已经存在就保存
	};
	struct OpenOptions {
		AccessMode _access_mode = AccessMode::ReadOnly;
		CreateMode _create_mode = CreateMode::OpenExisting;
		bool _truncate = false;
		mode_t _permissions = 0644;
	};
	class FileSystem {
	public:
		static FileObj open(const std::string &path, OpenOptions options);
		static bool exists(const std::string &path);
		static bool create_directory(const std::string &path);
		static bool create_directories(const std::string &path);
		static std::vector<std::filesystem::directory_entry>
		list_directory(const std::string &path);
		static void rename(const std::string &path,
						   const std::string &new_path);
		static bool remove_file(const std::string &path);
		static void sync_dir(const std::string &path);
		static void close(int fd);
	};

} // namespace atlas::io

#endif // ATLASDB_FILESYSTEM_H
