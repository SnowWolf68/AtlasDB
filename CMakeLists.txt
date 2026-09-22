cmake_minimum_required(VERSION 3.16)

project(AtlasDB VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(ATLASDB_BUILD_APP "Build the interactive AtlasDB demo" ON)

find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)

add_library(
	atlasdb
	src/Block.cpp
	src/BlockMeta.cpp
	src/Comparator.cpp
	src/MemTable.cpp
	src/Record.cpp
	src/SkipList.cpp
	src/log.cpp
	src/lsm.cpp
	src/sst.cpp
	src/TranContext.cpp
	src/util.cpp
	src/wal.cpp
)
add_library(AtlasDB::AtlasDB ALIAS atlasdb)

target_compile_features(atlasdb PUBLIC cxx_std_17)
target_include_directories(
	atlasdb
	PUBLIC
		$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
		$<INSTALL_INTERFACE:include>
)
target_link_libraries(atlasdb PUBLIC Threads::Threads ZLIB::ZLIB)

# FileSink paths are interpreted relative to the process working directory.
# This keeps built artifacts independent of the source checkout location.
target_compile_definitions(atlasdb PRIVATE ATLASDB_PROJECT_DIR="")

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
	target_compile_options(atlasdb PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(ATLASDB_BUILD_APP)
	add_executable(AtlasDB src/main.cpp)
	target_link_libraries(AtlasDB PRIVATE AtlasDB::AtlasDB)
endif()

include(CTest)
if(BUILD_TESTING)
	add_subdirectory(test)
endif()
