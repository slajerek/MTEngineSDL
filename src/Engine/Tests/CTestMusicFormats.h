#pragma once
#include "CTest.h"

// Proves MP3, FLAC and WAV actually DECODE, and decode correctly -- not merely
// that a decoder was linked. Every fixture is a 1 kHz sine, so a wrong sample
// rate, a wrong channel layout or a broken resampler shows up as a measurable
// frequency error rather than as silence somebody has to listen for.
class CTestMusicFormats : public CTest
{
public:
	CTestMusicFormats();
	~CTestMusicFormats() override;
	const char *GetName() override { return "MusicFormats"; }
	void Run(ITestCallback *callback) override;
	void Cancel() override;
};
