#pragma once

#include "ITestCallback.h"
#include <ctime>

#include <string>

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

	// True when this test completed via TestSkipped() -- it did NOT run. Public
	// because CTestSuite reads it when tallying, and it must not be counted as
	// a pass.
	bool WasSkipped() const { return skipped; }

	// Non-empty when a MANDATORY part of this test did not run. Public because
	// CTestSuite reads it when tallying -- see ReportRequiredGap().
	const std::string &GetRequiredGap() const { return requiredGap; }

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

	// Complete the test as SKIPPED -- did not run, rather than ran and passed.
	//
	// A skip reported as a pass is not a neutral simplification, it is a lie the
	// headline count repeats: CTestVideoRenderSmoke did exactly that for the
	// headless case, so the suite read 205/205 while the ONE guard covering
	// video orientation never executed on either backend, and a Metal converter
	// shipped playing every video upside-down.
	//
	// Skips still exit through TestCompleted(true, ...) so the dispatch chain,
	// ITestCallback and every existing implementer stay untouched; the suite
	// reads WasSkipped() when it tallies and reports them separately.
	//
	// `reason` must say what did NOT get verified, not merely that something was
	// absent -- "no RAW fixture, so the decode path was never exercised" rather
	// than "no fixture".
	void TestSkipped(const char *reason);

	// Record that a MANDATORY part of this test did not run.
	//
	// Distinct from TestSkipped() because the common case is not a whole test
	// skipping: CTestRawCodecCapability verifies build capabilities and then
	// silently omits the codec-DNG decode that DoD 8 calls required on all three
	// machines -- it PASSES, and the omission lives only in its summary string.
	// A promise in a string is not a gate.
	//
	// The test still completes normally, so an ordinary run stays green. A run
	// with --require-fixtures turns every recorded gap into a FAILURE, which is
	// what makes "required" mean something.
	//
	// Call before completing. Safe to call more than once; the reasons join.
	void ReportRequiredGap(const char *what);




	int currentStep;
	bool isRunning;
	// Latched by the first TestCompleted() call. A second one -- an assert
	// macro completing the test and a later completion path firing too, or a
	// late async callback -- used to push a DUPLICATE result AND re-drive the
	// suite's dispatch chain, so the run silently skipped its last test while
	// results.size() still equalled the test count: an N-1/N score with no
	// failing test named anywhere. One instance runs once, so a plain latch
	// is exactly right.
	bool completed = false;
	bool skipped = false;
	std::string requiredGap;
	ITestCallback *callback;
};
