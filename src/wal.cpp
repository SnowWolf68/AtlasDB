#include "wal.h"
#include "MemTable.h"
#include "Record.h"
#include "log.h"
#include "util.h"
#include <cerrno>
#include <cstddef>
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

/**
type 0x02 (1B) + count 4B + {op: 2B enc_key_len + enc_key + 2B value_len +
value} + crc 4B
*/
std::vector<std::uint8_t> WAL::encode_transaction(
	std::unordered_map<std::string, std::optional<std::string>> map,
	std::uint64_t commit_sequence) {
	std::vector<std::uint8_t> data;
	data.push_back(0x02);
	append_u32(data, map.size());
	for (auto &[user_key, value] : map) {
		ValueType value_type =
			value == std::nullopt ? ValueType::TombStone : ValueType::Value;
		std::string enc_key = InternalKey::encode(
			InternalKey(user_key, commit_sequence, value_type));
		append_u16(data, enc_key.size());
		append_str(data, enc_key);
		std::uint16_t value_size = value == std::nullopt ? 0 : value.value().size();
		append_u16(data, value_size);
		std::string value_str = value == std::nullopt ? "" : value.value();
		append_str(data, value_str);
	}
	std::uint32_t crc = crc32(0, data.data(), data.size());
	append_u32(data, crc);
	return data;
}

/**
type 0x02 (1B) + count 4B + {op: 2B enc_key_len + enc_key + 2B value_len +
value} + crc 4B
*/
std::vector<std::uint8_t> WAL::encode_transaction(const std::string &enc_key,
												  const std::string &value) {
	std::vector<std::uint8_t> data;
	data.push_back(0x02);
	append_u32(data, 1ULL);
	append_u16(data, enc_key.size());
	append_str(data, enc_key);
	append_u16(data, value.size());
	append_str(data, value);
	std::uint32_t crc = crc32(0, data.data(), data.size());
	append_u32(data, crc);
	return data;
}

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
				std::vector<std::uint8_t> file_bytes =
					file.read(0, file.size());
				auto it = file_bytes.begin();
				while (it != file_bytes.end()) {
					/**
					type 0x02 (1B) + count 4B +
					{op: 2B enc_key_len + enc_key + 2B value_len + value}
					 + crc 4B


					失败的处理方式:
						首先如果事务commit返回之前崩溃, 那么wal会写到一半没写完,
					出现残尾 但是此时commit没有给用户返回,
					用户知道这个事务没有提交成功 因此对于残尾的情况,
					恢复的时候直接把这部分跳过即可 (break)

						但是需要注意的是, 这里的残尾大部分都表现为长度不够,
					并不意味着长度不够的都是残尾 也有可能事务正常commit,
					但是存在磁盘上的文件坏掉了导致长度字段解析错误
						因此如果所有的长度不匹配的时候都用break,
					其实是有一定风险的, 即有可能将正常提交的事务当作残尾跳过掉
						但是实际情况下长度不匹配大部分都是正常的残尾,
					文件坏了导致事务出问题的几率比较小
						所以这里如果长度不匹配都采用的break跳过的方式

						而如果最后的crc检验出错,
					那么极大概率都是事务正常提交但是数据存在磁盘上坏了,
					因此这里就直接throw
					*/
					auto start_it = it;
					if (file_bytes.end() - it < 9) {
						LOG(Log::LogLevel::WARNING) << "torn tail";
						break;
					}
					std::uint8_t type = *it++;
					if (type != 0x02) {
						LOG(Log::ERROR) << "parse wal failed, type don't match";
						throw std::runtime_error("parse wal failed");
					}
					if (file_bytes.end() - it < 4) {
						LOG(Log::LogLevel::WARNING) << "torn tail";
						break;
					}
					std::uint32_t count = decode_to_uint32(it, it + 4);
					it += 4;
					// 先用一个map把decode出来的数据都存一下,
					// 后面如果crc校验通过了, 说明当前整个事务都没问题,
					// 然后再放到frozen_list中
					std::unordered_map<std::string, std::string>
						transaction_map;

					bool torn_tail = false;
					for (std::size_t i = 0; i < count; i++) {
						if (file_bytes.end() - it < 2) {
							torn_tail = true;
							LOG(Log::LogLevel::WARNING) << "torn tail";
							break;
						}
						std::uint16_t enc_key_len =
							decode_to_uint16(it, it + 2);
						it += 2;
						if (file_bytes.end() - it < enc_key_len) {
							torn_tail = true;
							LOG(Log::LogLevel::WARNING) << "torn tail";
							break;
						}
						std::string enc_key(it, it + enc_key_len);
						it += enc_key_len;
						if (file_bytes.end() - it < 2) {
							torn_tail = true;
							LOG(Log::LogLevel::WARNING) << "torn tail";
							break;
						}
						std::uint16_t value_len = decode_to_uint16(it, it + 2);
						it += 2;
						if (file_bytes.end() - it < value_len) {
							torn_tail = true;
							LOG(Log::LogLevel::WARNING) << "torn tail";
							break;
						}
						std::string value(it, it + value_len);
						it += value_len;
						transaction_map[enc_key] = value;
					}
					if (torn_tail) { break; }
					if (file_bytes.end() - it < 4) {
						LOG(Log::LogLevel::WARNING) << "torn tail";
						break;
					}
					std::uint32_t old_crc = decode_to_uint32(it, it + 4);
					std::vector<std::uint8_t> data(start_it, it);
					std::uint32_t crc = crc32(0, data.data(), data.size());
					if (old_crc != crc) {
						throw std::runtime_error("crc check failed");
					}
					for (auto &[enc_key, value] : transaction_map) {
						max_commit_sequence = std::max(
							max_commit_sequence,
							InternalKey::decode(enc_key)._commit_sequence);
						frozen_list.put(enc_key, value);
					}
					it += 4;
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
	bool res = _file.append_data(encode_transaction(enc_key, value));
	bool fsync_res = fsync();
	return res && fsync_res;
}

