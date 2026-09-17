#pragma once

#include "CTest.h"

// ENGINE-owned test for the OpenCV bridge (src/Engine/OpenCV/MT_OpenCV.h).
//
// It runs in BOTH branches and asserts different things in each: with the
// capability on, every supported image type round-trips and one call into each
// linked module proves the archive is real; with it off, the bridge reports
// unavailable and the resolved manifest agrees. A test that skipped itself when
// the capability is off would prove nothing about the off path -- the half most
// likely to break silently, because nobody looks at it.
//
// It lives in the engine rather than in an app so that every host linking the
// bridge gets the coverage, not only the host that happens to ship an OpenCV
// view. Apps register it in their own suite, the way the test-app template
// registers CTestKtx2Transcode.
class CTestOpenCVBridge : public CTest
{
public:
	CTestOpenCVBridge();
	virtual ~CTestOpenCVBridge();

	virtual const char *GetName() override { return "OpenCVBridge"; }
	virtual void Run(ITestCallback *callback) override;
	virtual void Cancel() override;
};
