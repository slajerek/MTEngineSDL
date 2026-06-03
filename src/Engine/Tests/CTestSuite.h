#pragma once

#include "ITestCallback.h"
#include <vector>
#include <string>
#include <memory>
#include <ctime>
#include <thread>
using namespace std;

class CTest;

struct CTestSuiteResult
{
	string testName;
	bool success;
	string summary;
};

// Runs all automated tests in sequence
class CTestSuite : public ITestCallback
{
public:
	CTestSuite();
	virtual ~CTestSuite();

	// Subclasses override to push tests into the vector
	virtual void RegisterTests() = 0;

	// CLI mode: run suite, optionally filtered by test name
	// Calls suite->RegisterTests() internally before filtering
	static void RunFromCLI(CTestSuite *suite, const char *testName);

	// True when CLI test mode is active (suppresses RUN_AUTOMATED_TESTS)
	static bool isCLIModeActive;

	// Timeout settings (seconds, 0 = no timeout)
	int defaultTestTimeoutSeconds = 30;
	int suiteTimeoutSeconds = 180;

	void Run();
	void Cancel();

	// ITestCallback
	virtual void OnTestStepCompleted(CTest *test, int stepId, bool success, const char *message) override;
	virtual void OnTestCompleted(CTest *test, bool success, const char *summary) override;

protected:
	vector<unique_ptr<CTest>> tests;

private:
	void RunNextTest();
	void WriteResults();
	void StartTimeoutWatchdog();

	vector<CTestSuiteResult> results;
	int currentTestIndex;
	bool isRunning;
	bool exitOnCompletion;
	string resultsFilePath;
	time_t suiteStartTime = 0;
};
