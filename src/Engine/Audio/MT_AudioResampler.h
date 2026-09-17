#ifndef _MT_AudioResampler_h_
#define _MT_AudioResampler_h_

#include "SYS_Defs.h"
#include <vector>

// Interpolation quality. Values are persisted by hosts as a user setting --
// DO NOT RENUMBER. Deliberately identical in value and meaning to
// EVideoAudioInterpolation (Video/CVideoAudioChannel.h), because
// CVideoAudioChannel is due to be converged onto this class and a host's stored
// setting must survive that.
enum class EAudioInterpolation : int
{
	Nearest = 0,
	Linear  = 1,
	Cubic   = 2,   // 4-point Catmull-Rom
	Sinc    = 3    // 8-point Hann-windowed sinc
};

// Streaming sample-rate conversion for INTERLEAVED STEREO FLOAT.
//
// Stereo only, on purpose: the engine mixes in stereo, so channel conversion is
// the caller's job and happens once, before this. A resampler that also
// downmixed would need a per-codec channel map and would be two components
// wearing one name.
//
// STREAMING is the whole point. The audio callback asks for small blocks, so
// the fractional read position and the interpolation history have to survive
// between calls: feeding one buffer as N chunks must produce exactly the same
// samples as feeding it whole. CTestAudioResampler asserts that bit-for-bit.
class MT_AudioResampler
{
public:
	MT_AudioResampler();

	// Rates in Hz. Clears all carried state; call on open and on seek.
	void Reset(int srcRate, int dstRate, EAudioInterpolation quality = EAudioInterpolation::Cubic);

	// srcRate == dstRate. Callers may then skip the copy entirely if they can.
	bool IsPassthrough() const { return passthrough; }

	int  GetSourceRate() const { return srcRate; }
	int  GetDestRate() const   { return dstRate; }

	// Consume up to srcFrames from src, write up to maxDstFrames to dst,
	// return frames written. *outSrcConsumed (optional) reports how many source
	// frames were absorbed -- which is normally ALL of them: what is not yet
	// convertible is retained internally as interpolation history rather than
	// handed back, so the caller never has to re-present a partial block.
	u32 Process(const float *src, u32 srcFrames, float *dst, u32 maxDstFrames,
	            u32 *outSrcConsumed = NULL);

	// Upper bound on Process()'s output for srcFrames of input, for sizing a
	// destination buffer. Deliberately generous by a few frames.
	u32 EstimateOutputFrames(u32 srcFrames) const;

private:
	float Interpolate(const float *scratch, int frameIndex, double frac, int channel) const;

	// The interpolation window. Sinc-8 is the widest consumer: it reads
	// [i-3, i+4], so every mode uses that window and one code path.
	static constexpr int kLookBehind = 3;
	static constexpr int kLookAhead  = 4;
	// History carried between calls: the window, plus room for the unconsumed
	// tail (< one output step). Sized well above any realistic ratio.
	static constexpr int kMaxHistoryFrames = 64;

	int    srcRate;
	int    dstRate;
	double ratio;          // srcRate / dstRate
	bool   passthrough;
	EAudioInterpolation quality;

	// THE READ POSITION IS SPLIT ON PURPOSE, and it is not a micro-optimisation.
	//
	// A single `double readPos` works, and CTestAudioResampler's block-invariance
	// step proves it is not good enough: fed one 48000-frame block the
	// accumulator climbs to ~48000, fed 1024-frame blocks it is rebased to near
	// zero every call, and adding the same ratio at those two magnitudes rounds
	// differently. The outputs then diverge in the last bits -- inaudible, but it
	// means "the same audio, chunked differently" is not the same audio, and a
	// resampler that cannot promise that cannot be reasoned about.
	//
	// With the integer part held separately, rebasing only ever subtracts an
	// integer from posInt, and posFrac performs bit-identical arithmetic in both
	// cases. It also removes accumulator drift over a long track entirely.
	int    posInt;         // integer position, in scratch-buffer frames
	double posFrac;        // fractional part, always in [0, 1)
	int    historyFrames;  // valid frames currently in history
	float  history[kMaxHistoryFrames * 2];

	std::vector<float> scratch;   // history + this call's input
};

#endif
//_MT_AudioResampler_h_
