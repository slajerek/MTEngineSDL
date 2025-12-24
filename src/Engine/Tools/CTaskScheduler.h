#pragma once

#include "CTask.h"
#include "SYS_Threading.h"

class CTaskScheduler
{
public:
	CTaskScheduler(const char *name);
	virtual ~CTaskScheduler();
	
	const char *name;
	
	virtual void RunTask(CTask *task);
	
	CTask *runningTask;
	CTask *replaceTask;
	
	virtual void ThreadRun(void *passData);
	
	CSlrMutex *mutex;

private:
	volatile bool shouldStop;
};
