#pragma once
#include <cstdint> // std::uint64_t, std::uint8_t
#include <string>

enum ValueType : std::uint8_t { TombStone, Value, NotFound };

class InternalKey {
public:
	InternalKey(std::string key, std::uint64_t commit_sequence,
				ValueType value_type);
	static std::string encode(const InternalKey &internal_key);
	static InternalKey decode(const std::string &str);

	std::string _key;
	std::uint64_t _commit_sequence;
	ValueType _value_type;
};

struct LookUpResult {
public:
	inline static const std::string EMPTY_VALUE = "";
	static const std::uint64_t EMPTY_COMSEQ = 0;
	LookUpResult(const std::string &value = EMPTY_VALUE,
				 std::uint64_t commit_sequence = EMPTY_COMSEQ,
				 ValueType value_type = NotFound);
	bool is_found() const;
	std::string _value;
	std::uint64_t _commit_sequence;
	ValueType _value_type;
};