#include "SND_SoundEngine.h"
#include "SYS_Threading.h"
#include "SYS_Main.h"
#include "SND_Main.h"
#include "SYS_FileSystem.h"
#include "CSlrString.h"
#include "SYS_DefaultConfig.h"
#include <cstring>

//#define WRITE_AUDIO_OUT_TO_FILE

CSoundEngine *gSoundEngine;

#if defined(WRITE_AUDIO_OUT_TO_FILE)
FILE *fpMainAudioOutWriter;
#endif

//static int getOneSampleSin();

// One frame is 4 bytes: 2 channels x signed 16-bit. SND_MainMixer works in
// FRAMES and writes one `int` per frame (the two 16-bit samples packed), which
// is why the old SDL2 callback divided its byte length by 4. Same arithmetic
// here -- SDL3 also asks in bytes.
#define SND_BYTES_PER_FRAME		4

// How much we mix per SND_MainMixer call. The stream may ask for more than one
// buffer's worth at a time (it is a stream, not a fixed device buffer), so the
// callback loops rather than assuming the request fits.
#define SND_MIX_CHUNK_FRAMES	2048

// SDL3 audio callback. Two things changed and both matter:
//
//  1. It no longer receives a buffer to FILL. It receives a STREAM and is told
//     how many more bytes that stream wants; we generate and SDL_PutAudioStreamData.
//     `additional_amount` is what is actually needed now; `total_amount` is the
//     whole queue depth and is not what we should mix.
//
//  2. `additional_amount` is not bounded by any buffer size we chose, so this
//     loops. An SDL2-shaped port that mixed once and assumed it had satisfied
//     the request would underrun under load and crackle.
void SDLCALL playCallback(void *udata, SDL_AudioStream *stream, int additional_amount, int total_amount)
{
//	LOGD("playCallback: additional_amount=%d total_amount=%d", additional_amount, total_amount);
	if (additional_amount <= 0)
		return;

	int framesWanted = additional_amount / SND_BYTES_PER_FRAME;

	gSoundEngine->LockMutex("playCallback");

	while (framesWanted > 0)
	{
		int outBuffer[SND_MIX_CHUNK_FRAMES];

		int chunkFrames = framesWanted;
		if (chunkFrames > SND_MIX_CHUNK_FRAMES)
			chunkFrames = SND_MIX_CHUNK_FRAMES;

//		// test sound
//		for (int i = 0; i < chunkFrames; i++)
//		{
//			outBuffer[i] = getOneSampleSin();
//		}

//		LOGD("SND_MainMixer numSamples=%d", chunkFrames);
		SND_MainMixer(outBuffer, chunkFrames);

		SDL_PutAudioStreamData(stream, outBuffer, chunkFrames * SND_BYTES_PER_FRAME);

#if defined(WRITE_AUDIO_OUT_TO_FILE)
		fwrite(outBuffer, SND_BYTES_PER_FRAME, chunkFrames, fpMainAudioOutWriter);
#endif

		framesWanted -= chunkFrames;
	}

	gSoundEngine->UnlockMutex("playCallback");

//	LOGD("playCallback completed");
}

void SND_Init()
{
	LOGA("SND_Init");

	SND_MainInitialize();

	gSoundEngine = new CSoundEngine();

	// SDL3: SDL_InitSubSystem returns BOOL. `< 0` compiles and is NEVER true,
	// so the "continue without audio" branch below would have become dead code
	// and a headless box with no sound hardware would have carried on as if a
	// device had opened.
	if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
	{
		// No audio subsystem available (e.g. headless/RDP session with no
		// sound hardware). Continue without sound rather than fatal-exiting;
		// playback calls become no-ops via hasAudioDevice.
		LOGWarning("SDL_InitSubSystem(SDL_INIT_AUDIO) failed, continuing without audio: %s\n", SDL_GetError());
	}
	else
	{
		gSoundEngine->DebugPrintAudioDevices();
		gSoundEngine->SetOutputAudioDevice(NULL);
		gSoundEngine->StartPlaying();
	}

	LOGA("SND_Init completed (hasAudioDevice=%d)", gSoundEngine->hasAudioDevice);
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
	this->audioStream = NULL;
	this->currentAudioDevice = 0;
	this->isPlaybackOn = false;
	this->hasAudioDevice = false;
}


CSoundEngine::~CSoundEngine()
{
	
}

