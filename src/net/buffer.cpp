//
// Created by test on 10/1/26.
//

#include "net/buffer.h"
#include "log.h"
#include <algorithm>

Buffer::Buffer() : _read_idx(0), _write_idx(0), _buffer(16) {}

void Buffer::move2front() {
	int count = _write_idx - _read_idx;
	for (int i = _read_idx; i < _write_idx; ++i) {
		_buffer[i - _read_idx] = _buffer[i];
	}
	_read_idx = 0;
	_write_idx = count;
}

void Buffer::add_capacity(int n) {
	if (_buffer.size() - _write_idx >= n) {
		return;
	} else if (_buffer.size() - _write_idx + _read_idx >= n) {
		// 整体前移, 不需要扩容vector
		move2front();
	} else {
		// 整体前移不够, 还需要扩容vector
		move2front();
		int rest_size = _buffer.size() - _write_idx;
		_buffer.resize(n - rest_size + _buffer.size());
	}
}

int Buffer::get_readable_size() const { return _write_idx - _read_idx; }

std::optional<std::vector<char>> Buffer::read(int size) {
	if (size < 0) { return std::nullopt; }
	int readable_size = get_readable_size();
	if (size > readable_size) { return std::nullopt; }
	std::vector<char> data(_buffer.begin() + _read_idx,
						   _buffer.begin() + _read_idx + size);
	_read_idx += size;
	return data;
}
void Buffer::append(const std::vector<char> &data) {
	add_capacity(data.size());
	std::copy(data.begin(), data.end(), _buffer.begin() + _write_idx);
	_write_idx += data.size();
}

void Buffer::append(const char *data, int size) {
	if (size < 0) {
		log_error("append size must be non-negative");
		return;
	}
	add_capacity(size);
	std::copy(data, data + size, _buffer.begin() + _write_idx);
	_write_idx += size;
}

std::string_view Buffer::peek() {
	return std::string_view(_buffer.data() + _read_idx, _write_idx - _read_idx);
}

bool Buffer::erase(int size) {
	if (size < 0) { return false; }
	if (get_readable_size() < size) { return false; }
	_read_idx += size;
	return true;
}