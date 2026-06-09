#include "CTestSuite.h"
#include "CTestRunner.h"
#include "CTest.h"
#include "SYS_Main.h"
#include "SYS_Funct.h"
#include "SYS_CommandLine.h"
#include <cstdio>
#include <cstdlib>
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

static void ExitCliTestProcess(int status)
{
	RestoreDefaultSignalHandlers();
	LOGM("CTestSuite: exiting CLI test process with status=%d", status);
	LOG_Shutdown();
	// CLI suites run inside MT_Render(); returning would continue into the
	// current frame's SDL present path before MTEngine observes SYS_Shutdown().
	std::_Exit(status);
}

bool CTestSuite::isCLIModeActive = false;

CTestSuite::CTestSuite()
{
	currentTestIndex = -1;
	isRunning = false;
	exitOnCompletion = false;
	stopOnFirstFailure = false;
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

	// Default: run every test and report all failures in one pass. Opt back into
	// fast-fail with --stop-on-first-failure. --all-plugin-tests asks the
	// subclass to also include its optional/removable tests.
	bool includeOptional = false;
	for (size_t i = 0; i < sysCommandLineArguments.size(); i++)
	{
		if (strcmp(sysCommandLineArguments[i], "--stop-on-first-failure") == 0)
			suite->stopOnFirstFailure = true;
		else if (strcmp(sysCommandLineArguments[i], "--all-plugin-tests") == 0)
			includeOptional = true;
	}
	// Running a specific test by name may target any compiled test, including an
	// optional one — so make all registrars available in that case.
	if (testName != NULL)
		includeOptional = true;
	suite->SetIncludeOptionalTests(includeOptional);

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
			ExitCliTestProcess(1);
			return;
		}
	}

	suite->Run();
}

void CTestSuite::ListTestsFromCLI(CTestSuite *suite)
{
	// --all-plugin-tests asks the subclass to also list its optional tests.
	for (size_t i = 0; i < sysCommandLineArguments.size(); i++)
		if (strcmp(sysCommandLineArguments[i], "--all-plugin-tests") == 0)
			suite->SetIncludeOptionalTests(true);

	suite->RegisterTests();

	const char *outPath = "tests/results/test_list.txt";
	FILE *f = fopen(outPath, "w");
	for (auto &t : suite->tests)
	{
		// Prefix on stdout so the orchestrator can parse past log noise; the
		// file holds "<category>\t<name>" so the runner can group core vs
		// plugin tests. (The orchestrator reads the first tab-separated field
		// as category and the name as the rest.)
		printf("CTESTSUITE_TEST: [%s] %s\n", t->category, t->GetName());
		if (f != NULL)
			fprintf(f, "%s\t%s\n", t->category, t->GetName());
	}
	if (f != NULL)
		fclose(f);
	fflush(stdout);

	LOGM("CTestSuite: listed %d tests to %s", (int)suite->tests.size(), outPath);
	LOG_Shutdown();
	std::_Exit(0);
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
	if (!useTimeoutWatchdogThread)
	{
		LOGS("CTestSuite: Timeout watchdog disabled (single-threaded completion)");
		return;
	}

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
				if (r.success)
					passed++;
			}
			LOGM("SUITE RESULTS: %d/%d passed", passed, (int)results.size());
			ExitCliTestProcess((passed == (int)results.size() && !results.empty()) ? 0 : 1);
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

	if (!success && stopOnFirstFailure)
	{
		LOGS("CTestSuite: Test failed, stopping suite (--stop-on-first-failure)");
		CTestRunner::isTestPending = false;
		isRunning = false;

		if (exitOnCompletion)
		{
			WriteResults();

			int passed = 0;
			for (auto &r : results)
			{
				if (r.success)
					passed++;
			}
			LOGM("SUITE RESULTS: %d/%d passed", passed, (int)results.size());
			ExitCliTestProcess(1);
		}
		return;
	}

	if (!success)
	{
		// Continue-on-failure (default): keep running so one pass surfaces every
		// failure instead of hiding the tests after the first failing one.
		LOGS("CTestSuite: [%s] FAILED - continuing to next test", test->GetName());
	}

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
		if (r.success)
			passed++;
	}
	fprintf(f, "---\n");
	fprintf(f, "RESULT: %d/%d passed\n", passed, (int)results.size());
	fclose(f);

	LOGM("CTestSuite: Results written to %s", resultsFilePath.c_str());
}
