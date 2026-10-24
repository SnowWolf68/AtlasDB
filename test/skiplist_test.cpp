#include "SkipList.h"

#include <cstddef>
#include <exception>
#include <iostream>
#include <map>
#include <random>
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

#define CHECK(expression) check((expression), #expression, __LINE__)

void check_value(SkipList &list, const std::string &key,
				 const std::string &expected) {
	std::string value = "unchanged";
	CHECK(list.get(key, value));
	CHECK(value == expected);
}

void test_empty_list() {
	SkipList list;

	CHECK(list.empty());
	CHECK(list.size() == 0);

	std::string value = "unchanged";
	CHECK(!list.get("missing", value));
	CHECK(value == "unchanged");
	CHECK(!list.del("missing"));
	CHECK(list.empty());
}

void test_put_get_and_update() {
	SkipList list;

	CHECK(list.put("charlie", "3"));
	CHECK(list.put("alpha", "1"));
	CHECK(list.put("bravo", "2"));
	CHECK(list.size() == 3);

	check_value(list, "alpha", "1");
	check_value(list, "bravo", "2");
	check_value(list, "charlie", "3");

	CHECK(!list.put("bravo", "updated"));
	CHECK(list.size() == 3);
	check_value(list, "bravo", "updated");
}

void test_delete() {
	SkipList list;
	CHECK(list.put("delta", "4"));
	CHECK(list.put("alpha", "1"));
	CHECK(list.put("charlie", "3"));
	CHECK(list.put("bravo", "2"));

	CHECK(list.del("alpha"));
	CHECK(list.size() == 3);
	CHECK(list.del("charlie"));
	CHECK(list.size() == 2);
	CHECK(list.del("delta"));
	CHECK(list.size() == 1);
	CHECK(list.del("bravo"));
	CHECK(list.empty());

	CHECK(!list.del("bravo"));
	CHECK(list.size() == 0);
}

void test_empty_key() {
	SkipList list;

	CHECK(list.put("", "empty-key-value"));
	CHECK(list.size() == 1);
	check_value(list, "", "empty-key-value");

	CHECK(!list.put("", "updated"));
	CHECK(list.size() == 1);
	check_value(list, "", "updated");

	CHECK(list.del(""));
	CHECK(list.empty());
	CHECK(!list.del(""));
}

void test_clear_and_reuse() {
	SkipList list;
	CHECK(list.put("a", "1"));
	CHECK(list.put("b", "2"));
	CHECK(list.put("c", "3"));

	list.clear();
	CHECK(list.empty());
	CHECK(list.size() == 0);

	std::string value;
	CHECK(!list.get("a", value));

	list.clear();
	CHECK(list.empty());

	CHECK(list.put("after-clear", "works"));
	CHECK(list.size() == 1);
	check_value(list, "after-clear", "works");
}

void test_copy_operations() {
	SkipList original;
	CHECK(original.put("a", "1"));
	CHECK(original.put("b", "2"));

	SkipList copy(original);
	CHECK(copy.size() == 2);
	check_value(copy, "a", "1");
	check_value(copy, "b", "2");

	CHECK(copy.del("a"));
	CHECK(copy.put("c", "3"));
	CHECK(original.size() == 2);
	check_value(original, "a", "1");
	std::string value;
	CHECK(!original.get("c", value));

	SkipList assigned;
	CHECK(assigned.put("old", "value"));
	assigned = original;
	CHECK(assigned.size() == 2);
	check_value(assigned, "a", "1");
	CHECK(!assigned.get("old", value));

	assigned = assigned;
	CHECK(assigned.size() == 2);
	check_value(assigned, "b", "2");
}

