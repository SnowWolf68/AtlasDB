#include "lsm.h"
#include "BastIterator.h"
#include "Block.h"
#include "Comparator.h"
#include "MemTable.h"
#include "Record.h"
#include "sst.h"
#include "util.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <vector>

LSMEngine::LSMEngine(const std::string &data_dir, std::size_t max_memtable_size,
					 std::size_t max_active_size, std::size_t compact_threshold,
					 std::size_t max_l0_sst_size)
	: _data_dir(data_dir), _memtable(max_active_size), _next_sst_id(0),
	  _cur_max_level(0), _max_memtable_size(max_memtable_size),
	  _compact_threshold(compact_threshold), _max_l0_sst_size(max_l0_sst_size),
	  _close(false) {
	std::filesystem::create_directories(_data_dir);
	// 扫描data_dir中的文件, 将数据恢复到sst中
	bool found = false;
	for (auto item : std::filesystem::directory_iterator(_data_dir)) {
		if (item.is_regular_file()) {
			std::optional<std::pair<std::size_t, std::size_t>> sst_info =
				decode_sst_path(
					item.path()); // std::filesystem::path可以隐式类型转换到std::string
			if (sst_info == std::nullopt) {
				// 解析有问题, 文件名有错误, 跳过, 继续解析下一个文件
				continue;
			}
			std::size_t sst_id = sst_info->first, level = sst_info->second;
			std::shared_ptr<SST> sst = SST::open(item.path(), sst_id);
			if (sst == nullptr) { continue; }
			std::unique_lock<std::shared_mutex> unique(_ssts_mutex);
			_sst_id_map[level].push_back(sst_id);
			_ssts[sst_id] = sst;
			_cur_max_level = std::max(_cur_max_level, level);
			_next_sst_id = std::max(_next_sst_id, sst_id);
			found = true;
		}
	}
	std::unique_lock<std::shared_mutex> unique(_ssts_mutex);
	if (found) { _next_sst_id++; }
	for (auto &[level, ids] : _sst_id_map) {
		if (level == 0) {
			std::sort(ids.begin(), ids.end(),
					  [](std::size_t a, std::size_t b) { return a > b; });
		} else {
			std::sort(ids.begin(), ids.end(),
					  [](std::size_t a, std::size_t b) { return a < b; });
		}
	}
	_com_seq_filepath = _data_dir + "/" + DEFAULT_COM_SEQ_FILENAME;
	if (std::filesystem::exists(_com_seq_filepath)) {
		FileObj seq_file = FileObj::open_read(_com_seq_filepath);
		std::uint64_t commit_sequence = seq_file.read_uint64(0);
		_commit_sequence.store(commit_sequence);
	} else {
		std::uint64_t init_seq = 1;
		std::vector<std::uint8_t> data = get_bytes_of_uint64t(init_seq);
		FileObj seq_file = FileObj::create_and_write(_com_seq_filepath, data);
		_commit_sequence.store(1);
	}
}

std::string LSMEngine::get_sst_path(std::size_t id, std::size_t level) {
	std::string id_str = std::to_string(id);
	std::size_t cur_len = id_str.size();
	for (int i = 0; i < 32 - cur_len; i++) {
		id_str = "0" + id_str;
	}
	return _data_dir + "/sst_" + id_str + "." + std::to_string(level);
}

// <id, level>
std::optional<std::pair<std::size_t, std::size_t>>
LSMEngine::decode_sst_path(const std::string &path_str) {
	std::filesystem::path path = path_str;
	std::string filename = path.filename();
	std::size_t find_pos = filename.find(".");
	if (find_pos == std::string::npos) { return std::nullopt; }
	try {
		std::string level_str = filename.substr(find_pos + 1),
					id_str = filename.substr(4, find_pos);
		std::size_t level = std::stoull(level_str),
					sst_id = std::stoull(id_str);
		return std::make_pair(sst_id, level);
	} catch (const std::exception &e) { return std::nullopt; }
}

/**
把 MemTable 中最老的一张表（frozen 队首）刷成磁盘上的一个 SST 文件，
并把新 SST 登记进引擎索引
	-- return:
		max_commit_sequence: 本次刷出的 SST 里最大 seq
*/
std::uint64_t LSMEngine::flush() {
	std::vector<std::uint8_t> data =
		get_bytes_of_uint64t(_commit_sequence.load());
	FileObj::create_and_write(_com_seq_filepath, data);

	SSTBuilder builder;
	std::vector<std::uint64_t> flushed_seqs;
	std::size_t cur_sst_id;
	std::unique_lock<std::shared_mutex> unique(_ssts_mutex);
	cur_sst_id = _next_sst_id;
	_next_sst_id++;
	std::shared_ptr<SST> sst = _memtable.flush_last(
		builder, get_sst_path(cur_sst_id, 0), cur_sst_id, flushed_seqs);
	if (sst == nullptr) { return 0; }
	_sst_id_map[0].push_front(cur_sst_id);
	_ssts[cur_sst_id] = sst;
	std::uint64_t max_commit_sequence = 0;
	for (auto x : flushed_seqs) {
		max_commit_sequence = std::max(max_commit_sequence, x);
	}
	if (_sst_id_map[0].size() >= _compact_threshold) { compact(); }
	return max_commit_sequence;
}

