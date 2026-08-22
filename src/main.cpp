#include <iostream>

#include "SkipList.h"
#include "log.h"

void test_log() {
	// std::cout << "hello" << std::endl;
	// Log::get_logger().use_file_sink("log", "/logs");
	Log::get_logger().set_minimum_log_level(Log::LogLevel::INFO);
	LOG(Log::LogLevel::INFO) << "qwewqeqw";
}

void test_SkipList() {
	SkipList sk_list;
	while (true) {
		int op = 0;
		std::cin >> op;
		if (op == 1) {
			std::string key, value;
			std::cin >> key >> value;
			sk_list.put(key, value);
		} else if (op == 2) {
			std::string key, value;
			std::cin >> key;
			bool res = sk_list.get(key, value);
			if (res) {
				std::cout << "value: " << value << std::endl;
			} else {
				std::cout << "null" << std::endl;
			}
		} else if (op == 3) {
			return;
		} else {
			continue;
		}
	}
}

int main() {

    // test_log();
	test_SkipList();

	return 0;
}