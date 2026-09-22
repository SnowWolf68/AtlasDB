#pragma once

#include "MemTable.h"
#include "util.h"
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class WAL {
	inline const static std::string DEFAULT_WAL_FILE_PREFFIX = "wal-";

public:
	static std::vector<std::uint8_t> encode(const std::string &enc_key,
											const std::string &value);
	static std::vector<std::uint8_t> encode_transaction(
		std::unordered_map<std::string, std::optional<std::string>> map,
		std::uint64_t commit_sequence);

	static std::vector<std::uint8_t>
	encode_transaction(const std::string &enc_key, const std::string &value);

	// static std::optional<std::pair<std::string, std::string>>
	// decode(std::vector<std::uint8_t> data);

	static bool delete_wal_by_id(const std::string &dir, std::uint64_t wal_id);

	WAL();
	WAL(const std::string &wal_dir, std::uint64_t wal_id);

	std::uint64_t load_wal(const std::string &dir, MemTable &memtable);
	bool append(const std::string &enc_key, const std::string &value);
	bool append(std::vector<std::uint8_t> data);
	bool clear();
	bool fsync();
	bool delete_();
	bool close();
	static std::string get_filepath(const std::string &dir, std::uint64_t id);

private:
	FileObj _file;

	bool check_id_str(const std::string &id_str);
	static std::vector<std::uint8_t> append_u8(std::vector<std::uint8_t> &data,
											   std::uint8_t u8);
	static std::vector<std::uint8_t> append_u16(std::vector<std::uint8_t> &data,
												std::uint16_t u16);
	static std::vector<std::uint8_t> append_u32(std::vector<std::uint8_t> &data,
												std::uint32_t u32);
	static std::vector<std::uint8_t> append_u64(std::vector<std::uint8_t> &data,
												std::uint8_t u64);
	static std::vector<std::uint8_t> append_str(std::vector<std::uint8_t> &data,
												const std::string &str);
};