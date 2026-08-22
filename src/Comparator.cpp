#include "Comparator.h"

int DefaultStringComparator::operator()(const std::string &left,
										const std::string &right) const {
	if (left == right) {
		return 0;
	} else if (left < right) {
		return -1;
	} else {
		return 1;
	}
}

int InternalKeyComparator::operator()(const std::string &left_str,
									  const std::string &right_str) const {
	InternalKey left = InternalKey::decode(left_str);
	InternalKey right = InternalKey::decode(right_str);
	if (left._key == right._key) {
		if (left._commit_sequence == right._commit_sequence) {
            // 虽然只要求key升序, seq降序, 但是value_type的逻辑也要写
            // 否则就会出现三者并不完全相同的两个对象比较出来是相同的
			if (left._value_type == right._value_type) {
				return 0;
			} else if (left._value_type > right._value_type) {
				return -1;
			} else {
				return 1;
			}
		} else if (left._commit_sequence > right._commit_sequence) {
			return -1;
		} else {
			return 1;
		}
	} else if (left._key < right._key) {
		return -1;
	} else {
		return 1;
	}
}