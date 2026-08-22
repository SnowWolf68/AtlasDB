cmake_minimum_required(VERSION 3.16)

project(AtlasDBSkipListTests LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

enable_testing()

add_executable(
    skiplist_test
    skiplist_test.cpp
    ../src/Comparator.cpp
    ../src/Record.cpp
    ../src/SkipList.cpp
)

target_include_directories(
    skiplist_test PRIVATE
    ../include
)

target_compile_features(skiplist_test PRIVATE cxx_std_17)
target_compile_options(skiplist_test PRIVATE -Wall -Wextra -Wpedantic)

add_test(NAME skiplist_test COMMAND skiplist_test)

add_executable(
    skiplist_iterator_test
    skiplist_iterator_test.cpp
    ../src/Comparator.cpp
    ../src/Record.cpp
    ../src/SkipList.cpp
)

target_include_directories(
    skiplist_iterator_test PRIVATE
    ../include
)

target_compile_features(skiplist_iterator_test PRIVATE cxx_std_17)
target_compile_options(
    skiplist_iterator_test PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

add_test(NAME skiplist_iterator_test COMMAND skiplist_iterator_test)

add_executable(
    internal_key_test
    internal_key_test.cpp
    ../src/Comparator.cpp
    ../src/Record.cpp
)

target_include_directories(
    internal_key_test PRIVATE
    ../include
)

target_compile_features(internal_key_test PRIVATE cxx_std_17)
target_compile_options(internal_key_test PRIVATE -Wall -Wextra -Wpedantic)

add_test(NAME internal_key_test COMMAND internal_key_test)

add_executable(
    memtable_test
    memtable_test.cpp
    ../src/Block.cpp
    ../src/BlockMeta.cpp
    ../src/Comparator.cpp
    ../src/MemTable.cpp
    ../src/Record.cpp
    ../src/SkipList.cpp
    ../src/log.cpp
    ../src/sst.cpp
    ../src/util.cpp
)

target_include_directories(
    memtable_test PRIVATE
    ../include
)

target_compile_features(memtable_test PRIVATE cxx_std_17)
target_compile_options(memtable_test PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(memtable_test PRIVATE ATLASDB_PROJECT_DIR="/tmp/")
target_link_libraries(memtable_test z)

add_test(NAME memtable_test COMMAND memtable_test)

add_executable(
    block_test
    block_test.cpp
    ../src/Block.cpp
    ../src/Comparator.cpp
    ../src/Record.cpp
)

target_include_directories(
    block_test PRIVATE
    ../include
)

target_compile_features(block_test PRIVATE cxx_std_17)
target_compile_options(block_test PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(block_test z)

add_test(NAME block_test COMMAND block_test)

add_executable(
    blockmeta_test
    blockmeta_test.cpp
    ../src/BlockMeta.cpp
)

target_include_directories(
    blockmeta_test PRIVATE
    ../include
)

target_compile_features(blockmeta_test PRIVATE cxx_std_17)
target_compile_options(blockmeta_test PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(blockmeta_test z)

add_test(NAME blockmeta_test COMMAND blockmeta_test)

add_executable(
    util_test
    util_test.cpp
    ../src/util.cpp
    ../src/log.cpp
)

target_include_directories(
    util_test PRIVATE
    ../include
)

target_compile_features(util_test PRIVATE cxx_std_17)
target_compile_options(util_test PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(util_test PRIVATE ATLASDB_PROJECT_DIR="/tmp/")

add_test(NAME util_test COMMAND util_test)

add_executable(
    sst_test
    sst_test.cpp
    ../src/sst.cpp
    ../src/Block.cpp
    ../src/BlockMeta.cpp
    ../src/Comparator.cpp
    ../src/Record.cpp
    ../src/util.cpp
    ../src/log.cpp
)

target_include_directories(
    sst_test PRIVATE
    ../include
)

target_compile_features(sst_test PRIVATE cxx_std_17)
target_compile_options(sst_test PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(sst_test PRIVATE ATLASDB_PROJECT_DIR="/tmp/")
target_link_libraries(sst_test z)

add_test(NAME sst_test COMMAND sst_test)

add_executable(
    flush_last_test
    flush_last_test.cpp
    ../src/MemTable.cpp
    ../src/SkipList.cpp
    ../src/sst.cpp
    ../src/Block.cpp
    ../src/BlockMeta.cpp
    ../src/Comparator.cpp
    ../src/Record.cpp
    ../src/util.cpp
    ../src/log.cpp
)

target_include_directories(
    flush_last_test PRIVATE
    ../include
)

target_compile_features(flush_last_test PRIVATE cxx_std_17)
target_compile_options(flush_last_test PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(flush_last_test PRIVATE ATLASDB_PROJECT_DIR="/tmp/")
target_link_libraries(flush_last_test z)

add_test(NAME flush_last_test COMMAND flush_last_test)

add_executable(
    lsm_engine_test
    lsm_engine_test.cpp
    ../src/lsm.cpp
    ../src/MemTable.cpp
    ../src/SkipList.cpp
    ../src/sst.cpp
    ../src/Block.cpp
    ../src/BlockMeta.cpp
    ../src/Comparator.cpp
    ../src/Record.cpp
    ../src/util.cpp
    ../src/log.cpp
)

target_include_directories(
    lsm_engine_test PRIVATE
    ../include
)

target_compile_features(lsm_engine_test PRIVATE cxx_std_17)
target_compile_options(lsm_engine_test PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(lsm_engine_test PRIVATE ATLASDB_PROJECT_DIR="/tmp/")
target_link_libraries(lsm_engine_test z)

add_test(NAME lsm_engine_test COMMAND lsm_engine_test)

add_executable(
    two_merge_iterator_test
    two_merge_iterator_test.cpp
    ../src/lsm.cpp
    ../src/MemTable.cpp
    ../src/SkipList.cpp
    ../src/sst.cpp
    ../src/Block.cpp
    ../src/BlockMeta.cpp
    ../src/Comparator.cpp
    ../src/Record.cpp
    ../src/util.cpp
    ../src/log.cpp
)

target_include_directories(
    two_merge_iterator_test PRIVATE
    ../include
)

target_compile_features(two_merge_iterator_test PRIVATE cxx_std_17)
target_compile_options(
    two_merge_iterator_test PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)
target_compile_definitions(
    two_merge_iterator_test PRIVATE
    ATLASDB_PROJECT_DIR="/tmp/"
)
target_link_libraries(two_merge_iterator_test z)

add_test(NAME two_merge_iterator_test COMMAND two_merge_iterator_test)

add_executable(
    gen_sst_test
    gen_sst_test.cpp
    ../src/lsm.cpp
    ../src/MemTable.cpp
    ../src/SkipList.cpp
    ../src/sst.cpp
    ../src/Block.cpp
    ../src/BlockMeta.cpp
    ../src/Comparator.cpp
    ../src/Record.cpp
    ../src/util.cpp
    ../src/log.cpp
)

target_include_directories(
    gen_sst_test PRIVATE
    ../include
)

target_compile_features(gen_sst_test PRIVATE cxx_std_17)
target_compile_options(
    gen_sst_test PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)
target_compile_definitions(
    gen_sst_test PRIVATE
    ATLASDB_PROJECT_DIR="/tmp/"
)
target_link_libraries(gen_sst_test z)

add_test(NAME gen_sst_test COMMAND gen_sst_test)

add_executable(
    compact_test
    compact_test.cpp
    ../src/lsm.cpp
    ../src/MemTable.cpp
    ../src/SkipList.cpp
    ../src/sst.cpp
    ../src/Block.cpp
    ../src/BlockMeta.cpp
    ../src/Comparator.cpp
    ../src/Record.cpp
    ../src/util.cpp
    ../src/log.cpp
)

target_include_directories(
    compact_test PRIVATE
    ../include
)

target_compile_features(compact_test PRIVATE cxx_std_17)
target_compile_options(
    compact_test PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)
target_compile_definitions(
    compact_test PRIVATE
    ATLASDB_PROJECT_DIR="/tmp/"
)
target_link_libraries(compact_test z)

add_test(NAME compact_test COMMAND compact_test)

add_executable(
    scan_test
    scan_test.cpp
    ../src/lsm.cpp
    ../src/MemTable.cpp
    ../src/SkipList.cpp
    ../src/sst.cpp
    ../src/Block.cpp
    ../src/BlockMeta.cpp
    ../src/Comparator.cpp
    ../src/Record.cpp
    ../src/util.cpp
    ../src/log.cpp
)

target_include_directories(
    scan_test PRIVATE
    ../include
)

target_compile_features(scan_test PRIVATE cxx_std_17)
target_compile_options(
    scan_test PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)
target_compile_definitions(
    scan_test PRIVATE
    ATLASDB_PROJECT_DIR="/tmp/"
)
target_link_libraries(scan_test z)

add_test(NAME scan_test COMMAND scan_test)

add_executable(
    predicate_test
    predicate_test.cpp
    ../src/lsm.cpp
    ../src/MemTable.cpp
    ../src/SkipList.cpp
    ../src/sst.cpp
    ../src/Block.cpp
    ../src/BlockMeta.cpp
    ../src/Comparator.cpp
    ../src/Record.cpp
    ../src/util.cpp
    ../src/log.cpp
)

target_include_directories(
    predicate_test PRIVATE
    ../include
)

target_compile_features(predicate_test PRIVATE cxx_std_17)
target_compile_options(
    predicate_test PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)
target_compile_definitions(
    predicate_test PRIVATE
    ATLASDB_PROJECT_DIR="/tmp/"
)
target_link_libraries(predicate_test z)

add_test(NAME predicate_test COMMAND predicate_test)

add_executable(
    facade_test
    facade_test.cpp
    ../src/lsm.cpp
    ../src/MemTable.cpp
    ../src/SkipList.cpp
    ../src/sst.cpp
    ../src/Block.cpp
    ../src/BlockMeta.cpp
    ../src/Comparator.cpp
    ../src/Record.cpp
    ../src/util.cpp
    ../src/log.cpp
)

target_include_directories(
    facade_test PRIVATE
    ../include
)

target_compile_features(facade_test PRIVATE cxx_std_17)
target_compile_options(
    facade_test PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)
target_compile_definitions(
    facade_test PRIVATE
    ATLASDB_PROJECT_DIR="/tmp/"
)
target_link_libraries(facade_test z)

add_test(NAME facade_test COMMAND facade_test)
