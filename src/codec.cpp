//
// Created by test on 10/23/26.
//

#include "codec.h"

#include <cstring>
#include <endian.h>

namespace codec {
	void append_u8(std::vector<std::uint8_t> &data, std::uint8_t byte) {
		std::size_t data_size = data.size();
		data.resize(data_size + 1);
		data.data()[data_size] = byte;
	}
	void append_u16_be(std::vector<std::uint8_t> &data, std::uint16_t value) {
		std::uint16_t be = htobe16(value);
		std::size_t bef_size = data.size();
		data.resize(data.size() + 2);
		memcpy(data.data() + bef_size, &be, 2);
	}
	void append_u32_be(std::vector<std::uint8_t> &data, std::uint32_t value) {
		std::uint32_t be = htobe32(value);
		std::size_t bef_size = data.size();
		data.resize(data.size() + 4);
		memcpy(data.data() + bef_size, &be, 4);
	}
	void append_u64_be(std::vector<std::uint8_t> &data, std::uint64_t value) {
		std::uint64_t be = htobe64(value);
		std::size_t bef_size = data.size();
		data.resize(data.size() + 8);
		memcpy(data.data() + bef_size, &be, 8);
	}
	void append_bytes(std::vector<std::uint8_t> &data,
					  const std::vector<std::uint8_t> &bytes) {
		if (bytes.empty()) { return; }
		std::size_t data_size = data.size(), bytes_size = bytes.size();
		data.resize(data.size() + bytes_size);
		memcpy(data.data() + data_size, bytes.data(), bytes_size);
	}

	void append_string(std::vector<std::uint8_t> &data,
					   const std::string &string) {
		if (string.empty()) { return; }
		std::size_t data_size = data.size();
		data.resize(data_size + string.size());
		memcpy(data.data() + data_size, string.data(), string.size());
	}

	std::optional<std::uint8_t> read_u8(const std::vector<std::uint8_t> &data, std::size_t pos) {
		if (pos > data.size() || data.size() - pos < 1) return std::nullopt;
		return data[pos];
	}
	std::optional<std::uint16_t>
	read_u16_be(const std::vector<std::uint8_t> &data, std::size_t pos) {
		if (pos > data.size() || data.size() - pos < 2) return std::nullopt;
		std::uint16_t h = 0;
		memcpy(&h, data.data() + pos, 2);
		return be16toh(h);
	}
	std::optional<std::uint32_t>
	read_u32_be(const std::vector<std::uint8_t> &data, std::size_t pos) {
		if (pos > data.size() || data.size() - pos < 4) return std::nullopt;
		std::uint32_t h = 0;
		memcpy(&h, data.data() + pos, 4);
		return be32toh(h);
	}
	std::optional<std::uint64_t>
	read_u64_be(const std::vector<std::uint8_t> &data, std::size_t pos) {
		if (pos > data.size() || data.size() - pos < 8) return std::nullopt;
		std::uint64_t h = 0;
		memcpy(&h, data.data() + pos, 8);
		return be64toh(h);
	}
	std::optional<std::vector<std::uint8_t>>
	read_bytes(const std::vector<std::uint8_t> &data, std::size_t pos,
			   std::size_t len) {
		if (pos > data.size() || data.size() - pos < len) return std::nullopt;
		if (len == 0) { return std::vector<std::uint8_t>(); }
		std::vector<std::uint8_t> bytes;
		bytes.resize(len);
		memcpy(bytes.data(), data.data() + pos, len);
		return bytes;
	}

	std::optional<std::string>
	read_string(const std::vector<std::uint8_t> &data, std::size_t pos,
				std::size_t len) {
		if (pos > data.size() || data.size() - pos < len) return std::nullopt;
		if (len == 0) { return std::string(); }
		std::string str;
		str.resize(len);
		memcpy(str.data(), data.data() + pos, len);
		return str;
	}
} // namespace codec