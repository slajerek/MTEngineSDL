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

// The results file, and why it is not simply a relative path any more.
//
// An MTEngineSDL app finds its assets through the CURRENT WORKING DIRECTORY --
// RES_ResolveResourceDir has two candidate roots and both are CWD-derived -- so
// a test run has to start in the release package, where assets/ sits beside the
// binary. A host application that needs assets cannot start anywhere else.
//
// But this path was a relative literal opened with fopen("w"), which does not
// create directories. Running from the package therefore wrote nothing: the
// app looked for tests/results/ UNDER the package, found none, logged a failure
// nobody reads, and the runner then parsed a stale file from the previous run
// or none at all. Silent, and it would have reported the previous run's verdict.
//
// So the path is resolvable, which is exactly what the testing rule allows:
// environment variables and CLI flags are acceptable for test configuration,
// output paths and deterministic fixtures, and not for skipping anything that
// is being measured. --results-file wins, then
// MT_TEST_RESULTS, then the historical relative default so that every existing
// invocation keeps behaving as it did.
// THE HARNESS'S OWN PROGRESS LINES ARE NOT A LOG LEVEL. In a --logs off build
// every LOGS above compiles to nothing, and a headless run that crashed then
// names no test at all -- the results file is never written, and the log
// holds only the crash. So with MT_DEBUG_LOGS=0 these go to stderr directly;
// with it on they stay LOGS, as before, so the two never double up.
#if MT_DEBUG_LOGS
#define MT_TEST_PROGRESS(...) LOGS(__VA_ARGS__)
#else
#define MT_TEST_PROGRESS(...) do { fprintf(stderr, "[TEST] " __VA_ARGS__); fprintf(stderr, "\n"); fflush(stderr); } while (0)
#endif

const char *MT_TestResultsPath()
{
	static string resolved;
	if (!resolved.empty())
		return resolved.c_str();

	for (size_t i = 0; i < sysCommandLineArguments.size(); i++)
	{
		if (strcmp(sysCommandLineArguments[i], "--results-file") == 0
			&& i + 1 < sysCommandLineArguments.size())
		{
			resolved = sysCommandLineArguments[i + 1];
			return resolved.c_str();
		}
	}

	const char *fromEnv = getenv("MT_TEST_RESULTS");
	if (fromEnv != NULL && fromEnv[0] != '\0')
	{
		resolved = fromEnv;
		return resolved.c_str();
	}

	resolved = "tests/results/last_run.txt";
	return resolved.c_str();
}

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
	requireFixtures = (getenv("PC_REQUIRE_FIXTURES") != NULL);

	// EAGERLY, before any test runs: the walk starts from the directory the
	// process began in, and a host that later changes its cwd (a command-line
	// option, a plugin, a jukebox) must not be able to make a first lazy call
	// latch the wrong root. See CTest::ProjectRootPath.
	if (CTest::ProjectRootPath().empty())
		LOGError("CTestSuite: no project root found (no mtengine.caps or .git above the start directory, and MT_TEST_PROJECT_DIR unset) -- fixture paths will not resolve");
}

CTestSuite::~CTestSuite()
{
}

