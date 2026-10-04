#include "resp.h"
#include "log.h"
#include <cstddef>
#include <exception>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

Resp::RespValue::RespValue() {}

Resp::RespValue::RespValue(std::vector<Resp::RespValue> array)
	: _value_type(RespValueType::Arrays), _array(array) {}


Resp::RespValue Resp::RespValue::SimpleString(std::string simple_string) {
	RespValue resp;
	resp._value_type = RespValueType::SimpleString;
	resp._simple_string = simple_string;
	return resp;
}
Resp::RespValue Resp::RespValue::BulkString(std::string bulk_string) {
	RespValue resp;
	resp._value_type = RespValueType::BulkString;
	resp._bulk_string = bulk_string;
	return resp;
}
Resp::RespValue Resp::RespValue::Errors(std::string errors) {
	RespValue resp;
	resp._value_type = RespValueType::Error;
	resp._errors = errors;
	return resp;
}
Resp::RespValue Resp::RespValue::Integer(std::int64_t integer) {
	RespValue resp;
	resp._value_type = RespValueType::Integer;
	resp._integer = integer;
	return resp;
}
Resp::RespValue Resp::RespValue::Array(std::vector<RespValue> array) {
	return RespValue(array);
}

Resp::DecodeResult Resp::DecodeResult::NeedMore() {
	DecodeResult res;
	res._state = DecodeState::NeedMore;
	return res;
}
Resp::DecodeResult Resp::DecodeResult::Error(const std::string &err) {
	DecodeResult res;
	res._state = DecodeState::Complete;
	res._result_value._errors = err;
	res._result_value._value_type = RespValueType::Error;
	return res;
}
Resp::DecodeResult
Resp::DecodeResult::SimpleString(const std::string &simple_string) {
	DecodeResult res;
	res._state = DecodeState::Complete;
	res._result_value._simple_string = simple_string;
	res._result_value._value_type = RespValueType::SimpleString;
	return res;
}
Resp::DecodeResult
Resp::DecodeResult::BulkString(const std::string &bulk_string) {
	DecodeResult res;
	res._state = DecodeState::Complete;
	res._result_value._bulk_string = bulk_string;
	res._result_value._value_type = RespValueType::BulkString;
	return res;
}
Resp::DecodeResult Resp::DecodeResult::Integer(std::int64_t n) {
	DecodeResult res;
	res._state = DecodeState::Complete;
	res._result_value._integer = n;
	res._result_value._value_type = RespValueType::Integer;
	return res;
}
Resp::DecodeResult Resp::DecodeResult::Array(std::vector<RespValue> array) {
	DecodeResult res;
	res._state = DecodeState::Complete;
	res._result_value._array = array;
	res._result_value._value_type = RespValueType::Arrays;
	return res;
}

Resp::DecodeResult
Resp::DecodeResult::DecodeErr(const std::string &decode_err_msg) {
	DecodeResult resp;
	resp._state = DecodeState::Error;
	resp._decode_err_msg = decode_err_msg;
	resp._result_value._value_type = RespValueType::Error;
	return resp;
}

Resp::Resp(std::size_t max_buffer_size, std::size_t max_single_parameter_size)
	: _max_buffer_size(max_buffer_size),
	  _max_single_parameter_size(max_single_parameter_size) {}

std::optional<std::string> Resp::encode_except_array(RespValue resp_value) {
	std::stringstream ss;
	switch (resp_value._value_type) {
		case RespValueType::SimpleString: {
			ss << "+" << resp_value._simple_string << SEP;
			break;
		}
		case RespValueType::Error: {
			ss << "-" << resp_value._errors << SEP;
			break;
		}
		case RespValueType::Integer: {
			ss << ":" << resp_value._integer << SEP;
			break;
		}
		case RespValueType::BulkString: {
			ss << "$" << resp_value._bulk_string.size() << SEP
			   << resp_value._bulk_string << SEP;
			break;
		}
		default: {
			return std::nullopt;
			break;
		}
	}
	return ss.str();
}

