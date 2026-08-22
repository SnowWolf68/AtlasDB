#pragma once
#include <cstdint> // std::uint64_t, std::uint8_t
#include <optional>
#include <string>
#include <vector>


class BlockMeta {
public:
	BlockMeta(std::uint32_t offset, std::string first_key,
			  std::string last_key);

	static std::vector<std::uint8_t> encode(std::vector<BlockMeta> blocks);
	static std::optional<std::vector<BlockMeta>>
	decode(std::vector<std::uint8_t> data);

    
	std::uint32_t _offset;
	std::string _first_key;
	std::string _last_key;

private:
	static std::vector<uint8_t> get_bytes_of_uint16t(std::uint16_t num);
	static std::vector<uint8_t> get_bytes_of_uint32t(std::uint32_t num);

	static std::uint32_t
	decode_to_uint32(std::vector<std::uint8_t>::const_iterator begin,
					 std::vector<std::uint8_t>::const_iterator end);

	static std::uint16_t
	decode_to_uint16(std::vector<std::uint8_t>::const_iterator begin,
					 std::vector<std::uint8_t>::const_iterator end);
};