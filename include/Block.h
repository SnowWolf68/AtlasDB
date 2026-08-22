#pragma once
#include "Comparator.h"
#include <cstddef>
#include <cstdint> // std::uint64_t, std::uint8_t
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class BlockIterator;

/**
data + offset (each 2B) + num_of_elements (2B) + CRC (4B) (with_hash = true)
*/
class Block : public std::enable_shared_from_this<Block> {
	friend class BlockIterator;

public:
	static const std::size_t MAX_VALUE_LEN = 64 * 1024;
	static const std::size_t DEFAULT_MAX_CAPACITY = 32 * 1024;
	static const std::size_t DEFAULT_KEY_LEN_SIZE = 2;
	static const std::size_t DEFAULT_VALUE_LEN_SIZE = 2;

	Block();

	Block(std::vector<uint8_t> data, std::vector<uint16_t> offsets,
		  std::size_t cur_size);

	bool add_entry(const std::string &enc_key, const std::string &value,
				   bool force);

	std::vector<std::uint8_t> encode(bool with_crc);

	static std::shared_ptr<Block> decode(std::vector<std::uint8_t> data,
										 bool with_crc);

	std::optional<std::pair<std::string, std::string>>
	get_value(const std::string &enc_key);

	std::optional<BlockIterator> get_exact(const std::string &enc_key);
	std::optional<BlockIterator> get_lower_bound(const std::string &enc_key);

	BlockIterator
	begin(std::optional<std::uint64_t> commit_sequence = std::nullopt,
		  bool keep_all_version = false);
	BlockIterator end();

	std::pair<BlockIterator, BlockIterator>
	preffix(const std::string &user_key,
			std::optional<std::uint64_t> commit_sequence);

	static std::string get_next_str(const std::string &str);

	std::optional<std::pair<BlockIterator, BlockIterator>>
	iter_monotony_predicate(std::function<int(const std::string &)> predicate,
							std::optional<std::uint64_t> commit_sequence);

	std::string get_first_key();
	bool is_first_key();
	bool empty();
	std::size_t size() const;
	std::size_t data_size() const;


private:
	std::vector<std::uint8_t> _data;
	std::vector<std::uint16_t> _offsets;
	std::size_t _cur_size;
	std::shared_ptr<Comparator<std::string>> _comparator;


	static std::vector<std::uint8_t> get_bytes(const std::string &enc_key,
											   const std::string &value);

	static std::vector<uint8_t> get_bytes_of_uint16t(std::uint16_t num);
	static std::vector<uint8_t> get_bytes_of_uint32t(std::uint32_t num);

	static std::pair<std::string, std::string>
	get_kv_at_offset(const std::vector<uint8_t> &data, std::size_t offset);

	std::optional<std::size_t> get_idx_by_enc_key(const std::string &enc_key);
	std::optional<std::size_t>
	get_idx_by_enc_key_lower_bound(const std::string &enc_key);
};

class BlockIterator {
	friend class Block;
	friend class SSTIterator;
	friend class SST;

public:
	BlockIterator &operator++();

	// <enc_key, value>
	std::pair<std::string, std::string> operator*() const;
	bool operator==(const BlockIterator &other) const;
	bool operator!=(const BlockIterator &other) const;

	std::optional<std::uint64_t> get_commit_sequence();
	bool get_keep_all_version();
	bool is_valid() const;
	std::size_t get_cur_idx() const;

	static BlockIterator invalid_iter();

private:
	BlockIterator(std::shared_ptr<Block> block, std::size_t cur_idx,
				  std::optional<std::uint64_t> commit_sequence,
				  bool keep_all_version = false);

	BlockIterator &clean_by_commit_sequence();

	std::shared_ptr<Block> _block; // nullptr表示当前iter无效
	std::size_t _cur_idx;
	std::optional<std::uint64_t> _commit_sequence;
	bool _keep_all_version;
};