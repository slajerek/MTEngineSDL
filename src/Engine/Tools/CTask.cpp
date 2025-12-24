#include "CTask.h"
#include "CSlrString.h"

CTask::CTask(const char *name)
{
	this->name = STRALLOC(name);
}

void CTask::Run()
{	
}

void CTask::Interrupt()
{
}


CTask::~CTask()
{
	STRFREE(name);
}

void CTask::DebugPrint()
{
	LOGD("CTask::DebugPrint: name=%s", name);
}
