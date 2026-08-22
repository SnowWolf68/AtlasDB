#pragma once
#include "BastIterator.h"
#include "Comparator.h"
#include "MemTable.h"
#include "Record.h"
#include "sst.h"
#include <atomic>
#include <cstddef>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class ConcatIterator;
class TwoMergeIterator;
class LSMEngineIterator;
class LSM;

class LSMEngine : public std::enable_shared_from_this<LSMEngine> {
	inline static const std::string DEFAULT_COM_SEQ_FILENAME = "seq.txt";

	friend LSMEngineIterator;
	static const std::size_t DEFAULT_MAX_MEMTABLE_SIZE = 64 * 1024 * 1024;
	static const std::size_t DEFAULT_COMPACT_THRESHOLD = 4;
	static const std::size_t DEFAULT_L0_SST_SIZE = 4 * 1024 * 1024;

public:
	LSMEngine(const std::string &data_dir,
			  std::size_t max_memtable_size = DEFAULT_MAX_MEMTABLE_SIZE,
			  std::size_t max_active_size = MemTable::MAX_ACTIVE_BYTES,
			  std::size_t compact_threshold = DEFAULT_COMPACT_THRESHOLD,
			  std::size_t max_l0_sst_size = DEFAULT_L0_SST_SIZE);

	std::string get_sst_path(std::size_t id, std::size_t level);

	// <id, level>
	std::optional<std::pair<std::size_t, std::size_t>>
	decode_sst_path(const std::string &path);

	/**
	把 MemTable 中最老的一张表（frozen 队首）刷成磁盘上的一个 SST 文件，
	并把新 SST 登记进引擎索引
	*/
	std::uint64_t flush();

	std::optional<std::uint64_t> put(const std::string &key,
									 const std::string &value,
									 std::uint64_t commit_sequence);
	std::optional<std::uint64_t> remove(const std::string &key,
										std::uint64_t commit_sequence);
	std::optional<std::uint64_t>
	put_batch(std::vector<std::pair<std::string, std::string>> kvs,
			  std::vector<std::uint64_t> commit_sequences);
	std::optional<std::uint64_t>
	remove_batch(std::vector<std::string> ks,
				 std::vector<std::uint64_t> commit_sequences);

	std::optional<std::uint64_t> put(const std::string &key,
									 const std::string &value);
	std::optional<std::uint64_t> remove(const std::string &key);
	std::optional<std::uint64_t>
	put_batch(std::vector<std::pair<std::string, std::string>> kvs);
	std::optional<std::uint64_t> remove_batch(std::vector<std::string> ks);

	/**
	-- param:
	commit_sequence: 快照点,
		返回 seq ≤ commit_sequence 的最新版本"，不是精确匹配
	*/
	LookUpResult get(const std::string &key, std::uint64_t commit_sequence);
	std::vector<LookUpResult>
	get_batch(const std::vector<std::string> &keys,
			  std::vector<std::uint64_t> commit_sequences);

	/**
	-- param:
	commit_sequence: 快照点,
		返回 seq ≤ commit_sequence 的最新版本"，不是精确匹配
	*/
	LookUpResult get(const std::string &key);
	std::vector<LookUpResult> get_batch(const std::vector<std::string> &keys);

	ConcatIterator level_begin(std::size_t level_idx,
							   std::optional<std::uint64_t> commit_sequence,
							   bool keep_all_version);
	ConcatIterator level_end(std::size_t level_idx);
	HeapIterator level_0_begin(std::optional<std::uint64_t> commit_sequence,
							   bool keep_all_version, bool skip_tombstone);
	HeapIterator level_0_end();
	std::vector<std::shared_ptr<SST>>
	generate_sst_from_bast_iter(BaseIterator &base_iter,
								std::size_t max_sst_size,
								std::size_t target_level);
	std::vector<std::shared_ptr<SST>> compact_l0_l1(std::size_t max_sst_size);
	std::vector<std::shared_ptr<SST>>
	compact_near_sorted_level(std::size_t first_level, std::size_t second_level,
							  std::size_t target_level,
							  std::size_t max_sst_size);
	// 压缩整个LSMEngine
	void compact();
	/**
		从start_level开始向上compact
	*/
	void compact(std::size_t start_level);

	std::size_t get_level_sst_size(std::size_t level);
	std::shared_mutex &get_mutex();
	std::size_t get_cur_max_level();
	HeapIterator memtable_begin(std::optional<std::uint64_t> commit_sequence,
								bool keep_all_version, bool skip_tombstone);
	LSMEngineIterator begin(std::optional<std::uint64_t> commit_sequence,
							bool keep_all_version, bool skip_tombstone = true);
	LSMEngineIterator begin();
	LSMEngineIterator end();

	HeapIterator
	iter_monotony_predicate(std::function<int(const std::string &)> predicate,
							std::optional<std::uint64_t> commit_sequence);