void test_move_operations() {
	SkipList source;
	CHECK(source.put("a", "1"));
	CHECK(source.put("b", "2"));

	SkipList moved(std::move(source));
	CHECK(moved.size() == 2);
	check_value(moved, "a", "1");
	check_value(moved, "b", "2");

	// Moved-from objects only have to remain valid. Clear before reuse so this
	// test does not depend on a particular moved-from state.
	source.clear();
	CHECK(source.put("reused", "yes"));
	check_value(source, "reused", "yes");

	SkipList move_source;
	CHECK(move_source.put("x", "24"));
	CHECK(move_source.put("y", "25"));

	SkipList move_target;
	CHECK(move_target.put("old", "value"));
	move_target = std::move(move_source);
	CHECK(move_target.size() == 2);
	check_value(move_target, "x", "24");
	check_value(move_target, "y", "25");
	std::string value;
	CHECK(!move_target.get("old", value));

	move_source.clear();
	CHECK(move_source.put("reused-again", "yes"));
	check_value(move_source, "reused-again", "yes");
}

void verify_against_map(SkipList &list,
						const std::map<std::string, std::string> &reference,
						int key_count) {
	CHECK(list.size() == reference.size());
	CHECK(list.empty() == reference.empty());

	for (int i = 0; i < key_count; ++i) {
		const std::string key = "key-" + std::to_string(i);
		std::string actual = "unchanged";
		const bool found = list.get(key, actual);
		const auto expected = reference.find(key);

		CHECK(found == (expected != reference.end()));
		if (found) {
			CHECK(actual == expected->second);
		} else {
			CHECK(actual == "unchanged");
		}
	}
}

void test_random_operations_against_std_map() {
	constexpr int key_count = 64;
	constexpr int operation_count = 5000;

	SkipList list(std::make_shared<DefaultStringComparator>(), 20260720);
	std::map<std::string, std::string> reference;
	std::mt19937 engine(20260720);
	std::uniform_int_distribution<int> operation_distribution(0, 99);
	std::uniform_int_distribution<int> key_distribution(0, key_count - 1);

	for (int step = 0; step < operation_count; ++step) {
		const int operation = operation_distribution(engine);
		const std::string key =
			"key-" + std::to_string(key_distribution(engine));

		if (operation < 45) {
			const std::string value = "value-" + std::to_string(step);
			const bool expected_insert = reference.find(key) == reference.end();
			CHECK(list.put(key, value) == expected_insert);
			reference[key] = value;
		} else if (operation < 75) {
			std::string actual = "unchanged";
			const bool found = list.get(key, actual);
			const auto expected = reference.find(key);
			CHECK(found == (expected != reference.end()));
			if (found) {
				CHECK(actual == expected->second);
			} else {
				CHECK(actual == "unchanged");
			}
		} else if (operation < 95) {
			CHECK(list.del(key) == (reference.erase(key) != 0));
		} else {
			list.clear();
			reference.clear();
		}

		CHECK(list.size() == reference.size());
		CHECK(list.empty() == reference.empty());

		if (step % 100 == 0) {
			verify_against_map(list, reference, key_count);
		}
	}

	verify_against_map(list, reference, key_count);
}

// ─── monotony predicate tests ──────────────────────────────────

void test_predicate_range_query() {
	SkipList list;
	CHECK(list.put("alpha", "1"));
	CHECK(list.put("bravo", "2"));
	CHECK(list.put("charlie", "3"));
	CHECK(list.put("delta", "4"));
	CHECK(list.put("echo", "5"));

	// predicate: "bravo" <= key < "delta"
	auto pred = [](const std::string &key) -> int {
		if (key < "bravo") return -1;
		if (key >= "delta") return 1;
		return 0;
	};

	auto result = list.iter_monotony_predicate(pred);
	CHECK(result.has_value());
	auto [begin, end] = result.value();
	CHECK(begin.is_valid());
	CHECK(end.is_valid());

	CHECK(begin.key() == "bravo");
	++begin;
	CHECK(begin.key() == "charlie");
	++begin;
	CHECK(!(begin != end));
}