bool WAL::append(std::vector<std::uint8_t> data) {
	bool file_append_res = _file.append_data(data);
	bool fsync_res = fsync();
	return file_append_res && fsync_res;
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

std::vector<std::uint8_t> WAL::append_u8(std::vector<std::uint8_t> &data,
										 std::uint8_t u8) {
	data.push_back(u8);
	return data;
}
std::vector<std::uint8_t> WAL::append_u16(std::vector<std::uint8_t> &data,
										  std::uint16_t u16) {
	std::vector<uint8_t> bytes;
	for (int i = 0; i < 2; i++) {
		bytes.push_back((u16 >> (8 - i * 8)) &
						static_cast<std::uint16_t>(0xff));
	}
	data.insert(data.end(), bytes.begin(), bytes.end());
	return data;
}
std::vector<std::uint8_t> WAL::append_u32(std::vector<std::uint8_t> &data,
										  std::uint32_t u32) {
	std::vector<uint8_t> bytes;
	for (int i = 0; i < 4; i++) {
		bytes.push_back((u32 >> (8 * 3 - i * 8)) &
						static_cast<std::uint16_t>(0xff));
	}
	data.insert(data.end(), bytes.begin(), bytes.end());
	return data;
}
std::vector<std::uint8_t> WAL::append_u64(std::vector<std::uint8_t> &data,
										  std::uint8_t u64) {
	std::vector<uint8_t> bytes;
	for (int i = 0; i < 8; i++) {
		bytes.push_back((u64 >> (8 * 7 - i * 8)) &
						static_cast<std::uint8_t>(0xff));
	}
	data.insert(data.end(), bytes.begin(), bytes.end());
	return data;
}
std::vector<std::uint8_t> WAL::append_str(std::vector<std::uint8_t> &data,
										  const std::string &str) {
	data.insert(data.end(), str.begin(), str.end());
	return data;
}