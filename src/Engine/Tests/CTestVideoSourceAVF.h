#pragma once
#include "CTest.h"

// Proves CVideoSourceAVFoundation -- the store build's video path on macOS --
// actually decodes, rather than merely compiling. Every existing video test in
// this programme keys on MT_ENABLE_FFMPEG and reports a hollow PASS when it is
// off, so without this the FFmpeg-free configuration had no coverage at all.
class CTestVideoSourceAVF : public CTest
{
public:
	CTestVideoSourceAVF();
	~CTestVideoSourceAVF() override;
	const char *GetName() override { return "VideoSourceAVF"; }
	void Run(ITestCallback *callback) override;
	void Cancel() override;
};
