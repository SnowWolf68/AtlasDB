#include "wal.h"
#include "MemTable.h"
#include "Record.h"
#include "codec.h"
#include "io/FileSystem.h"
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
std::vector<std::uint8_t> WAL::encode(const std::string &enc_key, const std::string &value) {
	std::vector<std::uint8_t> data;
	codec::append_u16_be(data, enc_key.size());
	codec::append_string(data, enc_key);
	codec::append_u16_be(data, value.size());
	codec::append_string(data, value);
	std::uint32_t crc = crc32(0, data.data(), data.size());
	codec::append_u32_be(data, crc);
	return data;
}

/**
type 0x02 (1B) + count 4B + {op: 2B enc_key_len + enc_key + 2B value_len +
value} + crc 4B
*/
std::vector<std::uint8_t>
WAL::encode_transaction(std::unordered_map<std::string, std::optional<std::string>> map,
						std::uint64_t commit_sequence) {
	std::vector<std::uint8_t> data;
	data.push_back(0x02);
	codec::append_u32_be(data, map.size());
	for (auto &[user_key, value] : map) {
		ValueType value_type = value == std::nullopt ? ValueType::TombStone : ValueType::Value;
		std::string enc_key =
			InternalKey::encode(InternalKey(user_key, commit_sequence, value_type));
		codec::append_u16_be(data, enc_key.size());
		codec::append_string(data, enc_key);
		std::uint16_t value_size = value == std::nullopt ? 0 : value.value().size();
		codec::append_u16_be(data, value_size);
		std::string value_str = value == std::nullopt ? "" : value.value();
		codec::append_string(data, value_str);
	}
	std::uint32_t crc = crc32(0, data.data(), data.size());
	codec::append_u32_be(data, crc);
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
	codec::append_u32_be(data, 1ULL);
	codec::append_u16_be(data, enc_key.size());
	codec::append_string(data, enc_key);
	codec::append_u16_be(data, value.size());
	codec::append_string(data, value);
	std::uint32_t crc = crc32(0, data.data(), data.size());
	codec::append_u32_be(data, crc);
	return data;
}

bool WAL::delete_wal_by_id(const std::string &dir, std::uint64_t wal_id) {
	std::string filepath = get_filepath(dir, wal_id);
	bool res = atlas::io::FileSystem::remove_file(filepath);
	std::string parent_path = std::filesystem::absolute(filepath).parent_path();
	atlas::io::FileSystem::sync_dir(parent_path);
	return res;
}

WAL::WAL() {}

