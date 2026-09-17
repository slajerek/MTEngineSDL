#include "CSlrMusicFileResampled.h"
#include "SND_SoundEngine.h"
#include "DBG_Log.h"
#include <cmath>
#include <cstring>

void CSlrMusicFileResampled::ResetResampler()
{
	// Cubic for every subclass: which decoder opened the file must not change
	// how it sounds. The engine rate is asked for ONCE, from the one place
	// that owns it (SND_GetEngineSampleRate) rather than a SOUND_SAMPLE_RATE
	// constant duplicated per call site.
	resampler.Reset((int)sourceSampleRate, (int)SND_GetEngineSampleRate(),
					EAudioInterpolation::Cubic);
	pendingFrames = 0;
	pendingPos = 0;
	exhausted = false;
}

void CSlrMusicFileResampled::FillResampled(int *mixBuffer, u32 numSamples)
{
	signed short *outBuf = (signed short *)mixBuffer;

	// PASSTHROUGH. The file already matches the engine, so the resampler is not
	// in the path at all and the decoder writes straight into the mixer's
	// buffer. Most 44.1 kHz content lands here, and keeping it a distinct path
	// is what makes this refactor safe: the common case stays byte-for-byte
	// what it was before any of this existed.
	if (resampler.IsPassthrough())
	{
		u32 got = DecodeNativeStereo(outBuf, numSamples);
		for (u32 i = got * 2; i < numSamples * 2; i++)
			outBuf[i] = 0;
		return;
	}

	const double ratio = (double)sourceSampleRate / (double)SND_GetEngineSampleRate();

	u32 written = 0;
	int refills = 0;
	while (written < numSamples)
	{
		// Hand over whatever the resampler already produced.
		while (written < numSamples && pendingPos < pendingFrames)
		{
			float l = pendingOut[(size_t)pendingPos * 2 + 0];
			float r = pendingOut[(size_t)pendingPos * 2 + 1];
			if (l < -1.0f) l = -1.0f; else if (l > 1.0f) l = 1.0f;
			if (r < -1.0f) r = -1.0f; else if (r > 1.0f) r = 1.0f;
			outBuf[(size_t)written * 2 + 0] = (signed short)(l * 32767.0f);
			outBuf[(size_t)written * 2 + 1] = (signed short)(r * 32767.0f);
			pendingPos++;
			written++;
		}
		if (written >= numSamples)
			break;

		// A stream that has ended still has to fill the block; without this the
		// loop would spin on a decoder that can no longer produce anything. The
		// refill cap is the same belt-and-braces guard the Ogg path always had,
		// for a decoder that returns frames but never enough of them.
		if (exhausted || ++refills > 8)
		{
			for (u32 i = written; i < numSamples; i++)
			{
				outBuf[(size_t)i * 2 + 0] = 0;
				outBuf[(size_t)i * 2 + 1] = 0;
			}
			return;
		}

		// Decode enough NATIVE frames for the engine frames still owed, plus
		// the resampler's interpolation lookahead.
		u32 needEngine = numSamples - written;
		u32 needNative = (u32)((double)needEngine * ratio) + 16;

		if (nativeScratch.size() < (size_t)needNative * 2)
			nativeScratch.resize((size_t)needNative * 2);
		u32 decoded = DecodeNativeStereo(nativeScratch.data(), needNative);
		if (decoded == 0)
		{
			exhausted = true;
			continue;   // the guard above fills the remainder and returns
		}

		if (resampleIn.size() < (size_t)decoded * 2)
			resampleIn.resize((size_t)decoded * 2);
		for (u32 i = 0; i < decoded * 2; i++)
			resampleIn[i] = (float)nativeScratch[i] / 32768.0f;

		u32 cap = resampler.EstimateOutputFrames(decoded);
		if (resampleOut.size() < (size_t)cap * 2)
			resampleOut.resize((size_t)cap * 2);

		u32 got = resampler.Process(resampleIn.data(), decoded,
									resampleOut.data(), cap, NULL);

		if (pendingOut.size() < (size_t)got * 2)
			pendingOut.resize((size_t)got * 2);
		for (u32 i = 0; i < got * 2; i++)
			pendingOut[i] = resampleOut[i];
		pendingFrames = got;
		pendingPos = 0;
	}
}

// ============================================================================
// Time, in NATIVE frames.
//
// Written once here so a subclass cannot get the invariant wrong by omission.
// Each is the obvious division -- the value of centralising them is not the
// arithmetic, it is that `sourceSampleRate` appears instead of the engine rate,
// in every one of them, permanently.
// ============================================================================

void CSlrMusicFileResampled::Seek(double second)
{
	if (second < 0.0) second = 0.0;
	SeekToSample((u64)(second * (double)sourceSampleRate));
}

void CSlrMusicFileResampled::Seek(u32 minute, u32 second)
{
	Seek((double)minute * 60.0 + (double)second);
}

void CSlrMusicFileResampled::SeekToMillisecond(u64 millisecond)
{
	SeekToSample((u64)((double)millisecond * (double)sourceSampleRate / 1000.0));
}

u32 CSlrMusicFileResampled::GetCurrentSecond()
{
	if (sourceSampleRate == 0) return 0;
	return (u32)(GetCurrentSampleNum() / (u64)sourceSampleRate);
}

u64 CSlrMusicFileResampled::GetCurrentMillisecond()
{
	if (sourceSampleRate == 0) return 0;
	return (u64)((double)GetCurrentSampleNum() * 1000.0 / (double)sourceSampleRate);
}

u64 CSlrMusicFileResampled::GetLengthSeconds()
{
	if (sourceSampleRate == 0) return 0;
	return GetLengthSamples() / (u64)sourceSampleRate;
}
