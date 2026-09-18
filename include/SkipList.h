#pragma once
#include "Comparator.h"
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <utility> // std::swap(), std::pair<>
#include <vector>  // std::shared_ptr


class SkipListNode {
public:
	inline static const std::string DEFAULT_KEY = "";
	inline static const std::string DEFAULT_VALUE = "";


	SkipListNode(const std::string &key, const std::string &value,
				 int level_count);

	std::string _key;
	std::string _value;
	std::vector<SkipListNode *> _next;
};

class SkipList {
public:
	class ConstIterator {
		friend class SkipList;

	public:
		bool is_valid() const;
		ConstIterator &operator++();
		bool operator==(const ConstIterator &other) const;
		bool operator!=(const ConstIterator &other) const;
		const std::string &key() const;
		const std::string &value() const;
		std::pair<std::string, std::string> operator*() const;




	private:
		ConstIterator(const SkipListNode *node,
					  const SkipList *owner); // 私有构造, 只有SkipList能够构造.

		const SkipListNode *_cur_node;
		const SkipList *_owner;
	};


	SkipList(std::shared_ptr<const Comparator<std::string>> cmp =
				 std::make_shared<DefaultStringComparator>(),
			 int seed = SEED_PARAM);
	SkipList(const SkipList &other);
	void swap(SkipList &other);
	SkipList(SkipList &&other);
	SkipList &operator=(const SkipList &other);
	SkipList &operator=(SkipList &&other);

	bool put(std::string key, std::string value);
	bool get(const std::string &key, std::string &value) const;
	bool del(const std::string &key);
	ConstIterator lower_bound(const std::string &key) const;

	ConstIterator begin() const;
	ConstIterator end() const;

	std::optional<std::pair<ConstIterator, ConstIterator>>
	iter_monotony_predicate(
		std::function<int(const std::string &)> predicate) const;

	std::string get_next_str(const std::string &str);

	std::optional<std::pair<SkipList::ConstIterator, SkipList::ConstIterator>>
	preffix_raw_string(const std::string &key);


	std::size_t size();
	std::size_t get_byte_size() const;
	bool empty() const;
	void clear();
	~SkipList();

private:
	static constexpr int MAX_LEVEL = 16;
	static constexpr double COIN_PARAM = 0.5;
	static constexpr int SEED_PARAM = 12345;


	SkipListNode _head;
	std::size_t _size;

	std::mt19937 _engine;
	std::bernoulli_distribution _coin;

	std::shared_ptr<const Comparator<std::string>> _cmp;
	std::size_t _cur_bytes;


	int get_level(int max_level);

	const SkipListNode *find_or_prev(const std::string &key) const;
	std::vector<SkipListNode *> find_with_path(const std::string &key);
	const SkipListNode *find(const std::string &key) const;
	const SkipListNode *lower_bound_node(const std::string &key) const;
};