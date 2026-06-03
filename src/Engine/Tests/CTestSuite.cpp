#include "CTestSuite.h"
#include "CTestRunner.h"
#include "CTest.h"
#include "SYS_Main.h"
#include "SYS_Funct.h"
#include <cstdio>
#include <cstring>
#include <thread>
#if !defined(_WIN32)
#include <signal.h>
#endif
using namespace std;

// Restore default signal handlers before SYS_Shutdown().
// ImGuiTestEngine_InstallDefaultCrashHandler() hooks SIGABRT etc. with a handler
// that calls abort(), which re-raises SIGABRT causing infinite CrashHandler spam
// if any signal fires during shutdown teardown.
static void RestoreDefaultSignalHandlers()
{
#if !defined(_WIN32)
	signal(SIGILL, SIG_DFL);
	signal(SIGABRT, SIG_DFL);
	signal(SIGFPE, SIG_DFL);
	signal(SIGSEGV, SIG_DFL);
	signal(SIGPIPE, SIG_DFL);
	signal(SIGBUS, SIG_DFL);
#endif
}

bool CTestSuite::isCLIModeActive = false;

CTestSuite::CTestSuite()
{
	currentTestIndex = -1;
	isRunning = false;
	exitOnCompletion = false;
}

CTestSuite::~CTestSuite()
{
}

void CTestSuite::RunFromCLI(CTestSuite *suite, const char *testName)
{
	isCLIModeActive = true;
	CTestRunner::isTestPending = true;

	suite->exitOnCompletion = true;
	suite->resultsFilePath = "tests/results/last_run.txt";

	// Register all tests from the subclass
	suite->RegisterTests();

	if (testName != NULL)
	{
		// Filter to matching test name, remove non-matching
		vector<unique_ptr<CTest>> filtered;
		for (auto &test : suite->tests)
		{
			if (strcmp(test->GetName(), testName) == 0)
			{
				filtered.push_back(std::move(test));
			}
		}
		suite->tests = std::move(filtered);

		if (suite->tests.empty())
		{
			LOGError("CTestSuite::RunFromCLI: No test found with name '%s'", testName);

			// Log available test names — re-register to list them
			CTestSuite *temp = suite;
			// Tests were already cleared by the move, re-register to show names
			temp->RegisterTests();
			LOGI("Available tests:");
			for (auto &t : temp->tests)
			{
				LOGI("  %s", t->GetName());
			}
			temp->tests.clear();

			// Write failure results
			FILE *f = fopen(suite->resultsFilePath.c_str(), "w");
			if (f)
			{
				fprintf(f, "[%s] FAIL: Test not found\n", testName);
				fprintf(f, "---\n");
				fprintf(f, "RESULT: 0/1 passed\n");
				fclose(f);
			}

			CTestRunner::isTestPending = false;
			RestoreDefaultSignalHandlers();
			SYS_Shutdown();
			delete suite;
			return;
		}
	}

	suite->Run();
}

void CTestSuite::Run()
{
	LOGS("CTestSuite::Run: Starting test suite with %d tests", (int)tests.size());
	isRunning = true;
	currentTestIndex = -1;
	suiteStartTime = time(NULL);
	results.clear();
	StartTimeoutWatchdog();
	RunNextTest();
}