/**
	-- return:
		空批: 当前水位
		nullopt = 引擎已关闭拒绝
		max_commit_sequence: 本次写的 seq / 整批 max
*/
std::optional<std::uint64_t> LSMEngine::put(const std::string &key,
											const std::string &value,
											std::uint64_t commit_sequence) {
	{
		std::shared_lock<std::shared_mutex> shared(_close_mutex);
		if (_close) { return std::nullopt; }
		_memtable.put(key, value, commit_sequence);
	}
	if (_memtable.get_total_size() >= _max_memtable_size) { flush(); }
	return commit_sequence;
}

/**
	-- return:
		空批: 当前水位
		nullopt = 引擎已关闭拒绝
		max_commit_sequence: 本次写的 seq / 整批 max
*/
std::optional<std::uint64_t> LSMEngine::remove(const std::string &key,
											   std::uint64_t commit_sequence) {
	{
		std::shared_lock<std::shared_mutex> shared(_close_mutex);
		if (_close) { return std::nullopt; }
		_memtable.remove(key, commit_sequence);
	}
	if (_memtable.get_total_size() >= _max_memtable_size) { flush(); }
	return commit_sequence;
}

/**
	-- return:
		空批: 当前水位
		nullopt = 引擎已关闭拒绝
		max_commit_sequence: 本次写的 seq / 整批 max
*/
std::optional<std::uint64_t>
LSMEngine::put_batch(std::vector<std::pair<std::string, std::string>> kvs,
					 std::vector<std::uint64_t> commit_sequences) {
	{
		std::shared_lock<std::shared_mutex> shared(_close_mutex);
		if (_close) { return std::nullopt; }
		_memtable.put_batch(kvs, commit_sequences);
	}
	if (_memtable.get_total_size() >= _max_memtable_size) { flush(); }
	std::uint64_t max_seq = 0;
	for (auto x : commit_sequences) {
		max_seq = std::max(max_seq, x);
	}
	if (max_seq == 0) { return _commit_sequence.load(); }
	return max_seq;
}

/**
	-- return:
		空批: 当前水位
		nullopt = 引擎已关闭拒绝
		max_commit_sequence: 本次写的 seq / 整批 max
*/
std::optional<std::uint64_t>
LSMEngine::remove_batch(std::vector<std::string> ks,
						std::vector<std::uint64_t> commit_sequences) {
	{
		std::shared_lock<std::shared_mutex> shared(_close_mutex);
		if (_close) { return std::nullopt; }
		_memtable.remove_batch(ks, commit_sequences);
	}
	if (_memtable.get_total_size() >= _max_memtable_size) { flush(); }
	std::uint64_t max_seq = 0;
	for (auto x : commit_sequences) {
		max_seq = std::max(max_seq, x);
	}
	if (max_seq == 0) { return _commit_sequence.load(); }
	return max_seq;
}
/**
	-- return:
		空批: 当前水位
		nullopt = 引擎已关闭拒绝
		max_commit_sequence: 本次写的 seq / 整批 max
*/
std::optional<std::uint64_t> LSMEngine::put(const std::string &key,
											const std::string &value) {
	if (value.size() > Block::MAX_VALUE_LEN - 1) { return std::nullopt; }
	std::uint64_t commit_sequence = _commit_sequence.fetch_add(1);
	return put(key, value, commit_sequence);
}
/**
	-- return:
		空批: 当前水位
		nullopt = 引擎已关闭拒绝
		max_commit_sequence: 本次写的 seq / 整批 max
*/
std::optional<std::uint64_t> LSMEngine::remove(const std::string &key) {
	std::uint64_t commit_sequence = _commit_sequence.fetch_add(1);
	return remove(key, commit_sequence);
}
/**
	-- return:
		空批: 当前水位
		nullopt = 引擎已关闭拒绝
		max_commit_sequence: 本次写的 seq / 整批 max
*/
std::optional<std::uint64_t>
LSMEngine::put_batch(std::vector<std::pair<std::string, std::string>> kvs) {
	for (std::size_t i = 0; i < kvs.size(); i++) {
		if (kvs[i].second.size() > Block::MAX_VALUE_LEN - 1) {
			return std::nullopt;
		}
	}
	std::vector<std::uint64_t> seqs;
	for (std::size_t i = 0; i < kvs.size(); i++) {
		seqs.push_back(_commit_sequence.fetch_add(1));
	}
	return put_batch(kvs, seqs);
}
/**
	-- return:
		空批: 当前水位
		nullopt = 引擎已关闭拒绝
		max_commit_sequence: 本次写的 seq / 整批 max
*/
std::optional<std::uint64_t>
LSMEngine::remove_batch(std::vector<std::string> ks) {
	std::vector<std::uint64_t> seqs;
	for (std::size_t i = 0; i < ks.size(); i++) {
		seqs.push_back(_commit_sequence.fetch_add(1));
	}
	return remove_batch(ks, seqs);
}

