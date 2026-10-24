//
// Created by test on 10/23/26.
//

#ifndef ATLASDB_CODEC_H
#define ATLASDB_CODEC_H
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace codec {
	void append_u8(std::vector<std::uint8_t> &data, std::uint8_t byte);
	void append_u16_be(std::vector<std::uint8_t> &data, std::uint16_t value);
	void append_u32_be(std::vector<std::uint8_t> &data, std::uint32_t value);
	void append_u64_be(std::vector<std::uint8_t> &data, std::uint64_t value);
	void append_bytes(std::vector<std::uint8_t> &data,
					  const std::vector<std::uint8_t> &bytes);
	void append_string(std::vector<std::uint8_t> &data,
					   const std::string &string);

	std::optional<std::uint8_t> read_u8(const std::vector<std::uint8_t> &data, std::size_t pos);
	std::optional<std::uint16_t>
	read_u16_be(const std::vector<std::uint8_t> &data, std::size_t pos);
	std::optional<std::uint32_t>
	read_u32_be(const std::vector<std::uint8_t> &data, std::size_t pos);
	std::optional<std::uint64_t>
	read_u64_be(const std::vector<std::uint8_t> &data, std::size_t pos);
	std::optional<std::vector<std::uint8_t>>
	read_bytes(const std::vector<std::uint8_t> &data, std::size_t pos,
			   std::size_t len);
	std::optional<std::string>
	read_string(const std::vector<std::uint8_t> &data, std::size_t pos,
				std::size_t len);
} // namespace codec

#endif // ATLASDB_CODEC_H
