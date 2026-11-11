#include "resp.h"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
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

// ─── encode ─────────────────────────────────────────────────────

void test_encode_scalars() {
	auto simple = Resp::encode(Resp::RespValue::SimpleString("OK"));
	CHECK(simple.has_value());
	CHECK(simple.value() == "+OK\r\n");

	auto empty_simple =
		Resp::encode(Resp::RespValue::SimpleString(""));
	CHECK(empty_simple.has_value());
	CHECK(empty_simple.value() == "+\r\n");

	auto error = Resp::encode(Resp::RespValue::Errors("ERR unknown"));
	CHECK(error.has_value());
	CHECK(error.value() == "-ERR unknown\r\n");

	auto integer = Resp::encode(Resp::RespValue::Integer(123));
	CHECK(integer.has_value());
	CHECK(integer.value() == ":123\r\n");

	auto negative = Resp::encode(Resp::RespValue::Integer(-5));
	CHECK(negative.has_value());
	CHECK(negative.value() == ":-5\r\n");

	// RESP 整数是 64 位:超过 32 位范围的值必须无损编码
	auto big = Resp::encode(Resp::RespValue::Integer(9000000000LL));
	CHECK(big.has_value());
	CHECK(big.value() == ":9000000000\r\n");

	auto bulk = Resp::encode(Resp::RespValue::BulkString("hello"));
	CHECK(bulk.has_value());
	CHECK(bulk.value() == "$5\r\nhello\r\n");

	auto empty_bulk = Resp::encode(Resp::RespValue::BulkString(""));
	CHECK(empty_bulk.has_value());
	CHECK(empty_bulk.value() == "$0\r\n\r\n");
}

void test_encode_array() {
	std::vector<Resp::RespValue> elements;
	elements.push_back(Resp::RespValue::BulkString("get"));
	elements.push_back(Resp::RespValue::BulkString("key"));
	auto array = Resp::encode(Resp::RespValue::Array(elements));
	CHECK(array.has_value());
	CHECK(array.value() == "*2\r\n$3\r\nget\r\n$3\r\nkey\r\n");

	auto empty = Resp::encode(Resp::RespValue::Array({}));
	CHECK(empty.has_value());
	CHECK(empty.value() == "*0\r\n");
}

void test_encode_rejects_nested_array() {
	// encode 只支持一层数组:数组元素本身又是数组时必须拒绝(nullopt)
	Resp::RespValue inner = Resp::RespValue::Array(
		{Resp::RespValue::BulkString("x")});
	auto nested = Resp::encode(Resp::RespValue::Array({inner}));
	CHECK(!nested.has_value());
}

// ─── decode: 标量帧 ─────────────────────────────────────────────

void test_decode_simple_string() {
	Resp resp;
	CHECK(resp.append("+OK\r\n"));
	Resp::DecodeResult res = resp.decode();
	CHECK(res._state == Resp::DecodeState::Complete);
	CHECK(res._result_value._value_type == Resp::RespValueType::SimpleString);
	CHECK(res._result_value._simple_string == "OK");
	// 整帧消费后 buffer 应为空:下一次 decode 只能 NeedMore
	CHECK(resp.decode()._state == Resp::DecodeState::NeedMore);
}

void test_decode_error_frame() {
	// '-' 帧是"成功解码出一个错误帧":状态 Complete + 类型 Error
	Resp resp;
	CHECK(resp.append("-ERR unknown\r\n"));
	Resp::DecodeResult res = resp.decode();
	CHECK(res._state == Resp::DecodeState::Complete);
	CHECK(res._result_value._value_type == Resp::RespValueType::Error);
	CHECK(res._result_value._errors == "ERR unknown");
}

