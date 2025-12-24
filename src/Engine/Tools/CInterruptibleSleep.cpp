#include "CInterruptibleSleep.h"
#include "DBG_Log.h"
#include <cstdio>

bool CInterruptibleSleep::SleepFor(std::chrono::milliseconds duration)
{
	std::unique_lock<std::mutex> lock(mutex);
	LOGD("CInterruptibleSleep::SleepFor Going to sleep for %lld ms", duration.count());
	isSleeping = true;
	bool was_interrupted = cond.wait_for(lock, duration, [this] { return isInterrupted; });
	isSleeping = false;
	return !was_interrupted;
}



void CInterruptibleSleep::Interrupt()
{
	std::lock_guard<std::mutex> lock(mutex);
	if (isSleeping)
	{
		isInterrupted = true;
		LOGD("CInterruptibleSleep::Interrupt: Interrupt issued during sleep");
		cond.notify_all();
	}
	else
	{
		LOGD("CInterruptibleSleep::Interrupt: Interrupt ignored, no thread sleeping");
	}
}

void CInterruptibleSleep::Reset()
{
	std::lock_guard<std::mutex> lock(mutex);
	isInterrupted = false;
	LOGD("CInterruptibleSleep::Reset Interrupted flag reset");
}