void test_predicate_no_match() {
	SkipList list;
	CHECK(list.put("alpha", "1"));
	CHECK(list.put("bravo", "2"));

	// predicate: key >= "z" (no match)
	auto pred = [](const std::string &key) -> int {
		if (key < "z") return -1;
		if (key > "z") return 1;
		return 0;
	};

	auto result = list.iter_monotony_predicate(pred);
	CHECK(!result.has_value());
}

void test_predicate_empty_list() {
	SkipList list;
	auto pred = [](const std::string &key) -> int {
		if (key < "a") return -1;
		if (key > "a") return 1;
		return 0;
	};

	auto result = list.iter_monotony_predicate(pred);
	CHECK(!result.has_value());
}

void test_predicate_single_match() {
	SkipList list;
	CHECK(list.put("alpha", "1"));
	CHECK(list.put("bravo", "2"));

	auto pred = [](const std::string &key) -> int {
		if (key < "bravo") return -1;
		if (key > "bravo") return 1;
		return 0;
	};

	auto result = list.iter_monotony_predicate(pred);
	CHECK(result.has_value());
	auto [begin, end] = result.value();
	CHECK(begin.key() == "bravo");
	++begin;
	CHECK(!(begin != end));
}

void test_predicate_all_match() {
	SkipList list;
	CHECK(list.put("a", "1"));
	CHECK(list.put("b", "2"));
	CHECK(list.put("c", "3"));

	// predicate: key >= "" (everything matches)
	auto pred = [](const std::string &key) -> int {
		// never > 0 means everything is in range
		if (key <= "z") return 0;
		return 1;
	};

	auto result = list.iter_monotony_predicate(pred);
	CHECK(result.has_value());
	auto [begin, end] = result.value();

	int count = 0;
	for (auto it = begin; it != end; ++it) count++;
	CHECK(count == 3);
}

void test_preffix_basic() {
	SkipList list(std::make_shared<DefaultStringComparator>());
	CHECK(list.put("apple", "1"));
	CHECK(list.put("application", "2"));
	CHECK(list.put("banana", "3"));
	CHECK(list.put("apricot", "4"));

	auto result = list.preffix_raw_string("app");
	CHECK(result.has_value());
	auto [begin, end] = result.value();

	// Prefix "app" covers ["app", "apq"); "apricot" is the first key past it.
	int count = 0;
	for (auto it = begin; it != end; ++it) count++;
	CHECK(count == 2);  // "apple", "application"
}

void test_preffix_no_match() {
	SkipList list(std::make_shared<DefaultStringComparator>());
	CHECK(list.put("apple", "1"));
	CHECK(list.put("banana", "2"));

	auto result = list.preffix_raw_string("z");
	CHECK(!result.has_value());
}

void test_preffix_exact_match() {
	SkipList list(std::make_shared<DefaultStringComparator>());
	CHECK(list.put("app", "1"));
	CHECK(list.put("apple", "2"));
	CHECK(list.put("apricot", "3"));

	auto result = list.preffix_raw_string("app");
	CHECK(result.has_value());
	auto [begin, end] = result.value();
	int count = 0;
	for (auto it = begin; it != end; ++it) count++;
	CHECK(count == 2);  // "app", "apple" (not "apricot")
}

void test_preffix_excludes_existing_upper_bound() {
	SkipList list;
	CHECK(list.put("apq", "excluded-upper-bound"));
	CHECK(list.put("application", "3"));
	CHECK(list.put("apricot", "excluded-after-bound"));
	CHECK(list.put("app", "1"));
	CHECK(list.put("apple", "2"));
	CHECK(list.put("ape", "excluded-before-prefix"));

	auto result = list.preffix_raw_string("app");
	CHECK(result.has_value());
	auto [begin, end] = result.value();
	const std::vector<std::pair<std::string, std::string>> expected = {
		{"app", "1"}, {"apple", "2"}, {"application", "3"}};
	auto it = begin;
	for (const auto &entry : expected) {
		CHECK(it != end);
		CHECK(it.is_valid());
		CHECK(*it == entry);
		++it;
	}
	CHECK(it == end);
	CHECK(end == list.lower_bound("apq"));
	CHECK(end.is_valid());
	CHECK(end.key() == "apq");
}