// SDL3 replaced index-based enumeration (SDL_GetNumAudioDevices +
// SDL_GetAudioDeviceName(index, iscapture)) with an ID ARRAY the caller must
// SDL_free. Names are then looked up by ID. The practical difference is that an
// ID stays valid while devices come and go, whereas an index silently meant a
// different device the moment somebody unplugged a headset.
void CSoundEngine::DebugPrintAudioDevices()
{
	LOGD("CSoundEngine::DebugPrintAudioDevices");
	int numDevices = 0;
	SDL_AudioDeviceID *deviceIds = SDL_GetAudioPlaybackDevices(&numDevices);
	LOGD("...SDL_GetAudioPlaybackDevices returned %d", numDevices);
	if (deviceIds == NULL)
		return;
	for (int index = 0; index < numDevices; index++)
	{
		const char *cDeviceName = SDL_GetAudioDeviceName(deviceIds[index]);
		LOGD("Device name=%s", cDeviceName ? cDeviceName : "(null)");
	}
	SDL_free(deviceIds);
}

std::list<const char *> *CSoundEngine::EnumerateAvailableOutputDevices()
{
	std::list<const char *> *audioDevices = new std::list<const char *>();

	int numDevices = 0;
	SDL_AudioDeviceID *deviceIds = SDL_GetAudioPlaybackDevices(&numDevices);
	if (deviceIds == NULL)
		return audioDevices;

	for (int index = 0; index < numDevices; index++)
	{
		const char *cDeviceName = SDL_GetAudioDeviceName(deviceIds[index]);
		if (cDeviceName != NULL)
			audioDevices->push_back(STRALLOC(cDeviceName));
	}
	SDL_free(deviceIds);
	return audioDevices;
}

