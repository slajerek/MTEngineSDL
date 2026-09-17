#ifndef _CSLRMUSICFILEMFAUDIO_H_
#define _CSLRMUSICFILEMFAUDIO_H_

// The Windows twin of CSlrMusicFileAVFAudio: AAC and ALAC through Media
// Foundation, so a Microsoft Store build decodes them without an MP4 demuxer
// or an LGPL library. Read that header first; the reasoning is identical.
//
// NEVER COMPILED. Written on macOS. See the Windows handover notes.

#include "CSlrMusicFileResampled.h"

#if defined(_WIN32) && (!defined(MT_CAP_MUSIC_PLAYER) || MT_CAP_MUSIC_PLAYER)

#include <string>
#include <vector>

class CSlrMusicFileMFAudio : public CSlrMusicFileResampled
{
public:
	const char *GetActiveDecoderName() override { return "mediafoundation"; }

	CSlrMusicFileMFAudio(const char *filePath);
	virtual ~CSlrMusicFileMFAudio();

	static bool CanPlay(const char *filePath);
	bool Init(const char *filePath);
	bool IsOpen() const;

	virtual void Play() override;
	virtual void Pause() override;
	virtual void Stop() override;
	virtual void Rewind() override;
	virtual bool IsPlaying() override;

	virtual u64 GetCurrentSampleNum() override;
	virtual void SeekToSample(u64 sampleNum) override;
	virtual u64 GetLengthSamples() override;

	virtual void FillBuffer(int *mixBuffer, u32 numSamples) override;
	virtual void Mix(int *mixBuffer, u32 numSamples) override;

	virtual u32 ResourceDeactivate(bool async) override { return 0; }
	virtual u32 ResourceActivate(bool async) override   { return 0; }
	virtual u32 ResourceGetSize() override;
	virtual const char *ResourceGetTypeName() override  { return "MusicFileMFAudio"; }

protected:
	u32 DecodeNativeStereo(s16 *out, u32 frames) override;

private:
	struct SImpl;
	SImpl *impl = nullptr;
	std::string path;
	bool playing = false;
	u64 lengthFrames = 0;
	u64 cursorFrames = 0;
	std::vector<s16> carry;
	size_t carryPos = 0;
};

#endif
#endif
