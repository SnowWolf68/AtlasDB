#include "sst.h"
#include "BlockMeta.h"
#include "Record.h"
#include "codec.h"
#include "io/FileSystem.h"
#include "io/FileUtil.h"
#include "log.h"
#include "util.h"

#include <cassert>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <utility>

bool SSTBuilder::flush_block(bool with_crc) {
	if (_block.empty()) { return false; }
	std::vector<std::uint8_t> block_data = _block.encode(with_crc);
	_block_metas.push_back(BlockMeta(_data.size(), _first_key, _last_key));
	_data.insert(_data.end(), block_data.begin(), block_data.end());
	_block = Block();
	_first_key = "";
	_last_key = "";
	return true;
}

bool SSTBuilder::add(const std::string &user_key, const std::string &value,
					 std::uint64_t commit_sequence, ValueType value_type) {
	std::string enc_key =
		InternalKey::encode(InternalKey(user_key, commit_sequence, value_type));
	if (_block.add_entry(enc_key, value,
						 false)) { // 先判断如果加入之后容量超了, 就不add
		if (_block.size() == 1) { _first_key = enc_key; }
		_last_key = enc_key;
		return true;
	}
	if (flush_block(true)) {
		_first_key = enc_key;
		_last_key = enc_key;
		bool res = _block.add_entry(enc_key, value, true);
		return res;
	} else { // 单条记录大小超过了Block的阈值, 需要先force add然后立即刷新flush
		bool res = _block.add_entry(enc_key, value, true);
		if (!res) { return false; }
		_first_key = enc_key;
		_last_key = enc_key;
		return flush_block(true);
	}
	return false;
}

bool SSTBuilder::add(const std::string &enc_key, const std::string &value) {
	InternalKey intr_key = InternalKey::decode(enc_key);
	return add(intr_key._key, value, intr_key._commit_sequence,
			   intr_key._value_type);
}

/**
Block数据 + Meta Section + Meta Offset (4B) + Magic number (1B)
*/
std::shared_ptr<SST> SSTBuilder::build(std::uint64_t sst_id,
									   const std::string &path) {
	if (!_block.empty()) { flush_block(true); }
	if (_block_metas.empty()) {
		LOG(Log::WARNING) << "build failed, block_metas is null";
		return nullptr;
	}
	std::vector<std::uint8_t> write_data(_data);
	std::vector<std::uint8_t> meta_section_data =
		BlockMeta::encode(_block_metas);
	std::uint32_t meta_section_offset = write_data.size();
	write_data.insert(write_data.end(), meta_section_data.begin(),
					  meta_section_data.end());
	codec::append_u32_be(write_data, meta_section_offset);
	write_data.push_back(MAGIC_NUM); // write_data构造完成

	atlas::io::OpenOptions open_options;
	open_options._access_mode = atlas::io::AccessMode::ReadWrite;
	open_options._create_mode = atlas::io::CreateMode::OpenOrCreate;
	open_options._truncate = true;
	atlas::io::FileObj file = atlas::io::FileSystem::open(path, open_options);
	atlas::io::atomic_write_all_replace(file, write_data);
	open_options._access_mode = atlas::io::AccessMode::ReadOnly;
	open_options._create_mode = atlas::io::CreateMode::OpenExisting;
	open_options._truncate = false;
	file = atlas::io::FileSystem::open(
		path, open_options); // rename之后文件fd换了, 需要重新打开绑定
	std::string first_key = _block_metas[0]._first_key;
	std::string last_key = _block_metas[_block_metas.size() - 1]._last_key;
	return std::make_shared<SST>(std::move(file), _block_metas,
								 meta_section_offset, sst_id, first_key,
								 last_key);
}

std::size_t SSTBuilder::get_estimated_size() {
	return _data.size() + _block.data_size();
}

bool SSTBuilder::empty() { return get_estimated_size() == 0; }

SST::SST(atlas::io::FileObj file, std::vector<BlockMeta> block_metas,
		 std::uint32_t meta_section_offset, std::uint64_t sst_id,
		 const std::string &first_key, const std::string &last_key)
	: _file(std::move(file)), _block_metas(block_metas),
	  _meta_section_offset(meta_section_offset), _sst_id(sst_id),
	  _first_key(first_key), _last_key(last_key),
	  _comparator(std::make_shared<InternalKeyComparator>()) {}

