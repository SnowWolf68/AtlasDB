#include "MemTable.h"
#include "Record.h"
#include <cassert>
#include <limits>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <utility>

MemTable::MemTable(std::size_t max_active_bytes,
				   std::shared_ptr<const Comparator<std::string>> comparator)
	: _active_bytes(0), _max_active_bytes(max_active_bytes), _frozen_bytes(0),
	  _comparator(comparator),
	  _active_list(std::make_shared<SkipList>(_comparator)),
	  _frozen_lists(std::deque<std::shared_ptr<const SkipList>>()),
	  _active_id(0) {}

bool MemTable::put_single_skiplist(std::shared_ptr<SkipList> skiplist,
								   const std::string &key,
								   const std::string &value,
								   std::uint64_t commit_sequence,
								   ValueType value_type) {
	InternalKey intr_key(key, commit_sequence, value_type);
	std::string enc_key = InternalKey::encode(intr_key);
	return skiplist->put(enc_key, value);
}

bool MemTable::put_single_skiplist(std::shared_ptr<SkipList> skiplist,
								   const std::string &enc_key,
								   const std::string &value) {
	return skiplist->put(enc_key, value);
}

/**
查询commit_sequence比传入的sequence小或等的第一条记录
(即小于等于最大可见sequence的记录)
*/
SkipList::ConstIterator MemTable::get_lower_bound_single_skiplist(
	std::shared_ptr<const SkipList> skiplist, const std::string &key,
	std::uint64_t commit_sequence) {
	InternalKey intr_key(key, commit_sequence, ValueType::Value);
	std::string enc_key = InternalKey::encode(intr_key);
	return skiplist->lower_bound(enc_key);
}

std::optional<std::string> MemTable::get_precise_single_skiplist(
	std::shared_ptr<const SkipList> skiplist, const std::string &key,
	std::uint64_t commit_sequence, ValueType value_type) {
	InternalKey intr_key(key, commit_sequence, value_type);
	std::string enc_key = InternalKey::encode(intr_key);
	std::string value;
	bool find = skiplist->get(enc_key, value);
	if (!find) { return std::nullopt; }
	return value;
}

std::optional<std::string>
MemTable::get_precise_single_skiplist(std::shared_ptr<const SkipList> skiplist,
									  const std::string &enc_key) {
	std::string value;
	if (!skiplist->get(enc_key, value)) { return std::nullopt; }
	return value;
}

/**
使用skiplist.put()接口进行TombStone标记
原记录仍然保留, 但是由于sequence不一样, 所以其实是两条不同的记录,
原记录在需要的时候也能够被查到

后续会单独处理旧记录的删除问题
*/
bool MemTable::remove_single_skiplist(std::shared_ptr<SkipList> skiplist,
									  const std::string &key,
									  std::uint64_t commit_sequence) {
	InternalKey intr_key(key, commit_sequence, ValueType::TombStone);
	std::string enc_key = InternalKey::encode(intr_key);
	return skiplist->put(enc_key, EMPTY_VALUE);
}

/**
true  -- 添加
false -- 更新
*/
MemTable::PutResult MemTable::put(const std::string &key,
								  const std::string &value,
								  std::uint64_t commit_sequence,
								  bool enable_frozen) {
	std::unique_lock<std::shared_mutex> active_unique(_active_mutex);
	std::optional<std::string> value_opt = get_precise_single_skiplist(
		_active_list, key, commit_sequence, ValueType::Value);
	bool res = put_single_skiplist(_active_list, key, value, commit_sequence);
	if (res) {
		// res == true: 添加
		// 更新 active_bytes
		_active_bytes += key.size() + value.size() + SEQ_AND_VALTYPE_SIZE;
		if (_active_bytes > _max_active_bytes && enable_frozen) {
			std::pair<std::uint64_t, std::uint64_t> id_pair =
				froze_with_active_unique();
			return PutResult(true, id_pair.first, id_pair.second);
		} else {
			return PutResult(true);
		}
	} else {
		// res == false: 更新
		// 理论上正常情况好像走不到这里, 走的都是添加的逻辑
		// 这种情况下find_it一定不是nullptr
		int old_value_bytes = value_opt.value().size(), cur_size = value.size();
		_active_bytes += (cur_size - old_value_bytes);
		if (_active_bytes > _max_active_bytes && enable_frozen) {
			std::pair<std::uint64_t, std::uint64_t> id_pair =
				froze_with_active_unique();
			return PutResult(false, id_pair.first, id_pair.second);
		} else {
			return PutResult(false);
		}
	}
}

