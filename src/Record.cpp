#include "Record.h"
#include <stdexcept> // std::logic_error()

InternalKey::InternalKey(std::string key, std::uint64_t commit_sequence,
						 ValueType value_type)
	: _key(key), _commit_sequence(commit_sequence), _value_type(value_type) {}

std::string InternalKey::encode(const InternalKey &internal_key) {
	std::uint64_t commit_sequence = internal_key._commit_sequence;
	std::string key = internal_key._key;
	ValueType value_type = internal_key._value_type;
	if (((commit_sequence >> (64 - 8)) & 0xff) != 0) {
		throw std::logic_error("commit_sequence must shorter than 7 bytes.");
	}
	std::string encode_str = key;
	for (int i = 0; i < 7; i++) {
		std::uint8_t cur_byte = commit_sequence >> ((6 - i) * 8);
		encode_str.push_back(cur_byte);
	}
	encode_str.push_back(value_type);
	return encode_str;
}

InternalKey InternalKey::decode(const std::string &str) {
	int byte_cnt = str.size();
	if (byte_cnt < 8) {
		throw std::logic_error(
			"encoded internal key must be at least 8 bytes.");
	}
	ValueType value_type = static_cast<ValueType>(str[byte_cnt - 1]);
	std::uint64_t commit_sequence = 0;
	for (int i = 0; i < 7; i++) {
		std::uint8_t byte = str[byte_cnt - 8 + i];
		commit_sequence = (commit_sequence << 8) | byte;
	}
	std::string key = str.substr(0, byte_cnt - 8);
	InternalKey intr_key(key, commit_sequence, value_type);
	return intr_key;
}

LookUpResult::LookUpResult(const std::string &value,
						   std::uint64_t commit_sequence, ValueType value_type)
	: _value(value), _commit_sequence(commit_sequence),
	  _value_type(value_type) {}

bool LookUpResult::is_found() const {
	return _value_type == Value || _value_type == TombStone;
}