/**
Block数据 + Meta Section + Meta Offset (4B) + Magic number (1B)
*/
std::shared_ptr<SST> SST::open(const std::string &path, std::uint64_t sst_id) {
	try { // read相关逻辑可能抛异常, 这里捕获一下, 如果有异常直接返回nullptr
		atlas::io::OpenOptions open_options;
		open_options._create_mode = atlas::io::CreateMode::OpenExisting;
		open_options._access_mode = atlas::io::AccessMode::ReadOnly;
		open_options._truncate = false;
		atlas::io::FileObj file =
			atlas::io::FileSystem::open(path, open_options);
		std::size_t file_size = file.size();
		if (file_size < 5) {
			LOG(Log::ERROR) << "SST file: " << path << " not valid";
			return nullptr;
		}
		std::vector<std::uint8_t> data(1);
		std::size_t read_size = file.read_at(file_size - 1, data);
		if (read_size != 1) {
			ERROR_LOG << "magic num read failed";
			return nullptr;
		}
		std::optional<std::uint8_t> magic_num_opt = codec::read_u8(data, 0);
		if (magic_num_opt == std::nullopt) {
			ERROR_LOG << "magic num read failed";
			return nullptr;
		}
		std::uint8_t magic_num = magic_num_opt.value();
		if (magic_num != SSTBuilder::MAGIC_NUM) {
			LOG(Log::ERROR)
				<< "magic num error, magic num: " << std::to_string(magic_num);
			return nullptr;
		}
		data.resize(4);
		read_size = file.read_at(file_size - 5, data);
		if (read_size != data.size()) {
			ERROR_LOG << "meta section offset data missing";
			return nullptr;
		}
		std::optional<std::uint32_t> meta_section_offset_opt =
			codec::read_u32_be(data, 0);
		if (meta_section_offset_opt == std::nullopt) {
			ERROR_LOG << "meta section offset decode failed";
			return nullptr;
		}
		std::uint32_t meta_section_offset = meta_section_offset_opt.value();
		if (meta_section_offset > file_size - 5) { return nullptr; }
		std::vector<std::uint8_t> block_meta_data(file_size -
												  meta_section_offset - 5);
		read_size = file.read_at(meta_section_offset, block_meta_data);
		if (read_size != block_meta_data.size()) {
			ERROR_LOG << "block meta data missing";
			return nullptr;
		}
		std::optional<std::vector<BlockMeta>> block_meta_optional =
			BlockMeta::decode(block_meta_data);
		if (block_meta_optional == std::nullopt) { return nullptr; }
		std::vector<BlockMeta> block_metas = block_meta_optional.value();
		if (block_metas.empty()) { return nullptr; }
		std::string first_key = block_meta_optional.value()[0]._first_key;
		std::string last_key =
			block_meta_optional.value()[block_meta_optional.value().size() - 1]
				._last_key;
		return std::make_shared<SST>(std::move(file), block_metas,
									 meta_section_offset, sst_id, first_key,
									 last_key);
	} catch (const std::exception &e) {
		LOG(Log::ERROR) << e.what();
		return nullptr;
	}
}

std::shared_ptr<Block> SST::read_block(std::size_t block_idx) const {
	try {
		if (block_idx >= _block_metas.size()) {
			LOG(Log::ERROR) << "read block idx more than block count";
			return nullptr;
		}
		std::uint32_t block_start_offset = _block_metas[block_idx]._offset;
		std::uint32_t block_end_offset =
			block_idx == _block_metas.size() - 1
				? _meta_section_offset
				: _block_metas[block_idx + 1]._offset;
		if (block_start_offset > _file.size() ||
			block_end_offset > _file.size() ||
			block_start_offset > block_end_offset ||
			block_end_offset > _meta_section_offset) {
			return nullptr;
		}

		std::vector<std::uint8_t> block_data(block_end_offset -
											 block_start_offset);
		std::size_t read_size = _file.read_at(block_start_offset, block_data);
		if (read_size != block_data.size()) {
			ERROR_LOG << "block data missing";
			return nullptr;
		}
		return Block::decode(block_data, true);
	} catch (const std::exception &e) {
		LOG(Log::ERROR) << e.what();
		return nullptr;
	}
}

std::optional<std::uint64_t>
SST::locate_block(const std::string &enc_key) const {
	std::size_t l = 0, r = _block_metas.size();
	while (l < r) {
		std::size_t mid = (l + r) >> 1;
		std::string cur_first_key = _block_metas[mid]._first_key,
					cur_last_key = _block_metas[mid]._last_key;
		// 找第一个 >= enc_key 的 last_key
		if ((*_comparator)(cur_last_key, enc_key) >= 0) {
			r = mid;
		} else {
			l = mid + 1;
		}
	}
	if (l != _block_metas.size()) { return l; }
	return std::nullopt;
}

