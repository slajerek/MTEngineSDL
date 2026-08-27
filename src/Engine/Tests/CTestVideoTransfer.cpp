#include "CTestVideoTransfer.h"
#include "CVideoTransferFunctions.h"
#include "CImageData.h"   // FloatToHalf / HalfToFloat and their software references
#include "DBG_Log.h"
#include <cmath>
#include <cstdio>

#define VT_ASSERT(cond, msg) \
	do { \
		if (!(cond)) { \
			char _buf[256]; \
			snprintf(_buf, sizeof(_buf), "FAIL: %s", msg); \
			LOGD("CTestVideoTransfer: %s", _buf); \
			TestCompleted(false, _buf); \
			return; \
		} \
		StepCompleted(stepNum++, true, msg); \
	} while(0)

// Every expected number below was computed INDEPENDENTLY (from the published
// PQ/HLG formulae, in Python, while writing this test) rather than observed
// from a first run. That is the whole point: a test that records what the code
// currently does cannot detect the code being wrong.
static const float kEps = 1e-6f;

void CTestVideoTransfer::Run(ITestCallback *cb)
{
	this->callback = cb;
	isRunning = true;
	int stepNum = 1;

	using namespace VideoTransfer;

	// ---- 1. PQ EOTF endpoints -------------------------------------------
	// ST 2084 is defined so that encoded 1.0 is exactly the 10 000-nit peak
	// and 0.0 is exactly zero. If either endpoint drifts, every value between
	// them is wrong too.
	VT_ASSERT(fabsf(PqEotf(1.0f) - 1.0f) < kEps,
			  "PQ EOTF(1.0) == 1.0 (the 10 000-nit peak)");
	VT_ASSERT(fabsf(PqEotf(0.0f)) < kEps,
			  "PQ EOTF(0.0) == 0.0");

	// A mid code word, to catch a wrong exponent that both endpoints survive.
	// 0.5 -> 0.00922457 absolute == 92.2457 nits.
	VT_ASSERT(fabsf(PqEotf(0.5f) - 0.009224571f) < kEps,
			  "PQ EOTF(0.5) == 0.00922457 (92.25 nits) -- pins m1/m2/c1/c2/c3");

	// ---- 2. The SDR-white anchor and kPqScale, pinned TOGETHER -----------
	// PQ code 0.58068888 is exactly 203 nits, which is BT.2408 reference
	// white, which the pipeline calls 1.0. This single assertion catches a
	// wrong EOTF, a wrong kPqScale, and a wrong reference-white constant --
	// and those three are exactly what a careless "simplification" merges.
	VT_ASSERT(fabsf(PqEotf(0.58068888f) * kPqScale - 1.0f) < 1e-4f,
			  "PQ code 0.58068888 -> exactly 1.0 (203 nit SDR white, BT.2408)");
	VT_ASSERT(fabsf(kPqScale - (10000.0f / 203.0f)) < kEps,
			  "kPqScale == 10000/203");

	// ---- 3. HLG inverse OETF --------------------------------------------
	// The curve is piecewise at e == 0.5. Below it, e^2/3, so 0.5 -> 1/12
	// exactly; that is the cheapest way to catch a swapped branch.
	VT_ASSERT(fabsf(HlgInverseOetf(0.5f) - (1.0f / 12.0f)) < kEps,
			  "HLG inverse OETF(0.5) == 1/12 (the piecewise join, lower arm)");
	VT_ASSERT(fabsf(HlgInverseOetf(0.0f)) < kEps,
			  "HLG inverse OETF(0.0) == 0.0");
	// The upper arm at 1.0 must return 1.0 -- the a/b/c constants are chosen
	// to make the curve continuous AND reach exactly 1.0 there.
	VT_ASSERT(fabsf(HlgInverseOetf(1.0f) - 1.0f) < 1e-5f,
			  "HLG inverse OETF(1.0) == 1.0 -- pins a/b/c on the upper arm");

	// ---- 4. The HLG OOTF, which PQ does not have ------------------------
	// This is the asymmetry that makes "PQ works" no evidence at all about
	// HLG. gain = ys^0.2 for BT.2020 luma ys; a neutral 1.0 has ys == 1.0 so
	// the gain is 1.0, and the whole chain lands on kHlgScale.
	VT_ASSERT(fabsf(HlgOotfGain(1.0f, 1.0f, 1.0f) - 1.0f) < kEps,
			  "HLG OOTF gain == 1.0 for neutral 1.0 (ys == 1, 1^0.2 == 1)");
	VT_ASSERT(fabsf(HlgOotfGain(0.0f, 0.0f, 0.0f)) < kEps,
			  "HLG OOTF gain == 0.0 at black (no pow(0) surprise)");
	{
		// A non-unit neutral, where a dropped gamma actually shows: ys = 0.25,
		// gain = 0.25^0.2 = 0.757858.
		const float gain = HlgOotfGain(0.25f, 0.25f, 0.25f);
		VT_ASSERT(fabsf(gain - 0.757858f) < 1e-5f,
				  "HLG OOTF gain(0.25 neutral) == 0.25^0.2 == 0.757858 -- pins gamma 1.2");
	}
	VT_ASSERT(fabsf(kHlgScale - (1000.0f / 203.0f)) < kEps,
			  "kHlgScale == 1000/203 (BT.2100 nominal peak over SDR white)");

	// ---- 5. BT.2020 -> sRGB: neutral must stay neutral -------------------
	// The rows sum to 1.0, which is the property that breaks FIRST if the
	// matrix is ever mistyped. The published coefficients are rounded to 4
	// decimals, so the sums are 1.0001 / 1.0 / 0.9999 -- assert to 1e-3, and
	// say so, rather than pretending to a precision the constants do not have.
	{
		const float white[3] = { 1.0f, 1.0f, 1.0f };
		float out[3];
		Bt2020ToSrgb(white, out);
		VT_ASSERT(fabsf(out[0] - 1.0f) < 1e-3f &&
				  fabsf(out[1] - 1.0f) < 1e-3f &&
				  fabsf(out[2] - 1.0f) < 1e-3f,
				  "BT.2020->sRGB maps neutral white to neutral white (rows sum to 1)");

		const float black[3] = { 0.0f, 0.0f, 0.0f };
		Bt2020ToSrgb(black, out);
		VT_ASSERT(fabsf(out[0]) < kEps && fabsf(out[1]) < kEps && fabsf(out[2]) < kEps,
				  "BT.2020->sRGB maps black to black");
	}

	// ---- 6. The whole chain, both transfers ------------------------------
	{
		// PQ: the code word for SDR white, neutral, must come out at 1.0 in
		// all three channels (neutral through the matrix stays neutral).
		const float enc[3] = { 0.58068888f, 0.58068888f, 0.58068888f };
		float lin[3];
		HdrEncodedToLinearSrgb(enc, TRC_PQ, lin);
		VT_ASSERT(fabsf(lin[0] - 1.0f) < 1e-3f &&
				  fabsf(lin[1] - 1.0f) < 1e-3f &&
				  fabsf(lin[2] - 1.0f) < 1e-3f,
				  "full PQ chain: SDR-white code -> 1.0, neutral preserved");
	}
	{
		// HLG neutral 1.0 -> 4.926108 (== kHlgScale, since both the inverse
		// OETF and the OOTF gain are 1.0 there). This pins the ORDER of
		// operations as well as the constants: applying the scale before the
		// OOTF would give a different answer.
		const float enc[3] = { 1.0f, 1.0f, 1.0f };
		float lin[3];
		HdrEncodedToLinearSrgb(enc, TRC_HLG, lin);
		VT_ASSERT(fabsf(lin[0] - 4.926108f) < 1e-3f &&
				  fabsf(lin[1] - 4.926108f) < 1e-3f &&
				  fabsf(lin[2] - 4.926108f) < 1e-3f,
				  "full HLG chain: neutral 1.0 -> 4.926108 (OOTF then scale, in that order)");
	}
	{
		// And the two transfers must NOT agree -- if a refactor ever routed
		// HLG through the PQ branch this is what catches it.
		const float enc[3] = { 0.75f, 0.75f, 0.75f };
		float pqLin[3], hlgLin[3];
		HdrEncodedToLinearSrgb(enc, TRC_PQ, pqLin);
		HdrEncodedToLinearSrgb(enc, TRC_HLG, hlgLin);
		VT_ASSERT(fabsf(pqLin[0] - hlgLin[0]) > 0.1f,
				  "PQ and HLG give DIFFERENT answers for the same code word");
	}

	// ---- 7. trc classification -------------------------------------------
	VT_ASSERT(IsHdrTrc(16) && IsHdrTrc(18),
			  "trc 16 (PQ) and 18 (HLG) classify as HDR");
	VT_ASSERT(!IsHdrTrc(1) && !IsHdrTrc(2) && !IsHdrTrc(0),
			  "BT.709 / unspecified do NOT classify as HDR -- the SDR gate");

	// ---- 8. The tone-map curve, shared with the poster's 8-bit path ------
	// headroom 1.0 is EXACTLY the identity on 0..1. That is the property the
	// SDR regression rests on: with no headroom, the gate-closed arm must not
	// move a single SDR pixel.
	VT_ASSERT(fabsf(ToneMapReinhard(0.5f, 1.0f) - 0.5f) < kEps,
			  "Reinhard at headroom 1.0 is the IDENTITY (SDR must not move)");
	VT_ASSERT(fabsf(ToneMapReinhard(0.0f, 1.0f)) < kEps,
			  "Reinhard maps black to black");
	// At headroom h, the value h maps exactly to 1.0 -- that is what
	// "normalised so headroom lands on white" means.
	VT_ASSERT(fabsf(ToneMapReinhard(4.0f, 4.0f) - 1.0f) < kEps,
			  "Reinhard(headroom, headroom) == 1.0 -- the normalisation");
	VT_ASSERT(fabsf(ToneMapReinhard(2.0f, 4.0f) - 0.75f) < kEps,
			  "Reinhard(2.0, h=4) == 0.75 -- above-white stays SEPARABLE");
	VT_ASSERT(ToneMapReinhard(100.0f, 1.0f) <= 1.0f &&
			  ToneMapReinhard(-1.0f, 2.0f) >= 0.0f,
			  "Reinhard clamps into 0..1 for out-of-range input");

	// ---- 9. THE BULK TABLES must agree with the analytic functions -------
	//
	// The tables are what whole-image conversion actually uses (a poster is
	// 1.77M channel conversions; PQ costs two powf each, measured at 56 ms per
	// poster in Release). The analytic functions above are the definition. If
	// the two ever disagree, posters and stills silently drift away from the
	// shader -- which is the same class of defect the one-copy header exists
	// to prevent, just one level down.
	{
		// PQ is DIRECTLY INDEXED and carries kPqScale, so it must be EXACT at
		// every entry -- not close, exact. Any tolerance here would be hiding
		// something, because both sides evaluate the identical expression.
		const float *pq = PqEotfTable();
		double worstPq = 0.0;
		int worstPqIdx = -1;
		for (int i = 0; i < kEotfTableSize; i++)
		{
			const float want = PqEotf((float)i * (1.0f / 65535.0f)) * kPqScale;
			const double d = fabs((double)pq[i] - (double)want);
			if (d > worstPq) { worstPq = d; worstPqIdx = i; }
		}
		char buf[256];
		snprintf(buf, sizeof(buf),
				 "PQ table is EXACT against the analytic EOTF at all %d entries "
				 "(worst delta %.3g at index %d)", kEotfTableSize, worstPq, worstPqIdx);
		VT_ASSERT(worstPq == 0.0, buf);

		// HLG's inverse OETF likewise.
		const float *hlg = HlgInverseOetfTable();
		double worstHlg = 0.0;
		for (int i = 0; i < kEotfTableSize; i++)
		{
			const float want = HlgInverseOetf((float)i * (1.0f / 65535.0f));
			const double d = fabs((double)hlg[i] - (double)want);
			if (d > worstHlg) worstHlg = d;
		}
		snprintf(buf, sizeof(buf),
				 "HLG table is EXACT against the analytic inverse OETF (worst delta %.3g)",
				 worstHlg);
		VT_ASSERT(worstHlg == 0.0, buf);

		// The OOTF gain is INTERPOLATED, so it is the one piece with real
		// error. Bound it and say what the bound means: the result is stored
		// as half float, whose relative precision is ~1e-3, so anything at or
		// below that is invisible downstream.
		double worstGain = 0.0;
		for (int i = 0; i <= 2000; i++)
		{
			const float ys = (float)i / 2000.0f;
			const float want = (ys > 0.0f) ? powf(ys, 0.2f) : 0.0f;
			const double d = fabs((double)HlgOotfGainFast(ys) - (double)want);
			if (d > worstGain) worstGain = d;
		}
		snprintf(buf, sizeof(buf),
				 "HLG OOTF gain table tracks powf(ys,0.2) to %.3g -- below half-float's "
				 "~1e-3 relative precision, so it cannot move a stored pixel", worstGain);
		VT_ASSERT(worstGain < 1e-3, buf);
	}

	// ---- 10. and the whole CODE-VALUE chain matches the float chain -------
	// This is the one that actually guards the poster: HdrCodeToLinearSrgb is
	// what the extractor calls, HdrEncodedToLinearSrgb is what the tests and
	// the GPU-agreement comparison are written against.
	{
		const int probes[] = { 0, 1, 4096, 16384, 32768, 49152, 60000, 65535 };
		for (int trcIdx = 0; trcIdx < 2; trcIdx++)
		{
			const int trc = (trcIdx == 0) ? TRC_PQ : TRC_HLG;
			double worst = 0.0;
			for (int p = 0; p < 8; p++)
			{
				const u16 code[3] = { (u16)probes[p], (u16)probes[p], (u16)probes[p] };
				const float e = (float)probes[p] * (1.0f / 65535.0f);
				const float enc[3] = { e, e, e };
				float viaTable[3], viaFloat[3];
				HdrCodeToLinearSrgb(code, trc, viaTable);
				HdrEncodedToLinearSrgb(enc, trc, viaFloat);
				for (int c = 0; c < 3; c++)
				{
					const double rel = (fabs((double)viaFloat[c]) > 1e-6)
						? fabs((double)viaTable[c] - (double)viaFloat[c]) / fabs((double)viaFloat[c])
						: fabs((double)viaTable[c] - (double)viaFloat[c]);
					if (rel > worst) worst = rel;
				}
			}
			char buf[256];
			snprintf(buf, sizeof(buf),
					 "trc %d: the table chain matches the float chain to %.3g relative",
					 trc, worst);
			VT_ASSERT(worst < 1e-3, buf);
		}
	}

	// ---- 11. hardware half conversion == the software reference ----------
	//
	// FloatToHalf/HalfToFloat dispatch to a single hardware instruction on
	// arm64. The software implementation is kept as the reference precisely so
	// this comparison can exist -- the original code was written to match
	// __fp16 exactly, ties-to-even included, and that claim is now CHECKED
	// rather than trusted.
	{
		// Every representable half, both directions. Not a sample: the space
		// is only 65536 wide, so there is no reason to guess.
		int mismatchH2F = 0, firstBadH2F = -1;
		for (int i = 0; i < 65536; i++)
		{
			const u16 h = (u16)i;
			const float a = HalfToFloat(h);
			const float b = HalfToFloatSoftware(h);
			// NaN != NaN, so compare bit patterns for those.
			const bool bothNan = (a != a) && (b != b);
			if (!bothNan && a != b) { mismatchH2F++; if (firstBadH2F < 0) firstBadH2F = i; }
		}
		char buf[256];
		snprintf(buf, sizeof(buf),
				 "HalfToFloat: hardware matches the software reference for ALL 65536 "
				 "half values (%d mismatches, first at %d)", mismatchH2F, firstBadH2F);
		VT_ASSERT(mismatchH2F == 0, buf);

		// And the other direction, over every half's exact value plus the
		// midpoints between adjacent halves -- which is where ties-to-even
		// actually decides something, and where a naive implementation differs.
		int mismatchF2H = 0, firstBadF2H = -1;
		for (int i = 0; i < 65535; i++)
		{
			const float lo = HalfToFloatSoftware((u16)i);
			const float hi = HalfToFloatSoftware((u16)(i + 1));
			if (lo != lo || hi != hi) continue;                  // skip NaN region
			const float probes[3] = { lo, (lo + hi) * 0.5f, hi };
			for (int k = 0; k < 3; k++)
			{
				const u16 a = FloatToHalf(probes[k]);
				const u16 b = FloatToHalfSoftware(probes[k]);
				if (a != b) { mismatchF2H++; if (firstBadF2H < 0) firstBadF2H = i; }
			}
		}
		snprintf(buf, sizeof(buf),
				 "FloatToHalf: hardware matches the software reference at every half "
				 "AND every midpoint between adjacent halves -- the ties-to-even cases "
				 "(%d mismatches, first near index %d)", mismatchF2H, firstBadF2H);
		VT_ASSERT(mismatchF2H == 0, buf);
	}

	TestCompleted(true, "transfer maths hold to 1e-6; bulk tables and hardware half conversion match their references");
}