/**
查询逻辑（整体按"新 → 旧"顺序）

1. memtable：调 _memtable.get(key, commit_sequence) 一次即可，
   内部已按 active → frozen(新→旧) 顺序查好；命中则返回（含 TombStone）。

2. L0：_sst_id_map[0] 按 id 降序（front = 最新），逐表查：
   命中即返回——第一个命中的就是最新版本，继续查只会读到旧值；
   命中 TombStone 同样立即返回（最新状态 = 已删除）。

3. L1+：for level = 1 .. _cur_max_level，逐层：
   ── key 有序从哪来：compact（4.5）按 key 全局排序后切成互不重叠的
	  key 段，每段一个 SST，并按键序依次分配 id——
	  因此"同层 SST 的 key 段有序且互不重叠"与"id 升序 = key 段顺序"
	  是 compact 保证的不变量（flush 只产生重叠的 L0，不维护它）。
	  当前（4.2）尚无 compact，L1+ 为空，逻辑按不变量已成立编写。
   ── 二分：对 sst_id 列表二分（id 升序即 key 段有序），
	  定位唯一可能含 key 的表（first_key <= key <= last_key），
	  表内查一次：命中返回；候选表内无 → break，进下一层。

4. TombStone 短路（全链路）：任一环节命中墓碑立即返回"已删除"，
   不再往下查——旧层即使有活值，也被该删除覆盖，查了也不能返回。

全部未命中 → 返回 NotFound。

-- param:
	commit_sequence: 快照点,
		返回 seq ≤ commit_sequence 的最新版本"，不是精确匹配
 */
LookUpResult LSMEngine::get(const std::string &user_key,
							std::uint64_t commit_sequence) {
	LookUpResult res = _memtable.get(user_key, commit_sequence);
	if (res.is_found()) { return res; }
	std::shared_lock<std::shared_mutex> shared(_ssts_mutex);
	return get_from_sst_without_lock(user_key, commit_sequence);
}

LookUpResult
LSMEngine::get_from_sst_without_lock(const std::string &user_key,
									 std::uint64_t commit_sequence) {
	std::string enc_key = InternalKey::encode(
		InternalKey(user_key, commit_sequence, ValueType::Value));
	for (auto &[level, ids] : _sst_id_map) {
		if (level == 0) {
			// level 0 顺序查找
			for (std::size_t id : ids) {
				std::optional<SSTIterator> find_it = _ssts[id]->get(enc_key);
				if (find_it == std::nullopt) { continue; }
				if (find_it->is_valid()) {
					std::string enc_key = (*find_it.value()).first,
								value = (*find_it.value()).second;
					InternalKey decode = InternalKey::decode(enc_key);
					return LookUpResult(value, decode._commit_sequence,
										decode._value_type);
				}
			}
		} else {
			std::size_t l = 0, r = ids.size();
			while (l < r) {
				std::size_t mid = (l + r) >> 1;
				std::shared_ptr<SST> cur_sst = _ssts[ids[mid]];
				std::shared_ptr<Comparator<std::string>> sst_cmp =
					cur_sst->get_comparator();
				// 查last_key >= enc_key的第一个位置
				if ((*sst_cmp)(cur_sst->get_last_key(), enc_key) >= 0) {
					r = mid;
				} else {
					l = mid + 1;
				}
			}
			if (l == ids.size()) { continue; }
			std::shared_ptr<SST> cur_sst = _ssts[ids[l]];
			std::optional<SSTIterator> find_it = cur_sst->get(enc_key);
			if (find_it == std::nullopt) { continue; }
			if (find_it->is_valid()) {
				std::string enc_key = (*find_it.value()).first,
							value = (*find_it.value()).second;
				InternalKey decode = InternalKey::decode(enc_key);
				return LookUpResult(value, decode._commit_sequence,
									decode._value_type);
			} else {
				continue;
			}
		}
	}
	return LookUpResult(LookUpResult::EMPTY_VALUE, LookUpResult::EMPTY_COMSEQ,
						NotFound);
}

std::vector<LookUpResult>
LSMEngine::get_batch(const std::vector<std::string> &keys,
					 std::vector<std::uint64_t> commit_sequences) {
	std::vector<LookUpResult> memtable_find_res =
		_memtable.get_batch(keys, commit_sequences);
	std::shared_lock<std::shared_mutex> shared(_ssts_mutex);
	for (std::size_t i = 0; i < keys.size(); i++) {
		LookUpResult res = memtable_find_res[i];
		if (!res.is_found()) {
			// 查sst
			LookUpResult sst_res =
				get_from_sst_without_lock(keys[i], commit_sequences[i]);
			if (sst_res.is_found()) { memtable_find_res[i] = sst_res; }
		}
	}
	return memtable_find_res;
}

LookUpResult LSMEngine::get(const std::string &key) {
	return get(key, _commit_sequence.load());
}
std::vector<LookUpResult>
LSMEngine::get_batch(const std::vector<std::string> &keys) {
	std::vector<std::uint64_t> seqs;
	for (std::size_t i = 0; i < keys.size(); i++) {
		seqs.push_back(_commit_sequence.load());
	}
	return get_batch(keys, seqs);
}

ConcatIterator::ConcatIterator(std::vector<std::shared_ptr<SST>> ssts,
							   std::size_t cur_sst_idx,
							   std::optional<std::uint64_t> commit_sequence,
							   bool keep_all_version)
	: _ssts(ssts), _cur_sst_idx(cur_sst_idx),
	  _sst_iter(SSTIterator::invalid_iter()), _commit_sequence(commit_sequence),
	  _keep_all_version(keep_all_version) {
	if (_ssts.size() > 0) {
		_sst_iter =
			_ssts[cur_sst_idx]->begin(_commit_sequence, _keep_all_version);
	}
	while (_ssts.size() > 0 && !_sst_iter.is_valid()) {
		_cur_sst_idx++;
		if (_cur_sst_idx >= _ssts.size()) {
			_sst_iter = SSTIterator::invalid_iter();
			return;
		}
		_sst_iter =
			_ssts[cur_sst_idx]->begin(_commit_sequence, _keep_all_version);
	}
}