	HeapIterator range(const std::string &start_user_key,
					   const std::string &end_user_key);
	void close();
	~LSMEngine();

private:
	LookUpResult get_from_sst_without_lock(const std::string &user_key,
										   std::uint64_t commit_sequence);
	bool delete_level(std::size_t level);
	void insert_level(std::size_t target_level,
					  std::vector<std::shared_ptr<SST>> ssts);

	std::string _data_dir;
	MemTable _memtable;
	std::map<std::size_t, std::deque<std::size_t>> _sst_id_map;
	std::unordered_map<std::size_t, std::shared_ptr<SST>> _ssts;
	std::size_t _next_sst_id;
	std::size_t _cur_max_level;
	std::shared_mutex _ssts_mutex; // 用来锁_sst_id_map以及_ssts两个资源
	std::size_t _max_memtable_size;
	std::size_t _compact_threshold;
	std::size_t _max_l0_sst_size;
	bool _close;
	std::atomic_uint64_t _commit_sequence;
	std::shared_mutex _close_mutex;
	std::string _com_seq_filepath;
};

class ConcatIterator : public BaseIterator {
	friend class LSMEngine;

public:
	ConcatIterator(std::vector<std::shared_ptr<SST>> ssts,
				   std::size_t cur_sst_idx,
				   std::optional<std::uint64_t> commit_sequence,
				   bool keep_all_version);

	// <enc_key, value>
	std::pair<std::string, std::string> operator*() const override;
	ConcatIterator &operator++() override;
	bool operator==(const BaseIterator &other) const override;
	bool operator!=(const BaseIterator &other) const override;
	bool is_valid() const override;
	static ConcatIterator invalid();

private:
	std::vector<std::shared_ptr<SST>> _ssts;
	std::size_t _cur_sst_idx;
	SSTIterator _sst_iter;
	std::optional<std::uint64_t> _commit_sequence;
	bool _keep_all_version;
};

class TwoMergeIterator : public BaseIterator {
public:
	TwoMergeIterator(std::shared_ptr<BaseIterator> first_iter,
					 std::shared_ptr<BaseIterator> second_iter,
					 bool keep_all_version = false);

	TwoMergeIterator &operator++() override;
	bool operator==(const BaseIterator &other) const override;
	bool operator!=(const BaseIterator &other) const override;
	// <enc_key, value>
	std::pair<std::string, std::string> operator*() const override;
	bool is_valid() const override;

private:
	void clean_by_version(const std::string &enc_key);
	void choose();

	std::shared_ptr<BaseIterator> _first_iter;
	std::shared_ptr<BaseIterator> _second_iter;
	bool _choose_first;
	bool _keep_all_version;
	std::shared_ptr<Comparator<std::string>> _comparator;
};

class LSMEngineIterator : public BaseIterator {
public:
	LSMEngineIterator() = default;
	LSMEngineIterator(std::shared_ptr<LSMEngine> engine,
					  std::optional<std::uint64_t> commit_sequence,
					  bool keep_all_version, bool skip_tombstone = true);

	// <enc_key, value>
	std::pair<std::string, std::string> operator*() const;
	BaseIterator &operator++();
	bool operator==(const BaseIterator &other) const;
	bool operator!=(const BaseIterator &other) const;
	bool is_valid() const;

private:
	bool choose();
	bool is_tombstone(std::shared_ptr<BaseIterator> cur_it);
	void skip_user_key(const std::string &user_key,
					   std::optional<std::size_t> white_list_idx);
	bool choose_and_skip();
	bool skip_other_version();

	std::shared_ptr<LSMEngine> _engine;
	std::vector<std::shared_ptr<BaseIterator>> _iters;
	std::size_t _cur_iter_idx;
	// 读锁, 防止读的时候因为compact导致数据丢失
	// iter生命周期中全程加读锁
	std::shared_lock<std::shared_mutex> _shared_lock;
	std::optional<std::uint64_t> _commit_sequence;
	bool _keep_all_version;
	bool _skip_tombstone;
	std::shared_ptr<Comparator<std::string>> _comparator;
};

class LSM {
public:
	LSM(const std::string &data_dir);

	std::optional<std::uint64_t> put(const std::string &key,
									 const std::string &value);
	std::optional<std::uint64_t> remove(const std::string &key);
	std::optional<std::uint64_t>
	put_batch(std::vector<std::pair<std::string, std::string>> kvs);
	std::optional<std::uint64_t> remove_batch(std::vector<std::string> ks);
	LookUpResult get(const std::string &key);
	std::vector<LookUpResult> get_batch(const std::vector<std::string> &keys);
	HeapIterator range(const std::string &start_user_key,
					   const std::string &end_user_key);
	LSMEngineIterator begin();
	LSMEngineIterator end();
	std::uint64_t flush();
	void close();

	~LSM();

private:
	std::shared_ptr<LSMEngine> _engine;
};