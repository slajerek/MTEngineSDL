//#import <Foundation/Foundation.h>
#include "SYS_Threading.h"
#include "SYS_Main.h"
#include "SYS_Platform.h"
#include <SDL.h>

#if !defined(WIN32)
#include <unistd.h>
#endif

void CSlrThread::ThreadRun(void *passData)
{
}

class CThreadPassData
{
public:
	void *passData;
	CSlrThread *threadStarter;
	float priority;
};


int ThreadStarterFuncRun(void *dataToPassWithArg)
{
	CThreadPassData *passArg = (CThreadPassData *)dataToPassWithArg;

//#if defined(MACOS)
//	[NSThread setThreadPriority:passArg->priority];
//#endif
	
	CSlrThread *threadStarter = passArg->threadStarter;
	void *passData = passArg->passData;
	delete passArg;
	
#if !defined(FINAL_RELEASE)
	if (threadStarter->isRunning == true)
	{
		LOGError("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
		LOGError("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
		LOGError("ThreadStarterFuncRun: threadStarter '%s' is already running", threadStarter->threadName);
		LOGError("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
		LOGError("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
		//SYS_FatalExit("ThreadStarterFuncRun: threadStarter '%s' is already running", threadStarter->threadName);
	}
#endif

//#if defined(IPHONE) || defined(MACOS)
////	NSLog(@"ThreadStarterFuncRun: threadId=%x threadName=%s", (u64)pthread_self(), threadStarter->threadName);
//#endif

	threadStarter->isRunning = true;
	threadStarter->ThreadRun(passData);
	threadStarter->isRunning = false;
	
	return 0;
}

void SYS_StartThread(CSlrThread *run, void *passData, float priority)
{
	// crashes on windows??
	LOGD("SYS_StartThread: %s %x isRunning=%s", run->threadName, run, STRBOOL(run->isRunning));

	if (run->isRunning)
	{
		SDL_threadID threadId = SDL_GetThreadID(run->thread);
		SYS_FatalExit("thread %d is already running", threadId);
	}
	
	CThreadPassData *passArg = new CThreadPassData();
	passArg->passData = passData;
	passArg->threadStarter = run;
	passArg->priority = priority;
	
	run->thread = SDL_CreateThread(ThreadStarterFuncRun, "thread", passArg);
	
	if (run->thread == NULL)
		SYS_FatalExit("SYS_StartThread: thread creation failed");
}

void SYS_StartThread(CSlrThread *run, void *passData)
{
	SYS_StartThread(run, passData, MT_THREAD_PRIORITY_NORMAL);
}

void SYS_StartThread(CSlrThread *run)
{
	SYS_StartThread(run, NULL, MT_THREAD_PRIORITY_NORMAL);
}

CSlrThread::CSlrThread()
{
	this->thread = NULL;
	this->isRunning = false;
	strcpy(this->threadName, "thread");
};

CSlrThread::CSlrThread(char *setThreadName)
{
	this->thread = NULL;
	this->isRunning = false;
	strcpy(this->threadName, setThreadName);
};

void CSlrThread::ThreadSetName(const char *name)
{
	strcpy(this->threadName, name);
	SYS_SetThreadName(name);
}

CSlrThread::~CSlrThread()
{
	if (this->thread != NULL)
	{
		SYS_KillThread(this);
	}
}

void SYS_KillThread(CSlrThread *thread)
{
//	SDL_KillThread(thread);
}

void SYS_SetThreadPriority(float priority)
{
//#if defined(IOS) || defined(MACOS)
//	[NSThread setThreadPriority:priority];
//#else
//	LOGTODO("SYS_SetThreadPriority: %f", priority);
//#endif
}

void SYS_SetThreadName(const char *name)
{
	PLATFORM_SetThreadName(name);
}

CSlrMutex::CSlrMutex(const char *name)
{
	strcpy(this->name, name);

	// SDL mutexes are recursive
	mutex = SDL_CreateMutex();
	
#if defined(MT_DEBUG_MUTEX)
	lockedLevel = 0;
#endif
}

CSlrMutex::~CSlrMutex()
{
	SDL_DestroyMutex(mutex);
}
	
void CSlrMutex::Lock()
{
#if defined(MT_DEBUG_MUTEX)
	u64 timeout = SYS_GetCurrentTimeInMillis() + 5000;
	
	while (SDL_TryLockMutex(mutex) == SDL_MUTEX_TIMEDOUT)
	{
		u64 now = SYS_GetCurrentTimeInMillis();
		if (now >= timeout)
		{
			LOGError("Mutex lock timeout, name=%s", this->name);
			return;
		}
		
		SYS_Sleep(5);
	}
	lockedLevel++;

#else
	SDL_LockMutex(mutex);
#endif	
}

bool CSlrMutex::TryLock()
{
	return ( SDL_TryLockMutex(mutex) != SDL_MUTEX_TIMEDOUT );
}

void CSlrMutex::Unlock()
{
#if defined(MT_DEBUG_MUTEX)
	lockedLevel--;
#endif
	
	SDL_UnlockMutex(mutex);
}

unsigned long SYS_GetProcessId()
{
#if defined(WIN32)
	return GetCurrentProcessId();
#else
	return getpid();
#endif
}