void test_decode_integers() {
	Resp resp;
	CHECK(resp.append(":123\r\n"));
	Resp::DecodeResult res = resp.decode();
	CHECK(res._state == Resp::DecodeState::Complete);
	CHECK(res._result_value._value_type == Resp::RespValueType::Integer);
	CHECK(res._result_value._integer == 123);

	CHECK(resp.append(":-1\r\n"));
	res = resp.decode();
	CHECK(res._state == Resp::DecodeState::Complete);
	CHECK(res._result_value._integer == -1);

	CHECK(resp.append(":-5\r\n"));
	res = resp.decode();
	CHECK(res._state == Resp::DecodeState::Complete);
	CHECK(res._result_value._integer == -5);

	// 64 位值域:超过 int32 的合法整数必须原样解出
	CHECK(resp.append(":9000000000\r\n"));
	res = resp.decode();
	CHECK(res._state == Resp::DecodeState::Complete);
	CHECK(res._result_value._integer == 9000000000LL);

	CHECK(resp.append(":9223372036854775807\r\n"));
	res = resp.decode();
	CHECK(res._state == Resp::DecodeState::Complete);
	CHECK(res._result_value._integer == 9223372036854775807LL);
}

void test_decode_integer_overflow_rejected() {
	// INT64_MAX + 1 超出协议值域:必须报错而非截断/垃圾
	Resp resp;
	CHECK(resp.append(":9223372036854775808\r\n"));
	Resp::DecodeResult res = resp.decode();
	CHECK(res._state == Resp::DecodeState::Error);
}

void test_decode_bulk_string() {
	Resp resp;
	CHECK(resp.append("$5\r\nhello\r\n"));
	Resp::DecodeResult res = resp.decode();
	CHECK(res._state == Resp::DecodeState::Complete);
	CHECK(res._result_value._value_type == Resp::RespValueType::BulkString);
	CHECK(res._result_value._bulk_string == "hello");
	CHECK(resp.decode()._state == Resp::DecodeState::NeedMore);

	// 空批量字符串是合法帧
	CHECK(resp.append("$0\r\n\r\n"));
	res = resp.decode();
	CHECK(res._state == Resp::DecodeState::Complete);
	CHECK(res._result_value._bulk_string.empty());
}

void test_decode_partial_frames_need_more() {
	// 半帧一律 NeedMore:长度行不完整 / payload 不完整 / 头都不完整
	const char *partials[] = {
		"+OK",		 // 无 SEP
		":12",		 // 无 SEP
		"$5",		 // 长度行无 SEP
		"$5\r\nhel",	 // payload 不足
		"*2",		 // 数组头不完整
		"*2\r\n+OK",	 // 数组头完整但元素不完整
	};
	for (const char *partial : partials) {
		Resp resp;
		CHECK(resp.append(partial));
		Resp::DecodeResult res = resp.decode();
		CHECK(res._state == Resp::DecodeState::NeedMore);
		// NeedMore 时值无意义:不得包含任何元素
		CHECK(res._result_value._array.empty());
	}
}

void test_decode_malformed_frames_rejected() {
	const char *malformed[] = {
		":5abc\r\n", // 整数带尾随垃圾
		":-\r\n",	 // 只有负号
		"$-1\r\n",	 // 负长度(服务端请求方向非法)
		"$-5\r\n",	 // 负长度
		"$5x\r\n",	 // 长度带尾随垃圾
		"$\r\n",	 // 空长度字段
		"X42\r\n",	 // 未知类型字节
	};
	for (const char *bad : malformed) {
		Resp resp;
		CHECK(resp.append(bad));
		Resp::DecodeResult res = resp.decode();
		CHECK(res._state == Resp::DecodeState::Error);
		CHECK(!res._decode_err_msg.empty());
	}
}

void test_decode_pipelining() {
	// 一段 buffer 连发三帧:逐次 decode 各自取走,最终恰好清空
	Resp resp;
	CHECK(resp.append("+OK\r\n:123\r\n$2\r\nhi\r\n"));

	Resp::DecodeResult res = resp.decode();
	CHECK(res._state == Resp::DecodeState::Complete);
	CHECK(res._result_value._simple_string == "OK");

	res = resp.decode();
	CHECK(res._state == Resp::DecodeState::Complete);
	CHECK(res._result_value._integer == 123);

	res = resp.decode();
	CHECK(res._state == Resp::DecodeState::Complete);
	CHECK(res._result_value._bulk_string == "hi");

	CHECK(resp.decode()._state == Resp::DecodeState::NeedMore);
}

