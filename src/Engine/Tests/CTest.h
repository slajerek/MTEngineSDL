#pragma once

#include "ITestCallback.h"
#include <ctime>

class CTest
{
public:
	CTest();
	virtual ~CTest();

	virtual const char *GetName() = 0;
	virtual void Run(ITestCallback *callback) = 0;
	virtual void Cancel() = 0;
	virtual void Teardown() {}

	bool IsRunning() { return isRunning; }

	// Per-test timeout in seconds (0 = use suite default)
	int timeoutSeconds = 0;

	// Time when test started running (set by suite)
	time_t startTime = 0;

	// Test grouping: "core" for engine/app-core tests, or a plugin name
	// (e.g. "Remapper", "Fire", "GoatTracker") for plugin tests. Set by the
	// per-plugin registrars. Used for grouped listing/reporting (see
	// CTestSuite::ListTestsFromCLI).
	const char *category = "core";

protected:
	void StepCompleted(int stepId, bool success, const char *message);
	void TestCompleted(bool success, const char *summary);

	int currentStep;
	bool isRunning;
	ITestCallback *callback;
};
