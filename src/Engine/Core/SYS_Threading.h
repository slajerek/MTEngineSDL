#ifndef _SYS_THREADING_H_
#define _SYS_THREADING_H_

//#include <SDL.h>
struct SDL_Thread;
typedef struct SDL_Thread SDL_Thread;

//#define USE_SDL_MUTEX
//#define MT_DEBUG_MUTEX

#if defined(MT_DEBUG_MUTEX)
#define USE_SDL_MUTEX
#endif

#if defined(USE_SDL_MUTEX)
struct SDL_mutex;
typedef struct SDL_mutex SDL_mutex;
#else
#include <mutex>
#endif

#include "SYS_Defs.h"

#define MT_THREAD_PRIORITY_NORMAL	0

class CSlrMutex
{
public:
	CSlrMutex(const char *name);
	~CSlrMutex();
	
	char name[64];
	
	
#if defined(USE_SDL_MUTEX)
	SDL_mutex *mutex;
#else
	std::recursive_mutex mutex;
#endif

#if defined(MT_DEBUG_MUTEX)
	// for debugging
	volatile int lockedLevel;
#endif
	
	void Lock();
	// returns true on success
	bool TryLock();
	void Unlock();
};

class CSlrThread
{
public:
	SDL_Thread *thread;
	volatile bool isRunning;
	char threadName[256];
	
	CSlrThread();
	CSlrThread(char *threadName);

	~CSlrThread();

	virtual void ThreadSetName(const char *name);
	virtual void ThreadRun(void *passData);
};

void SYS_StartThread(CSlrThread *run, void *passData, float priority);
void SYS_StartThread(CSlrThread *run, void *passData);
void SYS_StartThread(CSlrThread *run);
void SYS_KillThread(CSlrThread *run);

void SYS_SetThreadPriority(float priority);
void SYS_SetThreadName(const char *name);

void SYS_SetMainProcessPriorityBoostDisabled(bool isPriorityBoostDisabled);
void SYS_SetMainProcessPriority(int priority);

unsigned long SYS_GetProcessId();

#endif
//_SYS_THREADING_H_
