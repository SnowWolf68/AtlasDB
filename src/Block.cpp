#include "Block.h"

#include "codec.h"
#include "log.h"

#include <cassert>
#include <limits>
#include <stdexcept>
#include <zlib.h>

Block::Block()
	: _cur_size(0), _comparator(std::make_shared<InternalKeyComparator>()) {}

Block::Block(std::vector<uint8_t> data, std::vector<uint16_t> offsets,
			 std::size_t cur_size)
	: _data(data), _offsets(offsets), _cur_size(cur_size),
	  _comparator(std::make_shared<InternalKeyComparator>()) {}

/**
每个Entry: 2B key_len + enc_key + 2B val_len + value
offset: 2B
*/
bool Block::add_entry(const std::string &enc_key, const std::string &value,
					  bool force) {
	if (enc_key.size() > std::numeric_limits<uint16_t>::max() ||
		value.size() > std::numeric_limits<uint16_t>::max()) {
		ERROR_LOG
			<< "add entry failed, enc_key len or value len exceeds max len";
		throw std::runtime_error(
			"add entry failed, enc_key len or value len exceeds max len");
	}
	if (_offsets.size() == std::numeric_limits<std::uint16_t>::max()) {
		ERROR_LOG << "block entries exceeds max number";
		throw std::runtime_error("block entries exceeds max number");
	}
	std::size_t data_size = DEFAULT_KEY_LEN_SIZE + enc_key.size() +
							DEFAULT_VALUE_LEN_SIZE + value.size();
	if (_cur_size + data_size > DEFAULT_MAX_CAPACITY && !force) {
		return false;
	}
	if (_data.size() > std::numeric_limits<std::uint16_t>::max()) {
		ERROR_LOG << "add entry failed: entry offset exceeds uint16_t range";
		throw std::runtime_error(
			"add entry failed: entry offset exceeds uint16_t range");
	}
	_offsets.push_back(_data.size());
	codec::append_u16_be(_data, enc_key.size());
	codec::append_string(_data, enc_key);
	codec::append_u16_be(_data, value.size());
	codec::append_string(_data, value);
	_cur_size += data_size;
	return true;
}

/**
data + offset (each 2B) + num_of_elements (2B) + CRC (4B) (with_hash = true)
*/
std::vector<std::uint8_t> Block::encode(bool with_crc) {
	std::vector<std::uint8_t> res(_data.begin(), _data.end());
	for (std::uint16_t offset : _offsets) {
		codec::append_u16_be(res, offset);
	}
	codec::append_u16_be(res, _offsets.size());
	if (with_crc) {
		uint32_t crc = crc32(0, res.data(), res.size());
		codec::append_u32_be(res, crc);
	}
	return res;
}

/**
data + offset (each 2B) + num_of_elements (2B) + CRC (4B) (with_hash = true)
*/
std::shared_ptr<Block> Block::decode(std::vector<std::uint8_t> enc_data,
									 bool with_crc) {
	if ((with_crc && enc_data.size() < 6) ||
		(!with_crc && enc_data.size() < 2)) {
		return nullptr;
	}

	if (with_crc) {
		std::optional<uint32_t> crc_opt =
			codec::read_u32_be(enc_data, enc_data.size() - 4);
		if (crc_opt == std::nullopt) { return nullptr; }
		std::uint32_t crc = crc_opt.value();

		// check
		std::vector<std::uint8_t> enc_data_without_crc(enc_data.begin(),
													   enc_data.end() - 4);
		uint32_t cur_crc =
			crc32(0, enc_data_without_crc.data(), enc_data_without_crc.size());
		if (cur_crc != crc) { return nullptr; }
	}

	std::size_t tail_bytes = with_crc ? 4 : 0;
	std::optional<uint16_t> num_of_elements_opt =
		codec::read_u16_be(enc_data, enc_data.size() - tail_bytes - 2);
	if (num_of_elements_opt == std::nullopt) { return nullptr; }
	std::uint16_t num_of_elements = num_of_elements_opt.value();
	if (num_of_elements * 2 + 2 + tail_bytes > enc_data.size()) {
		return nullptr;
	}
	std::vector<std::uint16_t> offsets;
	std::size_t offset_start =
		enc_data.size() - 2 - tail_bytes - num_of_elements * 2;
	for (int i = offset_start; i < enc_data.size() - 2 - tail_bytes; i += 2) {
		std::optional<std::uint16_t> offset_opt =
			codec::read_u16_be(enc_data, i);
		if (offset_opt == std::nullopt) { return nullptr; }
		std::uint16_t offset = offset_opt.value();
		if (offset >= offset_start) {
			return nullptr;
		} // 这里和下面两处都需要加等于
		if (!offsets.empty() && offset <= offsets.back()) {
			return nullptr;
		} // 如果相等也不对, 因为如果没有数据, 也不能有offset
		offsets.push_back(offset);
	}

	std::vector<uint8_t> data(
		enc_data.begin(), enc_data.end() - 2 - tail_bytes - offsets.size() * 2);

	return std::make_shared<Block>(data, offsets, data.size());
}