ConcatIterator
LSMEngine::level_begin(std::size_t level_idx,
					   std::optional<std::uint64_t> commit_sequence,
					   bool keep_all_version) {
	std::shared_lock<std::shared_mutex> shared(_ssts_mutex);
	if (_sst_id_map.find(level_idx) == _sst_id_map.end()) {
		return ConcatIterator(std::vector<std::shared_ptr<SST>>(), 0,
							  std::nullopt, false);
	}
	std::vector<std::shared_ptr<SST>> ssts;
	for (auto sst_id : _sst_id_map[level_idx]) {
		ssts.push_back(_ssts[sst_id]);
	}
	return ConcatIterator(ssts, 0, commit_sequence, keep_all_version);
}
ConcatIterator LSMEngine::level_end(std::size_t level_idx) {
	std::shared_lock<std::shared_mutex> shared(_ssts_mutex);
	if (_sst_id_map.find(level_idx) == _sst_id_map.end()) {
		return ConcatIterator(std::vector<std::shared_ptr<SST>>(), 0,
							  std::nullopt, false);
	}
	return ConcatIterator(std::vector<std::shared_ptr<SST>>(),
						  _sst_id_map[level_idx].size(), std::nullopt, false);
}

HeapIterator
LSMEngine::level_0_begin(std::optional<std::uint64_t> commit_sequence,
						 bool keep_all_version, bool skip_tombstone) {
	HeapIterator heap_it(skip_tombstone, commit_sequence, keep_all_version);
	std::shared_lock<std::shared_mutex> shared(_ssts_mutex);
	if (_sst_id_map.find(0) != _sst_id_map.end()) {
		for (std::size_t l0_id : _sst_id_map[0]) {
			std::shared_ptr<SST> cur_sst = _ssts[l0_id];
			for (SSTIterator it = cur_sst->begin(std::nullopt, true);
				 it != cur_sst->end(); ++it) {
				heap_it.put((*it).first, (*it).second);
			}
		}
	}
	heap_it.clean_after_operate();
	return heap_it;
}
HeapIterator LSMEngine::level_0_end() {
	// 这里其实些什么都行, 因为==比的是heap是否为空, 其余都不参与比较
	return HeapIterator(false, true);
}

HeapIterator LSMEngine::iter_monotony_predicate(
	std::function<int(const std::string &)> predicate,
	std::optional<std::uint64_t> commit_sequence) {
	std::function<int(const std::string &)> wrapper_predicate =
		[predicate](const std::string &enc_key) -> int {
		std::string user_key = InternalKey::decode(enc_key)._key;
		return predicate(user_key);
	};
	HeapIterator heap_it = _memtable.iter_monotony_predicate(
		predicate,
		commit_sequence == std::nullopt ? HeapIterator::MAX_COMMIT_SEQUENCE
										: commit_sequence.value(),
		false, true);
	std::shared_lock<std::shared_mutex> shared(_ssts_mutex);
	for (std::size_t level = 0; level <= _cur_max_level; level++) {
		if (_sst_id_map.find(level) == _sst_id_map.end()) { continue; }
		if (level == 0) {
			for (std::size_t sst_id : _sst_id_map[0]) {
				std::optional<std::pair<SSTIterator, SSTIterator>> sst_its =
					_ssts[sst_id]->iter_monotony_predicate(predicate,
														   commit_sequence);
				if (sst_its != std::nullopt) {
					SSTIterator begin = sst_its->first, end = sst_its->second;
					for (; begin != end; ++begin) {
						heap_it.put((*begin).first, (*begin).second);
					}
				}
			}
		} else {
			std::deque<std::size_t> cur_level_ssts = _sst_id_map[level];
			std::size_t l = 0, r = cur_level_ssts.size();
			while (l < r) {
				std::size_t mid = (l + r) >> 1; // 当前level层的sst的下标
				// 找第一个predicate(last_key) >= 0的sst下标作为开始
				if (wrapper_predicate(
						_ssts[cur_level_ssts[mid]]->get_last_key()) >= 0) {
					r = mid;
				} else {
					l = mid + 1;
				}
			}
			if (l == cur_level_ssts.size()) { continue; }
			for (; l < cur_level_ssts.size(); l++) {
				std::optional<std::pair<SSTIterator, SSTIterator>> sst_its =
					_ssts[cur_level_ssts[l]]->iter_monotony_predicate(
						predicate, commit_sequence);
				if (sst_its != std::nullopt) {
					SSTIterator begin = sst_its->first, end = sst_its->second;
					for (; begin != end; ++begin) {
						heap_it.put((*begin).first, (*begin).second);
					}
				} else {
					break;
				}
			}
		}
	}
	heap_it.set_keep_all_version(false);
	heap_it.set_skip_tombstone(true);
	heap_it.clean_after_operate();
	return heap_it;
}

HeapIterator LSMEngine::range(const std::string &start_user_key,
							  const std::string &end_user_key) {
	std::function<int(const std::string &user_key)> predicate =
		[start_user_key, end_user_key](const std::string &user_key) -> int {
		if (user_key < start_user_key) return -1;
		else if (user_key >= start_user_key && user_key < end_user_key)
			return 0;
		else return 1;
	};
	return iter_monotony_predicate(predicate, _commit_sequence.load());
}