void CTestSuite::StartTimeoutWatchdog()
{
	std::thread watchdog([this]() {
		LOGS("CTestSuite: Timeout watchdog started (per-test=%ds, suite=%ds)", defaultTestTimeoutSeconds, suiteTimeoutSeconds);
		while (isRunning)
		{
			SYS_Sleep(1000);
			if (!isRunning)
				break;

			time_t now = time(NULL);

			// Check suite-level timeout
			if (suiteTimeoutSeconds > 0 && (now - suiteStartTime) >= suiteTimeoutSeconds)
			{
				LOGError("CTestSuite: Suite timeout (%d seconds) exceeded", suiteTimeoutSeconds);
				if (currentTestIndex >= 0 && currentTestIndex < (int)tests.size())
				{
					CTest *test = tests[currentTestIndex].get();
					test->Cancel();
					OnTestCompleted(test, false, "Suite timeout exceeded");
				}
				break;
			}

			// Check per-test timeout
			if (currentTestIndex >= 0 && currentTestIndex < (int)tests.size())
			{
				CTest *test = tests[currentTestIndex].get();
				if (test->IsRunning() && test->startTime > 0)
				{
					int testTimeout = (test->timeoutSeconds > 0) ? test->timeoutSeconds : defaultTestTimeoutSeconds;
					if (testTimeout > 0 && (now - test->startTime) >= testTimeout)
					{
						LOGError("CTestSuite: Test '%s' timeout (%d seconds) exceeded", test->GetName(), testTimeout);
						test->Cancel();
						OnTestCompleted(test, false, "Test timeout exceeded");
						// Don't break — continue monitoring for suite timeout and subsequent tests
					}
				}
			}
		}
	});
	watchdog.detach();
}

void CTestSuite::Cancel()
{
	LOGS("CTestSuite::Cancel");
	isRunning = false;

	if (currentTestIndex >= 0 && currentTestIndex < (int)tests.size())
	{
		tests[currentTestIndex]->Cancel();
	}
}

void CTestSuite::RunNextTest()
{
	if (!isRunning)
		return;

	currentTestIndex++;

	if (currentTestIndex >= (int)tests.size())
	{
		LOGS("CTestSuite: All tests completed");
		CTestRunner::isTestPending = false;
		isRunning = false;

		if (exitOnCompletion)
		{
			WriteResults();

			int passed = 0;
			for (auto &r : results)
			{
				if (r.success) passed++;
			}
			LOGM("SUITE RESULTS: %d/%d passed", passed, (int)results.size());
			RestoreDefaultSignalHandlers();
			SYS_Shutdown();
		}
		return;
	}

	CTest *test = tests[currentTestIndex].get();
	LOGS("CTestSuite: Running test %d/%d: %s", currentTestIndex + 1, (int)tests.size(), test->GetName());
	test->startTime = time(NULL);
	test->Run(this);
}

void CTestSuite::OnTestStepCompleted(CTest *test, int stepId, bool success, const char *message)
{
	LOGS("CTestSuite: [%s] Step %d %s: %s", test->GetName(), stepId, success ? "OK" : "FAILED", message);
}

void CTestSuite::OnTestCompleted(CTest *test, bool success, const char *summary)
{
	LOGS("CTestSuite: [%s] Completed %s: %s", test->GetName(), success ? "OK" : "FAILED", summary);

	results.push_back({test->GetName(), success, summary});

	if (!success)
	{
		LOGS("CTestSuite: Test failed, stopping suite");
		CTestRunner::isTestPending = false;
		isRunning = false;

		if (exitOnCompletion)
		{
			WriteResults();

			int passed = 0;
			for (auto &r : results)
			{
				if (r.success) passed++;
			}
			LOGM("SUITE RESULTS: %d/%d passed", passed, (int)results.size());
			RestoreDefaultSignalHandlers();
			SYS_Shutdown();
		}
		return;
	}

	// Run next test
	RunNextTest();
}

void CTestSuite::WriteResults()
{
	FILE *f = fopen(resultsFilePath.c_str(), "w");
	if (!f)
	{
		LOGError("CTestSuite::WriteResults: Failed to open %s", resultsFilePath.c_str());
		return;
	}

	int passed = 0;
	for (auto &r : results)
	{
		fprintf(f, "[%s] %s: %s\n", r.testName.c_str(), r.success ? "PASS" : "FAIL", r.summary.c_str());
		if (r.success) passed++;
	}
	fprintf(f, "---\n");
	fprintf(f, "RESULT: %d/%d passed\n", passed, (int)results.size());
	fclose(f);

	LOGM("CTestSuite: Results written to %s", resultsFilePath.c_str());
}
