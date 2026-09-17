#include "CTestEngineSampleRate.h"
#include "SYS_Defs.h"
#include "SND_SoundEngine.h"
#include "DBG_Log.h"
#include <cstdio>

// The engine test framework has no assertion macro (CTest.h offers
// StepCompleted/TestCompleted/TestSkipped). File-local, as CTestI18nEngine.cpp
// already does.
#define ASSERT_TRUE(cond, msg) do { if (!(cond)) { TestCompleted(false, (msg)); return; } } while (0)

CTestEngineSampleRate::CTestEngineSampleRate()  {}
CTestEngineSampleRate::~CTestEngineSampleRate() {}

void CTestEngineSampleRate::Run(ITestCallback *callback)
{
	this->callback = callback;
	isRunning = true;
	int stepNum = 1;

	u32 rate = SND_GetEngineSampleRate();

	ASSERT_TRUE(rate > 0, "FAIL: SND_GetEngineSampleRate() returned 0");
	char msg[128];
	snprintf(msg, sizeof(msg), "engine sample rate reported: %u Hz", (unsigned)rate);
	StepCompleted(stepNum++, true, msg);

	// No device (headless CI box, RDP session) means there is no stream to
	// cross-check against. The accessor still has to answer, which the step
	// above proved; skip only the SDL half.
	if (gSoundEngine == NULL || !gSoundEngine->hasAudioDevice || gSoundEngine->audioStream == NULL)
	{
		TestSkipped("no audio device: the accessor answered, but there is no stream to cross-check");
		return;
	}

	// THE REAL ASSERTION. src_spec is the format the engine feeds the stream --
	// i.e. what wanted.freq was actually set to. If the accessor and the device
	// were declared separately and drifted, these disagree.
	SDL_AudioSpec srcSpec;
	SDL_AudioSpec dstSpec;
	ASSERT_TRUE(SDL_GetAudioStreamFormat(gSoundEngine->audioStream, &srcSpec, &dstSpec),
	            "FAIL: SDL_GetAudioStreamFormat failed on the engine stream");

	if ((u32)srcSpec.freq != rate)
	{
		char buf[192];
		snprintf(buf, sizeof(buf),
		         "FAIL: two declarations of the engine rate disagree -- accessor says %u Hz, "
		         "the audio stream was opened at %d Hz", (unsigned)rate, srcSpec.freq);
		LOGError("CTestEngineSampleRate: %s", buf);
		TestCompleted(false, buf);
		return;
	}
	snprintf(msg, sizeof(msg), "the audio stream's source format agrees: %d Hz", srcSpec.freq);
	StepCompleted(stepNum++, true, msg);

	ASSERT_TRUE(srcSpec.channels == 2, "FAIL: the engine mix is not stereo");
	StepCompleted(stepNum++, true, "the engine mix is stereo, as the mixer assumes");

	TestCompleted(true, "the engine sample rate has one runtime source of truth");
}

void CTestEngineSampleRate::Cancel()
{
	isRunning = false;
}