/**
每个Entry: 2B key_len + enc_key + 2B val_len + value
offset: 2B
*/
std::pair<std::string, std::string>
Block::get_kv_at_offset(const std::vector<uint8_t> &data, std::size_t offset) {
	std::optional<std::uint16_t> key_len_opt = codec::read_u16_be(data, offset);
	if (key_len_opt == std::nullopt) {
		ERROR_LOG << "get_kv_at_offset failed, key_len decode failed";
		throw std::runtime_error(
			"get_kv_at_offset failed, key_len decode failed");
	}
	std::uint16_t key_len = key_len_opt.value();
	if (offset + 2 + key_len > data.size()) {
		ERROR_LOG << "key data missing";
		throw std::runtime_error("key data missing");
	}
	auto key_begin = data.begin() + offset + 2;
	std::string key(key_begin, key_begin + key_len);
	std::optional<std::uint16_t> val_len_opt =
		codec::read_u16_be(data, offset + 2 + key_len);
	if (val_len_opt == std::nullopt) {
		ERROR_LOG << "get_kv_at_offset failed, val_len decode failed";
		throw std::runtime_error(
			"get_kv_at_offset failed, val_len decode failed");
	}
	std::uint16_t val_len = val_len_opt.value();
	if (offset + 2 + key_len + 2 + val_len > data.size()) {
		ERROR_LOG << "value data missing";
		throw std::runtime_error("value data missing");
	}
	auto val_begin = key_begin + key_len + 2;
	std::string value(val_begin, val_begin + val_len);
	return std::make_pair(key, value);
}

std::optional<std::size_t>
Block::get_idx_by_enc_key(const std::string &enc_key) {
	int l = 0, r = static_cast<int>(_offsets.size()) - 1;
	while (l <= r) {
		int mid = (l + r) >> 1;
		std::pair<std::string, std::string> kv =
			get_kv_at_offset(_data, _offsets[mid]);
		if ((*_comparator)(kv.first, enc_key) < 0) {
			l = mid + 1;
		} else if ((*_comparator)(kv.first, enc_key) > 0) {
			r = mid - 1;
		} else {
			return mid;
		}
	}
	return std::nullopt;
}

std::optional<std::size_t>
Block::get_idx_by_enc_key_lower_bound(const std::string &enc_key) {
	std::size_t l = 0, r = _offsets.size();
	while (l < r) {
		int mid = (l + r) >> 1;
		std::pair<std::string, std::string> kv =
			get_kv_at_offset(_data, _offsets[mid]);
		if ((*_comparator)(kv.first, enc_key) < 0) {
			l = mid + 1;
		} else {
			r = mid;
		}
	}
	if (l == _offsets.size()) { return std::nullopt; }
	return l;
}

std::optional<std::size_t> Block::get_idx_by_enc_key_lower_bound_match_user_key(
	const std::string &enc_key) {
	std::optional<std::size_t> res = get_idx_by_enc_key_lower_bound(enc_key);
	if (res == std::nullopt) { return std::nullopt; }
	std::size_t idx = res.value();

	std::pair<std::string, std::string> kv =
		get_kv_at_offset(_data, _offsets[idx]);
	std::string find_user_key = InternalKey::decode(kv.first)._key,
				user_key = InternalKey::decode(enc_key)._key;
	if (find_user_key != user_key) { return std::nullopt; }
	return idx;
}