void test_preffix_only_upper_bound_is_not_a_match() {
	SkipList list;
	CHECK(list.put("ape", "before-prefix"));
	CHECK(list.put("apq", "upper-bound"));

	// The boundary is the last node; it must not create a false match.
	CHECK(!list.preffix_raw_string("app").has_value());
	CHECK(list.size() == 2);
	check_value(list, "apq", "upper-bound");
}

void test_preffix_carry_excludes_existing_upper_bound() {
	SkipList list;
	const std::string prefix("a\xff", 2);
	const std::vector<std::pair<std::string, std::string>> expected = {
		{prefix, "exact-prefix"},
		{prefix + std::string(1, '\0'), "nul-suffix"},
		{prefix + std::string(1, static_cast<char>(0xff)), "ff-suffix"}};
	CHECK(list.put("a", "before-prefix"));
	for (const auto &entry : expected) {
		CHECK(list.put(entry.first, entry.second));
	}
	CHECK(list.put("b", "upper-bound"));
	CHECK(list.put("ba", "after-bound"));

	// Carrying past the trailing 0xff makes "b" the exclusive upper bound.
	auto result = list.preffix_raw_string(prefix);
	CHECK(result.has_value());
	auto [begin, end] = result.value();
	auto it = begin;
	for (const auto &entry : expected) {
		CHECK(it != end);
		CHECK(it.is_valid());
		CHECK(*it == entry);
		++it;
	}
	CHECK(it == end);
	CHECK(end == list.lower_bound("b"));
	CHECK(end.is_valid());
	CHECK(end.key() == "b");
}

void test_preffix_ff_boundary() {
	SkipList list(std::make_shared<DefaultStringComparator>());
	const std::string k1("\xff\x00", 2);
	const std::string k2("\xff\xff", 2);
	CHECK(list.put(k1, "1"));
	CHECK(list.put(k2, "2"));

	auto result = list.preffix_raw_string(std::string("\xff", 1));
	CHECK(result.has_value());
	auto [begin, end] = result.value();
	int count = 0;
	for (auto it = begin; it != end; ++it) count++;
	CHECK(count == 2);
}

struct TestCase {
	const char *name;
	void (*run)();
};

} // namespace

int main() {
	const TestCase tests[] = {
		{"empty list", test_empty_list},
		{"put, get, and update", test_put_get_and_update},
		{"delete", test_delete},
		{"empty key", test_empty_key},
		{"clear and reuse", test_clear_and_reuse},
		{"copy operations", test_copy_operations},
		{"move operations", test_move_operations},
		{"random operations against std::map",
		 test_random_operations_against_std_map},
		{"predicate range query",
		 test_predicate_range_query},
		{"predicate no match", test_predicate_no_match},
		{"predicate empty list", test_predicate_empty_list},
		{"predicate single match",
		 test_predicate_single_match},
		{"predicate all match", test_predicate_all_match},
		{"preffix basic", test_preffix_basic},
		{"preffix no match", test_preffix_no_match},
		{"preffix exact match", test_preffix_exact_match},
		{"preffix excludes existing upper bound",
		 test_preffix_excludes_existing_upper_bound},
		{"preffix only upper bound is not a match",
		 test_preffix_only_upper_bound_is_not_a_match},
		{"preffix carry excludes existing upper bound",
		 test_preffix_carry_excludes_existing_upper_bound},
		{"preffix ff boundary", test_preffix_ff_boundary},
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

	std::cout << passed << '/' << (sizeof(tests) / sizeof(tests[0]))
			  << " tests passed\n";
	return passed == (sizeof(tests) / sizeof(tests[0])) ? 0 : 1;
}
