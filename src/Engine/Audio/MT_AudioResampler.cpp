#include "MT_AudioResampler.h"
#include "DBG_Log.h"
#include <cmath>
#include <cstring>

MT_AudioResampler::MT_AudioResampler()
{
	srcRate = 0;
	dstRate = 0;
	ratio = 1.0;
	passthrough = true;
	quality = EAudioInterpolation::Cubic;
	posInt = 0;
	posFrac = 0.0;
	historyFrames = 0;
	memset(history, 0, sizeof(history));
}

void MT_AudioResampler::Reset(int newSrcRate, int newDstRate, EAudioInterpolation q)
{
	srcRate = newSrcRate > 0 ? newSrcRate : 0;
	dstRate = newDstRate > 0 ? newDstRate : 0;
	quality = q;

	passthrough = (srcRate == dstRate) || srcRate <= 0 || dstRate <= 0;
	ratio = (srcRate > 0 && dstRate > 0) ? ((double)srcRate / (double)dstRate) : 1.0;

	// Silent history in front of the first real frame, so the interpolator has
	// a full window from the very first output sample and the stream does not
	// begin with a discontinuity.
	historyFrames = kLookBehind;
	memset(history, 0, sizeof(history));
	posInt = kLookBehind;
	posFrac = 0.0;
}

u32 MT_AudioResampler::EstimateOutputFrames(u32 srcFrames) const
{
	if (passthrough)
		return srcFrames;
	if (ratio <= 0.0)
		return srcFrames;
	return (u32)((double)srcFrames / ratio) + 8;
}

float MT_AudioResampler::Interpolate(const float *s, int i, double f, int c) const
{
	const int st = 2;   // stereo stride
	switch (quality)
	{
		case EAudioInterpolation::Nearest:
			return s[(size_t)(f < 0.5 ? i : i + 1) * st + c];

		case EAudioInterpolation::Linear:
		{
			float a = s[(size_t)i * st + c];
			float b = s[(size_t)(i + 1) * st + c];
			return a + (float)f * (b - a);
		}

		case EAudioInterpolation::Sinc:
		{
			// 8-tap Hann-windowed sinc, taps at i-3 .. i+4.
			double acc = 0.0;
			double norm = 0.0;
			for (int k = -3; k <= 4; k++)
			{
				double x = (double)k - f;
				double w;
				if (fabs(x) < 1e-9)
				{
					w = 1.0;
				}
				else
				{
					double px = M_PI * x;
					// Hann window over the 8-tap span.
					double win = 0.5 + 0.5 * cos(M_PI * x / 4.5);
					w = (sin(px) / px) * win;
				}
				acc  += w * (double)s[(size_t)(i + k) * st + c];
				norm += w;
			}
			// Normalising keeps unity gain on a DC signal; without it the
			// window's truncation shows up as a level wobble that tracks the
			// fractional phase.
			return (float)(norm != 0.0 ? acc / norm : acc);
		}

		case EAudioInterpolation::Cubic:
		default:
		{
			// Catmull-Rom, taps at i-1 .. i+2.
			double p0 = s[(size_t)(i - 1) * st + c];
			double p1 = s[(size_t)i * st + c];
			double p2 = s[(size_t)(i + 1) * st + c];
			double p3 = s[(size_t)(i + 2) * st + c];
			double a0 = -0.5 * p0 + 1.5 * p1 - 1.5 * p2 + 0.5 * p3;
			double a1 =        p0 - 2.5 * p1 + 2.0 * p2 - 0.5 * p3;
			double a2 = -0.5 * p0            + 0.5 * p2;
			return (float)(((a0 * f + a1) * f + a2) * f + p1);
		}
	}
}

u32 MT_AudioResampler::Process(const float *src, u32 srcFrames, float *dst,
                               u32 maxDstFrames, u32 *outSrcConsumed)
{
	if (outSrcConsumed) *outSrcConsumed = 0;
	if (src == NULL || dst == NULL || srcFrames == 0 || maxDstFrames == 0)
		return 0;

	if (passthrough)
	{
		u32 n = srcFrames < maxDstFrames ? srcFrames : maxDstFrames;
		memcpy(dst, src, (size_t)n * 2 * sizeof(float));
		if (outSrcConsumed) *outSrcConsumed = n;
		return n;
	}

	// scratch = carried history, then this call's input. Positions below are in
	// scratch frames, which is why readPos survives calls unchanged except for
	// the rebase at the end.
	const size_t total = (size_t)historyFrames + (size_t)srcFrames;
	scratch.resize(total * 2);
	if (historyFrames > 0)
		memcpy(scratch.data(), history, (size_t)historyFrames * 2 * sizeof(float));
	memcpy(scratch.data() + (size_t)historyFrames * 2, src, (size_t)srcFrames * 2 * sizeof(float));

	// The last frame index whose interpolation window is fully inside scratch.
	const int maxIndex = (int)total - 1 - kLookAhead;

	u32 written = 0;
	while (written < maxDstFrames && posInt <= maxIndex)
	{
		dst[(size_t)written * 2 + 0] = Interpolate(scratch.data(), posInt, posFrac, 0);
		dst[(size_t)written * 2 + 1] = Interpolate(scratch.data(), posInt, posFrac, 1);
		written++;

		posFrac += ratio;
		while (posFrac >= 1.0)
		{
			posFrac -= 1.0;
			posInt++;
		}
	}

	// Retain the window behind the next read position, plus everything ahead of
	// it that has not been converted yet. The caller is told it consumed the
	// whole block: what is unconverted lives here, not in the caller.
	int keepStart = posInt - kLookBehind;
	if (keepStart < 0) keepStart = 0;
	int keep = (int)total - keepStart;
	if (keep < 0) keep = 0;

	if (keep > kMaxHistoryFrames)
	{
		// Only reachable if the caller starves the output (maxDstFrames far
		// smaller than the block), which would silently drop audio. Say so.
		LOGWarning("MT_AudioResampler: history overflow (%d > %d) -- dropping %d frames; "
		           "the destination buffer is too small for this input block",
		           keep, kMaxHistoryFrames, keep - kMaxHistoryFrames);
		keepStart = (int)total - kMaxHistoryFrames;
		keep = kMaxHistoryFrames;
	}

	memcpy(history, scratch.data() + (size_t)keepStart * 2, (size_t)keep * 2 * sizeof(float));
	historyFrames = keep;
	posInt -= keepStart;   // integer-only rebase: posFrac is untouched, so the
	                       // next block's arithmetic is bit-identical to the
	                       // arithmetic an unchunked stream would perform.

	if (outSrcConsumed) *outSrcConsumed = srcFrames;
	return written;
}
