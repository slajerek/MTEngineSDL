#pragma once

#include "SYS_Main.h"

class CTask
{
public:
	CTask(const char *name);
	virtual ~CTask();
	
	const char *name;
	
	virtual void Run();
	virtual void Interrupt();
	
	virtual void DebugPrint();
};
