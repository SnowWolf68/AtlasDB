#include "SkipList.h"
#include <stdexcept> // std::logic_error()
#include <unordered_map>

SkipListNode::SkipListNode(const std::string &key, const std::string &value,
						   int level_count)
	: _key(key), _value(value), _next(level_count) {}

SkipList::SkipList(std::shared_ptr<const Comparator<std::string>> cmp, int seed)
	: _head(SkipListNode::DEFAULT_KEY, SkipListNode::DEFAULT_VALUE, MAX_LEVEL),
	  _size(0), _engine(seed), _coin(COIN_PARAM), _cmp(cmp), _cur_bytes(0) {
	if (_cmp == nullptr) {
		throw std::invalid_argument("comparator cannot be null");
	}
}


SkipList::SkipList(const SkipList &other)
	: _head(SkipListNode::DEFAULT_KEY, SkipListNode::DEFAULT_VALUE, MAX_LEVEL),
	  _size(other._size), _engine(other._engine), _coin(other._coin),
	  _cmp(other._cmp), _cur_bytes(other._cur_bytes) {
	// 由于上面几层连接指针的时候会出现跳跃,
	// 因此导致可能后续节点还没创建就要连接 因此先创建level 0的所有节点
	const SkipListNode *p = other._head._next[0];
	SkipListNode *q = &_head;
	std::unordered_map<std::string, SkipListNode *> map; // key -> 新list节点
	while (p != nullptr) {
		q->_next[0] = new SkipListNode(p->_key, p->_value, p->_next.size());
		map[p->_key] = q->_next[0];
		p = p->_next[0];
		q = q->_next[0];
	}
	// 连接上层指针
	p = &(other._head);
	q = &_head;
	while (p != nullptr) {
		q->_next.resize(p->_next.size());
		for (int i = 1; i < p->_next.size(); i++) {
			SkipListNode *node = p->_next[i];
			if (node != nullptr) {
				q->_next[i] =
					map.at(node->_key); // 找不到会报错, 用[]找不到会插入nullptr
			}
		}
		p = p->_next[0];
		q = q->_next[0];
	}
}

/**
swap()
的语义是交换两个已经存在、可以修改的对象，本身不要求参数必须是右值，所以应该接收普通左值引用。
*/
void SkipList::swap(SkipList &other) {
	std::swap(this->_head, other._head);
	std::swap(this->_engine, other._engine);
	std::swap(this->_coin, other._coin);
	std::swap(this->_size, other._size);
	std::swap(this->_cmp, other._cmp);
	std::swap(_cur_bytes, other._cur_bytes);
}

SkipList::SkipList(SkipList &&other)
	: _head(SkipListNode::DEFAULT_KEY, SkipListNode::DEFAULT_VALUE, MAX_LEVEL),
	  _size(0), _engine(SEED_PARAM), _coin(COIN_PARAM), _cmp(other._cmp),
	  _cur_bytes(0) {
	// 虽然参数类型是右值引用, 但只要它有名字, 表达式 other 本身就是左值。
	swap(other);
}

SkipList &SkipList::operator=(const SkipList &other) {
	if (this == &other) { return *this; }
	// copy and swap, 利用到拷贝构造, 减少冗余代码
	SkipList temp(other);
	swap(temp);
	return *this;
}

SkipList &SkipList::operator=(SkipList &&other) {
	if (this == &other) { return *this; }
	swap(other);
	return *this;
}

/**
find_or_prev不会用head与要查询的key作比较, 但是find_or_prev有可能会返回head
因此如果find_or_prev返回的key和find要查询的key相同时,
需要看一下find_or_prev返回的是不是head
*/
SkipListNode *SkipList::find(const std::string &key) {
	SkipListNode *res = find_or_prev(key);
	if (res != &_head && // 由于res有可能是_head, 如果对_head.key = ""
						 // 调用comparator会报错,
						 // 因此需要将与_head的比较放在前面
		(*_cmp)(res->_key, key) == 0) {
		return res;
	} else {
		return nullptr;
	}
}

/**
find_or_prev不会用head与要查询的key作比较
Note: 但是有可能会返回_head
*/
SkipListNode *SkipList::find_or_prev(const std::string &key) {
	int level_idx = _head._next.size() - 1;
	SkipListNode *p = &_head;
	while (level_idx > 0) {
		while (p->_next[level_idx] != nullptr &&
			   (*_cmp)(p->_next[level_idx]->_key, key) < 0) {
			p = p->_next[level_idx];
		}
		if (p->_next[level_idx] != nullptr &&
			(*_cmp)(p->_next[level_idx]->_key, key) == 0) {
			break;
		}
		level_idx--;
	}
	if (level_idx != 0) {
		return p->_next[level_idx];
	} else {
		while (p->_next[level_idx] != nullptr &&
			   (*_cmp)(p->_next[level_idx]->_key, key) < 0) {
			p = p->_next[level_idx];
		}
		if (p->_next[level_idx] != nullptr &&
			(*_cmp)(p->_next[level_idx]->_key, key) == 0) {
			return p->_next[level_idx];
		} else {
			return p;
		}
	}
}