std::optional<SSTIterator> SST::get(const std::string &enc_key) const {
	std::optional<std::uint64_t> block_idx_opt = locate_block(enc_key);
	if (block_idx_opt == std::nullopt) { return std::nullopt; }
	std::shared_ptr<Block> block = read_block(block_idx_opt.value());
	if (block == nullptr) { return std::nullopt; }
	std::optional<BlockIterator> block_iter_opt =
		block->lower_bound_match_user_key(enc_key, false);
	if (block_iter_opt == std::nullopt) { return std::nullopt; }
	return SSTIterator(shared_from_this(), block_idx_opt.value(),
					   block_iter_opt.value());
}

std::size_t SST::num_blocks() const { return _block_metas.size(); }
std::string SST::get_first_key() const { return _first_key; }
std::string SST::get_last_key() const { return _last_key; }
std::uint64_t SST::get_sst_id() const { return _sst_id; }

SSTIterator SST::begin(std::optional<std::uint64_t> commit_sequence,
					   bool keep_all_version) const {

	int block_idx = 0;
	SSTIterator sst_iter = SSTIterator::invalid_iter();
	while (block_idx < _block_metas.size()) {
		std::shared_ptr<Block> block = read_block(block_idx);
		if (block == nullptr) {
			return SSTIterator(shared_from_this(), num_blocks(),
							   BlockIterator::invalid_iter());
		}
		sst_iter = SSTIterator(shared_from_this(), block_idx,
							   block->begin(commit_sequence, keep_all_version));
		if (sst_iter.is_valid()) { break; }
		block_idx++;
	}
	if (!sst_iter.is_valid()) { sst_iter = end(); }
	return sst_iter;
}

SSTIterator SST::end() const {
	std::shared_ptr<Block> last_block = read_block(_block_metas.size() - 1);
	if (last_block == nullptr) {
		return SSTIterator(shared_from_this(), num_blocks(),
						   BlockIterator::invalid_iter());
	}
	return SSTIterator(shared_from_this(), num_blocks(), last_block->end());
}

void SSTIterator::check_valid_or_switch_to_next_block(
	std::optional<std::uint64_t> commit_sequence, bool keep_all_version) {
	while (!_block_iter.is_valid()) {
		// 下一个block
		if (_block_idx >= _sst->num_blocks()) {
			break;
		} // 如果当前已经是尾后, 直接返回
		_block_idx++;
		if (_block_idx >= _sst->num_blocks()) {
			break;
		} // 如果++后是尾后, 也直接返回
		std::shared_ptr<Block> block = _sst->read_block(
			_block_idx); // 可能有块但是块坏了, 坏块同样可能导致返回nullptr
		if (block == nullptr) {
			// 如果遇到坏块, 直接跳过到下一块, 构造一个invalid的iter,
			// 继续循环即可
			_block_iter = BlockIterator::invalid_iter();
			continue;
		}
		_block_iter = block->begin(commit_sequence, keep_all_version);
	}
}

/**
 * BlockIterator里的clean_by_commit_sequence只能清理某一个Block里的, 不能跨Block
 * 因此这里需要用这个东西实现跨Block的清理
 */
void SSTIterator::fresh_by_comseq() {
	std::optional<std::uint64_t> commit_sequence =
		_block_iter.get_commit_sequence();
	bool keep_all_version = _block_iter.get_keep_all_version();

	// 注意: 此时this (SSTIterator) 可能是 无效 状态
	if (!is_valid()) {
		if (_block_idx >= _sst->num_blocks()) {
			return;
		} else {
			++*this;
		}
	}
	_block_iter
		.clean_by_commit_sequence(); // 这个东西清完有可能搞出来无效的iter
	check_valid_or_switch_to_next_block(commit_sequence, keep_all_version);
}

