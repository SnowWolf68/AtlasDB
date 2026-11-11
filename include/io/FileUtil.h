//
// Created by test on 10/27/26.
//

#ifndef ATLASDB_FILEUTIL_H
#define ATLASDB_FILEUTIL_H
#include "FileObj.h"


#include <cstdint>
#include <string>
#include <vector>
namespace atlas::io {
	void read_exact_at(FileObj &file, std::vector<std::uint8_t> &buffer, std::size_t offset);
	void write_all_at(FileObj &file, const std::vector<std::uint8_t> &data, std::size_t offset);
	void append_all(FileObj &file, const std::vector<std::uint8_t> &data);
	void atomic_write_all_replace(const std::string &file_path,
								  const std::vector<std::uint8_t> &data);
	void atomic_write_all_replace(FileObj &file, const std::vector<std::uint8_t> &data);
} // namespace atlas::io

#endif // ATLASDB_FILEUTIL_H
