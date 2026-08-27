#pragma once
#include "CTest.h"

// S-5 Phase 5 Task 1: VALUE-LEVEL assertions on the shared HDR video transfer
// maths (CVideoTransferFunctions.h).
//
// WHY THIS EXISTS WHEN CTestVideoHdrPoster ALREADY DECODES A PQ CLIP: that
// test asserts the PQ peak lands inside a deliberately generous 4.0-16.0 band
// around the fixture's computed 7.92x, and for HLG it asserts only peak > 0.
// A botched extraction of these constants -- a wrong PQ m2, a dropped HLG OOTF
// gamma -- passes both. So the capability check cannot detect drift, and drift
// is precisely what Task 1 exists to prevent. These are exact numbers to 1e-6.
//
// Pure maths, no decoder, no GPU: runs headlessly and cannot be broken by a
// missing codec.
class CTestVideoTransfer : public CTest
{
public:
	CTestVideoTransfer() = default;
	~CTestVideoTransfer() = default;
	virtual const char *GetName() override { return "VideoTransfer"; }
	virtual void Run(ITestCallback *callback) override;
	virtual void Cancel() override { isRunning = false; }
};
