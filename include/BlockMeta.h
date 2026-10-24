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
};