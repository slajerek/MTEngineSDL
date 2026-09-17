#ifndef _CSLRMUSICFILERESAMPLED_H_
#define _CSLRMUSICFILERESAMPLED_H_

// Shared base for music files that decode at the FILE's rate and hand the
// mixer engine-rate frames.
//
// WHAT IT OWNS, and why it is worth a base class at all: the rate conversion
// and the native-frame arithmetic were duplicated between CSlrMusicFileOgg and
// CSlrMusicFileMiniaudio -- the resampler, the pending-output buffer, the
// refill loop, the clamp-and-convert, and six time accessors that are all the
// same division by the source rate. A second copy of that is a second place
// for the sample invariant to be got wrong, and it has been got wrong once
// already.
//
// THE SAMPLE INVARIANT, which is the whole reason this is centralised:
//
//     GetLengthSamples(), GetCurrentSampleNum() and SeekToSample() are in the
//     FILE's NATIVE PCM frames. Never engine frames.
//
// An app stores cue points as native-frame numbers. A 48 kHz file whose
// accessors reported engine-rate positions would move every cue in it, and
// that is exactly the defect that once made a whole soundtrack play 8.8% long
// and 1.45 semitones flat. Because every time accessor here divides by
// `sourceSampleRate`, a subclass cannot get this wrong by omission -- it would
// have to override one deliberately.
//
// WHAT A SUBCLASS PROVIDES: one decode call, plus the sample cursor. That is
// the entire surface. Everything else -- passthrough detection, the refill
// loop, float conversion, clamping, end-of-stream padding -- is here.

#include "CSlrMusicFile.h"
#include "MT_AudioResampler.h"
#include <vector>

class CSlrMusicFileResampled : public CSlrMusicFile
{
public:
	CSlrMusicFileResampled(const char *channelName) : CSlrMusicFile(channelName) {}
	virtual ~CSlrMusicFileResampled() {}

	// The file's own format, read from the container at open time and never
	// assumed. Before these existed the Ogg path simply took the engine rate on
	// faith, which is where the 1.45-semitone bug came from.
	u32 sourceSampleRate = 0;
	int sourceChannels = 0;

	virtual u32 GetSourceSampleRate() override { return sourceSampleRate; }
	virtual int GetSourceChannels() override   { return sourceChannels; }

	// --- the time accessors, all in NATIVE frames -------------------------
	// Expressed once, here, in terms of the two a subclass must implement.
	virtual void Seek(double second) override;
	virtual void Seek(u32 minute, u32 second) override;
	virtual void SeekToMillisecond(u64 millisecond) override;
	virtual u32 GetCurrentSecond() override;
	virtual u64 GetCurrentMillisecond() override;
	virtual u64 GetLengthSeconds() override;

	// Pull raw frames at the FILE's own rate, for analysis rather than
	// playback -- the waveform cache builds its envelope from these. Public
	// because the cache is not a subclass, and deliberately NOT FillBuffer():
	// that path is engine-rate and gain-applied, so an envelope built from it
	// would change shape with the volume slider.
	u32 ReadAnalysisFrames(s16 *out, u32 frames) { return DecodeNativeStereo(out, frames); }

protected:
	// THE ONE THING A SUBCLASS DECODES. Write up to `frames` NATIVE frames into
	// `out` as interleaved stereo s16 (mono must be widened by the subclass,
	// before this returns -- the resampler only ever sees stereo). Return the
	// number of real frames produced; 0 means the stream is exhausted.
	virtual u32 DecodeNativeStereo(s16 *out, u32 frames) = 0;

	// Rebuild the resampler for the current source rate and drop any half-
	// converted output. Call after open and after every seek: the resampler
	// carries a phase accumulator across calls, and leaving it in place over a
	// discontinuity smears the join.
	void ResetResampler();

	// The whole mixer-facing path: passthrough when the rates already match,
	// otherwise decode-and-convert until the block is full. Subclass
	// FillBuffer() implementations are one line -- a call to this.
	void FillResampled(int *mixBuffer, u32 numSamples);

	MT_AudioResampler resampler;

private:
	std::vector<s16>   nativeScratch;  // native frames, interleaved stereo s16
	std::vector<float> resampleIn;     // the same, as float
	std::vector<float> resampleOut;    // engine frames, stereo float
	std::vector<float> pendingOut;     // converted, not yet handed to the mixer
	u32 pendingFrames = 0;
	u32 pendingPos = 0;
	bool exhausted = false;
};

#endif