// ─── decode: 数组帧 ─────────────────────────────────────────────

void test_decode_array_command_form() {
	// Redis 客户端命令的真实形态:数组 + 批量字符串元素
	Resp resp;
	CHECK(resp.append("*2\r\n$3\r\nget\r\n$3\r\nkey\r\n"));
	Resp::DecodeResult res = resp.decode();
	CHECK(res._state == Resp::DecodeState::Complete);
	CHECK(res._result_value._value_type == Resp::RespValueType::Arrays);
	CHECK(res._result_value._array.size() == 2);
	CHECK(res._result_value._array[0]._value_type ==
		  Resp::RespValueType::BulkString);
	CHECK(res._result_value._array[0]._bulk_string == "get");
	CHECK(res._result_value._array[1]._bulk_string == "key");
	CHECK(resp.decode()._state == Resp::DecodeState::NeedMore);
}

void test_decode_array_mixed_elements() {
	Resp resp;
	CHECK(resp.append("*2\r\n+OK\r\n$3\r\nacc\r\n"));
	Resp::DecodeResult res = resp.decode();
	CHECK(res._state == Resp::DecodeState::Complete);
	CHECK(res._result_value._array.size() == 2);
	CHECK(res._result_value._array[0]._value_type ==
		  Resp::RespValueType::SimpleString);
	CHECK(res._result_value._array[0]._simple_string == "OK");
	CHECK(res._result_value._array[1]._value_type ==
		  Resp::RespValueType::BulkString);
	CHECK(res._result_value._array[1]._bulk_string == "acc");
}

void test_decode_array_bulk_position_invariant() {
	// 回归钉:bulk 元素的尾 SEP 校验曾漏加 start_idx,
	// bulk 在首位/末位/独占都必须同样可解
	Resp first;
	CHECK(first.append("*2\r\n$3\r\nacc\r\n+OK\r\n"));
	Resp::DecodeResult res = first.decode();
	CHECK(res._state == Resp::DecodeState::Complete);
	CHECK(res._result_value._array.size() == 2);
	CHECK(res._result_value._array[0]._bulk_string == "acc");

	Resp single;
	CHECK(single.append("*1\r\n$3\r\nacc\r\n"));
	res = single.decode();
	CHECK(res._state == Resp::DecodeState::Complete);
	CHECK(res._result_value._array.size() == 1);
	CHECK(res._result_value._array[0]._bulk_string == "acc");
}

void test_decode_partial_array_need_more() {
	// 声明的元素数没到齐:NeedMore 且值无意义(空数组)
	Resp resp;
	CHECK(resp.append("*2\r\n+OK\r\n"));
	Resp::DecodeResult res = resp.decode();
	CHECK(res._state == Resp::DecodeState::NeedMore);
	CHECK(res._result_value._array.empty());

	// 元素的 payload 中途截断同样 NeedMore
	Resp mid;
	CHECK(mid.append("*2\r\n$3\r\nge\r\n"));
	res = mid.decode();
	CHECK(res._state == Resp::DecodeState::NeedMore);
}

void test_decode_array_streamed_in_chunks() {
	// 流式语义:TCP 分片到达,先到的部分必须 NeedMore 而非报错
	Resp resp;
	CHECK(resp.append("*2\r\n$3\r\nge"));
	CHECK(resp.decode()._state == Resp::DecodeState::NeedMore);
	CHECK(resp.append("t\r\n$3\r\nkey\r\n"));
	Resp::DecodeResult res = resp.decode();
	CHECK(res._state == Resp::DecodeState::Complete);
	CHECK(res._result_value._array.size() == 2);
	CHECK(res._result_value._array[0]._bulk_string == "get");
	CHECK(res._result_value._array[1]._bulk_string == "key");
	CHECK(resp.decode()._state == Resp::DecodeState::NeedMore);
}

