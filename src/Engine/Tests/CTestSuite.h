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
	bool skipped = false;   // completed via CTest::TestSkipped(): did NOT run
	string requiredGap;     // non-empty: a MANDATORY part did not run
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

	// Register tests, write their names (one per line, "<category>\t<name>")
	// to tests/results/test_list.txt and stdout, then exit. Used by a
	// subprocess-per-test orchestrator to enumerate tests without hardcoding
	// the list.
	static void ListTestsFromCLI(CTestSuite *suite);

	// Hook for subclasses that gate optional/removable tests (e.g. plugin
	// tests) behind a flag. Called by RunFromCLI/ListTestsFromCLI before
	// RegisterTests(), with include=true when --all-plugin-tests is passed or a
	// specific test is requested by name. Default: no-op.
	virtual void SetIncludeOptionalTests(bool include) {}

	// True when CLI test mode is active (suppresses RUN_AUTOMATED_TESTS)
	static bool isCLIModeActive;

	// Timeout settings (seconds, 0 = no timeout). Only enforced when
	// useTimeoutWatchdogThread is true.
	int defaultTestTimeoutSeconds = 30;
	int suiteTimeoutSeconds = 180;

	// Opt-in background timeout watchdog. Disabled by default so suite
	// completion stays single-threaded (consumers that run a live emulator on
	// the main thread can deadlock against a second thread cancelling tests).
	bool useTimeoutWatchdogThread = false;

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
	// Re-entrancy guard for the test chain. A test that finishes synchronously
	// calls TestCompleted() from INSIDE its own Run(); if OnTestCompleted then
	// started the next test directly, every following test would execute nested
	// on the completing test's stack -- with all of its still-live scope guards
	// (temp-dir cleanup, global-settings restore, open browsers) unwound only
	// AFTER the whole suite had run. RunNextTest() therefore never starts a test
	// re-entrantly: it records the request and the dispatch loop picks it up once
	// Run() has returned and the test's destructors have fired.
	bool dispatchingTest = false;
	bool advanceRequested = false;
	bool exitOnCompletion;
	// When false (default), the suite runs every test and reports all failures
	// in one pass; a failing test no longer hides the tests after it. Set true
	// (CLI: --stop-on-first-failure) for fast-fail behaviour.
	bool stopOnFirstFailure;

	// --require-fixtures / PC_REQUIRE_FIXTURES=1: treat every recorded required
	// gap and required skip as a FAILURE. Off by default so a developer without
	// the fixture set stays green; on for the runs that claim DoD compliance.
	bool requireFixtures;
	string resultsFilePath;
	time_t suiteStartTime = 0;
};
