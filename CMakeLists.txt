cmake_minimum_required(VERSION 3.16)

project(AtlasDB VERSION 1.2.0 LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(ATLASDB_BUILD_APP "Build the AtlasDB RESP server" ON)

find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)

file(GLOB_RECURSE ATLASDB_SOURCES CONFIGURE_DEPENDS
	"${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)
list(REMOVE_ITEM ATLASDB_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")

add_library(atlasdb ${ATLASDB_SOURCES})
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
