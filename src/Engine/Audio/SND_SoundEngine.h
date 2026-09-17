#ifndef _SOUNDENGINE_H_
#define _SOUNDENGINE_H_

#include <list>
#include <SDL3/SDL.h>

#include "SYS_Defs.h"

// The rate the engine PRODUCES at. It is not a constant of nature: a future
// mixer application is expected to make it dynamic and may run at 96 kHz, and
// the engine's own low-latency audio plan already targets 48000.
//
// This macro is the INITIAL value only. Read the rate with
// SND_GetEngineSampleRate() -- never this macro, and never a literal.
//
// It used to be declared twice, here and as a bare `wanted.freq = 44100` where
// the device stream is opened, with nothing tying them together: changing one
// would have detuned every decoded stream while the device stayed put.
// CTestEngineSampleRate asserts the two now agree by asking SDL.
#define SOUND_SAMPLE_RATE 44100
#define SOUND_BUFFER_SIZE 48000

class CSlrString;
class CSlrMutex;

class CSoundEngine
{
public:
	CSoundEngine();
	~CSoundEngine();
	
	volatile bool isPlaybackOn;
	
	void StartPlaying();
	void StopPlaying();
	
	void LockMutex(const char *whoLocked);
	void UnlockMutex(const char *whoLocked);
	
	std::list<const char *> *EnumerateAvailableOutputDevices();
	bool SetOutputAudioDevice(const char *deviceName);
	void RestartAudioDevice();
	
	char deviceOutName[512];
	int deviceOutIndex;

	// SDL3 replaced the callback-on-a-device model with AUDIO STREAMS, and
	// removed the SDL 1.2-era SDL_OpenAudio() path entirely. So the old
	// isUsingAudioDeviceApi dual path is gone: there is exactly one way to open
	// a device now, which is a simplification we get for free.
	//
	// The stream is the handle for everything -- pause, resume and close all go
	// through it (SDL_DestroyAudioStream also closes the device that
	// SDL_OpenAudioDeviceStream opened for us).
	SDL_AudioStream *audioStream;
	SDL_AudioDeviceID currentAudioDevice;   // kept for logging/diagnostics only

	// false when no audio output device is available (e.g. headless/RDP
	// session with no sound hardware); playback becomes a silent no-op
	// instead of the engine fatal-exiting at startup.
	bool hasAudioDevice;

	// The rate this engine produces at, established ONCE when the device stream
	// is opened. Everything that converts between seconds and samples, or
	// resamples into the mix, must read it from here.
	u32 engineSampleRate;

	void DebugPrintAudioDevices();
	
private:
	char *whoLocked;
	CSlrMutex *mutex;
};

// The one runtime source of truth for the engine's production sample rate.
// Safe before SND_Init(): falls back to SOUND_SAMPLE_RATE while there is no
// engine yet, so early callers get the value the engine is about to use.
u32 SND_GetEngineSampleRate();

void SND_Init();
void SND_Start();
void SND_Shutdown();

extern CSoundEngine *gSoundEngine;

#endif
