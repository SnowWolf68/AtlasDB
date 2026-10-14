#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class Resp {
	static const std::size_t MAX_BUFFER_SIZE = 1024 * 1024;
	static const std::size_t MAX_SINGLE_PARAMETER_SIZE = 64 * 1024;
	inline static const std::string SEP = "\r\n";

public:
	enum class RespValueType {
		SimpleString,
		Error,
		Integer,
		BulkString,
		Arrays
	};
	class RespValue {
	public:
		RespValueType _value_type;
		std::string _simple_string;
		std::string _errors;
		std::int64_t _integer;
		std::string _bulk_string;
		std::vector<RespValue> _array;

		RespValue();
		RespValue(std::vector<RespValue> array);
		static RespValue SimpleString(std::string simple_string);
		static RespValue BulkString(std::string bulk_string);
		static RespValue Errors(std::string errors);
		static RespValue Integer(std::int64_t integer);
		static RespValue Array(std::vector<RespValue> array);
	};
	enum class DecodeState { Complete, NeedMore, Error };
	class DecodeResult {
	public:
		DecodeState _state;
		RespValue _result_value;
		std::string _decode_err_msg;

		static DecodeResult NeedMore();
		static DecodeResult Error(const std::string &err);
		static DecodeResult SimpleString(const std::string &simple_string);
		static DecodeResult BulkString(const std::string &bulk_string);
		static DecodeResult Integer(std::int64_t n);
		static DecodeResult Array(std::vector<RespValue> array);
		static DecodeResult DecodeErr(const std::string &decode_err_msg);
	};

	Resp(std::size_t max_buffer_size = MAX_BUFFER_SIZE,
		 std::size_t max_single_parameter_size = MAX_SINGLE_PARAMETER_SIZE);

	DecodeResult decode();
	bool append(std::string data);
	static std::optional<std::string> encode(RespValue resp_value);

private:
	static std::optional<std::string> encode_except_array(RespValue resp_value);

	// <DecodeResult, erase_size>
	std::pair<DecodeResult, std::optional<std::size_t>>
	decode_without_erase(
		std::optional<std::size_t> start_idx_opt = std::nullopt);
	static bool check_integer(const std::string &integer_str);

	std::string _buffer;
	std::size_t _max_buffer_size;
	std::size_t _max_single_parameter_size;
};