/**
先查active表, 能查到直接返回. 因为active总是最新的
active没有再查frozen, 并且frozen是从最新的list开始查, 能查到也是直接返回.
因为最新的list中的sequence一定也是最新的
*/
LookUpResult MemTable::get(const std::string &user_key,
						   std::uint64_t commit_sequence) {
	{
		std::shared_lock<std::shared_mutex> active_shared(_active_mutex);
		SkipList::ConstIterator active_it = get_lower_bound_single_skiplist(
			_active_list, user_key, commit_sequence);
		if (active_it.is_valid()) {
			InternalKey intr_key = InternalKey::decode(active_it.key());
			if (intr_key._key == user_key) {
				return LookUpResult(active_it.value(),
									intr_key._commit_sequence,
									intr_key._value_type);
			}
		}
	}
	{
		std::shared_lock<std::shared_mutex> frozen_shared(_frozen_mutex);
		for (auto it = _frozen_lists.rbegin(); it != _frozen_lists.rend();
			 it++) {
			SkipList::ConstIterator res_it =
				get_lower_bound_single_skiplist(*it, user_key, commit_sequence);
			if (res_it.is_valid()) {
				InternalKey intr_key = InternalKey::decode(res_it.key());
				if (intr_key._key == user_key) {
					return LookUpResult(res_it.value(),
										intr_key._commit_sequence,
										intr_key._value_type);
				}
			}
		}
		return LookUpResult(LookUpResult::EMPTY_VALUE,
							LookUpResult::EMPTY_COMSEQ, NotFound);
	}
}

MemTable::RemoveResult MemTable::remove(const std::string &key,
										std::uint64_t commit_sequence,
										bool enable_frozen) {
	std::unique_lock<std::shared_mutex> active_unique(_active_mutex);
	bool res = remove_single_skiplist(_active_list, key, commit_sequence);
	if (res) {
		_active_bytes += key.size() + EMPTY_VALUE.size() + SEQ_AND_VALTYPE_SIZE;
		if (_active_bytes > _max_active_bytes && enable_frozen) {
			std::pair<std::uint64_t, std::uint64_t> remove_res =
				froze_with_active_unique();
			return RemoveResult(true, remove_res.first, remove_res.second);
		} else {
			return RemoveResult(true);
		}
	} else {
		return RemoveResult(false);
	}
}

MemTable::FrozeResult MemTable::froze() {
	std::unique_lock<std::shared_mutex> active_unique(_active_mutex);
	if (_active_bytes > _max_active_bytes) {
		std::pair<std::uint64_t, std::uint64_t> froze_result =
			froze_with_active_unique();
		return FrozeResult(froze_result.first, froze_result.second);
	}
	return FrozeResult();
}

/**
<frozen_list_id, new_active_list_id>
*/
std::pair<std::uint64_t, std::uint64_t> MemTable::froze_with_active_unique() {
	if (_active_id == std::numeric_limits<std::uint64_t>::max()) {
		throw std::runtime_error("run out of active_id");
	}
	std::unique_lock<std::shared_mutex> frozen_unique(_frozen_mutex);
	_frozen_lists.push_back(_active_list);
	_frozen_ids.push_back(_active_id);
	_active_id++;
	_frozen_bytes += _active_bytes;
	_active_bytes = 0;
	_active_list = std::make_shared<SkipList>(_comparator);
	return std::make_pair(_frozen_ids.back(), _active_id);
}

