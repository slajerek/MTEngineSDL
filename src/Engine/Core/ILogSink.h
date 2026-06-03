#pragma once

#include <cstdarg>
#include <cstdio>
#include <string>

// Generic interface for log output.
// Views, network servers, and other subsystems can use this to decouple
// log output from specific UI implementations.
class ILogSink
{
public:
	virtual ~ILogSink() {}

	// Formatted log output (C-style variadic)
	virtual void AddLog(const char *fmt, ...) = 0;

	// String log output
	virtual void AddLogStr(const std::string &str) = 0;
};
