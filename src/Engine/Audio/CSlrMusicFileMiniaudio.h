#ifndef _CSLRMUSICFILEMINIAUDIO_H_
#define _CSLRMUSICFILEMINIAUDIO_H_

// MP3, FLAC and WAV playback, through miniaudio's decoders.
//
// WHY NOT FFmpeg. Every commercial build in this programme ships only through
// app stores, and LGPL-2.1 #6 cannot be satisfied inside a signed,
// receipt-checked bundle. miniaudio is public domain / MIT-0 and carries
// dr_mp3, dr_flac and dr_wav internally, so one vendored header covers three
// formats with no copyleft and no patent question -- MP3's patents expired
// worldwide in 2017.
//
// SHARES CSlrMusicFileResampled WITH THE OGG PATH. The rate conversion, the
// refill loop and the native-frame time accessors live in that base, so this
// class is a decoder and nothing else: it provides DecodeNativeStereo() and
// the sample cursor, and inherits the rest.
//
// THE SAMPLE INVARIANT, which is the thing most easily broken here:
// GetLengthSamples, GetCurrentSampleNum and SeekToSample are in the FILE's
// NATIVE PCM frames, never engine frames. Cue points stored by an app are
// native-frame numbers, so a 48 kHz file must not report engine-rate positions
// or every cue in that file moves. miniaudio is therefore configured to decode
// at the file's OWN rate rather than letting it resample, and the base converts.

#include "CSlrMusicFileResampled.h"

// Guarded on the capability: an app that resolves MT_CAP_MUSIC_PLAYER=0
// (c64d) must not link miniaudio at all, which is what the
// capability's forbidden_when_off symbol pattern now asserts.
#if !defined(MT_CAP_MUSIC_PLAYER) || MT_CAP_MUSIC_PLAYER

#include <string>
#include <vector>

class CSlrMusicFileMiniaudio : public CSlrMusicFileResampled
{
public:
	const char *GetActiveDecoderName() override { return "miniaudio"; }

	CSlrMusicFileMiniaudio(const char *filePath);
	virtual ~CSlrMusicFileMiniaudio();

	// True when this class can decode the extension. Extension-based, matching
	// the video sources' probes: opening a file to find out is work the playlist
	// does per row while browsing.
	static bool CanPlay(const char *filePath);

	bool Init(const char *filePath);

	virtual void Play() override;
	virtual void Pause() override;
	virtual void Stop() override;
	virtual void Rewind() override;
	virtual u64 GetCurrentSampleNum() override;
	virtual void SeekToSample(u64 sampleNum) override;
	virtual bool IsPlaying() override;
	virtual u64 GetLengthSamples() override;

	virtual void FillBuffer(int *mixBuffer, u32 numSamples) override;
	virtual void Mix(int *mixBuffer, u32 numSamples) override;

	virtual u32 ResourceDeactivate(bool async) override;
	virtual u32 ResourceActivate(bool async) override;
	virtual u32 ResourceGetSize() override;
	virtual const char *ResourceGetTypeName() override;



	bool IsOpen() const { return decoder != nullptr; }

protected:
	// The single decode call the base needs. Mono is widened here, before the
	// base ever sees it, so the resampler only handles stereo.
	u32 DecodeNativeStereo(s16 *out, u32 frames) override;

private:
	// Opaque so miniaudio.h stays out of every consumer's include graph -- it is
	// 96 000 lines and pulls in a great deal of platform surface.
	void *decoder = nullptr;

	std::string path;
	bool playing = false;
	bool endOfStream = false;
	u64 lengthFrames = 0;   // NATIVE frames


};

#endif

#endif // MT_CAP_MUSIC_PLAYER
