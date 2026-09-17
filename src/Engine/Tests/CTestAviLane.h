#ifndef _CTESTAVILANE_H_
#define _CTESTAVILANE_H_

#pragma once

#include "CTest.h"

// The AVI lane, end to end: RIFF demuxed by us, every codec decoded by the OS.
//
// NOT GATED ON MT_ENABLE_FFMPEG, deliberately, like CTestVideoSourceAVF and
// CTestWebMVpxLane. The whole point of the lane is the configuration that has
// no FFmpeg, and a test that skips itself there would leave it untested in
// exactly the build that ships.
class CTestAviLane : public CTest
{
public:
	CTestAviLane();
	virtual ~CTestAviLane();
	const char *GetName() override { return "AviLane"; }
	void Run(ITestCallback *callback) override;
	void Cancel() override;
};

#endif
//_CTESTAVILANE_H_
