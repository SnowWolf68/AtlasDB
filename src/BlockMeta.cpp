#include "BlockMeta.h"
#include <zlib.h>


std::vector<uint8_t> BlockMeta::get_bytes_of_uint16t(std::uint16_t num) {
	std::vector<uint8_t> ret;
	for (int i = 0; i < 2; i++) {
		ret.push_back((num >> (8 - i * 8)) & static_cast<std::uint16_t>(0xff));
	}
	return ret;
}

std::vector<uint8_t> BlockMeta::get_bytes_of_uint32t(std::uint32_t num) {
	std::vector<uint8_t> ret;
	for (int i = 0; i < 4; i++) {
		ret.push_back((num >> (8 * 3 - i * 8)) &
					  static_cast<std::uint16_t>(0xff));
	}
	return ret;
}

std::uint32_t
BlockMeta::decode_to_uint32(std::vector<std::uint8_t>::const_iterator begin,
							std::vector<std::uint8_t>::const_iterator end) {
	std::uint32_t res = 0;
	for (auto it = begin; it != end; it++) {
		res = (res << 8) | (*it);
	}
	return res;
}

std::uint16_t
BlockMeta::decode_to_uint16(std::vector<std::uint8_t>::const_iterator begin,
							std::vector<std::uint8_t>::const_iterator end) {
	std::uint16_t res = 0;
	for (auto it = begin; it != end; it++) {
		res = (res << 8) | (*it);
	}
	return res;
}

BlockMeta::BlockMeta(std::uint32_t offset, std::string first_key,
					 std::string last_key)
	: _offset(offset), _first_key(first_key), _last_key(last_key) {}


/**
num_entries (4B)
offset (4B) + first_key_len (2B) + first_key + last_key_len (2B) + last_key
hash (4B)
*/
std::vector<std::uint8_t> BlockMeta::encode(std::vector<BlockMeta> blocks) {
	std::vector<std::uint8_t> num_entries_bytes =
		get_bytes_of_uint32t(blocks.size());
	std::vector<std::uint8_t> res(num_entries_bytes.begin(),
								  num_entries_bytes.end());
	for (BlockMeta meta : blocks) {
		std::vector<std::uint8_t> offset_bytes =
			get_bytes_of_uint32t(meta._offset);
		res.insert(res.end(), offset_bytes.begin(), offset_bytes.end());

		std::vector<std::uint8_t> first_key_bytes =
			get_bytes_of_uint16t(meta._first_key.size());
		res.insert(res.end(), first_key_bytes.begin(), first_key_bytes.end());
		res.insert(res.end(), meta._first_key.begin(), meta._first_key.end());

		std::vector<std::uint8_t> last_key_bytes =
			get_bytes_of_uint16t(meta._last_key.size());
		res.insert(res.end(), last_key_bytes.begin(), last_key_bytes.end());
		res.insert(res.end(), meta._last_key.begin(), meta._last_key.end());
	}
	uint32_t crc = crc32(0UL, res.data(), res.size());
	std::vector<std::uint8_t> crc_bytes = get_bytes_of_uint32t(crc);
	res.insert(res.end(), crc_bytes.begin(), crc_bytes.end());
	return res;
}

/**
offset (4B) + first_key_len (2B) + first_key + last_key_len (2B) + last_key
*/
std::optional<std::vector<BlockMeta>>
BlockMeta::decode(std::vector<std::uint8_t> data) {
	if (data.size() < 4) { return std::nullopt; }
	std::vector<BlockMeta> res;
	std::uint32_t num_entires =
		decode_to_uint32(data.begin(), data.begin() + 4);
	int idx = 4;
	while (idx < data.size() - 4) {
		std::uint32_t offset =
			decode_to_uint32(data.begin() + idx, data.begin() + idx + 4);
		idx += 4;
		std::uint16_t first_key_len =
			decode_to_uint16(data.begin() + idx, data.begin() + idx + 2);
		idx += 2;
		std::string first_key(data.begin() + idx,
							  data.begin() + idx + first_key_len);
		idx += first_key_len;
		std::uint16_t last_key_len =
			decode_to_uint16(data.begin() + idx, data.begin() + idx + 2);
		idx += 2;
		std::string last_key(data.begin() + idx,
							 data.begin() + idx + last_key_len);
		idx += last_key_len;
		res.push_back(BlockMeta(offset, first_key, last_key));
	}
	std::uint32_t crc =
		decode_to_uint32(data.begin() + idx, data.begin() + idx + 4);
	idx += 4;
	std::uint32_t cur_crc = crc32(0UL, data.data(), data.size() - 4);
	if (cur_crc != crc) { return std::nullopt; }
	return res;
}