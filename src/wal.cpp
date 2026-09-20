#include "wal.h"
#include "MemTable.h"
#include "Record.h"
#include "util.h"
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <utility>
#include <vector>
#include <zlib.h>

/**
enc_key_len(2B) + enc_key + value_len(2B) + value + crc(4B)
*/
std::vector<std::uint8_t> WAL::encode(const std::string &enc_key,
									  const std::string &value) {
	std::vector<std::uint8_t> data;
	std::vector<std::uint8_t> enc_key_len_bytes =
								  get_bytes_of_uint16t(enc_key.size()),
							  value_len_bytes =
								  get_bytes_of_uint16t(value.size());
	data.insert(data.end(), enc_key_len_bytes.begin(), enc_key_len_bytes.end());
	data.insert(data.end(), enc_key.begin(), enc_key.end());
	data.insert(data.end(), value_len_bytes.begin(), value_len_bytes.end());
	data.insert(data.end(), value.begin(), value.end());
	std::uint32_t crc = crc32(0, data.data(), data.size());
	std::vector<std::uint8_t> crc_bytes = get_bytes_of_uint32t(crc);
	data.insert(data.end(), crc_bytes.begin(), crc_bytes.end());
	return data;
}
// std::optional<std::pair<std::string, std::string>>
// WAL::decode(std::vector<std::uint8_t> data) {
// 	try {
// 		if (data.size() < 8) { return std::nullopt; }
// 		auto it = data.begin();
// 		std::uint16_t enc_key_len = decode_to_uint16(it, it + 2);
// 		it += 2;
// 		std::string enc_key(it, it + enc_key_len);
// 		it += enc_key_len;
// 		std::uint16_t value_len = decode_to_uint16(it, it + 2);
// 		it += 2;
// 		std::string value(it, it + value_len);
// 		it += value_len;
// 		std::uint32_t crc = decode_to_uint32(it, it + 4),
// 					  cur_crc = crc32(0, data.data(), data.size() - 4);
// 		if (crc != cur_crc) { return std::nullopt; }
// 		return std::make_pair(enc_key, value);
// 	} catch (const std::exception &e) {
// 		std::cerr << "wal decode error: " << e.what() << std::endl;
// 		return std::nullopt;
// 	}
// }

bool WAL::delete_wal_by_id(const std::string &dir, std::uint64_t wal_id) {
	std::string filepath = get_filepath(dir, wal_id);
	bool res = std::filesystem::remove(filepath);
	FileObj::fsync_parent_path(filepath);
	return res;
}

WAL::WAL() {}

WAL::WAL(const std::string &wal_dir, std::uint64_t wal_id) {
	std::string filepath = get_filepath(wal_dir, wal_id);
	if (std::filesystem::exists(filepath)) {
		_file = FileObj::open_rw(filepath);
		if (!_file.is_valid()) {
			throw std::runtime_error("file: " + filepath + " open_rw failed");
		}
	} else {
		if (!std::filesystem::exists(wal_dir)) {
			std::filesystem::create_directories(wal_dir);
			FileObj::fsync_parent_path(wal_dir);
		}
		_file = FileObj::create_rw(filepath);
		if (!_file.is_valid()) {
			throw std::runtime_error("file: " + filepath + " create_rw failed");
		}
		FileObj::fsync_parent_path(filepath);
	}
}

bool WAL::check_id_str(const std::string &id_str) {
	if (id_str.empty()) return false;
	if (id_str == "0") return true;
	if (id_str != "0" && id_str[0] == '0') return false;
	for (char c : id_str) {
		if (!(c >= '0' && c <= '9')) { return false; }
	}
	return true;
}