std::optional<std::pair<std::string, std::string>>
Block::get_value(const std::string &enc_key) {
	std::optional<std::size_t> data_idx_opt = get_idx_by_enc_key(enc_key);
	if (data_idx_opt == std::nullopt) {
		return std::nullopt;
	} else {
		return get_kv_at_offset(_data, _offsets[data_idx_opt.value()]);
	}
}

/**
获取完全匹配的enc_key
*/
std::optional<BlockIterator> Block::get_exact(const std::string &enc_key) {
	std::uint64_t commit_sequence =
		InternalKey::decode(enc_key)._commit_sequence;
	std::optional<std::size_t> data_idx_opt = get_idx_by_enc_key(enc_key);
	if (data_idx_opt == std::nullopt) {
		return std::nullopt;
	} else {
		return BlockIterator(shared_from_this(), data_idx_opt.value(),
							 commit_sequence, false);
	}
}

/**
获取user_key完全匹配, commit_sequence小于等于给定commit_sequence的结果
*/
std::optional<BlockIterator>
Block::lower_bound_match_user_key(const std::string &enc_key,
								  bool keep_all_version) {
	std::uint64_t commit_sequence =
		InternalKey::decode(enc_key)._commit_sequence;
	std::optional<std::size_t> data_idx_opt =
		get_idx_by_enc_key_lower_bound_match_user_key(enc_key);
	if (data_idx_opt == std::nullopt) {
		return std::nullopt;
	} else {
		return BlockIterator(shared_from_this(), data_idx_opt.value(),
							 commit_sequence, keep_all_version);
	}
}

std::optional<BlockIterator>
Block::lower_bound(const std::string &enc_key,
				   std::optional<std::uint64_t> commit_sequence_opt,
				   bool keep_all_version) {
	std::uint64_t commit_sequence = commit_sequence_opt == std::nullopt
										? MAX_COMMIT_SEQUENCE
										: commit_sequence_opt.value();
	std::optional<std::size_t> idx_opt =
		get_idx_by_enc_key_lower_bound(enc_key);
	if (idx_opt == std::nullopt) { return std::nullopt; }
	std::size_t idx = idx_opt.value();
	BlockIterator it(shared_from_this(), idx, commit_sequence,
					 keep_all_version);
	if (!it.is_valid()) { return std::nullopt; }
	return it;
}

BlockIterator::BlockIterator(std::shared_ptr<Block> block, std::size_t cur_idx,
							 std::optional<std::uint64_t> commit_sequence,
							 bool keep_all_version)
	: _block(block), _cur_idx(cur_idx), _commit_sequence(commit_sequence),
	  _keep_all_version(keep_all_version) {
	if (_commit_sequence != std::nullopt) { clean_by_commit_sequence(); }
}

BlockIterator Block::begin(std::optional<std::uint64_t> commit_sequence,
						   bool keep_all_version) {
	return BlockIterator(shared_from_this(), 0, commit_sequence,
						 keep_all_version);
}

BlockIterator Block::end(std::optional<std::uint64_t> commit_sequence) {
	return BlockIterator(shared_from_this(), _offsets.size(), commit_sequence,
						 false);
}

// 主要是查询前缀, 因此如果需要进位可以直接将低位pop掉
std::string Block::get_next_str(const std::string &str) {
	std::vector<char> vec(str.begin(), str.end());
	int sz = vec.size();
	for (int i = sz - 1; i >= 0; i--) {
		if (vec[i] != static_cast<char>(0xff)) {
			vec[i] += 1;
			break;
		} else {
			vec.pop_back();
		}
	}
	return std::string(vec.begin(), vec.end());
}

std::pair<BlockIterator, BlockIterator>
Block::preffix(const std::string &user_key_preffix,
			   std::optional<std::uint64_t> commit_sequence_opt) {
	std::uint64_t commit_sequence = commit_sequence_opt == std::nullopt
										? MAX_COMMIT_SEQUENCE
										: commit_sequence_opt.value();
	std::string begin_enc_key = InternalKey::encode(
		InternalKey(user_key_preffix, commit_sequence, Value));
	std::optional<BlockIterator> begin_iter_opt =
		lower_bound(begin_enc_key, commit_sequence, false);
	if (begin_iter_opt == std::nullopt) { return std::make_pair(end(), end()); }
	BlockIterator begin_it = begin_iter_opt.value();

	std::string next_str = get_next_str(user_key_preffix);
	if (next_str == "") {
		BlockIterator end_it = end();
		return std::make_pair(begin_it, end_it);
	}
	std::string end_enc_key =
		InternalKey::encode(InternalKey(next_str, commit_sequence, Value));
	std::optional<BlockIterator> end_iter_opt =
		lower_bound(end_enc_key, commit_sequence, false);
	if (end_iter_opt == std::nullopt) {
		return std::make_pair(begin_it, end());
	}
	BlockIterator end_it = end_iter_opt.value();
	return std::make_pair(begin_it, end_it);
}

