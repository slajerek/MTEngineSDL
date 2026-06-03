#include "CVideoAudioChannel.h"
#include "SND_SoundEngine.h"
#include "SYS_Main.h"
#include <algorithm>
#include <cmath>
#include <cstring>

CVideoAudioChannel::CVideoAudioChannel()
: CAudioChannel("VideoAudio")
{
	engineSampleRate = SOUND_SAMPLE_RATE;
	resampleRatio = 48000.0 / (double)engineSampleRate;
	needsResample = (engineSampleRate != 48000);

	// 2 seconds of stereo audio at engine sample rate
	ringBufferCapacity = (size_t)engineSampleRate * 2 * 2;
	ringBuffer.resize(ringBufferCapacity, 0);

	resamplePos = 0.0;
}

CVideoAudioChannel::~CVideoAudioChannel()
{
}

// Clamp float to [-1.0, 1.0] range and convert to S16
static inline int16_t FloatToS16(float sample)
{
	float clamped = sample;
	if (clamped > 1.0f) clamped = 1.0f;
	if (clamped < -1.0f) clamped = -1.0f;
	return (int16_t)(clamped * 32767.0f);
}

void CVideoAudioChannel::PushSamples(const float *interleavedSamples, int numFrames, int numChannels)
{
	size_t wp = writePos.load(std::memory_order_relaxed);
	size_t rp = readPos.load(std::memory_order_acquire);

	// Helper: get a stereo frame from the source buffer (handles mono -> stereo expansion)
	auto getStereoFrame = [&](int frameIdx, float &outL, float &outR)
	{
		if (numChannels >= 2)
		{
			outL = interleavedSamples[frameIdx * numChannels];
			outR = interleavedSamples[frameIdx * numChannels + 1];
		}
		else
		{
			// Mono: duplicate to both channels
			outL = interleavedSamples[frameIdx];
			outR = interleavedSamples[frameIdx];
		}
	};

	// Helper: check if ring buffer has space for one stereo sample pair (2 int16_t values)
	auto hasSpace = [&]() -> bool
	{
		size_t used = wp - rp;  // works correctly with unsigned wraparound
		return used + 2 <= ringBufferCapacity;
	};

	// Helper: write one stereo S16 frame to ring buffer
	auto writeStereoFrame = [&](int16_t left, int16_t right)
	{
		ringBuffer[wp % ringBufferCapacity] = left;
		ringBuffer[(wp + 1) % ringBufferCapacity] = right;
		wp += 2;
	};

	if (!needsResample)
	{
		// Passthrough: source is already at engine sample rate (48kHz == 48kHz)
		for (int i = 0; i < numFrames; i++)
		{
			if (!hasSpace())
			{
				// Buffer full, drop remaining samples
				break;
			}

			float fL, fR;
			getStereoFrame(i, fL, fR);
			writeStereoFrame(FloatToS16(fL), FloatToS16(fR));
		}
	}
	else
	{
		// Resample 48kHz -> engineSampleRate using linear interpolation
		// resampleRatio = 48000 / engineSampleRate (e.g., ~1.0884 for 44100)
		// For each output sample, we advance resamplePos by resampleRatio in the source

		while (resamplePos < (double)(numFrames - 1))
		{
			if (!hasSpace())
			{
				// Buffer full, drop remaining samples
				break;
			}

			int idx0 = (int)resamplePos;
			int idx1 = idx0 + 1;
			double frac = resamplePos - (double)idx0;

			float l0, r0, l1, r1;
			getStereoFrame(idx0, l0, r0);
			getStereoFrame(idx1, l1, r1);

			// Linear interpolation
			float interpL = l0 + (float)frac * (l1 - l0);
			float interpR = r0 + (float)frac * (r1 - r0);

			writeStereoFrame(FloatToS16(interpL), FloatToS16(interpR));

			resamplePos += resampleRatio;
		}

		// Adjust resamplePos for next call: subtract consumed frames
		resamplePos -= (double)numFrames;
		if (resamplePos < 0.0)
			resamplePos = 0.0;
	}

	writePos.store(wp, std::memory_order_release);
}

void CVideoAudioChannel::MixIn(int *mixBuffer, u32 numSamples, int numAudioChannels)
{
	if (isMuted || bypass)
		return;

	// The engine packs two i16 values per int (stereo pair: left + right)
	// numSamples = number of stereo frames
	i16 *outPtr = (i16 *)mixBuffer;

	size_t rp = readPos.load(std::memory_order_relaxed);
	size_t wp = writePos.load(std::memory_order_acquire);

	u32 framesRead = 0;

	if (volume == 1.0f)
	{
		for (u32 i = 0; i < numSamples; i++)
		{
			// Need 2 int16_t values (one stereo frame) available
			if (rp + 2 > wp)
			{
				// Buffer underrun: leave remaining output as-is (already zeroed or has other channels)
				break;
			}

			i16 left  = ringBuffer[rp % ringBufferCapacity];
			i16 right = ringBuffer[(rp + 1) % ringBufferCapacity];
			rp += 2;

			// Add to mix buffer (same pattern as base CAudioChannel::MixIn)
			int sL = (int)left + (int)(*outPtr);
			int sR = (int)right + (int)(*(outPtr + 1));
			*outPtr = (i16)sL; outPtr++;
			*outPtr = (i16)sR; outPtr++;

			framesRead++;
		}
	}
	else
	{
		for (u32 i = 0; i < numSamples; i++)
		{
			if (rp + 2 > wp)
			{
				break;
			}

			i16 left  = ringBuffer[rp % ringBufferCapacity];
			i16 right = ringBuffer[(rp + 1) % ringBufferCapacity];
			rp += 2;

			i16 mL = (i16)((float)left * volume);
			i16 mR = (i16)((float)right * volume);

			int sL = (int)mL + (int)(*outPtr);
			int sR = (int)mR + (int)(*(outPtr + 1));
			*outPtr = (i16)sL; outPtr++;
			*outPtr = (i16)sR; outPtr++;

			framesRead++;
		}
	}

	readPos.store(rp, std::memory_order_release);
	framesPlayed.fetch_add(framesRead, std::memory_order_relaxed);
}

double CVideoAudioChannel::GetPlaybackPosition()
{
	return (double)framesPlayed.load(std::memory_order_relaxed) / (double)engineSampleRate;
}

void CVideoAudioChannel::Reset()
{
	writePos.store(0, std::memory_order_release);
	readPos.store(0, std::memory_order_release);
	framesPlayed.store(0, std::memory_order_release);
	resamplePos = 0.0;
}