/**
扫描data_dir下面所有wal-开头的wal文件, 每一个wal文件恢复成一个frozen_list,
并连同其对应编号一起放到memtable中

return: max_commit_sequence
*/
std::uint64_t WAL::load_wal(const std::string &dir, MemTable &memtable) {
	std::uint64_t max_id = 0, max_commit_sequence = 0;
	std::deque<std::pair<SkipList, std::uint64_t>> pr_deque;
	for (auto &entry : std::filesystem::directory_iterator(dir)) {
		if (entry.is_regular_file()) {
			std::string filename = entry.path().filename();
			if (filename.find("wal-") != std::string::npos) {
				std::string id_str = filename.substr(4);
				if (!check_id_str(id_str)) { continue; }
				std::uint64_t id = std::stoull(id_str);
				if (id == std::numeric_limits<std::uint64_t>::max()) {
					throw std::runtime_error("run out of wal_id");
				}
				max_id = std::max(max_id, id);
				FileObj file = FileObj::open_read(entry.path());
				SkipList frozen_list(std::make_shared<InternalKeyComparator>());
				file.seek_begin();
				std::vector<std::uint8_t> enc_key_len_bytes =
					file.read_continue(2);
				while (enc_key_len_bytes.size() == 2) {
					std::uint16_t enc_key_len = decode_to_uint16(
						enc_key_len_bytes.begin(), enc_key_len_bytes.end());
					std::vector<std::uint8_t> enc_key_bytes =
						file.read_continue(enc_key_len);
					if (enc_key_bytes.size() != enc_key_len) { break; }
					std::string enc_key(enc_key_bytes.begin(),
										enc_key_bytes.end());
					if (InternalKey::decode(enc_key)._value_type !=
							ValueType::Value &&
						InternalKey::decode(enc_key)._value_type !=
							ValueType::TombStone) {
						throw std::runtime_error("load wal failed");
					}
					std::vector<std::uint8_t> value_len_bytes =
						file.read_continue(2);
					if (value_len_bytes.size() != 2) { break; }
					std::uint16_t value_len = decode_to_uint16(
						value_len_bytes.begin(), value_len_bytes.end());
					std::vector<std::uint8_t> value_bytes =
						file.read_continue(value_len);
					if (value_bytes.size() != value_len) { break; }
					std::string value(value_bytes.begin(), value_bytes.end());
					std::vector<std::uint8_t> data(enc_key_len_bytes.begin(),
												   enc_key_len_bytes.end());
					data.insert(data.end(), enc_key_bytes.begin(),
								enc_key_bytes.end());
					data.insert(data.end(), value_len_bytes.begin(),
								value_len_bytes.end());
					data.insert(data.end(), value_bytes.begin(),
								value_bytes.end());
					std::vector<std::uint8_t> crc_bytes = file.read_continue(4);
					if (crc_bytes.size() != 4) { break; }
					std::uint32_t crc =
						decode_to_uint32(crc_bytes.begin(), crc_bytes.end());
					std::uint32_t cur_crc = crc32(0, data.data(), data.size());
					if (cur_crc == crc) {
						frozen_list.put(enc_key, value);
						max_commit_sequence = std::max(
							max_commit_sequence,
							InternalKey::decode(enc_key)._commit_sequence);
					} else {
						throw std::runtime_error("crc check failed");
					}
					enc_key_len_bytes = file.read_continue(2);
				}
				if (file.size() == 0) {
					std::filesystem::remove(entry.path());
					continue;
				}
				pr_deque.push_back(std::make_pair(frozen_list, id));
				memtable._frozen_bytes += frozen_list.get_byte_size();
			}
		}
	}
	std::sort(pr_deque.begin(), pr_deque.end(),
			  [](std::pair<SkipList, std::uint64_t> &first,
				 std::pair<SkipList, std::uint64_t> &second) {
				  return first.second < second.second;
			  });
	for (auto &pr : pr_deque) {
		memtable._frozen_lists.push_back(
			std::make_shared<const SkipList>(std::move(pr.first)));
		memtable._frozen_ids.push_back(pr.second);
	}
	memtable._active_id = max_id + 1;
	std::string filepath = get_filepath(dir, memtable._active_id);
	_file = FileObj::create_rw(filepath);
	if (!_file.is_valid()) {
		throw std::runtime_error(
			"file: " + get_filepath(dir, memtable._active_id) +
			" create failed");
	}
	FileObj::fsync_parent_path(filepath);
	return max_commit_sequence;
}

// void WAL::load_wal(MemTable &memtable) {
// 	_file.seek_begin();
// 	std::vector<std::uint8_t> enc_key_len_bytes = _file.read_continue(2);
// 	while (!enc_key_len_bytes.empty()) {
// 		std::uint16_t enc_key_len = decode_to_uint16(enc_key_len_bytes.begin(),
// 													 enc_key_len_bytes.end());
// 		std::vector<std::uint8_t> enc_key_bytes =
// 			_file.read_continue(enc_key_len);
// 		std::string enc_key(enc_key_bytes.begin(), enc_key_bytes.end());
// 		std::vector<std::uint8_t> value_len_bytes = _file.read_continue(2);
// 		std::uint16_t value_len =
// 			decode_to_uint16(value_len_bytes.begin(), value_len_bytes.end());
// 		std::vector<std::uint8_t> value_bytes = _file.read_continue(value_len);
// 		std::string value(value_bytes.begin(), value_bytes.end());
// 		std::vector<std::uint8_t> data(enc_key_len_bytes.begin(),
// 									   enc_key_len_bytes.end());
// 		data.insert(data.end(), enc_key_bytes.begin(), enc_key_bytes.end());
// 		data.insert(data.end(), value_len_bytes.begin(), value_len_bytes.end());
// 		data.insert(data.end(), value_bytes.begin(), value_bytes.end());
// 		std::vector<std::uint8_t> crc_bytes = _file.read_continue(4);
// 		std::uint32_t crc =
// 			decode_to_uint32(crc_bytes.begin(), crc_bytes.end());
// 		std::uint32_t cur_crc = crc32(0, data.data(), data.size());
// 		if (cur_crc == crc) { memtable.put_entry_data(enc_key, value); }
// 		enc_key_len_bytes = _file.read_continue(2);
// 	}
// }

bool WAL::append(const std::string &enc_key, const std::string &value) {
	bool res = _file.append_data(encode(enc_key, value));
	bool fsync_res = fsync();
	return res && fsync_res;
}

bool WAL::clear() {
	bool clear_res = _file.clear();
	bool fsync_res = fsync();
	return clear_res && fsync_res;
}
bool WAL::fsync() { return _file.fsync(); }

bool WAL::delete_() { return _file.delete_(); }

bool WAL::close() { return _file.close(); }

std::string WAL::get_filepath(const std::string &dir, std::uint64_t id) {
	return dir + "/" + DEFAULT_WAL_FILE_PREFFIX + std::to_string(id);
}