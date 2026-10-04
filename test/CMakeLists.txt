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
	tran_test
)

set(
	ATLASDB_NET_TEST_TARGETS
	resp_test
	inetaddress_test
	socket_test
	acceptor_test
	tcpconnection_test
	eventloop_test
	eventloopthread_test
	eventloopthreadpool_test
	timerqueue_test
	buffer_test
)

# Deterministic server lifecycle tests use GNU ld syscall wrapping on Linux.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
	list(APPEND ATLASDB_NET_TEST_TARGETS tcpserver_test)
endif()

list(APPEND ATLASDB_TEST_TARGETS ${ATLASDB_NET_TEST_TARGETS})

# Linux-only review regressions: the repro pauses a committing transaction at
# its WAL rotation via --wrap=fsync, which needs GNU ld. Other platforms skip
# the target entirely.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
	list(APPEND ATLASDB_TEST_TARGETS transaction_review_test)
endif()

foreach(test_target IN LISTS ATLASDB_TEST_TARGETS)
	if(test_target IN_LIST ATLASDB_NET_TEST_TARGETS)
		set(test_source net/${test_target}.cpp)
	else()
		set(test_source ${test_target}.cpp)
	endif()
	add_executable(${test_target} ${test_source})
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
	target_link_options(transaction_review_test PRIVATE "-Wl,--wrap=fsync")
	set_tests_properties(transaction_review_test PROPERTIES TIMEOUT 60)
	target_link_options(tcpserver_test PRIVATE
		"-Wl,--wrap=bind" "-Wl,--wrap=accept4" "-Wl,--wrap=epoll_wait")
	set_tests_properties(tcpserver_test PROPERTIES TIMEOUT 120)
endif()
set_tests_properties(wal_test PROPERTIES TIMEOUT 60)
set_tests_properties(eventloop_test PROPERTIES TIMEOUT 10)
set_tests_properties(eventloopthread_test PROPERTIES TIMEOUT 90)
set_tests_properties(eventloopthreadpool_test PROPERTIES TIMEOUT 90)
set_tests_properties(timerqueue_test PROPERTIES TIMEOUT 10)
set_tests_properties(acceptor_test PROPERTIES TIMEOUT 10)
set_tests_properties(tcpconnection_test PROPERTIES TIMEOUT 90)


add_executable(my_resp_test my_test/my_resp_test.cpp)
target_link_libraries(my_resp_test PRIVATE AtlasDB::AtlasDB)

add_executable(my_inetaddress_test net/my_test/my_inetaddress_test.cpp)
target_link_libraries(my_inetaddress_test PRIVATE AtlasDB::AtlasDB)
