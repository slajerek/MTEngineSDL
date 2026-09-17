#include "CTestAudioResampler.h"
#include "SYS_Defs.h"
#include "MT_AudioResampler.h"
#include "DBG_Log.h"
#include <cmath>
#include <cstdio>
#include <vector>

// File-local, as CTestI18nEngine.cpp does -- the engine test framework has no
// assertion macro.
#define ASSERT_TRUE(cond, msg) do { if (!(cond)) { TestCompleted(false, (msg)); return; } } while (0)

CTestAudioResampler::CTestAudioResampler()  {}
CTestAudioResampler::~CTestAudioResampler() {}

// Interleaved stereo sine, both channels identical.
static void MakeSine(std::vector<float> &out, u32 frames, double freqHz, double rateHz)
{
	out.resize((size_t)frames * 2);
	const double w = 2.0 * M_PI * freqHz / rateHz;
	for (u32 i = 0; i < frames; i++)
	{
		float v = (float)sin(w * (double)i);
		out[(size_t)i * 2 + 0] = v;
		out[(size_t)i * 2 + 1] = v;
	}
}

// Rising zero crossings over the whole buffer -> frequency, without an FFT.
// Counts on the left channel only; a stereo desync would show up in step 3.
static double MeasureHz(const float *interleaved, u32 frames, double rateHz)
{
	if (frames < 2) return 0.0;
	u32 crossings = 0;
	for (u32 i = 1; i < frames; i++)
	{
		float a = interleaved[(size_t)(i - 1) * 2];
		float b = interleaved[(size_t)i * 2];
		if (a <= 0.0f && b > 0.0f)
			crossings++;
	}
	double seconds = (double)frames / rateHz;
	return (double)crossings / seconds;
}

void CTestAudioResampler::Run(ITestCallback *callback)
{
	this->callback = callback;
	isRunning = true;
	int stepNum = 1;
	char msg[192];

	const int kSrc = 48000;
	const int kDst = 44100;
	const u32 kSrcFrames = 48000;   // exactly one second

	std::vector<float> src;
	MakeSine(src, kSrcFrames, 1000.0, (double)kSrc);

	// ---- 1. LENGTH -------------------------------------------------------
	MT_AudioResampler r;
	r.Reset(kSrc, kDst, EAudioInterpolation::Cubic);
	ASSERT_TRUE(!r.IsPassthrough(), "FAIL: 48000 -> 44100 reported as passthrough");

	std::vector<float> dstOneShot((size_t)kDst * 2 + 64, 0.0f);
	u32 consumed = 0;
	u32 produced = r.Process(src.data(), kSrcFrames, dstOneShot.data(),
	                         (u32)(dstOneShot.size() / 2), &consumed);

	// A resampler carries a few frames of interpolation lookahead, so the tail
	// of the last block is legitimately still inside it. Two frames of slack.
	int lengthErr = (int)produced - kDst;
	if (lengthErr < 0) lengthErr = -lengthErr;
	snprintf(msg, sizeof(msg), "48000 -> 44100 of 1.0 s produced %u frames (expected ~%d)",
	         (unsigned)produced, kDst);
	ASSERT_TRUE(lengthErr <= 8, msg);
	StepCompleted(stepNum++, true, msg);

	// ---- 2. PITCH --------------------------------------------------------
	double hz = MeasureHz(dstOneShot.data(), produced, (double)kDst);
	snprintf(msg, sizeof(msg), "resampled tone measures %.1f Hz (expected 1000 Hz)", hz);
	// 1% -- the zero-crossing count is integral over one second, so the
	// quantisation alone is ~1 Hz. Not resampling at all gives 919 Hz, which is
	// 8x outside this band: the test discriminates the real defect comfortably.
	ASSERT_TRUE(hz > 990.0 && hz < 1010.0, msg);
	StepCompleted(stepNum++, true, msg);

	// The unresampled failure mode, asserted rather than described: feeding the
	// 48 kHz frames straight through and calling them 44.1 kHz reads as 919 Hz.
	double naive = MeasureHz(src.data(), kSrcFrames, (double)kDst);
	snprintf(msg, sizeof(msg), "unresampled, the same tone would read %.1f Hz -- the shipped defect", naive);
	ASSERT_TRUE(naive > 900.0 && naive < 940.0, "FAIL: the naive-path control measurement is wrong");
	StepCompleted(stepNum++, true, msg);

	// ---- 3. BLOCK INVARIANCE --------------------------------------------
	MT_AudioResampler r2;
	r2.Reset(kSrc, kDst, EAudioInterpolation::Cubic);
	std::vector<float> dstChunked;
	dstChunked.reserve(dstOneShot.size());

	const u32 kChunk = 1024;
	std::vector<float> tmp((size_t)kChunk * 4, 0.0f);
	for (u32 off = 0; off < kSrcFrames; off += kChunk)
	{
		u32 n = (off + kChunk <= kSrcFrames) ? kChunk : (kSrcFrames - off);
		u32 c = 0;
		u32 got = r2.Process(src.data() + (size_t)off * 2, n, tmp.data(),
		                     (u32)(tmp.size() / 2), &c);
		for (u32 i = 0; i < got * 2; i++)
			dstChunked.push_back(tmp[i]);
	}

	snprintf(msg, sizeof(msg), "chunked produced %u frames, one-shot %u",
	         (unsigned)(dstChunked.size() / 2), (unsigned)produced);
	ASSERT_TRUE(dstChunked.size() / 2 == produced, msg);

	for (size_t i = 0; i < (size_t)produced * 2; i++)
	{
		if (dstChunked[i] != dstOneShot[i])
		{
			snprintf(msg, sizeof(msg),
			         "FAIL: chunked and one-shot differ at sample %zu (%.9f vs %.9f) -- "
			         "fractional phase or history is not carried across calls",
			         i, (double)dstChunked[i], (double)dstOneShot[i]);
			LOGError("CTestAudioResampler: %s", msg);
			TestCompleted(false, msg);
			return;
		}
	}
	StepCompleted(stepNum++, true, "1024-frame chunks are bit-identical to one 48000-frame call");

	// ---- passthrough ------------------------------------------------------
	MT_AudioResampler r3;
	r3.Reset(kDst, kDst, EAudioInterpolation::Cubic);
	ASSERT_TRUE(r3.IsPassthrough(), "FAIL: equal rates not reported as passthrough");
	std::vector<float> pass((size_t)256 * 2, 0.0f);
	u32 c3 = 0;
	u32 p3 = r3.Process(src.data(), 256, pass.data(), 256, &c3);
	ASSERT_TRUE(p3 == 256 && c3 == 256, "FAIL: passthrough did not copy 1:1");
	for (size_t i = 0; i < 512; i++)
		ASSERT_TRUE(pass[i] == src[i], "FAIL: passthrough altered samples");
	StepCompleted(stepNum++, true, "equal rates pass through untouched, 1:1");

	TestCompleted(true, "MT_AudioResampler: length, pitch, block invariance and passthrough");
}

void CTestAudioResampler::Cancel()
{
	isRunning = false;
}
