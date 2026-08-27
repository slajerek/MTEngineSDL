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

	// 2 seconds of stereo audio at engine sample rate
	ringBufferCapacity = (size_t)engineSampleRate * 2 * 2;
	ringBuffer.resize(ringBufferCapacity, 0);

	ResetResampleState();
}

CVideoAudioChannel::~CVideoAudioChannel()
{
}

void CVideoAudioChannel::ResetResampleState()
{
	lastSourceSampleRate = 0;   // force ratio rebuild on next push
	resampleRatio = 1.0;
	resamplePos = 0.0;
	needsResample = false;
	scratchL.assign(kHistoryFrames, 0.0f);
	scratchR.assign(kHistoryFrames, 0.0f);
}

// Clamp float to [-1.0, 1.0] range and convert to S16
static inline int16_t FloatToS16(float sample)
{
	float clamped = sample;
	if (clamped > 1.0f) clamped = 1.0f;
	if (clamped < -1.0f) clamped = -1.0f;
	return (int16_t)(clamped * 32767.0f);
}

// Interpolators. `s` is a scratch array, `i0` the integer source index, `frac`
// in [0,1). All taps are guaranteed in-bounds by the loop condition + the
// kHistoryFrames prefix.
static inline float InterpNearest(const float *s, int i0, double frac)
{
	return s[frac < 0.5 ? i0 : i0 + 1];
}
static inline float InterpLinear(const float *s, int i0, double frac)
{
	return s[i0] + (float)frac * (s[i0 + 1] - s[i0]);
}

static inline float InterpCubic(const float *s, int i0, double frac)
{
	// 4-point Catmull-Rom over taps i0-1 .. i0+2.
	float p0 = s[i0 - 1], p1 = s[i0], p2 = s[i0 + 1], p3 = s[i0 + 2];
	float t = (float)frac;
	return p1 + 0.5f * t * (p2 - p0
	         + t * (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3
	         + t * (3.0f * (p1 - p2) + p3 - p0)));
}

// 8-tap Hann-windowed sinc over taps i0-3 .. i0+4, normalized so DC passes at
// unity gain -- PRECOMPUTED as a polyphase table rather than evaluated per
// sample.
//
// The kernel depends only on `frac`, so computing it per output sample meant
// eight sin() and eight cos() calls for every sample of every channel: at 48 kHz
// stereo, about 1.5 million transcendental calls a second, all of them
// recomputing the same few hundred distinct answers. Measured on this machine:
//
//     shipped (sin/cos per sample)   62.16 ns/sample
//     512 phases, blended             4.56 ns/sample     13.6x faster
//
// The table quantises `frac` into kSincPhases positions and interpolates
// linearly BETWEEN two adjacent phase rows. That blend is what makes the
// approximation free rather than a trade: measured against the exact kernel
// over a 440 Hz - 19 kHz test signal at 44.1k -> 48k,
//
//     256 phases, nearest    -59.7 dB      512 phases, nearest    -65.7 dB
//     256 phases, blended   -111.3 dB      512 phases, blended   -123.2 dB
//
// and the samples are converted to S16 two lines later, whose own quantisation
// floor is -96 dB. So the blended table is below the noise floor of the format
// it feeds, while nearest-phase would have been ABOVE it and audibly worse than
// what this function is for. The extra row (kSincPhases + 1) exists so the
// blend never reads off the end.
//
// The normalisation is folded in at build time, which also removes the
// per-sample division the old code did.
enum { kSincPhases = 512, kSincTaps = 8 };

struct SSincTable
{
	float w[kSincPhases + 1][kSincTaps];
	SSincTable()
	{
		for (int p = 0; p <= kSincPhases; p++)
		{
			const double frac = (double)p / (double)kSincPhases;
			double tap[kSincTaps], wsum = 0.0;
			for (int k = -3; k <= 4; k++)
			{
				const double x = (double)k - frac;
				const double sinc = (x == 0.0) ? 1.0 : sin(M_PI * x) / (M_PI * x);
				const double win  = 0.5 * (1.0 + cos(M_PI * x / 4.0));  // Hann over |x| <= 4
				tap[k + 3] = sinc * win;
				wsum += tap[k + 3];
			}
			for (int k = 0; k < kSincTaps; k++)
				w[p][k] = (float)(wsum != 0.0 ? tap[k] / wsum
				                              : (k == 3 ? 1.0 : 0.0));
		}
	}
};

