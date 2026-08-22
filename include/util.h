#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

std::vector<uint8_t> get_bytes_of_uint16t(std::uint16_t num);
std::vector<uint8_t> get_bytes_of_uint32t(std::uint32_t num);
std::vector<uint8_t> get_bytes_of_uint64t(std::uint64_t num);


template <class iter>
std::uint64_t decode_to_uint64(iter begin, iter end) {
	std::uint64_t res = 0;
	for (auto it = begin; it != end; it++) {
		std::uint8_t uint_num = static_cast<std::uint8_t>(*it);
		res = (res << 8) | uint_num;
	}
	return res;
}

template <class iter>
std::uint32_t decode_to_uint32(iter begin, iter end) {
	std::uint32_t res = 0;
	for (auto it = begin; it != end; it++) {
		std::uint8_t uint_num = static_cast<std::uint8_t>(*it);
		res = (res << 8) | uint_num;
	}
	return res;
}

template <class iter>
std::uint16_t decode_to_uint16(iter begin, iter end) {
	std::uint16_t res = 0;
	for (auto it = begin; it != end; it++) {
		std::uint8_t uint_num = static_cast<std::uint8_t>(*it);
		res = (res << 8) | uint_num;
	}
	return res;
}


class FileObj {
public:
	FileObj();
	FileObj(const std::string &path);
	FileObj(const std::string &path, int fd);
	FileObj(const FileObj &other) = delete;
	FileObj(FileObj &&other);
	FileObj &operator=(FileObj &&other);
	~FileObj();


	static FileObj open_read(const std::string &path);

	static FileObj create_and_write(const std::string &path,
									std::vector<std::uint8_t> data);
	std::vector<std::uint8_t> read(std::size_t offset,
								   std::size_t length) const;
	std::uint64_t read_uint64(std::size_t offset) const;
	std::uint32_t read_uint32(std::size_t offset) const;
	std::uint16_t read_uint16(std::size_t offset) const;
	std::uint8_t read_uint8(std::size_t offset) const;
	std::size_t write_data(std::vector<std::uint8_t> data);
	std::size_t write_uint64(std::uint64_t data);
	static std::vector<std::uint8_t> get_bytes_of_uint64(std::uint64_t n);

	bool append_data(std::vector<std::uint8_t> data);

	bool close();
	std::size_t size() const;
	bool is_valid() const;
	bool delete_();

private:
	void swap(FileObj &&other);

	int _fd;
	std::string _path;
};