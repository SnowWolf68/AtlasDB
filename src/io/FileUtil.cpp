//
// Created by test on 10/27/26.
//

#include "io/FileUtil.h"

#include "io/FileSystem.h"
#include "log.h"
namespace atlas::io {
	void read_exact_at(FileObj &file, std::vector<std::uint8_t> &buffer, std::size_t offset) {
		std::size_t read_size = file.read_at(offset, buffer);
		if (read_size < buffer.size()) {
			ERROR_LOG << "read data less than buffer size";
			throw std::runtime_error("read data less than buffer size");
		}
	}
	void write_all_at(FileObj &file, const std::vector<std::uint8_t> &data, std::size_t offset) {
		std::size_t write_size = file.write_at(offset, data);
		if (write_size < data.size()) {
			ERROR_LOG << "write data less than buffer size";
			throw std::runtime_error("write data less than buffer size");
		}
	}
	void append_all(FileObj &file, const std::vector<std::uint8_t> &data) {
		std::size_t write_size = file.append(data);
		if (write_size < data.size()) {
			ERROR_LOG << "append data less than buffer size";
			throw std::runtime_error("append data less than buffer size");
		}
	}

	void atomic_write_all_replace(const std::string &file_path,
								  const std::vector<std::uint8_t> &data) {
		OpenOptions options;
		options._create_mode = CreateMode::OpenOrCreate;
		options._access_mode = AccessMode::WriteOnly;
		options._permissions = 0644;
		options._truncate = true;
		FileObj tmp_file;
		try {
			tmp_file = FileSystem::open(file_path + ".tmp", options);
			append_all(tmp_file, data);
			tmp_file.sync();
			std::string tmp_file_path = tmp_file.get_file_path(), original_file_path = file_path;
			std::filesystem::rename(tmp_file_path, original_file_path);
			std::filesystem::path parent_path =
				std::filesystem::path(original_file_path).parent_path();
			if (parent_path.empty()) { parent_path = std::filesystem::path("./"); }
			OpenOptions parent_options;
			FileObj parent_obj = FileSystem::open(parent_path, parent_options);
			parent_obj.sync();
		} catch (const std::exception &e) {
			ERROR_LOG << "file: " << tmp_file.get_file_path()
					  << " sync failed, error: " << e.what();
			try {
				FileSystem::remove_file(tmp_file.get_file_path());
			} catch (...) { ERROR_LOG << "file: " << tmp_file.get_file_path() << " remove_failed"; }
			throw;
		}
	}
	void atomic_write_all_replace(FileObj &file, const std::vector<std::uint8_t> &data) {
		atomic_write_all_replace(file.get_file_path(), data);
	}
} // namespace atlas::io
