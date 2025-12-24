#include "SND_SoundEngine.h"
#include "SYS_Threading.h"
#include "SYS_Main.h"
#include "SND_Main.h"
#include "SYS_FileSystem.h"
#include "CSlrString.h"
#include "SYS_DefaultConfig.h"

//#define WRITE_AUDIO_OUT_TO_FILE

CSoundEngine *gSoundEngine;

#if defined(WRITE_AUDIO_OUT_TO_FILE)
FILE *fpMainAudioOutWriter;
#endif

//static int getOneSampleSin();

void playCallback(void *udata, Uint8 *mixBuffer, int len)
{
//	LOGD("playCallback: len=%d", len);
	int numSamples = len/4;
	
	int *outBuffer = (int*)mixBuffer;
	
	gSoundEngine->LockMutex("playCallback");

//	// test sound
//	for (u32 i = 0; i < numSamples; i++)
//	{
//		outBuffer[i] = getOneSampleSin();
//	}

//	LOGD("SND_MainMixer numSamples=%d", numSamples);
	SND_MainMixer(outBuffer, numSamples);
	
	gSoundEngine->UnlockMutex("playCallback");
	
#if defined(WRITE_AUDIO_OUT_TO_FILE)
	fwrite(outBuffer, numSamples, 4, fpMainAudioOutWriter);
#endif

//	LOGD("playCallback completed");
}

void SND_Init()
{
	LOGA("SND_Init");
	
	if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0)
	{
		LOGError("SDL_InitSubSystem(SDL_INIT_AUDIO) failed: %s\n", SDL_GetError());
		SYS_FatalExit();
	}
	
	SND_MainInitialize();
	
	gSoundEngine = new CSoundEngine();
	gSoundEngine->DebugPrintAudioDevices();
	gSoundEngine->SetOutputAudioDevice(NULL);
	gSoundEngine->StartPlaying();
	
	LOGA("SDL_OpenAudio OK");
}

void SND_Start()
{
	// now sound is started with SND_Init, ideally we would like to init everything and then start the sound. This is TODO.
}

void SND_Shutdown()
{
	LOGM("SND_Shutdown");
	gSoundEngine->LockMutex("SND_Shutdown");
	gSoundEngine->StopPlaying();
	gSoundEngine->UnlockMutex("SND_Shutdown");
}

CSoundEngine::CSoundEngine()
{
	this->mutex = new CSlrMutex("CSoundEngine");
	
#if defined(WRITE_AUDIO_OUT_TO_FILE)
	char fpath[1024];
	sprintf(fpath, "%s/MTEngine-AudioOut.raw", gCPathToDocuments);
	
	fpMainAudioOutWriter = fopen(fpath, "wb");
	if (!fpMainAudioOutWriter)
	{
		SYS_FatalExit("CSoundEngine: opening MTEngine-AudioOut.raw for write failed");
	}
	
	LOGM("CSoundEngine: storing audio out to file %s", fpath);
#endif

	this->deviceOutName[0] = 0;
	this->currentAudioDevice = 0;
	this->isPlaybackOn = false;
}


CSoundEngine::~CSoundEngine()
{
	
}

void CSoundEngine::DebugPrintAudioDevices()
{
	LOGD("CSoundEngine::DebugPrintAudioDevices");
	int numDevices = SDL_GetNumAudioDevices(0);
	LOGD("...SDL_GetNumAudioDevices returned %d", numDevices);
	for (int index = 0; index < numDevices; index++)
	{
		const char *cDeviceName = SDL_GetAudioDeviceName(index, 0);
		LOGD("Device name=%s", cDeviceName);
	}
}

std::list<const char *> *CSoundEngine::EnumerateAvailableOutputDevices()
{
	std::list<const char *> *audioDevices = new std::list<const char *>();
		
	int numDevices = SDL_GetNumAudioDevices(0);
	for (int index = 0; index < numDevices; index++)
	{
		const char *cDeviceName = SDL_GetAudioDeviceName(index, 0);
		audioDevices->push_back(STRALLOC(cDeviceName));
	}
	return audioDevices;
}

