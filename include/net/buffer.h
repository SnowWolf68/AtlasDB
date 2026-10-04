//
// Created by test on 10/1/26.
//

#ifndef ATLASDB_BUFFER_H
#define ATLASDB_BUFFER_H

#include <optional>
#include <string_view>
#include <vector>

class Buffer {
public:
	Buffer();


	int get_readable_size() const;

	std::optional<std::vector<char>> read(int size);
	void append(const std::vector<char> &data);
	void append(const char *data, int size);

	std::string_view peek();

	// 清除指定字节数
	bool erase(int size);

private:
	void move2front();

	// 给当前buffer扩大n的容量
	void add_capacity(int n);

	int _read_idx;
	int _write_idx;
	std::vector<char> _buffer;
};

#endif // ATLASDB_BUFFER_H