/**
主要是减少锁开销, 内部还是多次调用单次的skiplist的接口, 但是不用频繁加锁
return: Always true
*/
bool MemTable::put_batch(std::vector<std::pair<std::string, std::string>> kvs,
						 std::vector<std::uint64_t> commit_sequences) {
	std::unique_lock<std::shared_mutex> active_unique(_active_mutex);
	for (int i = 0; i < kvs.size(); i++) {
		std::string key = kvs[i].first, value = kvs[i].second;
		std::uint64_t commit_sequence = commit_sequences[i];
		std::optional<std::string> value_opt = get_precise_single_skiplist(
			_active_list, key, commit_sequence, ValueType::Value);
		bool res =
			put_single_skiplist(_active_list, key, value, commit_sequence);
		if (res) {
			// res == true: 添加
			// 更新 active_bytes
			_active_bytes += key.size() + value.size() + SEQ_AND_VALTYPE_SIZE;
			if (_active_bytes > _max_active_bytes) {
				froze_with_active_unique();
			}
		} else {
			// res == false: 更新
			// 理论上正常情况好像走不到这里, 走的都是添加的逻辑
			// 这种情况下find_it一定不是nullptr
			int old_value_bytes = value_opt.value().size(),
				cur_size = value.size();
			_active_bytes += (cur_size - old_value_bytes);
			if (_active_bytes > _max_active_bytes) {
				froze_with_active_unique();
			}
		}
	}
	return true;
}

void MemTable::put_entry_data(const std::string &enc_key,
							  const std::string &value) {
	std::unique_lock<std::shared_mutex> active_unique(_active_mutex);
	_active_bytes += enc_key.size() + value.size();
	put_single_skiplist(_active_list, enc_key, value);
	if (_active_bytes > _max_active_bytes) { froze_with_active_unique(); }
}

/**
主要是减少锁开销, 内部还是多次调用单次的skiplist的接口, 但是不用频繁加锁
*/
std::vector<LookUpResult>
MemTable::get_batch(std::vector<std::string> ks,
					std::vector<std::uint64_t> commit_sequences) {
	std::vector<LookUpResult> res(ks.size());
	for (int i = 0; i < res.size(); i++) {
		res[i] = LookUpResult(LookUpResult::EMPTY_VALUE,
							  LookUpResult::EMPTY_COMSEQ, NotFound);
	}
	{
		std::shared_lock<std::shared_mutex> active_shared(_active_mutex);
		for (int i = 0; i < ks.size(); i++) {
			SkipList::ConstIterator find_it = get_lower_bound_single_skiplist(
				_active_list, ks[i], commit_sequences[i]);
			if (find_it.is_valid()) {
				InternalKey intr_key = InternalKey::decode(find_it.key());
				if (intr_key._key == ks[i]) {
					res[i]._value = find_it.value();
					res[i]._value_type = intr_key._value_type;
					res[i]._commit_sequence = intr_key._commit_sequence;
				}
			}
		}
	}
	{
		std::shared_lock<std::shared_mutex> frozen_shared(_frozen_mutex);
		for (int i = 0; i < ks.size(); i++) {
			if (res[i]._value_type == NotFound) {
				for (auto it = _frozen_lists.rbegin();
					 it != _frozen_lists.rend(); it++) {
					SkipList::ConstIterator find_it =
						get_lower_bound_single_skiplist(*it, ks[i],
														commit_sequences[i]);
					if (find_it.is_valid()) {
						InternalKey intr_key =
							InternalKey::decode(find_it.key());
						if (intr_key._key == ks[i]) {
							res[i]._value = find_it.value();
							res[i]._value_type = intr_key._value_type;
							res[i]._commit_sequence = intr_key._commit_sequence;
							break;
						}
					}
				}
			}
		}
	}
	return res;
}

/**
主要是减少锁开销, 内部还是多次调用单次的skiplist的接口, 但是不用频繁加锁
return: Always true
*/
bool MemTable::remove_batch(std::vector<std::string> ks,
							std::vector<std::uint64_t> commit_sequences) {
	std::unique_lock<std::shared_mutex> active_unique(_active_mutex);
	for (int i = 0; i < ks.size(); i++) {
		remove_single_skiplist(_active_list, ks[i], commit_sequences[i]);
		_active_bytes +=
			ks[i].size() + EMPTY_VALUE.size() + SEQ_AND_VALTYPE_SIZE;
	}
	if (_active_bytes > _max_active_bytes) { froze_with_active_unique(); }
	return true;
}


HeapIterator::HeapItem::HeapItem(const std::string &key,
								 const std::string &value,
								 std::uint64_t commit_sequence,
								 ValueType value_type)
	: _key(key), _value(value), _commit_sequence(commit_sequence),
	  _value_type(value_type) {}


