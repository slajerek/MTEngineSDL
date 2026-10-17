#pragma once
#include "CTest.h"

// CProcessSpawner::Run, the argv launcher build tasks use: exit codes,
// separate stdout/stderr capture, arguments passed verbatim (no shell),
// working directory, environment, PATH lookup, timeout and cancel stopping
// the whole process tree, streamed output, and a large output that would
// deadlock a reader that drains one pipe at a time. Spawns only the
// platform shell -- runs headlessly.
class CTestProcessSpawner : public CTest
{
public:
	CTestProcessSpawner() = default;
	~CTestProcessSpawner() = default;
	virtual const char *GetName() override { return "ProcessSpawner"; }
	virtual void Run(ITestCallback *callback) override;
	virtual void Cancel() override { isRunning = false; }
};
