#ifndef _CSLRMUSICFILEAVFAUDIO_H_
#define _CSLRMUSICFILEAVFAUDIO_H_

// AAC, ALAC and anything else AVFoundation decodes -- .m4a, .aac, .caf, .aiff --
// through the OS, on macOS.
//
// WHY THIS INSTEAD OF AN MP4 DEMUXER. AAC and ALAC both arrive inside MP4/M4A,
// and the plan of record (tracker 4.3) was to vendor or write an MP4 demuxer so
// their frames could be handed to CAudioDecoderAACNative -- 400 to 600 lines
// that must emit an AAC `esds` AudioSpecificConfig and an ALAC magic cookie,
// which are different structures. Measured instead: AVFoundation already
// demuxes AND decodes both to LPCM, so on macOS that whole item is unnecessary.
// The OS holds the codec licences, which is the same reason the video path
// routes through it.
//
// The Windows twin is CSlrMusicFileMFAudio; CSlrMusicFileNative.h picks between
// them, following the alias-header seam CAudioDecoderAACNative.h already
// established for the packet decoders. LINUX HAS NEITHER -- there is no OS
// media API to route to, so .m4a stays unsupported there rather than pretending.
//
// THE SAMPLE INVARIANT is the base's, and this class keeps its side of it: the
// reader is configured for the file's OWN sample rate, never the engine's, so
// GetLengthSamples/GetCurrentSampleNum/SeekToSample stay in native frames.

#include "CSlrMusicFileResampled.h"

#if defined(__APPLE__) && (!defined(MT_CAP_MUSIC_PLAYER) || MT_CAP_MUSIC_PLAYER)

#include <string>
#include <vector>

class CSlrMusicFileAVFAudio : public CSlrMusicFileResampled
{
public:
	const char *GetActiveDecoderName() override { return "avfoundation"; }

	CSlrMusicFileAVFAudio(const char *filePath);
	virtual ~CSlrMusicFileAVFAudio();

	// The containers AVFoundation decodes that the other music sources do not.
	// .mp3/.flac/.wav are DELIBERATELY absent even though AVFoundation reads
	// them: miniaudio already owns those, it behaves identically on all three
	// platforms, and two decoders competing for one extension is how a working
	// path gets silently replaced.
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
	virtual const char *ResourceGetTypeName() override  { return "MusicFileAVFAudio"; }

protected:
	u32 DecodeNativeStereo(s16 *out, u32 frames) override;

private:
	struct SImpl;
	SImpl *impl = nullptr;

	std::string path;
	bool playing = false;
	u64 lengthFrames = 0;    // NATIVE frames
	u64 cursorFrames = 0;    // NATIVE frames handed out so far

	// AVAssetReader delivers whole CMSampleBuffers; whatever a Decode call does
	// not consume is kept here for the next one rather than dropped.
	std::vector<s16> carry;
	size_t carryPos = 0;

	bool BuildReader(double startSeconds);
	void TeardownReader();
};

#endif // __APPLE__ && MT_CAP_MUSIC_PLAYER
#endif
