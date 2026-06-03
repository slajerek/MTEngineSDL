#ifndef _CVIDEOCHANNEL_H_
#define _CVIDEOCHANNEL_H_

#include "CAudioChannel.h"
#include <vector>
#include <atomic>
#include <cstdint>

class CVideoAudioChannel : public CAudioChannel
{
public:
	CVideoAudioChannel();
	virtual ~CVideoAudioChannel();

	// Called by CVideoPlayer decode thread -- push decoded Opus PCM (48kHz float interleaved)
	void PushSamples(const float *interleavedSamples, int numFrames, int numChannels);

	// Called by engine audio mixer -- S16 stereo interleaved (packed pairs in int*)
	virtual void MixIn(int *mixBuffer, u32 numSamples, int numAudioChannels) override;

	// Playback position for A/V sync (seconds)
	double GetPlaybackPosition();

	// Reset ring buffer (for seek)
	void Reset();

private:
	// Ring buffer stores stereo S16 samples at engine sample rate
	// Layout: [L0, R0, L1, R1, ...] as int16_t values
	std::vector<int16_t> ringBuffer;
	std::atomic<size_t> writePos{0};
	std::atomic<size_t> readPos{0};
	size_t ringBufferCapacity = 0;  // in int16_t samples (frames * 2)

	int engineSampleRate = 0;
	double resampleRatio = 1.0;
	double resamplePos = 0.0;
	bool needsResample = false;

	std::atomic<uint64_t> framesPlayed{0};
};

#endif
