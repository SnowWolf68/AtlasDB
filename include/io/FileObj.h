//
// Created by test on 10/27/26.
//

#ifndef ATLASDB_FILEOBJ_H
#define ATLASDB_FILEOBJ_H

#include <string>
#include <vector>

namespace atlas::io {
	enum class AccessMode { ReadOnly, WriteOnly, ReadWrite };
	class FileObj {
	public:
		FileObj();
		FileObj(int fd, std::string filepath, AccessMode mode);
		FileObj(const FileObj &other) = delete;
		FileObj &operator=(const FileObj &other) = delete;
		void swap(FileObj &&other);
		FileObj(FileObj &&other);
		FileObj &operator=(FileObj &&other);

		void close();
		std::size_t read_at(std::size_t offset,
							std::vector<std::uint8_t> &buffer) const;
		std::size_t write_at(std::size_t offset,
							 const std::vector<std::uint8_t> &data);
		std::size_t append(const std::vector<std::uint8_t> &data);
		bool truncate(std::size_t length);
		std::size_t size() const;
		void sync();
		bool is_open() const;
		void check_is_open() const;
		std::string get_file_path();

		~FileObj();

	private:
		int _fd;
		std::string _filepath;
		AccessMode _access_mode;
	};

} // namespace atlas::io




#endif // ATLASDB_FILEOBJ_H
