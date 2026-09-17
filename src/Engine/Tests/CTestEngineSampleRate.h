#pragma once

#include "CTest.h"

// CTestEngineSampleRate
// The engine sample rate must have exactly ONE declaration, read at runtime.
//
// It had two: SND_SoundEngine.h's SOUND_SAMPLE_RATE macro, and a bare 44100
// literal at SND_SoundEngine.cpp's wanted.freq. Nothing tied them together, so
// changing the macro would have detuned every decoded stream while the device
// stayed where it was -- the same class of defect as the missing resampler this
// test was written alongside.
//
// The assertion is deliberately NOT "the accessor equals the macro", which is a
// tautology satisfied by reading the macro. It asks SDL what the audio stream's
// source format actually IS and requires the accessor to agree.
class CTestEngineSampleRate : public CTest
{
public:
	CTestEngineSampleRate();
	virtual ~CTestEngineSampleRate();

	virtual const char *GetName() override { return "EngineSampleRate"; }
	virtual void Run(ITestCallback *callback) override;
	virtual void Cancel() override;
};
