#pragma once
#include <string>
#include "Record.h"

/**
规则:
== --- 0
<  --- -1
>  --- 1
*/
template <class T>
class Comparator {
public:
	virtual int operator()(const T &left, const T &right) const = 0;
	virtual ~Comparator() = default;
};

class DefaultStringComparator : public Comparator<std::string> {
public:
	int operator()(const std::string &left, const std::string &right) const;
};

class InternalKeyComparator:public Comparator<std::string>{
public:
    int operator()(const std::string &left, const std::string& right) const;
};