/**
从base_iter中创建一组新的SST, 每一个SST最大容量为max_sst_size, 目标层级是level
其中, 每一个sst的sst_id从LSMEngine的_next_sst_id中获取
	每一个sst的存储路径使用get_sst_path()函数获取
*/
std::vector<std::shared_ptr<SST>>
LSMEngine::generate_sst_from_bast_iter(BaseIterator &base_iter,
									   std::size_t max_sst_size,
									   std::size_t target_level) {
	std::vector<std::shared_ptr<SST>> ssts;
	SSTBuilder builder;
	bool is_full = false;
	std::string prev_user_key;
	while (base_iter.is_valid()) {
		builder.add((*base_iter).first, (*base_iter).second);
		std::string user_key = InternalKey::decode((*base_iter).first)._key;
		if (is_full && user_key != prev_user_key) {
			is_full = false;
			std::size_t sst_id = 0;
			sst_id = _next_sst_id;
			_next_sst_id++;
			std::shared_ptr<SST> sst =
				builder.build(sst_id, get_sst_path(sst_id, target_level));
			builder = SSTBuilder();
			ssts.push_back(sst);
		}
		if (builder.get_estimated_size() >= max_sst_size) { is_full = true; }
		prev_user_key = user_key;
		++base_iter;
	}
	if (!builder.empty()) {
		std::size_t sst_id = 0;
		sst_id = _next_sst_id;
		_next_sst_id++;
		std::shared_ptr<SST> sst =
			builder.build(sst_id, get_sst_path(sst_id, target_level));
		ssts.push_back(sst);
	}
	return ssts;
}

std::vector<std::shared_ptr<SST>>
LSMEngine::compact_l0_l1(std::size_t max_sst_size) {
	// 所有数据都要搬运进新表, tombstone也是数据, 并且保留所有version
	std::shared_ptr<HeapIterator> l0_iter =
		std::make_shared<HeapIterator>(false, true);
	std::shared_ptr<ConcatIterator> l1_iter = nullptr;
	if (_sst_id_map.find(0) != _sst_id_map.end()) {
		for (std::size_t l0_id : _sst_id_map[0]) {
			std::shared_ptr<SST> cur_sst = _ssts[l0_id];
			for (SSTIterator it = cur_sst->begin(std::nullopt, true);
				 it != cur_sst->end(); ++it) {
				l0_iter->put((*it).first, (*it).second);
			}
		}
	}
	if (_sst_id_map.find(1) != _sst_id_map.end()) {
		std::vector<std::shared_ptr<SST>> l1_ssts;
		for (std::size_t l1_id : _sst_id_map[1]) {
			l1_ssts.push_back(_ssts[l1_id]);
		}
		l1_iter =
			std::make_shared<ConcatIterator>(l1_ssts, 0, std::nullopt, true);
	}
	TwoMergeIterator merge_iter(l0_iter, l1_iter, true);
	return generate_sst_from_bast_iter(merge_iter, max_sst_size, 1);
}

std::vector<std::shared_ptr<SST>> LSMEngine::compact_near_sorted_level(
	std::size_t first_sorted_level, std::size_t second_sorted_level,
	std::size_t target_level, std::size_t max_sst_size) {
	std::shared_ptr<ConcatIterator> first_iter = nullptr, second_iter = nullptr;
	if (_sst_id_map.find(first_sorted_level) != _sst_id_map.end()) {
		std::vector<std::shared_ptr<SST>> ssts;
		for (std::size_t id : _sst_id_map[first_sorted_level]) {
			ssts.push_back(_ssts[id]);
		}
		first_iter =
			std::make_shared<ConcatIterator>(ssts, 0, std::nullopt, true);
	}
	if (_sst_id_map.find(second_sorted_level) != _sst_id_map.end()) {
		std::vector<std::shared_ptr<SST>> ssts;
		for (std::size_t id : _sst_id_map[second_sorted_level]) {
			ssts.push_back(_ssts[id]);
		}
		second_iter =
			std::make_shared<ConcatIterator>(ssts, 0, std::nullopt, true);
	}
	TwoMergeIterator merge_iter(first_iter, second_iter, true);
	return generate_sst_from_bast_iter(merge_iter, max_sst_size, target_level);
}

// 压缩整个LSMEngine
void LSMEngine::compact() { compact(0); }

bool LSMEngine::delete_level(std::size_t level) {
	if (_sst_id_map.find(level) != _sst_id_map.end()) {
		std::deque<std::size_t> deque_cur_level = _sst_id_map[level];
		for (std::size_t id : deque_cur_level) {
			_ssts[id]->delete_file();
			_ssts.erase(id);
		}
		_sst_id_map.erase(level);
	}
	return true;
}

void LSMEngine::insert_level(std::size_t target_level,
							 std::vector<std::shared_ptr<SST>> ssts) {
	if (ssts.empty()) return;
	_cur_max_level = std::max(_cur_max_level, target_level);
	for (std::shared_ptr<SST> sst : ssts) {
		std::uint64_t id = sst->get_sst_id();
		_sst_id_map[target_level].push_back(id);
		_ssts[id] = sst;
	}
}

void LSMEngine::close() {
	{
		std::unique_lock<std::shared_mutex> unique(_close_mutex);
		_close = true;
	}
	while (_memtable.get_total_size() > 0) {
		flush();
	}
	std::vector<std::uint8_t> data =
		get_bytes_of_uint64t(_commit_sequence.load());
	FileObj::create_and_write(_com_seq_filepath, data);
}

