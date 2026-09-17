#pragma once

#include "CTest.h"

// CTestAudioResampler
// Guards MT_AudioResampler with the three assertions that correspond to the
// three ways a resampler is got wrong:
//
//   1. LENGTH  -- the output frame count follows the rate ratio. A wrong ratio
//                 (or none at all, which is what the music path shipped) shows
//                 up here first.
//   2. PITCH   -- a 1 kHz sine stays 1 kHz. This is the assertion that would
//                 have caught the shipped defect: 48 kHz frames fed to a
//                 44.1 kHz stream sound at 919 Hz, and the frame COUNT alone
//                 cannot tell you that, because nothing counted them.
//   3. BLOCK INVARIANCE -- one long call and many short calls produce the same
//                 samples. Fractional phase and interpolation history carried
//                 across calls are exactly what breaks when this is skipped,
//                 and the audio callback always calls in short blocks.
class CTestAudioResampler : public CTest
{
public:
	CTestAudioResampler();
	virtual ~CTestAudioResampler();

	virtual const char *GetName() override { return "AudioResampler"; }
	virtual void Run(ITestCallback *callback) override;
	virtual void Cancel() override;
};
