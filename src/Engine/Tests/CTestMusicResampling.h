#pragma once

#include "CTest.h"

// CTestMusicResampling
// The music path converts the file's rate to the engine's, and the sample
// domain does not move while it does.
//
// Both halves matter. Without the first, 48 kHz content plays 8.8% long and
// 1.45 semitones flat -- which is what shipped. Without the second, fixing the
// first would silently invalidate every cue point ever stored, because
// cueInSample / cueOutSample / samplePosition are persisted in the FILE's
// native frames and handed straight to ov_pcm_seek.
class CTestMusicResampling : public CTest
{
public:
	CTestMusicResampling();
	virtual ~CTestMusicResampling();

	virtual const char *GetName() override { return "MusicResampling"; }
	virtual void Run(ITestCallback *callback) override;
	virtual void Cancel() override;
};
