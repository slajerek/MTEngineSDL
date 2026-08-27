#ifndef _SOUNDENGINE_H_
#define _SOUNDENGINE_H_

#include <list>
#include <SDL3/SDL.h>

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

	void DebugPrintAudioDevices();
	
private:
	char *whoLocked;
	CSlrMutex *mutex;
};

void SND_Init();
void SND_Start();
void SND_Shutdown();

extern CSoundEngine *gSoundEngine;

#endif