static inline const SSincTable &SincTable()
{
	// Function-local static: built once, on first use, and thread-safe
	// initialisation is guaranteed by the language. 16 KB.
	static const SSincTable table;
	return table;
}

static inline float InterpSinc8(const float *s, int i0, double frac)
{
	const SSincTable &t = SincTable();
	const double fp = frac * (double)kSincPhases;
	const int    p  = (int)fp;                    // frac in [0,1) -> p in [0,511]
	const float  a  = (float)(fp - (double)p);
	const float *w0 = t.w[p];
	const float *w1 = t.w[p + 1];
	const float *x  = s + i0 - 3;

	float acc = 0.0f;
	for (int k = 0; k < kSincTaps; k++)
		acc += (w0[k] + a * (w1[k] - w0[k])) * x[k];
	return acc;
}

void CVideoAudioChannel::PushSamples(const float *interleavedSamples, int numFrames,
                                     int numChannels, int sourceSampleRate)
{
	if (!interleavedSamples || numFrames <= 0 || numChannels <= 0)
		return;
	if (sourceSampleRate <= 0)
		sourceSampleRate = 48000;   // historic assumption

	if (sourceSampleRate != lastSourceSampleRate)
	{
		lastSourceSampleRate = sourceSampleRate;
		resampleRatio = (double)sourceSampleRate / (double)engineSampleRate;
		needsResample = (sourceSampleRate != engineSampleRate);
		resamplePos = 0.0;
		std::fill(scratchL.begin(), scratchL.begin() + kHistoryFrames, 0.0f);
		std::fill(scratchR.begin(), scratchR.begin() + kHistoryFrames, 0.0f);
	}

	// Same thread as Reset(), so the pair cannot be torn here; both are absolute
	// (re-based, never zeroed) counters -- `used` is unaffected by the base.
	uint64_t wp = writePos.load(std::memory_order_relaxed);
	uint64_t rp = readPos.load(std::memory_order_acquire);

	auto hasSpace = [&]() -> bool
	{
		uint64_t used = wp - rp;  // rp <= wp always (the mixer never overruns)
		return used + 2 <= ringBufferCapacity;
	};
	auto writeStereoFrame = [&](int16_t left, int16_t right)
	{
		ringBuffer[wp % ringBufferCapacity] = left;
		ringBuffer[(wp + 1) % ringBufferCapacity] = right;
		wp += 2;
	};
	auto srcL = [&](int frameIdx) -> float
	{
		return interleavedSamples[frameIdx * numChannels];
	};
	auto srcR = [&](int frameIdx) -> float
	{
		return numChannels >= 2 ? interleavedSamples[frameIdx * numChannels + 1]
		                        : interleavedSamples[frameIdx];  // mono -> both
	};

	if (!needsResample)
	{
		// Passthrough: source already at engine sample rate.
		for (int i = 0; i < numFrames; i++)
		{
			if (!hasSpace())
				break;  // buffer full, drop remaining samples
			writeStereoFrame(FloatToS16(srcL(i)), FloatToS16(srcR(i)));
		}
		writePos.store(wp, std::memory_order_release);
		return;
	}

	// Assemble [kHistoryFrames of carried history | this block] so every
	// interpolator tap is in-bounds even across block boundaries.
	scratchL.resize(kHistoryFrames + numFrames);
	scratchR.resize(kHistoryFrames + numFrames);
	for (int i = 0; i < numFrames; i++)
	{
		scratchL[kHistoryFrames + i] = srcL(i);
		scratchR[kHistoryFrames + i] = srcR(i);
	}

	const EVideoAudioInterpolation mode =
	    (EVideoAudioInterpolation)interpolation.load(std::memory_order_relaxed);
	// Widest right tap per mode: Nearest/Linear need i0+1, Cubic i0+2, Sinc i0+4.
	const int rightTaps = (mode == EVideoAudioInterpolation::Sinc)  ? 4
	                    : (mode == EVideoAudioInterpolation::Cubic) ? 2 : 1;
	const int lastIdx = kHistoryFrames + numFrames - 1;

	// s = continuous read position in scratch frame-index space.
	double s = (double)kHistoryFrames + resamplePos;
	while (true)
	{
		int i0 = (int)s;                 // s >= kHistoryFrames - 3 > 0 always
		double frac = s - (double)i0;
		if (i0 + rightTaps > lastIdx)
			break;                        // not enough right context yet -- carry
		if (!hasSpace())
			break;                        // buffer full, drop remaining samples

		float outL, outR;
		switch (mode)
		{
			case EVideoAudioInterpolation::Nearest:
				outL = InterpNearest(scratchL.data(), i0, frac);
				outR = InterpNearest(scratchR.data(), i0, frac);
				break;
			case EVideoAudioInterpolation::Cubic:
				outL = InterpCubic(scratchL.data(), i0, frac);
				outR = InterpCubic(scratchR.data(), i0, frac);
				break;
			case EVideoAudioInterpolation::Sinc:
				outL = InterpSinc8(scratchL.data(), i0, frac);
				outR = InterpSinc8(scratchR.data(), i0, frac);
				break;
			default:
				outL = InterpLinear(scratchL.data(), i0, frac);
				outR = InterpLinear(scratchR.data(), i0, frac);
				break;
		}
		writeStereoFrame(FloatToS16(outL), FloatToS16(outR));
		s += resampleRatio;
	}

	// Carry the last kHistoryFrames frames + the fractional position into
	// the next block. Only the right-context-exhausted exit bounds this by
	// -(rightTaps+1) > -6, which the 8-frame history serves (worst-case left
	// tap: Sinc needs i0-3 >= 0). The buffer-full exit instead drops the
	// rest of the block and can leave resamplePos arbitrarily negative --
	// clamp to the deepest position the history can serve.
	resamplePos = s - (double)(kHistoryFrames + numFrames);
	const double kMinCarry = -(double)(kHistoryFrames - 3);
	if (resamplePos < kMinCarry)
		resamplePos = kMinCarry;
	for (int i = 0; i < kHistoryFrames; i++)
	{
		scratchL[i] = scratchL[numFrames + i];
		scratchR[i] = scratchR[numFrames + i];
	}
	scratchL.resize(kHistoryFrames);
	scratchR.resize(kHistoryFrames);

	writePos.store(wp, std::memory_order_release);
}