void test_decode_empty_array() {
	Resp resp;
	CHECK(resp.append("*0\r\n"));
	Resp::DecodeResult res = resp.decode();
	CHECK(res._state == Resp::DecodeState::Complete);
	CHECK(res._result_value._value_type == Resp::RespValueType::Arrays);
	CHECK(res._result_value._array.empty());
	CHECK(resp.decode()._state == Resp::DecodeState::NeedMore);
}

void test_decode_array_member_error_atomic() {
	// 元素解析失败:整笔数组报 Error;按"Error 不消费 buffer"合同,
	// 同一 buffer 重复 decode 应得到同一个错误而非进度推进
	Resp resp;
	CHECK(resp.append("*1\r\n:abc\r\n"));
	Resp::DecodeResult res = resp.decode();
	CHECK(res._state == Resp::DecodeState::Error);
	res = resp.decode();
	CHECK(res._state == Resp::DecodeState::Error);
}

void test_decode_array_negative_size_rejected() {
	// 回归钉:负数组大小(含 -1)一律拒绝,与 bulk 的 size<0 语义对齐
	Resp resp;
	CHECK(resp.append("*-1\r\n"));
	CHECK(resp.decode()._state == Resp::DecodeState::Error);

	CHECK(resp.append("*-3\r\n"));
	CHECK(resp.decode()._state == Resp::DecodeState::Error);
}

void test_decode_nested_array_boundary() {
	Resp resp;
	CHECK(resp.append("*1\r\n*1\r\n+OK\r\n"));
	auto result = resp.decode();
	CHECK(result._state == Resp::DecodeState::Complete);
	CHECK(result._result_value._value_type == Resp::RespValueType::Arrays);
	CHECK(result._result_value._array.size() == 1);
	const auto &inner = result._result_value._array[0];
	CHECK(inner._value_type == Resp::RespValueType::Arrays);
	CHECK(inner._array.size() == 1);
	CHECK(inner._array[0]._value_type == Resp::RespValueType::SimpleString);
	CHECK(inner._array[0]._simple_string == "OK");
	CHECK(resp.decode()._state == Resp::DecodeState::NeedMore);
}

const std::string nested_array_frame =
	"*4\r\n+HEAD\r\n*3\r\n:1\r\n*0\r\n*1\r\n$3\r\nxyz\r\n+TAIL\r\n$3\r\nEND\r\n";

void check_nested_array_result(const Resp::DecodeResult &result) {
	CHECK(result._state == Resp::DecodeState::Complete);
	CHECK(result._result_value._value_type == Resp::RespValueType::Arrays);
	const auto &outer = result._result_value._array;
	CHECK(outer.size() == 4);
	CHECK(outer[0]._value_type == Resp::RespValueType::SimpleString);
	CHECK(outer[0]._simple_string == "HEAD");
	CHECK(outer[1]._value_type == Resp::RespValueType::Arrays);
	const auto &inner = outer[1]._array;
	CHECK(inner.size() == 3);
	CHECK(inner[0]._value_type == Resp::RespValueType::Integer);
	CHECK(inner[0]._integer == 1);
	CHECK(inner[1]._value_type == Resp::RespValueType::Arrays);
	CHECK(inner[1]._array.empty());
	CHECK(inner[2]._value_type == Resp::RespValueType::Arrays);
	CHECK(inner[2]._array.size() == 1);
	CHECK(inner[2]._array[0]._value_type == Resp::RespValueType::BulkString);
	CHECK(inner[2]._array[0]._bulk_string == "xyz");
	CHECK(outer[2]._value_type == Resp::RespValueType::SimpleString);
	CHECK(outer[2]._simple_string == "TAIL");
	CHECK(outer[3]._value_type == Resp::RespValueType::BulkString);
	CHECK(outer[3]._bulk_string == "END");
}

