#ifndef _SOUNDENGINE_H_
#define _SOUNDENGINE_H_

#include <list>
#include "SDL.h"

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
	
	char deviceOutName[512];
	int deviceOutIndex;

	SDL_AudioDeviceID currentAudioDevice;

	void DebugPrintAudioDevices();
	
private:
	char *whoLocked;
	CSlrMutex *mutex;
};

void SND_Init();
void SND_Shutdown();

extern CSoundEngine *gSoundEngine;

#endif
