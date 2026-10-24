#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>
#include <zlib.h>

// Build and tamper with on-disk fixtures independently of production codec.
namespace binary_test {

inline void append_u16(std::vector<std::uint8_t> &bytes, std::uint16_t value) {
	bytes.push_back(static_cast<std::uint8_t>(value >> 8));
	bytes.push_back(static_cast<std::uint8_t>(value));
}

inline void append_u32(std::vector<std::uint8_t> &bytes, std::uint32_t value) {
	for (int shift : {24, 16, 8, 0}) {
		bytes.push_back(static_cast<std::uint8_t>(value >> shift));
	}
}

inline void overwrite_u16(std::vector<std::uint8_t> &bytes, std::size_t pos,
						  std::uint16_t value) {
	bytes.at(pos) = static_cast<std::uint8_t>(value >> 8);
	bytes.at(pos + 1) = static_cast<std::uint8_t>(value);
}

inline void overwrite_u32(std::vector<std::uint8_t> &bytes, std::size_t pos,
						  std::uint32_t value) {
	for (unsigned i = 0; i < 4; ++i) {
		bytes.at(pos + i) = static_cast<std::uint8_t>(value >> (24 - i * 8));
	}
}

inline void append_crc(std::vector<std::uint8_t> &bytes) {
	const auto crc = crc32(0, bytes.data(), static_cast<uInt>(bytes.size()));
	append_u32(bytes, static_cast<std::uint32_t>(crc));
}

inline void refresh_crc(std::vector<std::uint8_t> &bytes) {
	if (bytes.size() < 4) { throw std::runtime_error("invalid CRC fixture"); }
	bytes.resize(bytes.size() - 4);
	append_crc(bytes);
}

inline std::vector<std::uint8_t> block_bytes(
	const std::vector<std::uint8_t> &data,
	const std::vector<std::uint16_t> &offsets, bool with_crc) {
	auto bytes = data;
	for (auto offset : offsets) { append_u16(bytes, offset); }
	append_u16(bytes, static_cast<std::uint16_t>(offsets.size()));
	if (with_crc) { append_crc(bytes); }
	return bytes;
}

} // namespace binary_test