void test_decode_nested_array_and_pipeline() {
	Resp resp;
	CHECK(resp.append(nested_array_frame + "*1\r\n$4\r\nPING\r\n"));
	check_nested_array_result(resp.decode());
	auto next = resp.decode();
	CHECK(next._state == Resp::DecodeState::Complete);
	CHECK(next._result_value._value_type == Resp::RespValueType::Arrays);
	CHECK(next._result_value._array.size() == 1);
	CHECK(next._result_value._array[0]._value_type == Resp::RespValueType::BulkString);
	CHECK(next._result_value._array[0]._bulk_string == "PING");
	CHECK(resp.decode()._state == Resp::DecodeState::NeedMore);
}

void test_decode_nested_array_byte_stream() {
	Resp resp;
	for (std::size_t i = 0; i < nested_array_frame.size(); ++i) {
		CHECK(resp.append(nested_array_frame.substr(i, 1)));
		auto result = resp.decode();
		if (i + 1 < nested_array_frame.size()) {
			CHECK(result._state == Resp::DecodeState::NeedMore);
			CHECK(result._result_value._array.empty());
		} else {
			check_nested_array_result(result);
		}
	}
	CHECK(resp.decode()._state == Resp::DecodeState::NeedMore);
}

void test_decode_nested_array_error_atomic() {
	Resp resp;
	CHECK(resp.append("*2\r\n+HEAD\r\n*1\r\nX"));
	auto result = resp.decode();
	CHECK(result._state == Resp::DecodeState::Error);
	CHECK(result._result_value._array.empty());
	CHECK(!result._decode_err_msg.empty());
	CHECK(resp.append("*1\r\n$4\r\nPING\r\n"));
	CHECK(resp.decode()._state == Resp::DecodeState::Error);
}

void test_decode_array_unknown_member_type_rejected() {
	for (const char *frame : {"*1\r\nX42\r\n", "*2\r\n+OK\r\nX42\r\n"}) {
		Resp resp;
		CHECK(resp.append(frame));
		auto result = resp.decode();
		CHECK(result._state == Resp::DecodeState::Error);
		CHECK(!result._decode_err_msg.empty());
		CHECK(result._result_value._array.empty());
		// Errors preserve the invalid frame; later commands cannot hide it.
		CHECK(resp.append("*1\r\n$4\r\nPING\r\n"));
		CHECK(resp.decode()._state == Resp::DecodeState::Error);
	}
}

void test_decode_array_unknown_member_type_streamed() {
	Resp resp;
	CHECK(resp.append("*1\r\n"));
	CHECK(resp.decode()._state == Resp::DecodeState::NeedMore);
	CHECK(resp.append("X"));
	// The type byte alone proves invalidity; no CRLF is needed to reject it.
	CHECK(resp.decode()._state == Resp::DecodeState::Error);
}

// ─── append 上限 ────────────────────────────────────────────────

void test_append_limits() {
	// 默认构造可用
	Resp fresh;
	CHECK(fresh.append("+OK\r\n"));

	// 自定义上限:单参数 64 字节 / 总 buffer 256 字节
	Resp resp(256, 64);
	CHECK(resp.append(std::string(64, 'a')));
	CHECK(resp.append(std::string(65, 'a'))); // chunk size is not parameter size

	// 总量:4×64 恰好填满,第 5 次必须拒绝
	Resp total(256, 64);
	for (int i = 0; i < 4; i++) {
		CHECK(total.append(std::string(64, 'b')));
	}
	CHECK(!total.append(std::string(64, 'b')));
}