/**
前 MAX_LEVEL 个元素：从最高层到第0层的前驱
如果找到 key：最后额外追加目标节点

find_with_path也不会把_head的key和要查询的key作比较
Note: 返回的vector中有可能会有_head
*/
std::vector<SkipListNode *> SkipList::find_with_path(const std::string &key) {
	std::vector<SkipListNode *> vec;
	int level_idx = _head._next.size() - 1;
	SkipListNode *p = &_head;
	while (level_idx >= 0) {
		while (p->_next[level_idx] != nullptr &&
			   (*_cmp)(p->_next[level_idx]->_key, key) < 0) {
			p = p->_next[level_idx];
		}
		vec.push_back(p);
		level_idx--;
	}
	if (vec.back()->_next[0] != nullptr &&
		(*_cmp)(vec.back()->_next[0]->_key, key) == 0) {
		vec.push_back(vec.back()->_next[0]);
	}
	return vec;
}

int SkipList::get_level(int max_level) {
	int level = 1;
	while (level + 1 <= max_level && _coin(_engine)) {
		level++;
	}
	return level;
}

/**
res:
	-false: 更新
	-true: 添加
*/
bool SkipList::put(std::string key, std::string value) {
	std::vector<SkipListNode *> vec = find_with_path(key);
	SkipListNode *find_res = vec[vec.size() - 1];
	if (find_res != &_head && (*_cmp)(find_res->_key, key) == 0) {
		// 相同key, 更新value
		std::int64_t diff = static_cast<std::int64_t>(value.size()) -
							static_cast<std::int64_t>(find_res->_value.size());
		_cur_bytes += diff;
		find_res->_value = value;
		return false;
	} else {
		// insert, find_res为其前驱, find_res有可能是head, 但是没有影响
		int new_node_level = get_level(SkipList::MAX_LEVEL);
		SkipListNode *new_node = new SkipListNode(key, value, new_node_level);
		new_node->_next[0] = find_res->_next[0];
		find_res->_next[0] = new_node; // level_idx = 0修改完毕
		// 修改level_idx > 0的层
		for (int i = 1; i < new_node_level; i++) {
			// 修改level_idx = i的层
			int prev_idx = static_cast<int>(vec.size()) - i - 1;
			new_node->_next[i] = vec[prev_idx]->_next[i];
			vec[prev_idx]->_next[i] = new_node;
		}
		_size++;
		_cur_bytes += key.size() + value.size();
		return true;
	}
}


bool SkipList::get(const std::string &key, std::string &value) {
	SkipListNode *find_res = find(key);
	if (find_res != nullptr) {
		value = find_res->_value;
		return true;
	} else {
		return false;
	}
}

bool SkipList::del(const std::string &key) {
	std::vector<SkipListNode *> vec = find_with_path(key);
	if (vec.back() == &_head || (*_cmp)(vec.back()->_key, key) != 0) {
		return false;
	} else {
		for (int i = 0; i < vec.size() - 1; i++) {
			int level_idx = MAX_LEVEL - 1 - i;
			if (vec[i]->_next[level_idx] != nullptr &&
				(*_cmp)(vec[i]->_next[level_idx]->_key, key) == 0) {
				vec[i]->_next[level_idx] =
					vec[i]->_next[level_idx]->_next[level_idx];
			}
		}
		std::string value = vec.back()->_value;
		delete vec.back();
		_cur_bytes -= key.size() + value.size();
		_size--;
		return true;
	}
}

/**
第一个 >= key 的node
*/
const SkipListNode *SkipList::lower_bound_node(const std::string &key) const {
	int level_idx = _head._next.size() - 1;
	const SkipListNode *p = &_head;
	while (level_idx >= 0) {
		while (p->_next[level_idx] != nullptr &&
			   (*_cmp)(p->_next[level_idx]->_key, key) < 0) {
			p = p->_next[level_idx];
		}
		level_idx--;
	}
	return p->_next[0];
}

SkipList::ConstIterator SkipList::lower_bound(const std::string &key) const {
	return ConstIterator(lower_bound_node(key), this);
}

SkipList::ConstIterator SkipList::begin() const {
	return ConstIterator(_head._next[0], this);
}
SkipList::ConstIterator SkipList::end() const {
	return ConstIterator(nullptr, this);
}

