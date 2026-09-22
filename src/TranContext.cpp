#include "TranContext.h"
#include "Record.h"
#include "log.h"
#include "lsm.h"
#include <optional>
#include <stdexcept>

TranContext::TranContext(std::shared_ptr<LSMEngine> engine,
						 std::uint64_t snapshot_seq, IsolationLevel level)
	: _engine(engine), _snapshot_seq(snapshot_seq), _iso_level(level),
	  _tran_state(TranState::Active) {}

TranContext::~TranContext() {
	if (_tran_state == TranState::Active) {
		LOG(Log::WARNING) << "transaction destroyed without commit/abort, "
							 "write set discarded";
	}
}

bool TranContext::put(const std::string &user_key, const std::string &value) {
	if (!check_user_key(user_key) || !check_value(value)) { return false; }
	if (_tran_state != TranState::Active) {
		LOG(Log::WARNING) << "transaction is not active";
		return false;
	}
	_map[user_key] = value;
	return true;
}
std::optional<std::string> TranContext::get(const std::string &user_key) {
	if (_tran_state != TranState::Active) {
		throw std::runtime_error("transaction is not active");
	}
	auto find_res = _map.find(user_key);
	if (find_res != _map.end()) { return find_res->second; }
	LookUpResult get_res;
	if (_iso_level == IsolationLevel::ReadCommited) {
		// RC
		// 默认用最新的commit_sequence去查, 查到最新的已提交版本
		get_res = _engine->get(user_key);
	} else {
		// RR
		// 用事务刚开始时的snapshot_seq, 不看当前事务之后的这些提交
		get_res = _engine->get(user_key, _snapshot_seq);
	}
	if (get_res._value_type == ValueType::NotFound ||
		get_res._value_type == ValueType::TombStone) {
		return std::nullopt;
	} else {
		return get_res._value;
	}
}

bool TranContext::remove(const std::string &user_key) {
	if (!check_user_key(user_key)) { return false; }
	if (_tran_state != TranState::Active) {
		throw std::runtime_error("transaction is not active");
	}
	_map[user_key] = std::nullopt;
	return true;
}

CommitResult TranContext::commit() {
	if (_tran_state != TranState::Active) {
		throw std::runtime_error("trancaction state is not active");
	}
	CommitResult res = _engine->commit(_map, _iso_level, _snapshot_seq);
	_map.clear();
	_tran_state = res.status == CommitResult::Status::Committed
					  ? TranState::Committed
					  : TranState::Aborted;
	return res;
}

void TranContext::abort() {
	if (_tran_state == TranState::Active) {
		_tran_state = TranState::Aborted;
	} else if (_tran_state == TranState::Committed) {
		throw std::runtime_error("transaction is already commited!");
	}
}

bool TranContext::check_user_key(const std::string &user_key) {
	return user_key.size() <= MAX_USER_KEY_LEN;
}
bool TranContext::check_value(const std::string &value) {
	return value.size() <= MAX_VALUE_LEN;
}