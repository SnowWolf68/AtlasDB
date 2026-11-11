#pragma once
#include "lsm.h"
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

class TranContext {
	enum TranState { Active, Committed, Aborted };

	static const std::size_t MAX_USER_KEY_LEN = 65535 - 8;
	static const std::size_t MAX_VALUE_LEN = 65535;

	friend class LSMEngine;

public:

	~TranContext();

	// 删掉拷贝构造
	TranContext(const TranContext &other) = delete;

	bool put(const std::string &user_key, const std::string &value);
	std::optional<std::string> get(const std::string &user_key);
	bool remove(const std::string &user_key);

	CommitResult commit();
	void abort();



private:
	TranContext(std::shared_ptr<LSMEngine> engine, std::uint64_t snapshot_seq,
				IsolationLevel level = IsolationLevel::ReadCommited);
	bool check_user_key(const std::string &user_key);
	bool check_value(const std::string &value);


	std::shared_ptr<LSMEngine> _engine;
	std::unordered_map<std::string, std::optional<std::string>> _map;
	std::uint64_t _snapshot_seq;	// 事务开始时, 当前全局的版本号到了多少
	IsolationLevel _iso_level;
	TranState _tran_state;
};