HeapIterator::HeapIterator(bool skip_tombstone, bool keep_all_version)
	: _skip_tombstone(skip_tombstone), _commit_sequence(MAX_COMMIT_SEQUENCE),
	  _keep_all_version(keep_all_version) {}

HeapIterator::HeapIterator(bool skip_tombstone,
						   std::optional<std::uint64_t> commit_sequence,
						   bool keep_all_version)
	: _skip_tombstone(skip_tombstone), _commit_sequence(MAX_COMMIT_SEQUENCE),
	  _keep_all_version(keep_all_version) {
	if (commit_sequence != std::nullopt) {
		_commit_sequence = commit_sequence.value();
	}
}

HeapIterator::HeapIterator(bool skip_tombstone, std::uint64_t commit_sequence,
						   bool keep_all_version)
	: _skip_tombstone(skip_tombstone), _commit_sequence(commit_sequence),
	  _keep_all_version(keep_all_version) {}

/**
Compare定义的是 "比对方小/大" 的意思
std::less/std::greater 定义的是 "选用大顶堆/小顶堆"
	如果选用less, 那么 a < b 返回true时, 交换a, b, 因此堆顶最大 --> 大顶堆
	greater反之 --> 小顶堆
*/
bool HeapIterator::HeapItem::operator>(const HeapItem &other) const {
	InternalKey left(_key, _commit_sequence, _value_type),
		right(other._key, other._commit_sequence, other._value_type);
	return _comparator(InternalKey::encode(left), InternalKey::encode(right)) >
		   0;
}

HeapIterator &HeapIterator::clean_by_commit_sequence() {
	while (!_heap.empty() && _heap.top()._commit_sequence > _commit_sequence) {
		_heap.pop();
	}
	return *this;
}

void HeapIterator::put(const std::string &user_key, const std::string &value,
					   std::uint64_t commit_sequence, ValueType value_type) {
	HeapItem item(user_key, value, commit_sequence, value_type);
	_heap.push(item);
}

void HeapIterator::put(const std::string &enc_key, const std::string &value) {
	InternalKey intr_key = InternalKey::decode(enc_key);
	put(intr_key._key, value, intr_key._commit_sequence, intr_key._value_type);
}

HeapIterator &HeapIterator::operator++() {
	if (_heap.empty()) { return *this; }
	HeapItem top = _heap.top();
	_heap.pop();
	if (!_keep_all_version) {
		while (!_heap.empty() && _heap.top()._key == top._key) {
			_heap.pop();
		}
	}
	clean_after_operate();
	// clean_by_commit_sequence();
	// if (_skip_tombstone) { clean_tombstone(); }
	return *this;
}

HeapIterator MemTable::begin(std::optional<std::uint64_t> commit_sequence_opt,
							 bool keep_all_version, bool skip_tombston) const {
	std::uint64_t commit_sequence = MAX_COMMIT_SEQUENCE;
	if (commit_sequence_opt != std::nullopt) {
		commit_sequence = commit_sequence_opt.value();
	}
	HeapIterator heap_iterator(skip_tombston, commit_sequence,
							   keep_all_version);
	{
		std::shared_lock<std::shared_mutex> active_shared(_active_mutex);
		for (SkipList::ConstIterator it = _active_list->begin();
			 it != _active_list->end(); ++it) {
			InternalKey intr_key = InternalKey::decode(it.key());
			HeapIterator::HeapItem item(intr_key._key, it.value(),
										intr_key._commit_sequence,
										intr_key._value_type);
			heap_iterator._heap.push(item);
		}
	}
	{
		std::shared_lock<std::shared_mutex> frozen_shared(_frozen_mutex);
		for (std::shared_ptr<const SkipList> lists_ptr : _frozen_lists) {
			for (SkipList::ConstIterator it = lists_ptr->begin();
				 it != lists_ptr->end(); ++it) {
				InternalKey intr_key = InternalKey::decode(it.key());
				HeapIterator::HeapItem item(intr_key._key, it.value(),
											intr_key._commit_sequence,
											intr_key._value_type);
				heap_iterator._heap.push(item);
			}
		}
	}
	// heap_iterator.clean_by_commit_sequence();
	// // 如果value_type是tombstone, 那么弹掉这个key的所有sequence版本
	// if (heap_iterator._skip_tombstone) { heap_iterator.clean_tombstone(); }
	heap_iterator.clean_after_operate();
	return heap_iterator;
}

