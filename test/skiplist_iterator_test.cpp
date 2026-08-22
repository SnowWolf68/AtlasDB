#include "SkipList.h"

#include <cstddef>
#include <exception>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

class TestFailure : public std::runtime_error {
public:
	explicit TestFailure(const std::string &message)
		: std::runtime_error(message) {}
};

void check(bool condition, const char *expression, int line) {
	if (condition) {
		return;
	}

	std::ostringstream message;
	message << "line " << line << ": check failed: " << expression;
	throw TestFailure(message.str());
}

template <typename Function>
void check_throws_logic_error(Function &&function, const char *expression,
							  int line) {
	try {
		function();
	} catch (const std::logic_error &) {
		return;
	} catch (const std::exception &error) {
		std::ostringstream message;
		message << "line " << line << ": " << expression
				<< " threw a different std::exception: " << error.what();
		throw TestFailure(message.str());
	} catch (...) {
		std::ostringstream message;
		message << "line " << line << ": " << expression
				<< " threw a non-standard exception";
		throw TestFailure(message.str());
	}

	std::ostringstream message;
	message << "line " << line << ": " << expression
			<< " did not throw std::logic_error";
	throw TestFailure(message.str());
}

#define CHECK(expression) check((expression), #expression, __LINE__)
#define CHECK_THROWS_LOGIC_ERROR(expression)                                  \
	check_throws_logic_error([&]() { static_cast<void>(expression); },          \
							 #expression, __LINE__)

void check_position(const SkipList::ConstIterator &iterator,
					const std::string &expected_key,
					const std::string &expected_value) {
	CHECK(iterator.is_valid());
	CHECK(iterator.key() == expected_key);
	CHECK(iterator.value() == expected_value);
}

void test_empty_list_iterator() {
	SkipList list;
	const SkipList &view = list;

	auto iterator = view.begin();
	const auto end = view.end();

	CHECK(iterator == end);
	CHECK(!(iterator != end));
	CHECK(!iterator.is_valid());
	CHECK(view.lower_bound("anything") == end);

	SkipList::ConstIterator &increment_result = ++iterator;
	CHECK(&increment_result == &iterator);
	CHECK(iterator == end);

	CHECK_THROWS_LOGIC_ERROR(iterator.key());
	CHECK_THROWS_LOGIC_ERROR(iterator.value());
	CHECK_THROWS_LOGIC_ERROR(*iterator);
}

void test_ordered_iteration_and_dereference() {
	SkipList list;
	CHECK(list.put("delta", "4"));
	CHECK(list.put("", "empty-key"));
	CHECK(list.put("charlie", "3"));
	CHECK(list.put("alpha", "1"));
	CHECK(list.put("bravo", "2"));

	const std::vector<std::pair<std::string, std::string>> expected = {
		{"", "empty-key"},
		{"alpha", "1"},
		{"bravo", "2"},
		{"charlie", "3"},
		{"delta", "4"},
	};

	const SkipList &view = list;
	auto iterator = view.begin();
	for (const auto &entry : expected) {
		CHECK(iterator != view.end());
		CHECK(iterator.is_valid());
		CHECK(iterator.key() == entry.first);
		CHECK(iterator.value() == entry.second);
		CHECK(*iterator == entry);
		++iterator;
	}

	CHECK(iterator == view.end());
	CHECK(!iterator.is_valid());

	std::vector<std::pair<std::string, std::string>> range_result;
	for (auto entry : view) {
		range_result.push_back(std::move(entry));
	}
	CHECK(range_result == expected);
}

void test_iterator_copy_and_owner_identity() {
	SkipList first_list;
	CHECK(first_list.put("alpha", "1"));
	CHECK(first_list.put("bravo", "2"));

	const SkipList &first_view = first_list;
	auto first = first_view.begin();
	auto copy = first;

	CHECK(first == copy);
	++first;
	CHECK(first != copy);
	check_position(copy, "alpha", "1");
	check_position(first, "bravo", "2");

	auto copied_entry = *copy;
	copied_entry.second = "changed-copy";
	CHECK(copy.value() == "1");

	SkipList second_list;
	CHECK(second_list.put("alpha", "1"));
	const SkipList &second_view = second_list;

	CHECK(first_view.begin() != second_view.begin());
	CHECK(first_view.end() != second_view.end());
}

void test_lower_bound_boundaries() {
	SkipList list;
	CHECK(list.put("bravo", "2"));
	CHECK(list.put("delta", "4"));
	CHECK(list.put("foxtrot", "6"));

	const SkipList &view = list;

	check_position(view.lower_bound(""), "bravo", "2");
	check_position(view.lower_bound("bravo"), "bravo", "2");
	check_position(view.lower_bound("charlie"), "delta", "4");
	check_position(view.lower_bound("delta"), "delta", "4");
	check_position(view.lower_bound("echo"), "foxtrot", "6");
	check_position(view.lower_bound("foxtrot"), "foxtrot", "6");
	CHECK(view.lower_bound("zulu") == view.end());
}

void test_empty_and_binary_keys() {
	SkipList list;
	const std::string binary_key("a\0b", 3);
	const std::string binary_before("a\0a", 3);

	CHECK(list.put("", "empty-key"));
	CHECK(list.put("a", "plain-a"));
	CHECK(list.put(binary_key, "binary-key"));
	CHECK(list.put("b", "plain-b"));

	const SkipList &view = list;
	check_position(view.lower_bound(""), "", "empty-key");
	check_position(view.lower_bound("a"), "a", "plain-a");
	check_position(view.lower_bound(binary_before), binary_key, "binary-key");
	check_position(view.lower_bound(binary_key), binary_key, "binary-key");
	check_position(view.lower_bound("b"), "b", "plain-b");
}

struct TestCase {
	const char *name;
	void (*run)();
};

} // namespace

int main() {
	const TestCase tests[] = {
		{"empty list iterator", test_empty_list_iterator},
		{"ordered iteration and dereference",
		 test_ordered_iteration_and_dereference},
		{"iterator copy and owner identity",
		 test_iterator_copy_and_owner_identity},
		{"lower_bound boundaries", test_lower_bound_boundaries},
		{"empty and binary keys", test_empty_and_binary_keys},
	};

	std::size_t passed = 0;
	for (const TestCase &test : tests) {
		try {
			test.run();
			++passed;
			std::cout << "[PASS] " << test.name << '\n';
		} catch (const std::exception &error) {
			std::cerr << "[FAIL] " << test.name << ": " << error.what()
					  << '\n';
		}
	}

	std::cout << passed << '/'
			  << (sizeof(tests) / sizeof(tests[0])) << " tests passed\n";

	return passed == (sizeof(tests) / sizeof(tests[0])) ? 0 : 1;
}