// Resolves a device NAME to an SDL3 device ID. Returns
// SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK when the name is NULL, and 0 when a name
// was given but no device matches -- the caller must tell those apart, because
// "user asked for a headset that is currently unplugged" and "user asked for
// the default" want different fallbacks.
static SDL_AudioDeviceID SND_FindPlaybackDeviceByName(const char *deviceName)
{
	if (deviceName == NULL)
		return SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;

	int numDevices = 0;
	SDL_AudioDeviceID *deviceIds = SDL_GetAudioPlaybackDevices(&numDevices);
	if (deviceIds == NULL)
		return 0;

	SDL_AudioDeviceID found = 0;
	for (int index = 0; index < numDevices; index++)
	{
		const char *cDeviceName = SDL_GetAudioDeviceName(deviceIds[index]);
		if (cDeviceName != NULL && strcmp(cDeviceName, deviceName) == 0)
		{
			found = deviceIds[index];
			break;
		}
	}
	SDL_free(deviceIds);
	return found;
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

	// SDL3: destroying the stream also closes the device SDL_OpenAudioDeviceStream
	// opened for it, so this is the whole teardown. The SDL2 version needed a
	// two-branch teardown (SDL_CloseAudioDevice vs SDL_CloseAudio) because it
	// supported both the modern and the SDL 1.2-era open; SDL3 has only one.
	if (audioStream != NULL)
	{
		SDL_DestroyAudioStream(audioStream);
		audioStream = NULL;
	}
	currentAudioDevice = 0;

	int bufferNumSamples = 512; //1024;  // Good low-latency value for callback
	gApplicationDefaultConfig->GetInt("AudioBufferNumSamples", &bufferNumSamples, 512);

	if (bufferNumSamples > 8192)
		bufferNumSamples = 8192;
	if (bufferNumSamples < 16)
		bufferNumSamples = 16;

	// SDL3's SDL_AudioSpec has no `samples` field -- the buffer size is not part
	// of the format any more, because a stream can be fed in any size. The
	// device-side buffer is requested through this hint instead, and it is
	// advisory: SDL may pick something else. Keeping the same value as SDL2 so
	// latency does not shift under us.
	char bufferNumSamplesStr[32];
	sprintf(bufferNumSamplesStr, "%d", bufferNumSamples);
	SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, bufferNumSamplesStr);

	// Set the audio format
	SDL_AudioSpec wanted;
	memset(&wanted, 0x00, sizeof(SDL_AudioSpec));
	wanted.freq = 44100;
	wanted.format = SDL_AUDIO_S16LE;
	wanted.channels = 2;    // 1 = mono, 2 = stereo

	// With zero enumerated playback devices, SDL's WASAPI backend still
	// blocks for a hardcoded 8 seconds (SDL_IMMDevice_Get's retry loop for
	// IMMDeviceEnumerator_GetDefaultAudioEndpoint, other/lib/SDL-release-*/
	// src/core/windows/SDL_immdevice.c) before failing -- a workaround for
	// flaky Intel drivers on real hardware that can't ever succeed when
	// there is no device to become default. Skip straight to "no audio"
	// instead of paying that stall on every startup/headless test run.
	int numPlaybackDevices = 0;
	SDL_AudioDeviceID *probeIds = SDL_GetAudioPlaybackDevices(&numPlaybackDevices);
	if (probeIds != NULL)
		SDL_free(probeIds);

	if (numPlaybackDevices == 0)
	{
		LOGWarning("CSoundEngine::SetOutputAudioDevice: no audio devices enumerated, skipping open (continuing without sound)");
		hasAudioDevice = false;
		currentAudioDevice = 0;
		deviceOutName[0] = 0;
		this->UnlockMutex("CSoundEngine::SetOutputAudioDevice");
		return false;
	}

	// SDL3 has ONE way to open a device, so the SDL2-era
	// "AudioUseDefaultOutputDeviceApi" fork is gone: it chose between
	// SDL_OpenAudioDevice and the SDL 1.2-compatible SDL_OpenAudio, and SDL3
	// removed the latter outright. The setting is now inert; it is deliberately
	// NOT read here rather than read and ignored, so a future reader does not
	// go looking for the behaviour it used to select.
	SDL_AudioDeviceID deviceId = SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;

	if (deviceName != NULL)
	{
		strncpy(deviceOutName, deviceName, sizeof(deviceOutName)-1);
		deviceOutName[sizeof(deviceOutName)-1] = 0;

		deviceId = SND_FindPlaybackDeviceByName(deviceName);
		if (deviceId == 0)
		{
			// Named device is gone (unplugged, renamed, different machine).
			// Falling back to the default is right -- silence would be a worse
			// answer than "your headset is not here, using the speakers" -- but
			// the name is cleared so we do not later claim to be on a device we
			// are not.
			LOGError("Couldn't find audio device: %s -- falling back to default output", deviceName);
			deviceOutName[0] = 0;
			deviceId = SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
		}
	}
	else
	{
		deviceOutName[0] = 0;
	}

	audioStream = SDL_OpenAudioDeviceStream(deviceId, &wanted, playCallback, NULL);
	if (audioStream == NULL)
	{
		// No audio output device available. Log and continue
		// silently instead of fatal-exiting the whole app.
		LOGWarning("Couldn't open audio, continuing without sound: %s\n", SDL_GetError());
		hasAudioDevice = false;
		currentAudioDevice = 0;
		deviceOutName[0] = 0;
		this->UnlockMutex("CSoundEngine::SetOutputAudioDevice");
		return false;
	}

	currentAudioDevice = SDL_GetAudioStreamDevice(audioStream);
	hasAudioDevice = true;
	LOGM("CSoundEngine::SetOutputAudioDevice: opened stream on device=%d name='%s'",
		 currentAudioDevice, deviceOutName[0] ? deviceOutName : "(default)");

	if (wasPlaybackOn)
	{
		this->StartPlaying();
	}

	this->UnlockMutex("CSoundEngine::SetOutputAudioDevice");

	return true;
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

// SDL3: SDL_OpenAudioDeviceStream OPENS THE DEVICE PAUSED (SDL_audio.h:1754),
// so this call is not optional the way SDL_PauseAudioDevice(dev, 0) was -- miss
// it and the app is simply silent, with no error anywhere.
//
// And it must be the STREAM resume, SDL_ResumeAudioStreamDevice (:1767), not
// SDL_ResumeAudioDevice (:833), which takes a device ID and is for devices
// opened the other way. Both exist, both compile with the right argument, and
// only one of them does anything for a stream-opened device.
void CSoundEngine::StartPlaying()
{
	if (hasAudioDevice && audioStream != NULL)
	{
		SDL_ResumeAudioStreamDevice(audioStream);
	}
	isPlaybackOn = true;
}

void CSoundEngine::StopPlaying()
{
	if (hasAudioDevice && audioStream != NULL)
	{
		SDL_PauseAudioStreamDevice(audioStream);
	}
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
