#ifndef _CSLRMUSIC_H_
#define _CSLRMUSIC_H_

#include "SYS_Defs.h"
#include "CAudioChannel.h"
#include "CSlrResourceBase.h"

#define MUSIC_TYPE_UNKNOWN	0x00
#define MUSIC_TYPE_OGG	0x01

class CSlrMusicFile : public CAudioChannel, public CSlrResourceBase
{
public:
	u8 type;
	CSlrMusicFile(const char *channelName);
	virtual ~CSlrMusicFile();

	// ---- THE SAMPLE DOMAIN --------------------------------------------------
	//
	// SeekToSample, GetCurrentSampleNum and GetLengthSamples are defined in the
	// FILE's NATIVE PCM frames. Never in engine frames. A resampler may sit
	// between this class and the mixer, and it does not change what a sample is.
	//
	// This is load-bearing, not pedantry. Playlist cue points are persisted to
	// disk in these units (cueInSample / cueOutSample / samplePosition in
	// Audio/CSlrMusicPlaylistPersistence.cpp), and a cue is a property of the
	// CONTENT, not of the playback device: the same playlist has to mean the
	// same thing on a machine running at 44.1 kHz, 48 kHz or 96 kHz. The engine
	// rate is not a constant either -- a future mixer application is expected to
	// make it dynamic.
	//
	// Convert to seconds with GetSourceSampleRate(), never with
	// SOUND_SAMPLE_RATE. Getting that wrong is exactly the bug that shipped in
	// CSlrMusicFileOgg::Seek(double), which multiplied seconds by the ENGINE
	// rate and handed the result to ov_pcm_seek, whose argument is native.
	// -------------------------------------------------------------------------

	// The file's own PCM rate and channel count, i.e. the units the methods
	// above speak in. Defaults describe a source that needs no conversion.
	// WHAT ACTUALLY DECODED THIS FILE. Stable lowercase token, never display
	// copy: "tremor", "miniaudio", "avfoundation", "mediafoundation".
	//
	// It exists so the licence rule can be TESTED. "AAC went to the OS decoder"
	// was previously unobservable -- the decoder handle is private -- so the
	// only available assertion was the negative one (no FFmpeg symbol linked),
	// which says nothing in a private build where FFmpeg is linked for video.
	// With this, a test can assert BOTH that the bytes are right and that the
	// decoder that produced them is one a store build may ship.
	virtual const char *GetActiveDecoderName() { return "unknown"; }

	virtual u32 GetSourceSampleRate();
	virtual int GetSourceChannels() { return 2; }

	virtual void Play();
	virtual void Pause();
	virtual void Stop();
	virtual void Rewind();
	virtual void Seek(double second);
	virtual void Seek(u32 minute, u32 second);
	virtual u32 GetCurrentSecond();
	virtual u64 GetCurrentSampleNum();
	virtual void SeekToSample(u64 sampleNum);
	virtual void SeekToMillisecond(u64 millisecond);
	virtual u64 GetCurrentMillisecond();
	virtual u64 GetLengthSeconds();
	virtual u64 GetLengthSamples();
	
	virtual bool IsPlaying();

	// overwrites buffer
	virtual void Mix(int *mixBuffer, u32 numSamples);
	virtual void MixFloat(float *mixBufferL, float *mixBufferR, u32 numSamples);
	
	// adds to buffer
	virtual void MixIn(int *mixBuffer, u32 numSamples);
	virtual void MixInFloat(float *mixBufferL, float *mixBufferR, u32 numSamples);
	
	volatile bool repeat;

	bool shouldBeDestroyedByEngine;
	bool shouldBeRemovedByEngine;

	// resource should free memory, @returns memory freed
	virtual u32 ResourceDeactivate(bool async);

	// resource should load itself, @returns memory allocated
	virtual u32 ResourceActivate(bool async);

	// get size of resource in bytes
	virtual u32 ResourceGetSize();

	virtual const char *ResourceGetTypeName();
};

#endif
//_CSLRMUSIC_H_

