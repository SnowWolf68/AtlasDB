set(
	ATLASDB_TEST_TARGETS
	skiplist_test
	skiplist_iterator_test
	internal_key_test
	memtable_test
	block_test
	blockmeta_test
	util_test
	sst_test
	flush_last_test
	lsm_engine_test
	two_merge_iterator_test
	gen_sst_test
	compact_test
	scan_test
	predicate_test
	facade_test
	wal_test
)

foreach(test_target IN LISTS ATLASDB_TEST_TARGETS)
	add_executable(${test_target} ${test_target}.cpp)
	target_link_libraries(${test_target} PRIVATE AtlasDB::AtlasDB)

	if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
		target_compile_options(${test_target} PRIVATE -Wall -Wextra -Wpedantic)
	endif()

	add_test(NAME ${test_target} COMMAND ${test_target})
endforeach()

# Deterministic WAL I/O failures in this test executable only; production
# library code is unchanged. Other platforms still run the portable cases.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
	target_compile_definitions(wal_test PRIVATE ATLASDB_WAL_FAULT_INJECTION=1)
	target_link_options(wal_test PRIVATE "-Wl,--wrap=write" "-Wl,--wrap=fsync")
endif()
set_tests_properties(wal_test PROPERTIES TIMEOUT 60)