void test_bulk_parameter_limit() {
	const std::string frame = "$64\r\n" + std::string(64, 'x') + "\r\n";
	Resp resp(256, 64);
	CHECK(resp.append(frame + frame)); // One append exceeds per-parameter limit.
	for (int i = 0; i < 2; ++i) {
		auto result = resp.decode();
		CHECK(result._state == Resp::DecodeState::Complete);
		CHECK(result._result_value._bulk_string == std::string(64, 'x'));
	}
	CHECK(resp.decode()._state == Resp::DecodeState::NeedMore);
	for (const auto &prefix : {std::string(), std::string("*1\r\n")}) {
		Resp too_large(256, 64);
		CHECK(too_large.append(prefix + "$65\r\n"));
		CHECK(too_large.decode()._state == Resp::DecodeState::Error);
	}
}

void test_append_rejection_preserves_buffer() {
	Resp resp(16, 8);
	CHECK(resp.append("$5\r\nhe"));
	CHECK(!resp.append(std::string(17, 'x')));
	CHECK(resp.decode()._state == Resp::DecodeState::NeedMore);
	CHECK(resp.append("llo\r\n"));
	auto result = resp.decode();
	CHECK(result._state == Resp::DecodeState::Complete);
	CHECK(result._result_value._bulk_string == "hello");
}

void check_value_equal(const Resp::RespValue &actual, const Resp::RespValue &expected) {
	CHECK(actual._value_type == expected._value_type);
	switch (expected._value_type) {
		case Resp::RespValueType::SimpleString: CHECK(actual._simple_string == expected._simple_string); break;
		case Resp::RespValueType::Error: CHECK(actual._errors == expected._errors); break;
		case Resp::RespValueType::Integer: CHECK(actual._integer == expected._integer); break;
		case Resp::RespValueType::BulkString: CHECK(actual._bulk_string == expected._bulk_string); break;
		case Resp::RespValueType::Arrays:
			CHECK(actual._array.size() == expected._array.size());
			for (std::size_t index = 0; index < expected._array.size(); ++index) {
				check_value_equal(actual._array[index], expected._array[index]);
			}
			break;
	}
}

void test_nested_binary_frame_at_every_split_boundary() {
	const std::string binary("A\0B\r\nC\xff", 7);
	const std::string frame = "*4\r\n$7\r\n" + binary +
		"\r\n+OK\r\n:-9223372036854775808\r\n*2\r\n-ERR nested\r\n*0\r\n";
	const auto expected = Resp::RespValue::Array({
		Resp::RespValue::BulkString(binary), Resp::RespValue::SimpleString("OK"),
		Resp::RespValue::Integer(std::numeric_limits<std::int64_t>::min()),
		Resp::RespValue::Array({Resp::RespValue::Errors("ERR nested"), Resp::RespValue::Array({})})
	});
	for (std::size_t split = 0; split <= frame.size(); ++split) {
		Resp resp;
		CHECK(resp.append(frame.substr(0, split)));
		auto result = resp.decode();
		if (split < frame.size()) {
			CHECK(result._state == Resp::DecodeState::NeedMore);
			CHECK(resp.decode()._state == Resp::DecodeState::NeedMore);
			CHECK(resp.append(frame.substr(split)));
			result = resp.decode();
		}
		CHECK(result._state == Resp::DecodeState::Complete);
		check_value_equal(result._result_value, expected);
		CHECK(resp.decode()._state == Resp::DecodeState::NeedMore);
	}
}

void test_mixed_pipeline_with_changing_chunk_sizes() {
	const std::string binary("\0x\r\n\xff", 5);
	const std::string wire = "+\r\n-ERR\r\n:-9223372036854775808\r\n$5\r\n" + binary +
		"\r\n*2\r\n$0\r\n\r\n:9223372036854775807\r\n";
	const std::vector<Resp::RespValue> expected{
		Resp::RespValue::SimpleString(""), Resp::RespValue::Errors("ERR"),
		Resp::RespValue::Integer(std::numeric_limits<std::int64_t>::min()),
		Resp::RespValue::BulkString(binary),
		Resp::RespValue::Array({Resp::RespValue::BulkString(""),
			Resp::RespValue::Integer(std::numeric_limits<std::int64_t>::max())})
	};
	Resp resp;
	std::size_t offset = 0, received = 0;
	while (offset < wire.size()) {
		const std::size_t amount = std::min<std::size_t>(1 + offset % 13, wire.size() - offset);
		CHECK(resp.append(wire.substr(offset, amount)));
		offset += amount;
		for (;;) {
			auto result = resp.decode();
			if (result._state == Resp::DecodeState::NeedMore) { break; }
			CHECK(result._state == Resp::DecodeState::Complete && received < expected.size());
			check_value_equal(result._result_value, expected[received++]);
		}
	}
	CHECK(received == expected.size());
	CHECK(resp.decode()._state == Resp::DecodeState::NeedMore);
}

