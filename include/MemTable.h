#pragma once
#include "BastIterator.h"
#include "Comparator.h"
#include "Record.h"
#include "SkipList.h"
#include "sst.h"
#include <cstdint> // std::uint64_t, std::uint8_t
#include <deque>
#include <memory>
#include <optional>
#include <queue>
#include <shared_mutex>
#include <string>
#include <utility>

class HeapIterator : public BaseIterator {
	friend class MemTable;

public:
	static const std::uint64_t MAX_COMMIT_SEQUENCE = ((1ULL << 56) - 1);

	class HeapItem {
	public:
		inline static const InternalKeyComparator _comparator;

		HeapItem(const std::string &key, const std::string &value,
				 std::uint64_t commit_sequence, ValueType value_type);

		bool operator>(const HeapItem &other) const;

		std::string _key;
		std::string _value;
		std::uint64_t _commit_sequence;
		ValueType _value_type;
	};

	HeapIterator(bool skip_tombstone, bool keep_all_version = false);
	HeapIterator(bool skip_tombstone,
				 std::optional<std::uint64_t> commit_sequence,
				 bool keep_all_version);
	HeapIterator(bool skip_tombstone, std::uint64_t commit_sequence,
				 bool keep_all_version = false);

	void put(const std::string &user_key, const std::string &value,
			 std::uint64_t commit_sequence, ValueType value_type);
	void put(const std::string &enc_key, const std::string &value);

	HeapIterator &operator++() override;
	bool operator==(const BaseIterator &other) const override;
	bool operator!=(const BaseIterator &other) const override;
	// <enc_key, value>
	std::pair<std::string, std::string> operator*() const override;
	bool is_valid() const override;

	// 在put完成之后进行清理, 依据commit_sequence以及skip_tomb_stone
	void clean_after_operate();
	void set_skip_tombstone(bool skip_tombstone);
	void set_keep_all_version(bool keep_all_version);

private:
	HeapIterator &clean_tombstone();
	HeapIterator &clean_by_commit_sequence();


	std::priority_queue<HeapItem, std::vector<HeapItem>, std::greater<HeapItem>>
		_heap;
	bool _skip_tombstone;
	std::uint64_t _commit_sequence;
	bool _keep_all_version;
};

class MemTable {

	friend class WAL;

	inline static const std::string EMPTY_VALUE = "";
	static const int SEQ_AND_VALTYPE_SIZE = 8;

public:
	class PutResult {
	public:
		bool _is_add;
		std::optional<std::uint64_t> _frozen_list_id;
		std::optional<std::uint64_t> _new_active_list_id;

		PutResult(bool is_add);
		PutResult(bool is_add, std::uint64_t frozen_list_id,
				  std::uint64_t new_active_list_id);
	};

	class RemoveResult {
	public:
		bool _is_success;
		std::optional<std::uint64_t> _frozen_list_id;
		std::optional<std::uint64_t> _new_active_list_id;

		RemoveResult(bool is_success);
		RemoveResult(bool is_success, std::uint64_t frozen_list_id,
					 std::uint64_t new_active_list_id);
	};

	class FlushLastResult {
	public:
		std::shared_ptr<SST> _sst;
		std::optional<std::uint64_t> _flush_id;
		std::optional<std::uint64_t> _new_active_id;

		FlushLastResult(std::shared_ptr<SST> sst,
						std::optional<std::uint64_t> flush_id,
						std::optional<std::uint64_t> new_active_id);
	};

	static const std::size_t MAX_ACTIVE_BYTES = 4ULL * 1024 * 1024; // 4MB

	MemTable(std::size_t max_active_bytes = MAX_ACTIVE_BYTES,
			 std::shared_ptr<const Comparator<std::string>> comparator =
				 std::make_shared<InternalKeyComparator>());


	PutResult put(const std::string &key, const std::string &value,
				  std::uint64_t commit_sequence);

	LookUpResult get(const std::string &key, std::uint64_t commit_sequence);

	RemoveResult remove(const std::string &key, std::uint64_t commit_sequence);

	bool put_batch(std::vector<std::pair<std::string, std::string>> kvs,
				   std::vector<std::uint64_t> commit_sequences);

	void put_entry_data(const std::string &enc_key, const std::string &value);

	std::vector<LookUpResult>
	get_batch(std::vector<std::string> ks,
			  std::vector<std::uint64_t> commit_sequences);

	bool remove_batch(std::vector<std::string> ks,
					  std::vector<std::uint64_t> commit_sequences);

	HeapIterator begin(std::optional<std::uint64_t> commit_sequence,
					   bool keep_all_version, bool skip_tombstone = true) const;
	HeapIterator end() const;
	HeapIterator begin_preffix(const std::string &key,
							   std::uint64_t commit_sequence) const;

	HeapIterator
	iter_monotony_predicate(std::function<int(const std::string &)> predicate,
							std::uint64_t commit_sequence, bool skip_tombstone,
							bool keep_all_version);

	MemTable::FlushLastResult
	flush_last(SSTBuilder &builder, const std::string &sst_path,
			   std::uint64_t sst_id, std::vector<std::uint64_t> &flushed_seqs);
	std::size_t get_total_size();
	void clear();

private:
	std::size_t _active_bytes;
	std::size_t _max_active_bytes;
	std::size_t _frozen_bytes;
	std::shared_ptr<const Comparator<std::string>> _comparator;

	std::shared_ptr<SkipList> _active_list;
	std::deque<std::shared_ptr<const SkipList>> _frozen_lists;

	// mutable: 这个字段即使在const中也可以修改
	// 由于即使是const的memtable对象,
	// 其也需要进行加锁解锁操作(例如frozen_list中的list)
	mutable std::shared_mutex _active_mutex;
	mutable std::shared_mutex
		_frozen_mutex; // frozen_lists只有读, 但是有可能会增加skiplist,
					   // 因此整个容器一把锁即可

	std::uint64_t _active_id;
	std::deque<std::uint64_t> _frozen_ids;

	bool put_single_skiplist(std::shared_ptr<SkipList> skiplist,
							 const std::string &key, const std::string &value,
							 std::uint64_t commit_sequence,
							 ValueType value_type = ValueType::Value);
	bool put_single_skiplist(std::shared_ptr<SkipList> skiplist,
							 const std::string &enc_key,
							 const std::string &value);
	SkipList::ConstIterator
	get_lower_bound_single_skiplist(std::shared_ptr<const SkipList> skiplist,
									const std::string &key,
									std::uint64_t commit_sequence);

	std::optional<std::string> get_precise_single_skiplist(
		std::shared_ptr<const SkipList> skiplist, const std::string &key,
		std::uint64_t commit_sequence, ValueType value_type);

	std::optional<std::string>
	get_precise_single_skiplist(std::shared_ptr<const SkipList> skiplist,
								const std::string &enc_key);

	bool remove_single_skiplist(std::shared_ptr<SkipList> skiplist,
								const std::string &key,
								std::uint64_t commit_sequence);

	/**
	<frozen_list_id, new_active_list_id>
	*/
	std::pair<std::uint64_t, std::uint64_t> froze_with_active_unique();

	static std::string get_next_str(const std::string &str);
};
