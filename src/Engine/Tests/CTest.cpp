#include "SYS_Defs.h"
#include "CTest.h"
#include "DBG_Log.h"

CTest::CTest()
{
	currentStep = 0;
	isRunning = false;
	callback = NULL;
}

CTest::~CTest()
{
}

void CTest::StepCompleted(int stepId, bool success, const char *message)
{
	if (callback)
	{
		callback->OnTestStepCompleted(this, stepId, success, message);
	}
}

void CTest::ReportRequiredGap(const char *what)
{
	if (what == NULL || what[0] == 0)
		return;
	if (!requiredGap.empty())
		requiredGap += "; ";
	requiredGap += what;
	// LOUD even in a normal run: the whole point is that this stops being
	// something only a careful reader of a summary string would notice.
	LOGError("CTest: %s -- REQUIRED COVERAGE MISSING: %s", GetName(), what);
}

void CTest::TestSkipped(const char *reason)
{
	// Set BEFORE completing: TestCompleted() dispatches to the suite
	// synchronously, and the suite reads WasSkipped() inside that callback.
	skipped = true;
	TestCompleted(true, reason);
}

void CTest::TestCompleted(bool success, const char *summary)
{
	if (completed)
	{
		// Never re-enter the suite's dispatch chain (see CTest.h). Loud,
		// because a double completion means the test's own control flow is
		// wrong even though the run may still score green.
		LOGError("CTest: %s completed twice ('%s') -- ignoring the second call",
		         GetName(), summary ? summary : "");
		return;
	}
	completed = true;
	isRunning = false;
	Teardown();
	if (callback)
	{
		callback->OnTestCompleted(this, success, summary);
	}
}