// MIXER THREAD. Read-compute-store against a ring whose positions are
// individually atomic but NOT jointly: a decode-thread Reset() (seek service)
// re-bases epochBase/writePos/readPos as three separate stores and can land
// anywhere inside this function. So the final advancement is a CAS against the
// readPos this call READ AT ENTRY -- if a Reset() intervened, the CAS fails and
// the quantum is ABANDONED (see the tail of the function). The A/V clock is
// derived from readPos, so an abandoned quantum is automatically never counted.
//
// The CAS is trustworthy only because Reset() re-bases the counters FORWARD
// instead of zeroing them: no position value is ever revisited, so a stale
// rpEntry cannot compare equal to a live readPos (see the header).
void CVideoAudioChannel::MixIn(int *mixBuffer, u32 numSamples, int numAudioChannels)
{
	if (bypass)
		return;

	// SILENT-DRAIN gate. Two independent reasons to contribute silence:
	//  - isMuted: the host's (sticky) mute.
	//  - seekPending: a seek is in the player's mailbox but the decode thread
	//    has not serviced it yet, so the ring still holds PRE-seek PCM. The user
	//    has already left that position; it must not be heard.
	// Both still CONSUME the ring: readPos -- and so the A/V clock behind
	// GetPlaybackPosition() -- keeps advancing, so CVideoPlayer::Update() keeps
	// displaying frames while muted, and an unmute (or a gate drop) never
	// replays a stale ring backlog. Only the mix-in is skipped.
	const bool silent = isMuted || seekPending.load(std::memory_order_acquire);

	// readPos ACQUIRE (not relaxed), and FIRST: if we observe Reset()'s re-based
	// readPos then its earlier writePos store must be visible to the writePos load
	// below -- otherwise we could pair a fresh readPos with a stale writePos.
	const uint64_t rpEntry = readPos.load(std::memory_order_acquire);
	const uint64_t wp      = writePos.load(std::memory_order_acquire);

	// A Reset() in flight is the only way to see an INCONSISTENT pair here (it
	// re-bases writePos before readPos, so we can hold an old-epoch rpEntry
	// against a re-based wp). Both tells are checked, and both mean "consume
	// nothing": PushSamples() guarantees used <= ringBufferCapacity in every
	// settled state, so anything else is a torn read -- and draining on it would
	// pull a whole quantum of garbage out of a ring that is being re-based under
	// us. (The CAS below would reject the quantum anyway; this just avoids doing
	// the work, and avoids MIXING that garbage on the audible path.)
	//
	// NOT strict less-than: hasSpace() gates each WRITE on used_before + 2 <=
	// capacity, so the last accepted write can carry a legitimately full ring
	// from used == capacity - 2 to used == capacity exactly (PushAndDrain-style
	// callers that push far more than 2s before ever draining hit this for
	// real, no Reset() involved). A strict `<` here would report framesAvail
	// == 0 for that real, common, non-torn full-ring state and never recover
	// (nothing but a MixIn() drain changes `used`), which is a worse and more
	// reachable bug than the torn read this guard is defending against -- a
	// torn used == capacity (old_used == 0 at Reset()) is already handled
	// correctly by the CAS below rejecting the stale rpEntry regardless of
	// what this guard decides, per the comment above.
	const uint64_t used = (wp > rpEntry) ? (wp - rpEntry) : 0;
	const u32 framesAvail = (used <= (uint64_t)ringBufferCapacity) ? (u32)(used / 2) : 0;
	const u32 framesToRead = framesAvail < numSamples ? framesAvail : numSamples;

	u32 framesRead = 0;

	if (silent)
	{
		framesRead = framesToRead;   // drain, contribute nothing
	}
	else
	{
		// The engine packs two i16 values per int (stereo pair: left + right);
		// numSamples = number of stereo frames.
		i16 *outPtr = (i16 *)mixBuffer;
		uint64_t rp = rpEntry;

		if (volume == 1.0f)
		{
			for (u32 i = 0; i < framesToRead; i++)
			{
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
			for (u32 i = 0; i < framesToRead; i++)
			{
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
	}

	if (framesRead == 0)
		return;   // underrun (or a Reset() in flight): nothing consumed, nothing to publish

	// TEST-ONLY (null in production): the window a Reset() has to race is exactly
	// from the entry load above to the CAS below, and it is a handful of
	// instructions wide on the silent path. See SetDebugMixInPreCasHook() --
	// unlike the setter, this call site is NOT PC_TEST_STAT_COUNTER-guarded
	// (see the member comment in the header for why: it is a single relaxed
	// load of a member that always exists, at negligible cost).
	void (*hook)(void *) = debugPreCasHook.load(std::memory_order_acquire);
	if (hook)
		hook(debugPreCasHookArg.load(std::memory_order_acquire));

	// Publish the consumption. If a Reset() re-based readPos since our entry load,
	// the CAS fails and we ABANDON this quantum: readPos stays at the new epoch's
	// base, so neither the ring nor the (derived) A/V clock counts a quantum that
	// the seek discarded. (A blind store here is the bug this replaces: it would
	// have left readPos from the OLD epoch standing against a re-based writePos,
	// i.e. readPos > writePos -- a ring whose "used" count is an unsigned
	// underflow, which makes PushSamples() drop every sample forever.)
	//
	// The CAS can only be relied on because Reset() never revisits a position
	// value (header): with a zeroing Reset(), an rpEntry of 0 -- the state of
	// EVERY epoch during its first consuming quantum -- would compare equal to the
	// next epoch's fresh 0 and the CAS would succeed across the Reset().
	//
	// Note the non-silent branch above has already summed mixed audio into
	// mixBuffer by this point, so a failed CAS there leaves that audio AUDIBLE.
	// It is bounded to ONE already-mixed quantum (~5-20 ms of pre-seek audio: a
	// click, not corruption -- the ring itself stays protected), and reaching it
	// takes a mixer that entered here BEFORE the gate went up (`silent` is sampled
	// once, at entry) and was then preempted across the whole submit -> wake ->
	// source->Seek() -> Reset() window. Bounded and rare, so it is not worth a
	// scratch-buffer copy + late commit -- but it IS reachable; nothing in the
	// code enforces otherwise.
	const uint64_t rpAdvanced = rpEntry + (uint64_t)framesRead * 2;
	uint64_t expected = rpEntry;
	readPos.compare_exchange_strong(expected, rpAdvanced,
	                                std::memory_order_acq_rel, std::memory_order_relaxed);
}

// ANY THREAD (render thread in practice: CVideoPlayer::Update()'s A/V sync).
// Seconds of audio CONSUMED since the last Reset() -- FROZEN contract.
//
// readPos FIRST, then epochBase (both acquire, mirroring Reset()'s store order):
// if we see a re-based readPos then that Reset()'s epochBase store is visible
// too, so the pair can never be (new readPos, stale base) -- which would report a
// colossal position. The remaining torn pair, (old readPos, new base), is caught
// by the rp < base test and reported as 0.0: a Reset() HAS landed, and 0.0 is
// exactly what the new epoch's clock reads.
double CVideoAudioChannel::GetPlaybackPosition()
{
	const uint64_t rp   = readPos.load(std::memory_order_acquire);
	const uint64_t base = epochBase.load(std::memory_order_acquire);
	const uint64_t framesPlayed = (rp > base) ? ((rp - base) / 2) : 0;
	return (double)framesPlayed / (double)engineSampleRate;
}

// ANY THREAD (decode thread in practice: CVideoPlayer's audio pump-ahead).
// readPos FIRST (acquire), then writePos -- mirrors MixIn()'s pairing logic;
// a Reset() racing us can only produce a pair whose difference exceeds the
// ring capacity, which reads as 0 ("low"), never as a bogus large backlog.
double CVideoAudioChannel::GetBufferedSeconds()
{
	const uint64_t rp = readPos.load(std::memory_order_acquire);
	const uint64_t wp = writePos.load(std::memory_order_acquire);
	const uint64_t used = (wp > rp) ? (wp - rp) : 0;
	if (used > (uint64_t)ringBufferCapacity)
		return 0.0;
	return ((double)(used / 2)) / (double)engineSampleRate;
}

// DECODE THREAD (CVideoPlayer::ServiceSeekOnDecodeThread), same thread as
// PushSamples() -- which is what makes the non-atomic resampler state below safe
// to touch here. The three stores are NOT joint; the mixer's CAS on readPos is
// what makes a concurrent MixIn() safe (see MixIn).
void CVideoAudioChannel::Reset()
{
	// RE-BASE, don't zero. Every epoch starts strictly PAST the previous epoch's
	// writePos, so a position value is never revisited and the mixer's CAS can
	// always tell a stale rpEntry from a live readPos (see the header for the
	// wedge that zeroing allowed). +ringBufferCapacity is the smallest jump that
	// clears every position the old epoch could still be holding -- and it keeps
	// base % ringBufferCapacity where the old writePos left it, so the modulo
	// indexing is unaffected. The ring is logically EMPTY either way: readPos ==
	// writePos.
	const uint64_t base = writePos.load(std::memory_order_relaxed) + (uint64_t)ringBufferCapacity;

	// Store order: epochBase, writePos, THEN readPos -- readers load readPos first
	// (see GetPlaybackPosition/MixIn), so observing the new readPos guarantees the
	// other two are visible. Never the other way round.
	epochBase.store(base, std::memory_order_release);
	writePos.store(base, std::memory_order_release);
	readPos.store(base, std::memory_order_release);
	ResetResampleState();
}

// SetDebugMixInPreCasHook() is defined inline in the header (guarded by
// PC_TEST_STAT_COUNTER) -- see there for why it does not have an out-of-line
// definition here.

// TEST-ONLY, quiescent-only -- see the declaration.
bool CVideoAudioChannel::DebugRingInvariantHolds() const
{
	const uint64_t rp   = readPos.load(std::memory_order_acquire);
	const uint64_t wp   = writePos.load(std::memory_order_acquire);
	const uint64_t base = epochBase.load(std::memory_order_acquire);
	return base <= rp && rp <= wp && (wp - rp) <= (uint64_t)ringBufferCapacity;
}