std::optional<std::string> Resp::encode(RespValue resp_value) {
	std::stringstream ss;
	if (resp_value._value_type == RespValueType::SimpleString ||
		resp_value._value_type == RespValueType::Error ||
		resp_value._value_type == RespValueType::BulkString ||
		resp_value._value_type == RespValueType::Integer) {
		std::optional<std::string> opt = encode_except_array(resp_value);
		if (opt == std::nullopt) { return std::nullopt; }
		ss << opt.value();
	} else if (resp_value._value_type == RespValueType::Arrays) {
		ss << "*" << resp_value._array.size() << SEP;
		for (RespValue value : resp_value._array) {
			std::optional<std::string> opt = encode_except_array(value);
			if (opt == std::nullopt) { return std::nullopt; }
			ss << opt.value();
		}
	} else {
		return std::nullopt;
	}
	return ss.str();
}

bool Resp::append(std::string data) {
	if (_buffer.size() + data.size() > _max_buffer_size) { return false; }
	_buffer.append(data);
	return true;
}

bool Resp::check_integer(const std::string &integer_str) {
	if (integer_str.empty()) { return false; }
	if (integer_str[0] != '-' &&
		!(integer_str[0] >= '0' && integer_str[0] <= '9')) {
		return false;
	}
	for (std::size_t i = 1; i < integer_str.size(); i++) {
		char c = integer_str[i];
		if (!(c >= '0' && c <= '9')) { return false; }
	}
	return true;
}

std::pair<Resp::DecodeResult, std::optional<std::size_t>>
Resp::decode_except_array_without_erase(
	std::optional<std::size_t> start_idx_opt) {
	std::size_t start_idx =
		start_idx_opt == std::nullopt ? 0 : start_idx_opt.value();
	if (_buffer.size() - start_idx == 0) {
		return std::make_pair(DecodeResult::NeedMore(), std::nullopt);
	}
	char type = _buffer[0 + start_idx];
	switch (type) {
		case '+': {
			std::size_t pos = _buffer.find(SEP, start_idx);
			if (pos != _buffer.npos) {
				std::string simple_string =
					_buffer.substr(1 + start_idx, pos - start_idx - 1);
				return std::make_pair(DecodeResult::SimpleString(simple_string),
									  pos - start_idx + 2);
			}
			break;
		}
		case '-': {
			std::size_t pos = _buffer.find(SEP, start_idx);
			if (pos != _buffer.npos) {
				std::string errors =
					_buffer.substr(1 + start_idx, pos - start_idx - 1);
				return std::make_pair(DecodeResult::Error(errors),
									  pos - start_idx + 2);
			}
			break;
		}
		case ':': {
			std::size_t pos = _buffer.find(SEP, start_idx);
			if (pos != _buffer.npos) {
				std::string integer_str =
					_buffer.substr(1 + start_idx, pos - 1 - start_idx);
				if (!check_integer(integer_str)) {
					return std::make_pair(
						DecodeResult::DecodeErr("parse integer failed"),
						std::nullopt);
				}
				std::int64_t integer = 0;
				try {
					integer = std::stoll(integer_str);
				} catch (const std::exception &e) {
					LOG(Log::ERROR) << "stoll error, error: " << e.what();
					return std::make_pair(
						DecodeResult::DecodeErr("integer format error"),
						std::nullopt);
				}
				return std::make_pair(DecodeResult::Integer(integer),
									  pos - start_idx + 2);
			}
			break;
		}
		case '$': {
			std::size_t pos = _buffer.find(SEP, start_idx);
			if (pos != _buffer.npos) {
				std::string size_str =
					_buffer.substr(1 + start_idx, pos - start_idx - 1);
				if (!check_integer(size_str)) {
					return std::make_pair(
						DecodeResult::DecodeErr("parse integer failed"),
						std::nullopt);
				}
				int size = -1;
				try {
					size = std::stoi(size_str);
				} catch (const std::exception &e) {
					LOG(Log::ERROR) << "stoi error, error: " << e.what();
					return std::make_pair(
						DecodeResult::DecodeErr("bulkstring size format error"),
						std::nullopt);
				}
				if (size < 0) {
					return std::make_pair(
						DecodeResult::DecodeErr("bulkstring size format error"),
						std::nullopt);
				}
				if (size > _max_single_parameter_size) {
					return std::make_pair(
						DecodeResult::DecodeErr(
							"bulkstring size exceeds max single param size"),
						std::nullopt);
				}
				if (1 + size_str.size() + 2 + size + 2 >
					_buffer.size() - start_idx) {
					return std::make_pair(DecodeResult::NeedMore(),
										  std::nullopt);
				}
				// 判断最后结尾的是不是SEP
				std::string last = _buffer.substr(
					1 + size_str.size() + start_idx + 2 + size, 2);
				if (last != SEP) {
					return std::make_pair(
						DecodeResult::DecodeErr("bulk string missing SEP"),
						std::nullopt);
				}
				std::string bulkstring =
					_buffer.substr(1 + size_str.size() + 2 + start_idx, size);
				return std::make_pair(DecodeResult::BulkString(bulkstring),
									  1 + size_str.size() + 2 + size + 2);
			}
			break;
		}
	}
	return std::make_pair(DecodeResult::NeedMore(), std::nullopt);
}