WAL::WAL(const std::string &wal_dir, std::uint64_t wal_id) {
	std::string filepath = get_filepath(wal_dir, wal_id);
	if (!std::filesystem::exists(wal_dir)) {
		bool res = atlas::io::FileSystem::create_directories(wal_dir);
		if (!res) {
			ERROR_LOG << "wal_dir: " << wal_dir << " create failed";
			throw std::runtime_error("wal_dir: " + wal_dir + " create failed");
		}
		std::string parent_path =
			std::filesystem::absolute(std::filesystem::path(wal_dir)).parent_path();
		atlas::io::FileSystem::sync_dir(parent_path);
	}
	if (!std::filesystem::exists(filepath)) {
		atlas::io::OpenOptions options;
		options._access_mode = atlas::io::AccessMode::ReadWrite;
		options._create_mode = atlas::io::CreateMode::CreateNew;
		options._truncate = true;
		_file = atlas::io::FileSystem::open(filepath, options);
	} else {
		atlas::io::OpenOptions options;
		options._access_mode = atlas::io::AccessMode::ReadWrite;
		options._create_mode = atlas::io::CreateMode::OpenExisting;
		options._truncate = false;
		_file = atlas::io::FileSystem::open(filepath, options);
	}
	atlas::io::FileSystem::sync_dir(wal_dir);
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
				// FileObj file = FileObj::open_read(entry.path());
				atlas::io::OpenOptions options;
				options._access_mode = atlas::io::AccessMode::ReadOnly;
				options._create_mode = atlas::io::CreateMode::OpenExisting;
				options._truncate = false;
				atlas::io::FileObj file = atlas::io::FileSystem::open(entry.path(), options);
				SkipList frozen_list(std::make_shared<InternalKeyComparator>());
				std::size_t file_size = file.size(), offset = 0;
				while (offset < file_size) {
					std::uint32_t crc = 0;
					std::vector<std::uint8_t> buffer(1);
					std::size_t read_size = file.read_at(offset, buffer);
					if (read_size != 1) {
						if (file_size - offset == read_size) { break; }
						throw_error("wal file: " + file.get_file_path() + "load failed");
					}
					std::uint8_t type = buffer[0];
					if (type != 0x02) {
						throw_error("wal file: " + file.get_file_path() + "load failed");
					}
					offset += 1;
					crc = crc32(crc, buffer.data(), buffer.size());

					buffer.resize(4);
					read_size = file.read_at(offset, buffer);
					if (read_size != 4) {
						if (file_size - offset == read_size) { break; }
						throw_error("wal file: " + file.get_file_path() + "load failed");
					}
					std::optional<std::uint32_t> count_opt = codec::read_u32_be(buffer, 0);
					if (count_opt == std::nullopt) {
						throw_error("wal file: " + file.get_file_path() + "load failed");
					}
					std::uint32_t count = count_opt.value();
					offset += 4;
					crc = crc32(crc, buffer.data(), buffer.size());

					bool torn_tail = false;
					std::vector<std::pair<std::string, std::string>> transaction_pr;
					for (int i = 0; i < count; i++) {
						std::vector<std::uint8_t> buffer(2);
						std::size_t read_size = file.read_at(offset, buffer);
						if (read_size != 2) {
							if (file_size - offset == read_size) {
								torn_tail = true;
								break;
							}
							throw_error("wal file: " + file.get_file_path() + "load failed");
						}
						std::optional<std::uint16_t> enc_key_len_opt =
							codec::read_u16_be(buffer, 0);
						if (enc_key_len_opt == std::nullopt) {
							throw_error("wal file: " + file.get_file_path() + "load failed");
						}
						std::uint16_t enc_key_len = enc_key_len_opt.value();
						offset += 2;
						crc = crc32(crc, buffer.data(), buffer.size());

						buffer.resize(enc_key_len);
						read_size = file.read_at(offset, buffer);
						if (read_size != enc_key_len) {
							if (file_size - offset == read_size) {
								torn_tail = true;
								break;
							}
							throw_error("wal file: " + file.get_file_path() + "load failed");
						}
						std::optional<std::string> enc_key_opt =
							codec::read_string(buffer, 0, buffer.size());
						if (enc_key_opt == std::nullopt) {
							throw_error("wal file: " + file.get_file_path() + "load failed");
						}
						std::string enc_key = enc_key_opt.value();
						offset += enc_key_len;
						crc = crc32(crc, buffer.data(), buffer.size());

						buffer.resize(2);
						read_size = file.read_at(offset, buffer);
						if (read_size != 2) {
							if (file_size - offset == read_size) {
								torn_tail = true;
								break;
							}
							throw_error("wal file: " + file.get_file_path() + "load failed");
						}
						std::optional<std::uint16_t> value_len_opt = codec::read_u16_be(buffer, 0);
						if (value_len_opt == std::nullopt) {
							throw_error("wal file: " + file.get_file_path() + "load failed");
						}
						std::uint16_t value_len = value_len_opt.value();
						offset += 2;
						crc = crc32(crc, buffer.data(), buffer.size());

						buffer.resize(value_len);
						read_size = file.read_at(offset, buffer);
						if (read_size != value_len) {
							if (file_size - offset == read_size) {
								torn_tail = true;
								break;
							}
							throw_error("wal file: " + file.get_file_path() + "load failed");
						}
						std::optional<std::string> value_opt =
							codec::read_string(buffer, 0, buffer.size());
						if (value_opt == std::nullopt) {
							throw_error("wal file: " + file.get_file_path() + "load failed");
						}
						std::string value = value_opt.value();
						offset += value_len;
						crc = crc32(crc, buffer.data(), buffer.size());

						transaction_pr.push_back(std::make_pair(enc_key, value));
					}
					if (torn_tail) { break; }

					buffer.resize(4);
					read_size = file.read_at(offset, buffer);
					if (read_size != 4) {
						if (file_size - offset == read_size) {
							torn_tail = true;
							break;
						}
						throw_error("wal file: " + file.get_file_path() + "load failed");
					}
					std::optional<std::uint32_t> old_crc_opt = codec::read_u32_be(buffer, 0);
					if (old_crc_opt == std::nullopt) {
						throw_error("wal file: " + file.get_file_path() + "load failed");
					}
					std::uint32_t old_crc = old_crc_opt.value();
					offset += 4;

					if (old_crc != crc) {
						throw_error("wal file: " + file.get_file_path() + "load failed");
					}
					for (auto &pr : transaction_pr) {
						std::string enc_key = pr.first, value = pr.second;
						max_commit_sequence = std::max(
							max_commit_sequence, InternalKey::decode(enc_key)._commit_sequence);
						frozen_list.put(enc_key, value);
					}
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
	std::sort(
		pr_deque.begin(), pr_deque.end(),
		[](std::pair<SkipList, std::uint64_t> &first, std::pair<SkipList, std::uint64_t> &second) {
			return first.second < second.second;
		});
	for (auto &pr : pr_deque) {
		memtable._frozen_lists.push_back(std::make_shared<const SkipList>(std::move(pr.first)));
		memtable._frozen_ids.push_back(pr.second);
	}
	memtable._active_id = max_id + 1;
	std::string filepath = get_filepath(dir, memtable._active_id);
	atlas::io::OpenOptions options;
	options._access_mode = atlas::io::AccessMode::ReadWrite;
	options._create_mode = atlas::io::CreateMode::CreateNew;
	options._truncate = true;
	_file = atlas::io::FileSystem::open(filepath, options);
	std::string parent_path = std::filesystem::absolute(filepath).parent_path();
	atlas::io::FileSystem::sync_dir(parent_path);
	return max_commit_sequence;
}

bool WAL::append(const std::string &enc_key, const std::string &value) {
	std::vector<std::uint8_t> data = encode_transaction(enc_key, value);
	std::size_t append_size = 0;
	try {
		append_size = _file.append(data);
	} catch (const std::exception &e) {
		LOG(Log::LogLevel::ERROR) << "Error while append(): " << e.what();
		return false;
	}
	bool res = append_size == data.size();
	bool fsync_res = fsync();
	return res && fsync_res;
}

bool WAL::append(std::vector<std::uint8_t> data) {
	std::size_t append_size = 0;
	try {
		append_size = _file.append(data);
	} catch (const std::exception &e) {
		LOG(Log::LogLevel::ERROR) << "Error while append(): " << e.what();
		return false;
	}
	bool res = append_size == data.size();
	bool fsync_res = fsync();
	return res && fsync_res;
}

bool WAL::fsync() {
	try {
		_file.sync();
	} catch (const std::exception &e) {
		LOG(Log::LogLevel::ERROR) << "Error while fsync(): " << e.what();
		return false;
	}
	return true;
}

bool WAL::delete_() { return atlas::io::FileSystem::remove_file(_file.get_file_path()); }

void WAL::close() { _file.close(); }

std::string WAL::get_filepath(const std::string &dir, std::uint64_t id) {
	return dir + "/" + DEFAULT_WAL_FILE_PREFFIX + std::to_string(id);
}