/**
	从start_level开始向上compact
*/
void LSMEngine::compact(std::size_t start_level) {
	while (start_level <= _cur_max_level) {
		if (_sst_id_map.find(start_level) == _sst_id_map.end() ||
			_sst_id_map[start_level].size() < _compact_threshold) {
			break;
		}
		std::vector<std::shared_ptr<SST>> new_ssts;
		if (start_level == 0) {
			new_ssts = compact_l0_l1(get_level_sst_size(1));
		} else {
			new_ssts = compact_near_sorted_level(
				start_level, start_level + 1, start_level + 1,
				get_level_sst_size(start_level + 1));
		}
		delete_level(start_level);
		delete_level(start_level + 1);
		insert_level(start_level + 1, new_ssts);
		start_level++;
	}
}

std::size_t LSMEngine::get_level_sst_size(std::size_t level) {
	std::size_t p = 1;
	for (std::size_t i = 0; i < level; i++) {
		p = p * _compact_threshold;
	}
	return _max_l0_sst_size * p;
}

std::shared_mutex &LSMEngine::get_mutex() { return _ssts_mutex; }

std::size_t LSMEngine::get_cur_max_level() { return _cur_max_level; }

HeapIterator
LSMEngine::memtable_begin(std::optional<std::uint64_t> commit_sequence,
						  bool keep_all_version, bool skip_tombstone) {
	return _memtable.begin(commit_sequence, keep_all_version, skip_tombstone);
}

LSMEngineIterator LSMEngine::begin(std::optional<std::uint64_t> commit_sequence,
								   bool keep_all_version, bool skip_tombstone) {
	return LSMEngineIterator(shared_from_this(), commit_sequence,
							 keep_all_version, skip_tombstone);
}

LSMEngineIterator LSMEngine::begin() {
	return begin(_commit_sequence.load(), false, true);
}

LSMEngineIterator LSMEngine::end() { return LSMEngineIterator(); }

LSMEngine::~LSMEngine() {
	try {
		close();
	} catch (const std::exception &e) { std::cerr << e.what() << std::endl; }
}

// <enc_key, value>
std::pair<std::string, std::string> ConcatIterator::operator*() const {
	if (!is_valid()) { throw std::runtime_error("concat iter is not valid."); }
	return *_sst_iter;
}
ConcatIterator &ConcatIterator::operator++() {
	if (!is_valid()) { throw std::runtime_error("concat iter is not valid."); }
	++_sst_iter;
	while (!_sst_iter.is_valid()) {
		// 当前sst走完了
		_cur_sst_idx++;
		if (_cur_sst_idx >= _ssts.size()) { return *this; }
		_sst_iter =
			_ssts[_cur_sst_idx]->begin(_commit_sequence, _keep_all_version);
	}
	return *this;
}
bool ConcatIterator::operator==(const BaseIterator &other) const {
	const ConcatIterator *other_ptr =
		dynamic_cast<const ConcatIterator *>(&other);
	if (other_ptr == nullptr) { return false; }
	return _cur_sst_idx == other_ptr->_cur_sst_idx &&
		   ((!_sst_iter.is_valid() && !other_ptr->_sst_iter.is_valid()) ||
			_sst_iter == other_ptr->_sst_iter);
}
bool ConcatIterator::operator!=(const BaseIterator &other) const {
	return !(*this == other);
}
bool ConcatIterator::is_valid() const {
	return _cur_sst_idx < _ssts.size() && _sst_iter.is_valid();
}

ConcatIterator ConcatIterator::invalid() {
	return ConcatIterator(std::vector<std::shared_ptr<SST>>(), 0, std::nullopt,
						  true);
}

TwoMergeIterator::TwoMergeIterator(std::shared_ptr<BaseIterator> first_iter,
								   std::shared_ptr<BaseIterator> second_iter,
								   bool keep_all_version)
	: _first_iter(first_iter), _second_iter(second_iter),
	  _keep_all_version(keep_all_version),
	  _comparator(std::make_shared<InternalKeyComparator>()) {
	choose();
}

void TwoMergeIterator::clean_by_version(const std::string &enc_key) {
	if (_keep_all_version) {
		// 只需要跳完全相同的enc_key
		while (_first_iter != nullptr && _first_iter->is_valid() &&
			   (*(*_first_iter)).first == enc_key) {
			++(*_first_iter);
		}
		while (_second_iter != nullptr && _second_iter->is_valid() &&
			   (*(*_second_iter)).first == enc_key) {
			++(*_second_iter);
		}
	} else {
		// 只要user_key相同就全跳过
		std::string user_key = InternalKey::decode(enc_key)._key;
		while (_first_iter != nullptr && _first_iter->is_valid() &&
			   InternalKey::decode((*(*_first_iter)).first)._key == user_key) {
			++(*_first_iter);
		}
		while (_second_iter != nullptr && _second_iter->is_valid() &&
			   InternalKey::decode((*(*_second_iter)).first)._key == user_key) {
			++(*_second_iter);
		}
	}
}

void TwoMergeIterator::choose() {
	if (_first_iter != nullptr && _second_iter != nullptr &&
		_first_iter->is_valid() && _second_iter->is_valid()) {
		std::string first_enc_key = (*(*_first_iter)).first,
					second_enc_key = (*(*_second_iter)).first;
		if ((*_comparator)(first_enc_key, second_enc_key) < 0) {
			_choose_first = true;
		} else if ((*_comparator)(first_enc_key, second_enc_key) > 0) {
			_choose_first = false;
		} else {
			_choose_first = true;
			if (_second_iter != nullptr && _second_iter->is_valid()) {
				++(*_second_iter);
			}
		}
	} else if (_first_iter != nullptr && _first_iter->is_valid()) {
		_choose_first = true;
	} else if (_second_iter != nullptr && _second_iter->is_valid()) {
		_choose_first = false;
	} else {
		_choose_first = true;
	}
}