std::optional<std::pair<BlockIterator, BlockIterator>>
Block::iter_monotony_predicate(
	std::function<int(const std::string &)> predicate,
	std::optional<std::uint64_t> commit_sequence) {
	// 封装一个比较enc_key的predicate
	std::function<int(const std::string &)> wrapper_predicate =
		[predicate](const std::string &enc_key) -> int {
		return predicate(InternalKey::decode(enc_key)._key);
	};

	std::size_t l = 0, r = _offsets.size();
	while (l < r) {
		std::size_t mid = (l + r) >> 1;
		std::string cur_enc_key = get_kv_at_offset(_data, _offsets[mid]).first;
		if (wrapper_predicate(cur_enc_key) < 0) {
			l = mid + 1;
		} else {
			r = mid;
		}
	}
	BlockIterator begin_it(shared_from_this(), l, commit_sequence, false);

	l = 0;
	r = _offsets.size();
	while (l < r) {
		std::size_t mid = (l + r) >> 1;
		std::string cur_enc_key = get_kv_at_offset(_data, _offsets[mid]).first;
		if (wrapper_predicate(cur_enc_key) <= 0) {
			l = mid + 1;
		} else {
			r = mid;
		}
	}
	BlockIterator end_it(shared_from_this(), l, commit_sequence, false);
	if (begin_it == end_it) { return std::nullopt; }
	return std::make_pair(begin_it, end_it);
}

bool Block::empty() { return _cur_size == 0; }

std::size_t Block::size() const { return _offsets.size(); }

std::size_t Block::data_size() const { return _cur_size; }

BlockIterator &BlockIterator::clean_by_commit_sequence() {
	if (_block == nullptr) { return *this; }
	while (_cur_idx < _block->_offsets.size() &&
		   _commit_sequence != std::nullopt &&
		   InternalKey::decode(Block::get_kv_at_offset(
								   _block->_data, _block->_offsets[_cur_idx])
								   .first)
				   ._commit_sequence > _commit_sequence) {
		_cur_idx++;
	}
	return *this;
}

BlockIterator &BlockIterator::operator++() {
	if (!is_valid()) { throw std::runtime_error("iter is invalid."); }
	std::string old_user_key =
		InternalKey::decode(
			_block->get_kv_at_offset(_block->_data, _block->_offsets[_cur_idx])
				.first)
			._key;
	_cur_idx++;
	if (!_keep_all_version) {
		while (_cur_idx < _block->_offsets.size() &&
			   InternalKey::decode(
				   _block
					   ->get_kv_at_offset(_block->_data,
										  _block->_offsets[_cur_idx])
					   .first)
					   ._key == old_user_key) {
			_cur_idx++;
		}
	}
	if (_commit_sequence != std::nullopt) { clean_by_commit_sequence(); }
	return *this;
}

std::pair<std::string, std::string> BlockIterator::operator*() const {
	if (!is_valid()) { throw std::runtime_error("iter is invalid."); }
	return Block::get_kv_at_offset(_block->_data, _block->_offsets[_cur_idx]);
}

bool BlockIterator::operator==(const BlockIterator &other) const {
	return _block == other._block && _cur_idx == other._cur_idx;
}

bool BlockIterator::operator!=(const BlockIterator &other) const {
	return !(*this == other);
}

std::optional<std::uint64_t> BlockIterator::get_commit_sequence() {
	return _commit_sequence;
}
bool BlockIterator::get_keep_all_version() { return _keep_all_version; }

bool BlockIterator::is_valid() const {
	if (_block == nullptr) { return false; }
	return _cur_idx < _block->_offsets.size();
}

std::size_t BlockIterator::get_cur_idx() const { return _cur_idx; }

BlockIterator BlockIterator::invalid_iter() {
	return BlockIterator(nullptr, 0, std::nullopt, false);
}