HeapIterator &HeapIterator::clean_tombstone() {
	// 如果value_type是tombstone, 那么弹掉这个key的所有sequence版本
	while (!_heap.empty() && _heap.top()._value_type != Value) {
		std::string last_tomb_key = _heap.top()._key;
		while (!_heap.empty() && _heap.top()._key == last_tomb_key) {
			_heap.pop();
		}
		clean_by_commit_sequence();
	}
	return *this;
}

// 在put完成之后进行清理, 依据commit_sequence以及skip_tomb_stone
void HeapIterator::clean_after_operate() {
	clean_by_commit_sequence();
	if (_skip_tombstone) { clean_tombstone(); }
}

void HeapIterator::set_skip_tombstone(bool skip_tombstone) {
	_skip_tombstone = skip_tombstone;
}
void HeapIterator::set_keep_all_version(bool keep_all_version) {
	_keep_all_version = keep_all_version;
}

HeapIterator MemTable::end() const { return HeapIterator(true); }
bool HeapIterator::operator==(const BaseIterator &other) const {
	return !((*this) != other);
}
/**
只有this, other都为空才返回false, 其余情况都返回true
*/
bool HeapIterator::operator!=(const BaseIterator &other) const {
	const HeapIterator *other_ptr = dynamic_cast<const HeapIterator *>(&other);
	if (other_ptr == nullptr) { return true; }
	return !(_heap.empty() && other_ptr->_heap.empty());
}

std::pair<std::string, std::string> HeapIterator::operator*() const {
	assert(!_heap.empty());
	HeapItem heap_item = _heap.top();
	std::string enc_key = InternalKey::encode(
					InternalKey(heap_item._key, heap_item._commit_sequence,
								heap_item._value_type)),
				value = heap_item._value;
	return std::make_pair(enc_key, value);
}

bool HeapIterator::is_valid() const { return !_heap.empty(); }