TwoMergeIterator &TwoMergeIterator::operator++() {
	if (!is_valid()) { return *this; }
	std::string cur_enc_key =
		_choose_first ? (*(*_first_iter)).first : (*(*_second_iter)).first;
	clean_by_version(cur_enc_key);
	choose();
	return *this;
}

bool TwoMergeIterator::operator==(const BaseIterator &other) const {
	const TwoMergeIterator *other_ptr =
		dynamic_cast<const TwoMergeIterator *>(&other);
	if (other_ptr == nullptr) { return false; }
	bool this_first_valid = _first_iter != nullptr && _first_iter->is_valid(),
		 this_second_valid =
			 _second_iter != nullptr && _second_iter->is_valid(),
		 other_first_valid = other_ptr->_first_iter != nullptr &&
							 other_ptr->_first_iter->is_valid(),
		 other_second_valid = other_ptr->_second_iter != nullptr &&
							  other_ptr->_second_iter->is_valid();
	return ((!this_first_valid && !other_first_valid) ||
			(this_first_valid && other_first_valid &&
			 (*_first_iter) == (*other_ptr->_first_iter))) &&
		   ((!this_second_valid && !other_second_valid) ||
			(this_second_valid && other_second_valid &&
			 (*_second_iter) == (*other_ptr->_second_iter))) &&
		   _choose_first == other_ptr->_choose_first &&
		   _keep_all_version == other_ptr->_keep_all_version;
}
bool TwoMergeIterator::operator!=(const BaseIterator &other) const {
	return !((*this) == other);
}
// <enc_key, value>
std::pair<std::string, std::string> TwoMergeIterator::operator*() const {
	if (_choose_first) {
		if (_first_iter != nullptr && _first_iter->is_valid()) {
			return *(*_first_iter);
		} else {
			throw std::runtime_error("TwoMergeIterator is not valid.");
		}
	} else {
		if (_second_iter != nullptr && _second_iter->is_valid()) {
			return *(*_second_iter);
		} else {
			throw std::runtime_error("TwoMergeIterator is not valid.");
		}
	}
}
bool TwoMergeIterator::is_valid() const {
	return (_choose_first && _first_iter != nullptr &&
			_first_iter->is_valid()) ||
		   (!_choose_first && _second_iter != nullptr &&
			_second_iter->is_valid());
}

LSMEngineIterator::LSMEngineIterator(
	std::shared_ptr<LSMEngine> engine,
	std::optional<std::uint64_t> commit_sequence, bool keep_all_version,
	bool skip_tombstone)
	: _engine(engine), _cur_iter_idx(0), _shared_lock(_engine->get_mutex()),
	  _commit_sequence(commit_sequence), _keep_all_version(keep_all_version),
	  _skip_tombstone(skip_tombstone),
	  _comparator(std::make_shared<InternalKeyComparator>()) {
	_iters.push_back(std::make_shared<HeapIterator>(std::move(
		_engine->memtable_begin(_commit_sequence, keep_all_version, false))));
	for (std::size_t level = 0; level <= _engine->get_cur_max_level();
		 level++) {
		if (_engine->_sst_id_map.find(level) == _engine->_sst_id_map.end()) {
			continue;
		}
		if (level == 0) {
			HeapIterator heap_it = _engine->level_0_begin(
				_commit_sequence, _keep_all_version, false);
			_iters.push_back(
				std::make_shared<HeapIterator>(std::move(heap_it)));
		} else {
			_iters.push_back(
				std::make_shared<ConcatIterator>(std::move(_engine->level_begin(
					level, _commit_sequence, _keep_all_version))));
		}
	}
	// memtable层的HeapIterator以及level0的HeapIterator,
	// 		构造参数skip_tombstone均为false, 遍历的是全量数据
	// level[1, ..]这些层的Concatiterator本身就是全量数据
	// 因此这里LSMEngineIterator中获取的就是全量数据(包括tombstone)
	// 而最终呈现出来的需要跳过tombstone, 因此需要在operator++中进行跳过

	// 比较选出第一个遍历的位置
	choose_and_skip();
}

bool LSMEngineIterator::is_tombstone(std::shared_ptr<BaseIterator> cur_it) {
	if (cur_it == nullptr || !cur_it->is_valid()) { return false; }
	std::string enc_key = (*(*cur_it)).first;
	return InternalKey::decode(enc_key)._value_type == TombStone;
}

void LSMEngineIterator::skip_user_key(
	const std::string &user_key, std::optional<std::size_t> white_list_idx) {
	for (std::size_t i = 0; i < _iters.size(); i++) {
		if (white_list_idx != std::nullopt && white_list_idx.value() == i) {
			continue;
		}
		if (_iters[i] != nullptr && _iters[i]->is_valid()) {
			while (_iters[i]->is_valid() &&
				   InternalKey::decode((*(*_iters[i])).first)._key ==
					   user_key) {
				++(*_iters[i]);
			}
		}
	}
}

bool LSMEngineIterator::skip_other_version() {
	if (_keep_all_version) {
		return true; // 不需要过滤
	}
	if (_cur_iter_idx >= _iters.size() || !_iters[_cur_iter_idx]->is_valid()) {
		return false;
	}
	// 跳过user_key相同的其他所有版本
	std::string user_key =
		InternalKey::decode((*(*_iters[_cur_iter_idx])).first)._key;
	skip_user_key(user_key, _cur_iter_idx);
	return true;
}