bool CSoundEngine::SetOutputAudioDevice(const char *deviceName)
{
	LOGM("CSoundEngine::SetOutputAudioDevice: deviceName=%s", deviceName ? deviceName : "NULL");
	
	// Note, we first pause playback as per comment
	bool wasPlaybackOn = isPlaybackOn;
	if (isPlaybackOn)
	{
		this->StopPlaying();
	}

	this->LockMutex("CSoundEngine::SetOutputAudioDevice");
	
	SDL_CloseAudioDevice(currentAudioDevice);
	SDL_CloseAudio();
	currentAudioDevice = 0;
	
	SDL_AudioSpec wanted;
	
	int bufferNumSamples = 512; //1024;  // Good low-latency value for callback
	gApplicationDefaultConfig->GetInt("AudioBufferNumSamples", &bufferNumSamples, 512);
	
	if (bufferNumSamples > 8192)
		bufferNumSamples = 8192;
	if (bufferNumSamples < 16)
		bufferNumSamples = 16;
	
	// Set the audio format
	wanted.freq = 44100;
	wanted.format = AUDIO_S16LSB;
	wanted.channels = 2;    // 1 = mono, 2 = stereo
	wanted.samples = bufferNumSamples;
	wanted.callback = playCallback;
	wanted.userdata = NULL;

	if (deviceName == NULL)
	{
		if (SDL_OpenAudio(&wanted, NULL) < 0)
		{
			LOGError("Couldn't open audio: %s\n", SDL_GetError());
			SYS_FatalExit();
		}
		
		// BUG: this causes emulation fps to drop, although it is recommended version by the SDL authors, it does not work properly for some reason
//		currentAudioDevice = SDL_OpenAudioDevice(NULL, false, &wanted, NULL, 0);
//		if (currentAudioDevice == 0)
//		{
//			LOGError("Couldn't open default audio output device: %s\n", SDL_GetError());
//			SYS_FatalExit();
//		}
		
		deviceOutName[0] = 0;
		currentAudioDevice = 1;
	}
	else
	{
		strcpy(deviceOutName, deviceName);
		currentAudioDevice = SDL_OpenAudioDevice(deviceName, false, &wanted, NULL, 0);
		if (currentAudioDevice == 0)
		{
			LOGError("Couldn't open audio device: %s (device=%s)\n", SDL_GetError(), deviceName);
			
			// TODO: fallback to mono
			if (SDL_OpenAudio(&wanted, NULL) < 0)
			{
				LOGError("Couldn't open audio: %s\n", SDL_GetError());
				SYS_FatalExit();
			}
			currentAudioDevice = 1;
		}
		else
		{
			SDL_PauseAudioDevice(currentAudioDevice, 0);
		}
	}

	LOGM("CSoundEngine::SetOutputAudioDevice: opened device %d", currentAudioDevice);
	
	if (wasPlaybackOn)
	{
		this->StartPlaying();
	}
	
	this->UnlockMutex("CSoundEngine::SetOutputAudioDevice");
	
	return true;;
}

void CSoundEngine::RestartAudioDevice()
{
	if (deviceOutName[0] == 0)
	{
		SetOutputAudioDevice(NULL);
	}
	else
	{
		SetOutputAudioDevice(deviceOutName);
	}
}

void CSoundEngine::StartPlaying()
{
	SDL_PauseAudioDevice(currentAudioDevice, 0);
	isPlaybackOn = true;
}

void CSoundEngine::StopPlaying()
{
	SDL_PauseAudioDevice(currentAudioDevice, 1);
	isPlaybackOn = false;
}

void CSoundEngine::LockMutex(const char *whoLocked)
{
	//	LOGD("CSoundEngine::LockMutex: %s", whoLocked);
	this->mutex->Lock();
}

void CSoundEngine::UnlockMutex(const char *whoLocked)
{
	//	LOGD("CSoundEngine::UnlockMutex: %s", whoLocked);
	this->mutex->Unlock();
}

// for testing purposes
//static int getOneSampleSin()
//{
//	static float sinPosL = 0;
//	static float sinSpeedL = 0.003;
//	static float sinChangeL = 0.000001;
//	static float sinPosR = 0;
//	static float sinSpeedR = 0.02;
//	static float sinChangeR = 0.00001;
//	
//	short chanL = (sin(sinPosL) * 650);
//	short chanR = (sin(sinPosR) * 650);
//	
//	sinPosL += sinSpeedL;
//	if (sinPosL > 1)
//		sinPosL = -1;
//	
//	sinSpeedL += sinChangeL;
//	
//	if (sinSpeedL > 0.03 || sinSpeedL < 0.003)
//		sinChangeL = -sinChangeL;
//	
//	sinPosR += sinSpeedR;
//	if (sinPosR > 1)
//		sinPosR = -1;
//	
//	sinSpeedR += sinChangeR;
//	
//	if (sinSpeedR > 0.03 || sinSpeedR < 0.003)
//		sinChangeR = -sinChangeR;
//	
//	chanL = 0xFFFF - chanL;
//	
//	return ((chanR & 0x0000FFFF) << 16) | (chanL & 0x0000FFFF);
//}

