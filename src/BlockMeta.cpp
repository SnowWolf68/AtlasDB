#include "BlockMeta.h"
#include "codec.h"
#include "log.h"

#include <limits>
#include <zlib.h>

BlockMeta::BlockMeta(std::uint32_t offset, std::string first_key,
					 std::string last_key)
	: _offset(offset), _first_key(first_key), _last_key(last_key) {}


/**
num_entries (4B)
for 每一个block:  offset (4B) + first_key_len (2B) + first_key + last_key_len
(2B) + last_key hash (4B)
*/
std::vector<std::uint8_t> BlockMeta::encode(std::vector<BlockMeta> blocks) {
	std::vector<std::uint8_t> res;
	codec::append_u32_be(res, blocks.size());
	for (BlockMeta meta : blocks) {
		if (meta._first_key.size() > std::numeric_limits<uint16_t>::max() ||
			meta._last_key.size() > std::numeric_limits<uint16_t>::max()) {
			ERROR_LOG << "first key len or last key size exceeds max len";
			throw std::runtime_error(
				"first key len or last key size exceeds max len");
		}
		codec::append_u32_be(res, meta._offset);
		codec::append_u16_be(res, meta._first_key.size());
		codec::append_string(res, meta._first_key);
		codec::append_u16_be(res, meta._last_key.size());
		codec::append_string(res, meta._last_key);
	}
	uint32_t crc = crc32(0UL, res.data(), res.size());
	codec::append_u32_be(res, crc);
	return res;
}

/**
num_entries (4B)
for 每一个block:  offset (4B) + first_key_len (2B) + first_key + last_key_len
(2B) + last_key hash (4B)
*/
std::optional<std::vector<BlockMeta>>
BlockMeta::decode(std::vector<std::uint8_t> data) {
	if (data.size() < 4) { return std::nullopt; }
	std::vector<BlockMeta> res;
	std::optional<std::uint32_t> num_entries_opt = codec::read_u32_be(data, 0);
	if (num_entries_opt == std::nullopt) { return std::nullopt; }
	std::uint32_t num_entries = num_entries_opt.value();
	int idx = 4;
	while (idx < data.size() - 4) {
		std::optional<std::uint32_t> offset_opt = codec::read_u32_be(data, idx);
		if (offset_opt == std::nullopt) { return std::nullopt; }
		std::uint32_t offset = offset_opt.value();
		idx += 4;
		std::optional<std::uint16_t> first_key_len_opt =
			codec::read_u16_be(data, idx);
		if (first_key_len_opt == std::nullopt) { return std::nullopt; }
		std::uint16_t first_key_len = first_key_len_opt.value();
		idx += 2;
		std::optional<std::string> first_key_opt =
			codec::read_string(data, idx, first_key_len);
		if (first_key_opt == std::nullopt) { return std::nullopt; }
		std::string first_key = first_key_opt.value();
		idx += first_key_len;
		std::optional<std::uint16_t> last_key_len_opt =
			codec::read_u16_be(data, idx);
		if (last_key_len_opt == std::nullopt) { return std::nullopt; }
		std::uint16_t last_key_len = last_key_len_opt.value();
		idx += 2;
		std::optional<std::string> last_key_opt =
			codec::read_string(data, idx, last_key_len);
		if (last_key_opt == std::nullopt) { return std::nullopt; }
		std::string last_key = last_key_opt.value();
		idx += last_key_len;
		res.push_back(BlockMeta(offset, first_key, last_key));
	}
	std::optional<std::uint32_t> crc_opt = codec::read_u32_be(data, idx);
	if (crc_opt == std::nullopt) { return std::nullopt; }
	std::uint32_t crc = crc_opt.value();
	std::uint32_t cur_crc = crc32(0UL, data.data(), data.size() - 4);
	if (cur_crc != crc) { return std::nullopt; }
	if (num_entries != res.size()) { return std::nullopt; }
	return res;
}