// 主要是查询前缀, 因此如果需要进位可以直接将低位pop掉
std::string MemTable::get_next_str(const std::string &str) {
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

HeapIterator MemTable::begin_preffix(const std::string &key,
									 std::uint64_t commit_sequence) const {
	HeapIterator heap_it(true, commit_sequence);
	{
		std::shared_lock<std::shared_mutex> active_shared(_active_mutex);
		std::string begin_key =
			InternalKey::encode(InternalKey(key, commit_sequence, Value));
		SkipList::ConstIterator beg = _active_list->lower_bound(begin_key);

		std::string next_usr_key = get_next_str(key);
		SkipList::ConstIterator end = _active_list->end();
		if (next_usr_key != "") {
			std::string end_key = InternalKey::encode(
				InternalKey(next_usr_key, commit_sequence, Value));
			end = _active_list->lower_bound(end_key);
		}

		for (auto it = beg; it != end; ++it) {
			InternalKey intr_key = InternalKey::decode(it.key());
			HeapIterator::HeapItem item(intr_key._key, it.value(),
										intr_key._commit_sequence,
										intr_key._value_type);
			heap_it._heap.push(item);
		}
	}
	{
		std::shared_lock<std::shared_mutex> frozen_shared(_frozen_mutex);
		for (std::shared_ptr<const SkipList> frozen_list : _frozen_lists) {
			std::string enc_key =
				InternalKey::encode(InternalKey(key, commit_sequence, Value));
			SkipList::ConstIterator beg = frozen_list->lower_bound(enc_key);

			std::string next_usr_key = get_next_str(key);
			SkipList::ConstIterator end = frozen_list->end();
			if (next_usr_key != "") {
				std::string end_key = InternalKey::encode(
					InternalKey(next_usr_key, commit_sequence, Value));
				end = frozen_list->lower_bound(end_key);
			}

			for (auto it = beg; it != end; ++it) {
				InternalKey intr_key = InternalKey::decode(it.key());
				HeapIterator::HeapItem item(intr_key._key, it.value(),
											intr_key._commit_sequence,
											intr_key._value_type);
				heap_it._heap.push(item);
			}
		}
	}
	heap_it.clean_after_operate();
	// heap_it.clean_by_commit_sequence();
	// if (heap_it._skip_tombstone) { heap_it.clean_tombstone(); }
	return heap_it;
}

/**
这里的predicate中的参数string传入的是user_key,
而底层skiplist中的predicate需要一个能够判断enc_key的predicate
因此可以将这里的predicate包装一下传给skiplist

此外, 这里的commit_sequence只用于HeapIterator层的过滤,
底层的skiplist对commit_sequence透明, 无法过滤
*/
HeapIterator MemTable::iter_monotony_predicate(
	std::function<int(const std::string &)> predicate,
	std::uint64_t commit_sequence, bool skip_tombstone, bool keep_all_version) {
	std::function<int(const std::string &)> wrapper_predicate =
		[predicate](const std::string &enc_key) -> int {
		InternalKey intr_key = InternalKey::decode(enc_key);
		return predicate(intr_key._key);
	};
	HeapIterator heap_it(skip_tombstone, commit_sequence, keep_all_version);
	{
		std::shared_lock<std::shared_mutex> active_shared(_active_mutex);
		std::optional<
			std::pair<SkipList::ConstIterator, SkipList::ConstIterator>>
			res = _active_list->iter_monotony_predicate(wrapper_predicate);
		if (res != std::nullopt) {
			SkipList::ConstIterator begin_it = res->first, end_it = res->second;
			for (auto it = begin_it; it != end_it; ++it) {
				InternalKey intr_key = InternalKey::decode(it.key());
				heap_it._heap.push(HeapIterator::HeapItem(
					intr_key._key, it.value(), intr_key._commit_sequence,
					intr_key._value_type));
			}
		}
	}
	{
		std::shared_lock<std::shared_mutex> frozen_shared(_frozen_mutex);
		for (std::shared_ptr<const SkipList> frozen_list : _frozen_lists) {
			std::optional<
				std::pair<SkipList::ConstIterator, SkipList::ConstIterator>>
				res = frozen_list->iter_monotony_predicate(wrapper_predicate);
			if (res != std::nullopt) {
				SkipList::ConstIterator begin_it = res->first,
										end_it = res->second;
				for (auto it = begin_it; it != end_it; ++it) {
					InternalKey intr_key = InternalKey::decode(it.key());
					heap_it._heap.push(HeapIterator::HeapItem(
						intr_key._key, it.value(), intr_key._commit_sequence,
						intr_key._value_type));
				}
			}
		}
	}
	heap_it.clean_after_operate();
	// heap_it.clean_by_commit_sequence();
	// if (heap_it._skip_tombstone) { heap_it.clean_tombstone(); }
	return heap_it;
}

MemTable::FlushLastResult
MemTable::flush_last(SSTBuilder &builder, const std::string &sst_path,
					 std::uint64_t sst_id,
					 std::vector<std::uint64_t> &flushed_seqs) {
	std::optional<std::uint64_t> flush_id = std::nullopt,
								 new_active_id = std::nullopt;

	std::shared_ptr<const SkipList> last_list;
	int flag = 0;

	{
		std::unique_lock<std::shared_mutex> frozen_unique(_frozen_mutex);
		while (!_frozen_lists.empty() && _frozen_lists.front()->empty()) {
			_frozen_lists.pop_front();
			_frozen_ids.pop_front();
		}
	}

	{
		std::shared_lock<std::shared_mutex> frozen_shared(_frozen_mutex);
		if (!_frozen_lists.empty()) {
			flag = 1;
			last_list = _frozen_lists.front();
			// _frozen_lists.pop_front();
			flush_id = _frozen_ids.front();
			// _frozen_ids.pop_front();
			// _frozen_bytes -= last_list->get_byte_size();
		}
	}
	// std::shared_ptr 重载了与 nullptr
	// 的比较（operator==(std::nullptr_t)，C++11 起），last_list
	// == nullptr 判断的就是"这个 shared_ptr 当前是否持有指针"
	{
		std::shared_lock<std::shared_mutex> active_shared(_active_mutex);
		std::shared_lock<std::shared_mutex> frozen_shared(_frozen_mutex);
		if (last_list == nullptr) {
			if (!_frozen_lists.empty()) {
				flag = 1;
				last_list = _frozen_lists.front();
				// _frozen_lists.pop_front();
				flush_id = _frozen_ids.front();
				// _frozen_ids.pop_front();
				// _frozen_bytes -= last_list->get_byte_size();
			} else {
				if (!_active_list->empty()) {
					flag = 2;
					last_list = _active_list;
					// _active_bytes = 0;
					// _active_list = std::make_shared<SkipList>(_comparator);
					flush_id = _active_id;
					// _active_id++;
					// new_active_id = _active_id;
				}
			}
		}
	}

	if (last_list == nullptr) {
		// 此时说明frozen和active中都没有数据, 返回空
		return FlushLastResult(nullptr, std::nullopt, std::nullopt);
	}

	for (auto it = last_list->begin(); it != last_list->end(); ++it) {
		std::string enc_key = (*it).first, value = (*it).second;
		builder.add(InternalKey::decode(enc_key)._key, value,
					InternalKey::decode(enc_key)._commit_sequence,
					InternalKey::decode(enc_key)._value_type);
		flushed_seqs.push_back(InternalKey::decode(enc_key)._commit_sequence);
	}
	std::shared_ptr<SST> sst = builder.build(sst_id, sst_path);
	if (sst != nullptr) {
		if (flag == 1) {
			std::unique_lock<std::shared_mutex> frozen_unique(_frozen_mutex);
			_frozen_lists.pop_front();
			_frozen_ids.pop_front();
			_frozen_bytes -= last_list->get_byte_size();
		} else if (flag == 2) {
			std::unique_lock<std::shared_mutex> active_unique(_active_mutex);
			if (_active_id == std::numeric_limits<std::uint64_t>::max()) {
				throw std::runtime_error("run out of active_id");
			}
			_active_bytes = 0;
			_active_list = std::make_shared<SkipList>(_comparator);
			_active_id++;
			new_active_id = _active_id;
		}
	}
	return FlushLastResult(sst, flush_id, new_active_id);
}

std::size_t MemTable::get_total_size() {
	std::shared_lock<std::shared_mutex> active_shared(_active_mutex);
	std::shared_lock<std::shared_mutex> frozen_shared(_frozen_mutex);
	return _active_bytes + _frozen_bytes;
}

void MemTable::clear() {
	std::unique_lock<std::shared_mutex> active_unique(_active_mutex);
	std::unique_lock<std::shared_mutex> frozen_unique(_frozen_mutex);
	_active_list->clear();
	_active_bytes = 0;
	_frozen_lists.clear();
	_frozen_bytes = 0;
	_frozen_ids.clear();
	_active_id = 0;
}


MemTable::PutResult::PutResult(bool is_add)
	: _is_add(is_add), _frozen_list_id(std::nullopt),
	  _new_active_list_id(std::nullopt) {}
MemTable::PutResult::PutResult(bool is_add, std::uint64_t frozen_list_id,
							   std::uint64_t new_active_list_id)
	: _is_add(is_add), _frozen_list_id(frozen_list_id),
	  _new_active_list_id(new_active_list_id) {}

MemTable::RemoveResult::RemoveResult(bool is_success)
	: _is_success(is_success), _frozen_list_id(std::nullopt),
	  _new_active_list_id(std::nullopt) {}
MemTable::RemoveResult::RemoveResult(bool is_success,
									 std::uint64_t frozen_list_id,
									 std::uint64_t new_active_list_id)
	: _is_success(is_success), _frozen_list_id(frozen_list_id),
	  _new_active_list_id(new_active_list_id) {}

MemTable::FlushLastResult::FlushLastResult(
	std::shared_ptr<SST> sst, std::optional<std::uint64_t> flush_id,
	std::optional<std::uint64_t> new_active_id)
	: _sst(sst), _flush_id(flush_id), _new_active_id(new_active_id) {}

MemTable::FrozeResult::FrozeResult()
	: _frozen_list_id(std::nullopt), _new_active_list_id(std::nullopt) {}
MemTable::FrozeResult::FrozeResult(std::uint64_t frozen_list_id,
								   std::uint64_t new_active_list_id)
	: _frozen_list_id(frozen_list_id), _new_active_list_id(new_active_list_id) {
}