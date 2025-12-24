#include "CTaskScheduler.h"
#include "CSlrString.h"

CTaskScheduler::CTaskScheduler(const char *name)
{
	this->name = STRALLOC(name);
	mutex = new CSlrMutex("CTaskScheduler");

	runningTask = NULL;
	replaceTask = NULL;
	shouldStop = false;
}

CTaskScheduler::~CTaskScheduler()
{
}

void CTaskScheduler::RunTask(CTask *task)
{
	
}


void CTaskScheduler::ThreadRun(void *passData)
{
	LOGD("CTaskScheduler::ThreadRun");
	
	while(!shouldStop)
	{
		mutex->Lock();
		if (replaceTask != NULL)
		{
			runningTask = replaceTask;
			replaceTask = NULL;
			
			mutex->Unlock();
			
			runningTask->Run();
		}
		else
		{
			mutex->Unlock();
			SYS_Sleep(20);
		}
	}
}
