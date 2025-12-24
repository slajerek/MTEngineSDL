#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>

class CInterruptibleSleep {
public:
	CInterruptibleSleep() = default;
	~CInterruptibleSleep() = default;

	// Sleep for specified duration (in milliseconds) unless interrupted
	bool SleepFor(std::chrono::milliseconds duration);

	// Interrupt any ongoing sleep
	virtual void Interrupt();

	// Reset interrupted state
	void Reset();

private:
	std::condition_variable cond;
	std::mutex mutex;
	bool isInterrupted = false;
	bool isSleeping = false;
};
