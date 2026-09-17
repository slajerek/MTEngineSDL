#pragma once

#include "CTest.h"

// CTestWebMVpxLane
// Measures whether CVideoSourceWebMVpx can carry a VP9 + Opus WebM WITH AUDIO.
//
// Why this test exists rather than a routing change. CVideoPlayer routes to this
// source only for alpha-channel VP9 (`ProbeIsAlphaVP9WebM`), and the one test of
// the lane anywhere in the programme uses a file that has alpha and NO audio. So
// nobody knows whether the lane handles ordinary VP9+Opus -- and that is exactly
// what one app ships as its only video (assets/videos/intro.webm), currently
// decoded by FFmpeg.
//
// The lane is nestegg (ISC) + libvpx (BSD-3) + libopus (BSD-3): licence-clean and
// already linked. If it carries audio, moving that content off FFmpeg is a
// routing change. If it does not, it is a feature, and the difference decides
// how tracker item 1.8 is scheduled. Broadening the routing without knowing
// would change behaviour for a heavier consumer whose suite cannot run here.
class CTestWebMVpxLane : public CTest
{
public:
	CTestWebMVpxLane();
	virtual ~CTestWebMVpxLane();

	virtual const char *GetName() override { return "WebMVpxLane"; }
	virtual void Run(ITestCallback *callback) override;
	virtual void Cancel() override;
};
