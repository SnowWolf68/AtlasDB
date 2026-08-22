#pragma once
#include <string>

class BaseIterator {
public:
	// <enc_key, value>
	virtual std::pair<std::string, std::string> operator*() const = 0;
	virtual BaseIterator &operator++() = 0;
	virtual bool operator==(const BaseIterator &other) const = 0;
	virtual bool operator!=(const BaseIterator &other) const = 0;
	virtual bool is_valid() const = 0;

private:
};