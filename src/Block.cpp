#include "Block.h"
#include <cassert>
#include <stdexcept>
#include <zlib.h>

Block::Block()
	: _cur_size(0), _comparator(std::make_shared<InternalKeyComparator>()) {}

Block::Block(std::vector<uint8_t> data, std::vector<uint16_t> offsets,
			 std::size_t cur_size)
	: _data(data), _offsets(offsets), _cur_size(cur_size),
	  _comparator(std::make_shared<InternalKeyComparator>()) {}

std::vector<std::uint8_t> Block::get_bytes(const std::string &enc_key,
										   const std::string &value) {
	std::vector<std::uint8_t> bytes;
	std::uint16_t key_size = enc_key.size(), val_size = value.size();
	for (int i = 0; i < 2; i++) {
		bytes.push_back((key_size >> (8 - i * 8)) &
						static_cast<std::uint16_t>(0xff));
	}
	for (int i = 0; i < key_size; i++) {
		bytes.push_back(enc_key[i]);
	}
	for (int i = 0; i < 2; i++) {
		bytes.push_back((val_size >> (8 - i * 8)) &
						static_cast<std::uint16_t>(0xff));
	}
	for (int i = 0; i < val_size; i++) {
		bytes.push_back(value[i]);
	}
	return bytes;
}


/**
每个Entry: 2B key_len + enc_key + 2B val_len + value
offset: 2B
*/
bool Block::add_entry(const std::string &enc_key, const std::string &value,
					  bool force) {
	std::size_t data_size = DEFAULT_KEY_LEN_SIZE + enc_key.size() +
							DEFAULT_VALUE_LEN_SIZE + value.size();
	if (_cur_size + data_size > DEFAULT_MAX_CAPACITY && !force) {
		return false;
	}
	_offsets.push_back(_data.size());
	std::vector<std::uint8_t> bytes = get_bytes(enc_key, value);
	_data.insert(_data.end(), bytes.begin(), bytes.end());
	_cur_size += data_size;
	return true;
}

std::vector<uint8_t> Block::get_bytes_of_uint16t(std::uint16_t num) {
	std::vector<uint8_t> ret;
	for (int i = 0; i < 2; i++) {
		ret.push_back((num >> (8 - i * 8)) & static_cast<std::uint16_t>(0xff));
	}
	return ret;
}

std::vector<uint8_t> Block::get_bytes_of_uint32t(std::uint32_t num) {
	std::vector<uint8_t> ret;
	for (int i = 0; i < 4; i++) {
		ret.push_back((num >> (8 * 3 - i * 8)) &
					  static_cast<std::uint16_t>(0xff));
	}
	return ret;
}

/**
data + offset (each 2B) + num_of_elements (2B) + CRC (4B) (with_hash = true)
*/
std::vector<std::uint8_t> Block::encode(bool with_crc) {
	std::vector<std::uint8_t> res(_data.begin(), _data.end());
	for (std::uint16_t offset : _offsets) {
		std::vector<std::uint8_t> offset_bytes = get_bytes_of_uint16t(offset);
		res.insert(res.end(), offset_bytes.begin(), offset_bytes.end());
	}
	std::uint16_t num_of_elements = _offsets.size();
	std::vector<uint8_t> num_of_elements_bytes =
		get_bytes_of_uint16t(num_of_elements);
	res.insert(res.end(), num_of_elements_bytes.begin(),
			   num_of_elements_bytes.end());
	if (with_crc) {
		uint32_t crc = crc32(0, res.data(), res.size());
		std::vector<uint8_t> crc_bytes = get_bytes_of_uint32t(crc);
		res.insert(res.end(), crc_bytes.begin(), crc_bytes.end());
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
		uint32_t crc = 0;
		for (int i = enc_data.size() - 4; i < enc_data.size(); i++) {
			crc = ((crc << 8) | enc_data[i]);
		}

		// check
		std::vector<std::uint8_t> enc_data_without_crc(enc_data.begin(),
													   enc_data.end() - 4);
		uint32_t cur_crc =
			crc32(0, enc_data_without_crc.data(), enc_data_without_crc.size());
		if (cur_crc != crc) { return nullptr; }
	}

	std::size_t tail_bytes = with_crc ? 4 : 0;

	uint16_t num_of_elements = 0;
	for (int i = enc_data.size() - 2 - tail_bytes;
		 i < enc_data.size() - tail_bytes; i++) {
		num_of_elements = ((num_of_elements << 8) | enc_data[i]);
	}
	std::vector<std::uint16_t> offsets;
	for (int i = enc_data.size() - 2 - tail_bytes - num_of_elements * 2;
		 i < enc_data.size() - 2 - tail_bytes; i += 2) {
		std::uint16_t offset = 0;
		for (int j = 0; j < 2; j++) {
			offset = ((offset << 8) | enc_data[i + j]);
		}
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
	std::uint16_t key_len = 0;
	for (int i = 0; i < 2; i++) {
		key_len = ((key_len << 8) | data[offset + i]);
	}
	auto key_begin = data.begin() + offset + 2;
	std::string key(key_begin, key_begin + key_len);
	std::uint16_t val_len = 0;
	for (int i = 0; i < 2; i++) {
		val_len = ((val_len << 8) | data[offset + 2 + key_len + i]);
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
	std::string user_key = InternalKey::decode(enc_key)._key;
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
	std::pair<std::string, std::string> kv =
		get_kv_at_offset(_data, _offsets[l]);
	std::string find_user_key = InternalKey::decode(kv.first)._key;
	if (find_user_key != user_key) { return std::nullopt; }
	return l;
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
							 commit_sequence);
	}
}