/**
memtable: c@5, d@9, k@10(活值), y@2
L1:       a@10, k@20(墓碑), z@1
L2: 	  e@5, f@9, g@2, k@15(活值), y@2

可以发现, 由于排序规则是按照user_key升序, commit_sequence降序
那么, 对于一个同时处于多个source的user_key(比如这里的user_key = k)
当choose选择到user_key = k时, 首先选择的一定是commit_sequence最大的一个,
并且此时另外几个iter也一定走到了k的位置
(反证, 如果没走到, 那么对应的user_key一定比k小, 那么下一个就会走这个iter)
因此此时只需要看当前user_key = k对应的是不是tombstone,
如果是, 那么让其余几个iter把这个user_key跳过即可

	-- 这个函数单独用来choose, choose_and_skip负责skip_tombstone
	-- return : 全都是无效的时候返回false
*/
bool LSMEngineIterator::choose() {
	// 找到第一个有效的位置放置当前的_cur_iter_idx,
	// 避免后续和cur_idx比较的时候cur无效
	for (std::size_t i = 0; i < _iters.size(); i++) {
		if (_iters[i] != nullptr && _iters[i]->is_valid()) {
			_cur_iter_idx = i;
		}
	}
	if (_cur_iter_idx >= _iters.size() || !_iters[_cur_iter_idx]->is_valid()) {
		return false;
	}
	for (std::size_t i = 0; i < _iters.size(); i++) {
		if (!_iters[i]->is_valid()) { continue; }
		std::string min_enc_key = (*(*_iters[_cur_iter_idx])).first,
					enc_key = (*(*_iters[i])).first;
		if (_iters[i] != nullptr && _iters[i]->is_valid() &&
			(*_comparator)(enc_key, min_enc_key) < 0) {
			_cur_iter_idx = i;
		}
	}
	if (!_keep_all_version) skip_other_version();
	return true;
}

bool LSMEngineIterator::choose_and_skip() {
	bool res = false;
	res = choose();
	while (_skip_tombstone && _cur_iter_idx < _iters.size() &&
		   _iters[_cur_iter_idx]->is_valid() &&
		   is_tombstone(_iters[_cur_iter_idx])) {
		skip_user_key(
			InternalKey::decode((*(*_iters[_cur_iter_idx])).first)._key,
			std::nullopt);
		// 重新choose
		res = choose();
	}
	return res;
}

// <enc_key, value>
std::pair<std::string, std::string> LSMEngineIterator::operator*() const {
	if (!is_valid()) { throw std::runtime_error("iter is not valid."); }
	return *(*(_iters[_cur_iter_idx]));
}
BaseIterator &LSMEngineIterator::operator++() {
	if (!is_valid()) { return *this; }
	++(*_iters[_cur_iter_idx]);
	choose_and_skip();
	return *this;
}
bool LSMEngineIterator::operator==(const BaseIterator &other) const {
	const LSMEngineIterator *other_ptr =
		dynamic_cast<const LSMEngineIterator *>(&other);
	if (other_ptr == nullptr) { return false; }
	return (!is_valid() && !other_ptr->is_valid()) ||
		   (is_valid() && other_ptr->is_valid() &&
			_cur_iter_idx == other_ptr->_cur_iter_idx &&
			*(_iters[_cur_iter_idx]) ==
				*(other_ptr->_iters[other_ptr->_cur_iter_idx]));
}
bool LSMEngineIterator::operator!=(const BaseIterator &other) const {
	return !(*this == other);
}
bool LSMEngineIterator::is_valid() const {
	if (_engine == nullptr || _cur_iter_idx >= _iters.size()) return false;
	return _iters[_cur_iter_idx]->is_valid();
}

LSM::LSM(const std::string &data_dir)
	: _engine(std::make_shared<LSMEngine>(data_dir)) {}
std::optional<std::uint64_t> LSM::put(const std::string &key,
									  const std::string &value) {
	return _engine->put(key, value);
}
std::optional<std::uint64_t> LSM::remove(const std::string &key) {
	return _engine->remove(key);
}
std::optional<std::uint64_t>
LSM::put_batch(std::vector<std::pair<std::string, std::string>> kvs) {
	return _engine->put_batch(kvs);
}
std::optional<std::uint64_t> LSM::remove_batch(std::vector<std::string> ks) {
	return _engine->remove_batch(ks);
}
LookUpResult LSM::get(const std::string &key) {
	LookUpResult res = _engine->get(key);
	if (res._value_type == TombStone) { res._value_type = NotFound; }
	return res;
}
std::vector<LookUpResult> LSM::get_batch(const std::vector<std::string> &keys) {
	std::vector<LookUpResult> res = _engine->get_batch(keys);
	for (auto &r : res) {
		if (r._value_type == TombStone) { r._value_type = NotFound; }
	}
	return res;
}
HeapIterator LSM::range(const std::string &start_user_key,
						const std::string &end_user_key) {
	return _engine->range(start_user_key, end_user_key);
}
LSMEngineIterator LSM::begin() { return _engine->begin(); }
LSMEngineIterator LSM::end() { return _engine->end(); }
std::uint64_t LSM::flush() { return _engine->flush(); }
void LSM::close() { _engine->close(); }
LSM::~LSM() {}