void CTestSuite::RunFromCLI(CTestSuite *suite, const char *testName)
{
	isCLIModeActive = true;
	CTestRunner::isTestPending = true;

	suite->exitOnCompletion = true;
	suite->resultsFilePath = MT_TestResultsPath();

	// Default: run every test and report all failures in one pass. Opt back into
	// fast-fail with --stop-on-first-failure. --all-plugin-tests asks the
	// subclass to also include its optional/removable tests.
	bool includeOptional = false;
	for (size_t i = 0; i < sysCommandLineArguments.size(); i++)
	{
		if (strcmp(sysCommandLineArguments[i], "--stop-on-first-failure") == 0)
			suite->stopOnFirstFailure = true;
		if (strcmp(sysCommandLineArguments[i], "--require-fixtures") == 0)
			suite->requireFixtures = true;
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

	// BESIDE THE RESULTS FILE, not at a cwd-relative path: a final build runs
	// from prod/, and this list was the one thing the engine itself wrote
	// into a release package.
	std::string outPath = MT_TestResultsPath();
	size_t slash = outPath.find_last_of("/\\");
	outPath = (slash == std::string::npos) ? std::string("test_list.txt")
										   : outPath.substr(0, slash + 1) + "test_list.txt";
	FILE *f = fopen(outPath.c_str(), "w");
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

	LOGM("CTestSuite: listed %d tests to %s", (int)suite->tests.size(), outPath.c_str());
	LOG_Shutdown();
	std::_Exit(0);
}

void CTestSuite::Run()
{
	MT_TEST_PROGRESS("CTestSuite::Run: Starting test suite with %d tests", (int)tests.size());
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
		MT_TEST_PROGRESS("CTestSuite: Timeout watchdog disabled (single-threaded completion)");
		return;
	}

	std::thread watchdog([this]() {
		MT_TEST_PROGRESS("CTestSuite: Timeout watchdog started (per-test=%ds, suite=%ds)", defaultTestTimeoutSeconds, suiteTimeoutSeconds);
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
	MT_TEST_PROGRESS("CTestSuite::Cancel");
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

	// Re-entrant call: we are inside test->Run() below, because the test called
	// TestCompleted() synchronously (the common case -- and the ONLY case for a
	// test that aborts early on a failed assertion). Starting test N+1 here would
	// run the rest of the suite nested inside test N's stack frame, so every RAII
	// guard test N still holds (scoped temp dirs, saved/restored global settings,
	// live browsers and decode pools) would only unwind after the last test had
	// finished -- leaking test N's mutated global state into all of them. Hand the
	// advance back to the dispatch loop instead; it runs once Run() has returned
	// and the guards have fired.
	if (dispatchingTest)
	{
		advanceRequested = true;
		return;
	}

	do
	{
		advanceRequested = false;
		currentTestIndex++;

		if (currentTestIndex >= (int)tests.size())
		{
			MT_TEST_PROGRESS("CTestSuite: All tests completed");
			CTestRunner::isTestPending = false;
			isRunning = false;

			if (exitOnCompletion)
			{
				WriteResults();

				int passed = 0, skipped = 0, gaps = 0;
				for (auto &r : results)
				{
					if (!r.requiredGap.empty())
					{
						gaps++;
						LOGM("REQUIRED COVERAGE MISSING: [%s] %s",
							 r.testName.c_str(), r.requiredGap.c_str());
					}
					if (r.skipped)
						skipped++;
					else if (r.success)
						passed++;
				}
				if (gaps > 0)
					LOGM("SUITE: %d test(s) reported REQUIRED coverage gaps%s", gaps,
						 requireFixtures ? " -- FAILING the run (--require-fixtures)"
										 : " -- re-run with --require-fixtures to make these fail");
				// Skips are reported SEPARATELY and are not counted as passes.
				// The exit code still treats them as non-failures -- a missing
				// fixture must not turn a machine red -- but the number no
				// longer claims coverage that did not happen.
				if (skipped > 0)
					LOGM("SUITE RESULTS: %d/%d passed, %d SKIPPED (did not run)",
						 passed, (int)results.size() - skipped, skipped);
				else
					LOGM("SUITE RESULTS: %d/%d passed", passed, (int)results.size());
				// Exit on FAILURES, not on "passed == total". Skips are not
				// failures -- a machine without a RAW fixture or a GT2 plugin
				// must not go red -- but they are no longer counted as passes
				// either, so the old equality would now fail every run that
				// skips anything.
				const bool anyFailed = (passed + skipped) != (int)results.size();
				// Under --require-fixtures a recorded gap is a failure, and so is
				// any skip: that mode exists for the runs that claim DoD
				// compliance, where "the fixture was absent" is exactly the
				// thing being checked.
				const bool strictViolation = requireFixtures && (gaps > 0 || skipped > 0);
				ExitCliTestProcess((!anyFailed && !strictViolation && !results.empty()) ? 0 : 1);
			}
			return;
		}

		CTest *test = tests[currentTestIndex].get();
		MT_TEST_PROGRESS("CTestSuite: Running test %d/%d: %s", currentTestIndex + 1, (int)tests.size(), test->GetName());
		test->startTime = time(NULL);

		dispatchingTest = true;
		test->Run(this);
		dispatchingTest = false;

		// advanceRequested is set only by the re-entrant branch above, i.e. the
		// test completed synchronously. A test that completes later (async) leaves
		// it false: the loop exits and its own OnTestCompleted -- arriving on an
		// empty dispatch stack -- drives the chain onward exactly as before.
	} while (advanceRequested && isRunning);
}

void CTestSuite::OnTestStepCompleted(CTest *test, int stepId, bool success, const char *message)
{
	MT_TEST_PROGRESS("CTestSuite: [%s] Step %d %s: %s", test->GetName(), stepId, success ? "OK" : "FAILED", message);
}

void CTestSuite::OnTestCompleted(CTest *test, bool success, const char *summary)
{
	MT_TEST_PROGRESS("CTestSuite: [%s] Completed %s: %s", test->GetName(), success ? "OK" : "FAILED", summary);

	results.push_back({test->GetName(), success, test->WasSkipped(), test->GetRequiredGap(), summary});

	if (!success && stopOnFirstFailure)
	{
		MT_TEST_PROGRESS("CTestSuite: Test failed, stopping suite (--stop-on-first-failure)");
		CTestRunner::isTestPending = false;
		isRunning = false;

		if (exitOnCompletion)
		{
			WriteResults();

			int passed = 0, skipped = 0;
			for (auto &r : results)
			{
				if (r.skipped)
					skipped++;
				else if (r.success)
					passed++;
			}
			if (skipped > 0)
				LOGM("SUITE RESULTS: %d/%d passed, %d SKIPPED (did not run)",
					 passed, (int)results.size() - skipped, skipped);
			else
				LOGM("SUITE RESULTS: %d/%d passed", passed, (int)results.size());
			ExitCliTestProcess(1);
		}
		return;
	}

	if (!success)
	{
		// Continue-on-failure (default): keep running so one pass surfaces every
		// failure instead of hiding the tests after the first failing one.
		MT_TEST_PROGRESS("CTestSuite: [%s] FAILED - continuing to next test", test->GetName());
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
		fprintf(f, "[%s] %s: %s\n", r.testName.c_str(),
		        r.skipped ? "SKIP" : (r.success ? "PASS" : "FAIL"), r.summary.c_str());
		if (r.success && !r.skipped)
			passed++;
	}
	fprintf(f, "---\n");
	int skippedCount = 0;
	for (auto &r : results)
		if (r.skipped) skippedCount++;
	if (skippedCount > 0)
		fprintf(f, "RESULT: %d/%d passed, %d SKIPPED (did not run)\n",
		        passed, (int)results.size() - skippedCount, skippedCount);
	else
		fprintf(f, "RESULT: %d/%d passed\n", passed, (int)results.size());
	fclose(f);

	LOGM("CTestSuite: Results written to %s", resultsFilePath.c_str());
}