std::optional<std::pair<SkipList::ConstIterator, SkipList::ConstIterator>>
SkipList::iter_monotony_predicate(
	std::function<int(const std::string &)> predicate) const {
	const SkipListNode *p = &_head;
	int level_idx = _head._next.size() - 1;
	while (level_idx >= 0) {
		while (p->_next[level_idx] != nullptr &&
			   predicate(p->_next[level_idx]->_key) < 0) {
			p = p->_next[level_idx];
		}
		level_idx--;
	}
	// 第一个 predicate(p->next[0]->_key) >= 0 的位置
	ConstIterator begin_it(p->_next[0], this); // [begin_it, end_it)
	// 如果要查找的全部区间都在当前skiplist的key左侧,
	// 那么这里begin_it返回的是第一个数据节点(不在区间内)
	// 并且下面查出来的end_it也是第一个数据节点 (begin_it == end_it)
	// 此时仍然满足 [begin_it, end_it) 的要求, 没有问题

	p = &_head;
	level_idx = _head._next.size() - 1;
	while (level_idx >= 0) {
		while (p->_next[level_idx] != nullptr &&
			   predicate(p->_next[level_idx]->_key) <= 0) {
			p = p->_next[level_idx];
		}
		level_idx--;
	}
	// 第一个 predicate(p->next[0]->_key) > 0 的位置
	ConstIterator end_it(p->_next[0], this); // [begin_it, end_it)

	// 1. 区间在skiplist全部key的最左边:
	//      begin_it == end_it == 第一个有效数据位置
	// 2. 区间在skiplist全部key的最右边:
	//      begin_it == end_it == nullptr
	// 3. 如果整个skiplist为空: begin_it == end_it == nullptr
	if (begin_it == end_it) {
		return std::nullopt;
	} else {
		return std::make_pair(begin_it, end_it);
	}
}

// 主要是查询前缀, 因此如果需要进位可以直接将低位pop掉
std::string SkipList::get_next_str(const std::string &str) {
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

// [start, end)
std::optional<std::pair<SkipList::ConstIterator, SkipList::ConstIterator>>
SkipList::preffix_raw_string(const std::string &key) {
	SkipListNode *p = &_head;
	int level_idx = _head._next.size() - 1;
	while (level_idx >= 0) {
		while (p->_next[level_idx] != nullptr &&
			   (*_cmp)(p->_next[level_idx]->_key, key) < 0) {
			p = p->_next[level_idx];
		}
		level_idx--;
	}
	ConstIterator begin_it(p->_next[0], this);

	std::string next_key = get_next_str(key);
	if (next_key == "") {
		ConstIterator end_it = end();
		return std::make_pair(begin_it, end_it);
	}

	p = &_head;
	level_idx = _head._next.size() - 1;
	while (level_idx >= 0) {
		while (p->_next[level_idx] != nullptr &&
			   (*_cmp)(p->_next[level_idx]->_key, next_key) <= 0) {
			p = p->_next[level_idx];
		}
		level_idx--;
	}
	ConstIterator end_it(p->_next[0], this);

	if (begin_it == end_it) {
		return std::nullopt;
	} else {
		return std::make_pair(begin_it, end_it);
	}
}

std::size_t SkipList::size() { return _size; }

std::size_t SkipList::get_byte_size() const { return _cur_bytes; }

bool SkipList::empty() { return _size == 0; }

void SkipList::clear() {
	SkipListNode *p = _head._next[0];
	while (p != nullptr) {
		SkipListNode *q = p->_next[0];
		delete p;
		p = q;
	}
	for (int i = 0; i < _head._next.size(); i++) {
		_head._next[i] = nullptr;
	}
	_size = 0;
	_cur_bytes = 0;
}

SkipList::~SkipList() { clear(); }


SkipList::ConstIterator::ConstIterator(const SkipListNode *node,
									   const SkipList *owner)
	: _cur_node(node), _owner(owner) {}

bool SkipList::ConstIterator::is_valid() const { return _cur_node != nullptr; }

SkipList::ConstIterator &SkipList::ConstIterator::operator++() {
	if (is_valid()) { _cur_node = _cur_node->_next[0]; }
	return *this;
}

bool SkipList::ConstIterator::operator==(const ConstIterator &other) const {
	return other._owner == _owner && other._cur_node == _cur_node;
}

bool SkipList::ConstIterator::operator!=(const ConstIterator &other) const {
	return !(other._owner == _owner && other._cur_node == _cur_node);
}

const std::string &SkipList::ConstIterator::key() const {
	if (!is_valid()) { throw std::logic_error("iterator not valid!"); }
	return _cur_node->_key;
}

const std::string &SkipList::ConstIterator::value() const {
	if (!is_valid()) { throw std::logic_error("iterator not valid!"); }
	return _cur_node->_value;
}

std::pair<std::string, std::string> SkipList::ConstIterator::operator*() const {
	if (!is_valid()) { throw std::logic_error("iterator not valid!"); }
	return std::pair<std::string, std::string>(_cur_node->_key,
											   _cur_node->_value);
}