std::optional<std::pair<SSTIterator, SSTIterator>>
SST::iter_monotony_predicate(std::function<int(const std::string &)> predicate,
							 std::optional<std::uint64_t> commit_sequence) {
	// 封装一个比较enc_key的predicate
	std::function<int(const std::string &)> wrapper_predicate =
		[predicate](const std::string &enc_key) -> int {
		return predicate(InternalKey::decode(enc_key)._key);
	};
	std::size_t l = 0, r = _block_metas.size();
	while (l < r) {
		std::size_t mid = (l + r) >> 1; // mid不可能取到r, 也就不可能越界
		if (wrapper_predicate(_block_metas[mid]._last_key) < 0) {
			l = mid + 1;
		} else {
			r = mid;
		}
	}
	std::shared_ptr<Block> blk = read_block(l); // 如果遇到坏块, 这里可能为空
	while (blk == nullptr) {
		l++;
		// 但是这里的l == r可能越界
		if (l >= _block_metas.size()) {
			// 如果begin_sst_it都越界了, 说明没有区间
			return std::nullopt;
		}
		blk = read_block(l);
	}

	// 默认情况下, 如果当前l块不符合要求, 那么返回end(),
	// 后续往后找第一个符合要求的
	SSTIterator sst_begin_iter(shared_from_this(), l,
							   blk->end(commit_sequence));

	std::optional<std::pair<BlockIterator, BlockIterator>>
		begin_block_predicate_res =
			blk->iter_monotony_predicate(predicate, commit_sequence);
	if (begin_block_predicate_res != std::nullopt) {
		sst_begin_iter = SSTIterator(shared_from_this(), l,
									 begin_block_predicate_res->first);
	}
	sst_begin_iter.fresh_by_comseq();
	if (!sst_begin_iter.is_valid() ||
		wrapper_predicate((*sst_begin_iter).first) != 0) {
		return std::nullopt;
	}

	l = 0;
	r = _block_metas.size();
	while (l < r) {
		std::size_t mid = (l + r) >> 1;
		if (wrapper_predicate(_block_metas[mid]._last_key) > 0) {
			r = mid;
		} else {
			l = mid + 1;
		}
	}
	// 读第一个predicate(first_key) > 0的block
	blk = read_block(l); // 如果遇到坏块, 这里可能为空
	while (blk == nullptr) {
		l++;
		// 但是这里的l == r可能越界
		if (l >= _block_metas.size()) {
			// 如果end_sst_it越界了, 说明(-1, 0, 1)中, 没有(1)的部分
			SSTIterator end_sst_iter = end();
			return std::make_pair(sst_begin_iter, end_sst_iter);
		}
		blk = read_block(l);
	}

	SSTIterator sst_end_iter(shared_from_this(), l,
							 blk->begin(commit_sequence, false));

	std::optional<std::pair<BlockIterator, BlockIterator>>
		end_block_predicate_res =
			blk->iter_monotony_predicate(predicate, commit_sequence);
	if (end_block_predicate_res != std::nullopt) {
		sst_end_iter =
			SSTIterator(shared_from_this(), l, end_block_predicate_res->second);
		if (!sst_end_iter.is_valid()) { ++sst_end_iter; }
	}

	sst_end_iter.fresh_by_comseq();

	if (sst_begin_iter == sst_end_iter) { return std::nullopt; }
	return std::make_pair(sst_begin_iter, sst_end_iter);
}

std::shared_ptr<Comparator<std::string>> SST::get_comparator() const {
	return _comparator;
}

bool SST::delete_file() {
	std::string file_path = _file.get_file_path();
	return atlas::io::FileSystem::remove_file(file_path);
}

SSTIterator::SSTIterator(std::shared_ptr<const SST> sst, std::size_t block_idx,
						 BlockIterator block_iter)
	: _sst(sst), _block_idx(block_idx), _block_iter(block_iter) {}

SSTIterator &SSTIterator::operator++() {
	std::optional<std::uint64_t> commit_sequence =
		_block_iter.get_commit_sequence();
	bool keep_all_version = _block_iter.get_keep_all_version();

	if (_block_iter.is_valid()) {
		// 如果一开始block_iter是有效状态, 才考虑++, 否则直接切换到下一块
		++_block_iter;
		if (_block_iter.is_valid()) { return *this; }
	}
	// fresh_by_comseq();		// 这里不能直接调用这个东西,
	// 因为fresh_by_comseq里面也会调用operator++, 这样会造成无限递归
	check_valid_or_switch_to_next_block(commit_sequence, keep_all_version);
	return *this;
}
std::pair<std::string, std::string> SSTIterator::operator*() const {
	if (!is_valid()) { throw std::runtime_error("block_iter is invalid."); }
	return *_block_iter;
}
bool SSTIterator::operator==(const SSTIterator &other) const {
	return _sst == other._sst && _block_idx == other._block_idx &&
		   (_block_idx == _sst->num_blocks() ||
			_block_iter.get_cur_idx() == other._block_iter.get_cur_idx());
}
bool SSTIterator::operator!=(const SSTIterator &other) const {
	return !(*this == other);
}
bool SSTIterator::is_valid() const {
	return _block_iter.is_valid() && _block_idx < _sst->num_blocks();
}
SSTIterator SSTIterator::invalid_iter() {
	return SSTIterator(nullptr, 0, BlockIterator::invalid_iter());
}