/**
获取user_key完全匹配, commit_sequence小于等于给定commit_sequence的结果
*/
std::optional<BlockIterator>
Block::get_lower_bound(const std::string &enc_key) {
	std::uint64_t commit_sequence =
		InternalKey::decode(enc_key)._commit_sequence;
	std::optional<std::size_t> data_idx_opt =
		get_idx_by_enc_key_lower_bound(enc_key);
	if (data_idx_opt == std::nullopt) {
		return std::nullopt;
	} else {
		return BlockIterator(shared_from_this(), data_idx_opt.value(),
							 commit_sequence);
	}
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

BlockIterator Block::end() {
	return BlockIterator(shared_from_this(), _offsets.size(), std::nullopt,
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
										? ((1ULL << 56) - 1)
										: commit_sequence_opt.value();
	std::string begin_enc_key = InternalKey::encode(
		InternalKey(user_key_preffix, commit_sequence, Value));

	std::size_t l = 0,
				r = _offsets.size(); // enc_end_key可能超过了当前key的范围,
									 // 因此r有可能在offsets的下标范围之外
	while (l < r) {
		std::size_t mid = (l + r) >> 1;
		std::string cur_key = get_kv_at_offset(_data, _offsets[mid]).first;
		if ((*_comparator)(cur_key, begin_enc_key) < 0) {
			l = mid + 1;
		} else {
			r = mid;
		}
	}
	BlockIterator begin_it(shared_from_this(), l, commit_sequence_opt);
	std::string next_str = get_next_str(user_key_preffix);
	if (next_str == "") {
		BlockIterator end_it = end();
		return std::make_pair(begin_it, end_it);
	}
	std::string end_enc_key =
		InternalKey::encode(InternalKey(next_str, commit_sequence, Value));
	l = 0, r = _offsets.size(); // enc_end_key可能超过了当前key的范围,
								// 因此r有可能在offsets的下标范围之外
	while (l < r) {
		std::size_t mid = (l + r) >> 1;
		std::string cur_key = get_kv_at_offset(_data, _offsets[mid]).first;
		if ((*_comparator)(cur_key, end_enc_key) < 0) {
			l = mid + 1;
		} else {
			r = mid;
		}
	}
	BlockIterator end_it(shared_from_this(), l, commit_sequence_opt);
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
	BlockIterator begin_it(shared_from_this(), l, commit_sequence);

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
	BlockIterator end_it(shared_from_this(), l, commit_sequence);
	if (begin_it == end_it) { return std::nullopt; }
	return std::make_pair(begin_it, end_it);
}

std::string Block::get_first_key() {
	assert(_offsets.size() > 0);
	return get_kv_at_offset(_data, _offsets[0]).first;
}

bool Block::is_first_key() { return _offsets.size() == 1; }

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
	return BlockIterator(nullptr, 0, std::nullopt);
}