void test_numeric_header_overflow_and_invalid_terminators_are_sticky_errors() {
	const std::vector<std::string> malformed{
		"*-\r\n", "*+1\r\n", "*2147483648\r\n", "*999999999999999999999\r\n",
		"$-\r\n", "$2147483648\r\n", "$999999999999999999999\r\n",
		":-9223372036854775809\r\n", "$3\r\nabcXX"
	};
	for (const auto &bad : malformed) {
		for (const auto &prefix : {std::string(), std::string("*1\r\n")}) {
			Resp resp;
			CHECK(resp.append("+OK\r\n" + prefix + bad));
			CHECK(resp.decode()._state == Resp::DecodeState::Complete);
			CHECK(resp.decode()._state == Resp::DecodeState::Error);
			CHECK(resp.append("+NEXT\r\n"));
			CHECK(resp.decode()._state == Resp::DecodeState::Error);
		}
	}
}

// ─── test runner ────────────────────────────────────────────────

struct TestCase {
	const char *name;
	void (*run)();
};

} // namespace

int main() {
	const TestCase tests[] = {
		{"encode scalars", test_encode_scalars},
		{"encode array", test_encode_array},
		{"encode rejects nested array", test_encode_rejects_nested_array},
		{"decode simple string", test_decode_simple_string},
		{"decode error frame", test_decode_error_frame},
		{"decode integers", test_decode_integers},
		{"decode integer overflow rejected",
		 test_decode_integer_overflow_rejected},
		{"decode bulk string", test_decode_bulk_string},
		{"decode partial frames need more",
		 test_decode_partial_frames_need_more},
		{"decode malformed frames rejected",
		 test_decode_malformed_frames_rejected},
		{"decode pipelining", test_decode_pipelining},
		{"decode array command form", test_decode_array_command_form},
		{"decode array mixed elements", test_decode_array_mixed_elements},
		{"decode array bulk position invariant",
		 test_decode_array_bulk_position_invariant},
		{"decode partial array need more",
		 test_decode_partial_array_need_more},
		{"decode array streamed in chunks",
		 test_decode_array_streamed_in_chunks},
		{"decode empty array", test_decode_empty_array},
		{"decode array member error atomic",
		 test_decode_array_member_error_atomic},
		{"decode array negative size rejected",
		 test_decode_array_negative_size_rejected},
		{"decode nested array boundary", test_decode_nested_array_boundary},
		{"decode nested array and pipeline", test_decode_nested_array_and_pipeline},
		{"decode nested array byte stream", test_decode_nested_array_byte_stream},
		{"decode nested array error atomic", test_decode_nested_array_error_atomic},
		{"decode array unknown member type rejected",
		 test_decode_array_unknown_member_type_rejected},
		{"decode array unknown member type streamed",
		 test_decode_array_unknown_member_type_streamed},
		{"append limits", test_append_limits},
		{"bulk parameter limit", test_bulk_parameter_limit},
		{"append rejection preserves buffer", test_append_rejection_preserves_buffer},
		{"nested binary frame at every split boundary", test_nested_binary_frame_at_every_split_boundary},
		{"mixed pipeline with changing chunk sizes", test_mixed_pipeline_with_changing_chunk_sizes},
		{"numeric header overflow and terminator errors stay sticky", test_numeric_header_overflow_and_invalid_terminators_are_sticky_errors},
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
