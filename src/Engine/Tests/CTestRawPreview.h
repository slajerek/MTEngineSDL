#pragma once

#include "CTest.h"

// RD-A task 2: the RAW preview safety net (design #3).
//
// LoadRAWPreview is the only path by which the photo app displays a RAW today,
// and it had no test coverage anywhere before this class. It must be
// recorded green on BOTH sides of the USE_JPEG flip (RD-A task 3), because
// that flag changes unpack_thumb()'s behaviour (design #4.3) and a test
// written only afterwards cannot separate "the flag broke it" from "it was
// always so".
//
// Needs a real RAW from PC_RAW_FIXTURE_DIR (git-ignored tier); skips with a
// REPORTED message when the directory or fixture is absent.
class CTestRawPreview : public CTest
{
public:
	virtual const char *GetName() override { return "RawPreview"; }
	virtual void Run(ITestCallback *callback) override;
	virtual void Cancel() override { isRunning = false; }
};