Resp::DecodeResult Resp::decode() {
	if (_buffer.size() == 0) { return DecodeResult::NeedMore(); }
	char type = _buffer[0];
	if (type == '*') {
		std::size_t pos = _buffer.find(SEP);
		if (pos != _buffer.npos) {
			std::size_t erase_count = 0;
			std::string array_size_str = _buffer.substr(1, pos - 1);
			if (!check_integer(array_size_str)) {
				return DecodeResult::DecodeErr("parse integer failed");
			}
			int array_size = -1;
			try {
				array_size = std::stoi(array_size_str);
			} catch (const std::exception &e) {
				LOG(Log::ERROR) << "stoi parse failed, array_size decode "
								   "failed, error: "
								<< e.what();
				return DecodeResult::DecodeErr(
					"stoi parse failed, array_size decode failed.");
			}
			if (array_size < 0) {
				return DecodeResult::DecodeErr(
					"stoi parse failed, array_size decode failed.");
			}
			erase_count += (1 + array_size_str.size() + 2);
			DecodeResult result;
			for (int i = 0; i < array_size; i++) {
				auto res = decode_except_array_without_erase(erase_count);
				if (res.first._state == DecodeState::NeedMore) {
					return DecodeResult::NeedMore();
				} else if (res.first._state == DecodeState::Error ||
						   res.second == std::nullopt) {
					LOG(Log::ERROR) << "array member decode failed, errmsg: "
									<< res.first._decode_err_msg;
					return DecodeResult::DecodeErr(
						"array member decode failed, errmsg: " +
						res.first._decode_err_msg);
				}
				result._result_value._array.push_back(res.first._result_value);
				erase_count += res.second.value();
			}
			_buffer.erase(0, erase_count);
			result._result_value._value_type = RespValueType::Arrays;
			result._state = DecodeState::Complete;
			return result;
		}
	} else if (type == '+' || type == '-' || type == ':' || type == '$') {
		std::pair<DecodeResult, std::optional<std::size_t>> res =
			decode_except_array_without_erase();
		if (res.first._state == DecodeState::Complete) {
			if (res.second == std::nullopt) {
				LOG(Log::ERROR) << "erase count error";
				return DecodeResult::Error("erase count error");
			}
			_buffer.erase(0, res.second.value());
			return res.first;
		} else if (res.first._state == DecodeState::NeedMore) {
			return DecodeResult::NeedMore();
		} else {
			// Error
			return res.first;
		}
	} else {
		return DecodeResult::DecodeErr("type error, type: " +
									   std::to_string(type));
	}
	return DecodeResult::NeedMore();
}