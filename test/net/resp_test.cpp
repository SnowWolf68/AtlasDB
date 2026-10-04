#include "resp.h"

#include <cstdint>
#include <exception>
#include <iostream>
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
	// 已知边界(非缺陷):嵌套数组未支持,元素层的 '*' 解析不了,
	// 表现为 NeedMore。RESP2 请求方向命令不含嵌套数组。
	Resp resp;
	CHECK(resp.append("*1\r\n*1\r\n+OK\r\n\r\n"));
	CHECK(resp.decode()._state == Resp::DecodeState::NeedMore);
}

// ─── append 上限 ────────────────────────────────────────────────

void test_append_limits() {
	// 默认构造可用
	Resp fresh;
	CHECK(fresh.append("+OK\r\n"));

	// 自定义上限:单参数 64 字节 / 总 buffer 256 字节
	Resp resp(256, 64);
	CHECK(resp.append(std::string(64, 'a')));
	CHECK(!resp.append(std::string(65, 'a')));

	// 总量:4×64 恰好填满,第 5 次必须拒绝
	Resp total(256, 64);
	for (int i = 0; i < 4; i++) {
		CHECK(total.append(std::string(64, 'b')));
	}
	CHECK(!total.append(std::string(64, 'b')));
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
		{"append limits", test_append_limits},
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
