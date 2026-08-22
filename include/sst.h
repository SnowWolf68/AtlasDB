#pragma once
#include "Block.h"
#include "BlockMeta.h"
#include "Record.h"
#include "util.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

class SST;
class SSTIterator;

class SSTBuilder {
public:
	static constexpr std::uint8_t MAGIC_NUM = 0b01111110;

	bool add(const std::string &user_key, const std::string &value,
			 std::uint64_t commit_sequence, ValueType value_type);
	bool add(const std::string &enc_key, const std::string &value);

	std::shared_ptr<SST> build(std::uint64_t sst_id, const std::string &path);
	std::size_t get_estimated_size();
	bool empty();

private:
	Block _block;
	std::vector<BlockMeta> _block_metas;
	std::vector<std::uint8_t> _data;
	std::string _first_key;
	std::string _last_key;

	bool flush_block(bool with_crc);
};

class SST : public std::enable_shared_from_this<SST> {
public:
	SST(FileObj file, std::vector<BlockMeta> block_metas,
		std::uint32_t meta_section_offset, std::uint64_t sst_id,
		const std::string &first_key, const std::string &last_key);

	static std::shared_ptr<SST> open(const std::string &path,
									 std::uint64_t sst_id);
	std::optional<SSTIterator> get(const std::string &enc_key) const;

	std::shared_ptr<Block> read_block(std::size_t block_idx) const;

	std::size_t num_blocks() const;
	std::string get_first_key() const;
	std::string get_last_key() const;
	std::uint64_t get_sst_id() const;

	SSTIterator begin(std::optional<std::uint64_t> commit_sequence,
					  bool keep_all_version) const;
	SSTIterator end() const;

	std::optional<std::pair<SSTIterator, SSTIterator>>
	iter_monotony_predicate(std::function<int(const std::string &)> predicate,
							std::optional<std::uint64_t> commit_sequence);
	std::shared_ptr<Comparator<std::string>> get_comparator() const;
	// 删除磁盘上的SST文件
	bool delete_file();
private:
	FileObj _file;
	std::vector<BlockMeta> _block_metas;
	std::uint32_t _meta_section_offset;
	std::uint64_t _sst_id;
	std::string _first_key;
	std::string _last_key;
	std::shared_ptr<Comparator<std::string>> _comparator;

	std::optional<std::uint64_t> locate_block(const std::string &enc_key) const;
};

class SSTIterator {
	friend class SST;

public:
	SSTIterator &operator++();

    // <enc_key, value>
	std::pair<std::string, std::string> operator*() const;
	bool operator==(const SSTIterator &other) const;
	bool operator!=(const SSTIterator &other) const;
	bool is_valid() const;
    static SSTIterator invalid_iter();


private:
	SSTIterator(std::shared_ptr<const SST> sst, std::size_t block_idx,
				BlockIterator block_iter);

	void fresh_by_comseq();

	std::shared_ptr<const SST> _sst;
	std::size_t _block_idx;
	BlockIterator _block_iter;
};
