#include "CTestSurfaceEncoding.h"
#include "MT_SurfaceEncoding.h"
#include "CVideoTransferFunctions.h"   // PqEotf, kPqPeakNits, kSdrReferenceWhiteNits
#include "MT_SrgbCurve.h"              // SrgbExtendedEncode, the forward curve
#include "DBG_Log.h"
#include <cmath>
#include <cstdio>

#define SE_ASSERT(cond, msg) \
	do { \
		if (!(cond)) { \
			char _buf[256]; \
			snprintf(_buf, sizeof(_buf), "FAIL: %s", msg); \
			LOGD("CTestSurfaceEncoding: %s", _buf); \
			TestCompleted(false, _buf); \
			return; \
		} \
		StepCompleted(stepNum++, true, msg); \
	} while(0)

// Every expected number below is either an exact algebraic consequence of the
// definitions (80 nits == scRGB 1.0, so 200 nits == 2.5) or is computed HERE
// from an independent function -- the PQ expectation is bisected out of
// VideoTransfer::PqEotf rather than hardcoded, so this test cannot agree with a
// wrong inverse by having copied its output.
static const float kEps = 1e-6f;

// Bisect the monotonic PqEotf for the code value that produces `absLuminance01`.
// Monotonic on [0,1] by construction, so bisection converges; 60 halvings takes
// the interval below float resolution.
static float PqCodeForAbsLuminance(float absLuminance01)
{
	float lo = 0.0f, hi = 1.0f;
	for (int i = 0; i < 60; i++)
	{
		const float mid = 0.5f * (lo + hi);
		if (VideoTransfer::PqEotf(mid) < absLuminance01)
			lo = mid;
		else
			hi = mid;
	}
	return 0.5f * (lo + hi);
}

void CTestSurfaceEncoding::Run(ITestCallback *cb)
{
	this->callback = cb;
	isRunning = true;
	int stepNum = 1;

	using namespace MTSurfaceEncoding;

	// ---- 1. The nits <-> scRGB convention -------------------------------
	// scRGB is DEFINED with 1.0 == 80 nits (IEC 61966-2-2). Everything else in
	// this file rests on that, so it is pinned first.
	SE_ASSERT(fabsf(kScRgbReferenceWhiteNits - 80.0f) < kEps,
			  "scRGB reference white is 80 nits (IEC 61966-2-2)");
	SE_ASSERT(fabsf(ScRgbSdrWhiteFromNits(80.0f) - 1.0f) < kEps,
			  "ScRgbSdrWhiteFromNits(80) == 1.0 -- scRGB's own white");
	SE_ASSERT(fabsf(ScRgbSdrWhiteFromNits(200.0f) - 2.5f) < kEps,
			  "ScRgbSdrWhiteFromNits(200) == 2.5 -- Windows 11's common default, "
			  "and the number SDL reports for it");

	// ---- 2. Black is black, at every scale -------------------------------
	for (int i = 0; i < 3; i++)
	{
		const float s = (i == 0) ? 1.0f : (i == 1 ? 2.5f : 12.0f);
		SE_ASSERT(fabsf(EncodedSrgbToScRgb(0.0f, s)) < kEps,
				  "EncodedSrgbToScRgb(0, s) == 0 for any s");
	}

	// ---- 3. THE DIRECTION OF THE WHITE-LEVEL SCALE -----------------------
	//
	// This is the assertion the whole file exists for. SDL's property is the
	// scRGB value SDR white BECOMES, so our 1.0 must be emitted AS that value.
	// A divide -- the first draft of the S-6 plan -- gives 0.4 here instead of
	// 2.5, which is 32 nits beside 200-nit SDR windows.
	SE_ASSERT(fabsf(EncodedSrgbToScRgb(1.0f, 1.0f) - 1.0f) < kEps,
			  "EncodedSrgbToScRgb(1, 1) == 1 -- identity at an 80-nit SDR white "
			  "(and the macOS case, where SDL always reports 1.0)");
	SE_ASSERT(fabsf(EncodedSrgbToScRgb(1.0f, 2.5f) - 2.5f) < 1e-5f,
			  "EncodedSrgbToScRgb(1, 2.5) == 2.5 -- our white lands ON the "
			  "display's SDR white (a DIVIDE would give 0.4)");

	// The user-visible symptom of the inversion, named as such: turning the
	// Windows SDR-brightness slider UP must make the app BRIGHTER.
	{
		const float dim    = EncodedSrgbToScRgb(1.0f, 1.5f);
		const float bright = EncodedSrgbToScRgb(1.0f, 3.0f);
		SE_ASSERT(bright > dim,
				  "raising the SDR white level RAISES our scRGB output "
				  "(the app must not get darker as the slider goes up)");
	}

	// ---- 3b. THE CURVE ITSELF, pinned to an ABSOLUTE value ---------------
	//
	// Everything else about the scRGB path is an endpoint (0 and 1 are fixed
	// points of ANY power curve) or a round trip through the matching encode
	// (self-consistent by construction). So if SrgbExtendedEncode AND
	// SrgbExtendedDecode both carried 2.2 instead of 2.4, every other step in
	// this file would still pass -- and the D3D11 resolve would ship a wrong
	// gamma with a green suite. A1 review round 1 caught that gap.
	//
	// 0.21404114 is computed from the published IEC 61966-2-1 formula --
	// ((0.5 + 0.055) / 1.055) ^ 2.4 -- not observed from a run. It is also the
	// number this file's comments name four times as the one that separates
	// right from wrong.
	SE_ASSERT(fabsf(EncodedSrgbToScRgb(0.5f, 1.0f) - 0.21404114f) < 1e-6f,
			  "mid-grey 0.5 decodes to 0.21404114 -- THE absolute pin on the "
			  "curve, and the only step here a wrong exponent cannot survive");
	// The encode half, pinned the same way: 0.21404114 must encode back to 0.5.
	SE_ASSERT(fabsf(SrgbExtendedEncode(0.21404114f) - 0.5f) < 1e-6f,
			  "SrgbExtendedEncode(0.21404114) == 0.5 -- the forward curve, "
			  "pinned absolutely rather than only against its own inverse");
	// And the LINEAR segments. NOTE WHERE THESE SAMPLE: **inside** each toe,
	// never at its edge.
	//
	// Review round 2 caught round 1 asserting this AT 0.04045, the branch value
	// itself -- where the two arms agree to 2.3e-9 BY DESIGN (continuity is
	// what makes it a usable curve), i.e. 43x INSIDE the 1e-7 tolerance. So it
	// pinned the slope, could not possibly pin the branch point, and its
	// comment claimed otherwise. The defect it let through is the likeliest
	// typo in this curve: writing the decode's ENCODED-domain threshold
	// 0.04045 as the encode's LINEAR-domain 0.0031308. Sampling INSIDE the
	// segment separates the arms by ~5000x the tolerance instead.
	SE_ASSERT(fabsf(EncodedSrgbToScRgb(0.01f, 1.0f) - (0.01f / 12.92f)) < 1e-7f,
			  "0.01 is INSIDE the decode toe: /12.92 = 0.00077399, not the power "
			  "arm's 0.00124505 -- THIS is what pins the 0.04045 branch point");
	SE_ASSERT(fabsf(SrgbExtendedEncode(0.001f) - (0.001f * 12.92f)) < 1e-7f,
			  "0.001 is INSIDE the encode toe: x12.92 = 0.01292, not the power "
			  "arm's 0.00432701 -- pins SrgbExtendedEncode's 0.0031308 branch point");
	// The branch value itself still earns a step, but for what it CAN prove:
	// that the curve is CONTINUOUS there, which is a different property from
	// either arm being right.
	SE_ASSERT(fabsf(EncodedSrgbToScRgb(0.04045f, 1.0f) - (0.04045f / 12.92f)) < 1e-7f,
			  "the curve is CONTINUOUS at 0.04045: both arms give 0.0031308 "
			  "(this pins the 12.92 slope; the branch point is pinned above)");

	// ---- 4. The decode half, proved by round trip ------------------------
	// Encoding 2.0 and decoding it back must return 2.0. This is what proves
	// EncodedSrgbToScRgb is using the real inverse curve rather than something
	// that merely happens to fix 0 and 1.
	SE_ASSERT(fabsf(EncodedSrgbToScRgb(SrgbExtendedEncode(2.0f), 1.0f) - 2.0f) < 1e-5f,
			  "round trip: EncodedSrgbToScRgb(SrgbExtendedEncode(2.0), 1.0) == 2.0");
	SE_ASSERT(fabsf(EncodedSrgbToScRgb(SrgbExtendedEncode(0.5f), 1.0f) - 0.5f) < 1e-5f,
			  "round trip at mid-grey: 0.5 survives encode-then-decode");

	// Above-white survives the SCALE too: 2x SDR white is 2x the DISPLAY's SDR
	// white, not 2x 80 nits. This is the number a user with headroom sees.
	SE_ASSERT(fabsf(EncodedSrgbToScRgb(SrgbExtendedEncode(2.0f), 2.5f) - 5.0f) < 1e-4f,
			  "above-white scales: 2.0 at an SDR white of 2.5 is 5.0 scRGB");

	// ---- 5. Sign symmetry -------------------------------------------------
	// A wide-gamut colour converted into sRGB primaries can land NEGATIVE, and
	// folding that to zero silently clips the gamut the float pipeline exists
	// to preserve (same reasoning as SrgbExtendedEncode, MT_SrgbCurve.h).
	{
		const float x = 0.6f, s = 2.5f;
		SE_ASSERT(fabsf(EncodedSrgbToScRgb(-x, s) + EncodedSrgbToScRgb(x, s)) < 1e-6f,
				  "EncodedSrgbToScRgb(-x, s) == -EncodedSrgbToScRgb(x, s)");
	}

	// ---- 6. THE SDR SWAPCHAIN IS THE EXACT IDENTITY -----------------------
	//
	// Bit for bit, at EVERY scale including 2.5 -- the resolve pass must not
	// touch an R8G8B8A8_UNORM/G22 swapchain, because the pipeline already wrote
	// sRGB-encoded values and that is what such a swapchain wants. Decoding
	// "at scale 1.0" instead moves 0.5 to 0.214: ~73 LSB across the whole UI on
	// the DEFAULT Windows path (S-6 plan review round 2).
	{
		const float vs[] = { 0.0f, 0.04045f, 0.5f, 1.0f, 1.5f, -0.25f };
		const float ss[] = { 1.0f, 2.5f, 0.5f };
		for (int i = 0; i < 6; i++)
		{
			for (int j = 0; j < 3; j++)
			{
				if (ResolveToSurface(vs[i], false, ss[j]) != vs[i])
				{
					char buf[192];
					snprintf(buf, sizeof(buf),
							 "FAIL: SDR resolve is not the identity: v=%.6f s=%.3f -> %.6f",
							 vs[i], ss[j], ResolveToSurface(vs[i], false, ss[j]));
					TestCompleted(false, buf);
					return;
				}
			}
		}
		StepCompleted(stepNum++, true,
					  "ResolveToSurface(v, swapchainIsLinear=false, s) == v exactly, "
					  "for every v and every s");
	}

	// ...and the LINEAR arm is exactly EncodedSrgbToScRgb.
	//
	// THIS IS A STRUCTURAL GUARD, NOT A VALUE CHECK, and A1 review round 1 was
	// right to call the first version's comment misleading: today
	// ResolveToSurface's true arm literally `return`s EncodedSrgbToScRgb(...),
	// so this compares one call with the same call and cannot fail for any
	// value of the maths, right or wrong. What it DOES catch is somebody later
	// hand-inlining a second copy of the decode into the resolve arm -- which
	// is exactly how the HLSL twin and this header would drift apart.
	{
		const float vs[] = { 0.0f, 0.04045f, 0.5f, 1.0f, 1.5f, -0.25f };
		for (int i = 0; i < 6; i++)
		{
			if (ResolveToSurface(vs[i], true, 2.5f) != EncodedSrgbToScRgb(vs[i], 2.5f))
			{
				TestCompleted(false, "FAIL: linear resolve arm disagrees with EncodedSrgbToScRgb");
				return;
			}
		}
		StepCompleted(stepNum++, true,
					  "ResolveToSurface(v, true, s) == EncodedSrgbToScRgb(v, s)");
	}

	// ---- 7. PQ, the path 203 nits DOES belong to --------------------------
	//
	// kPqPeakNits is duplicated from VideoTransfer deliberately (this header
	// must not pull in the video include path), so the duplication is pinned
	// rather than trusted.
	SE_ASSERT(fabsf(kPqPeakNits - VideoTransfer::kPqPeakNits) < kEps,
			  "MTSurfaceEncoding::kPqPeakNits == VideoTransfer::kPqPeakNits");

	// NOT zero, and saying so matters: ST 2084's OETF at L=0 is (c1/1)^m2 =
	// 0.8359375^78.84375 = 7.3096e-7, the encoding's own floor. The first
	// version of this test asserted "== 0" with a 1e-6 tolerance, which passed
	// with 27% margin against a value the message called zero (A1 review round
	// 1). In 10-bit that floor is still code word 0, so nothing is wrong with
	// the maths -- only with what a reader would have believed.
	SE_ASSERT(fabsf(PqInverseEotf(0.0f) - 7.3096e-7f) < 1e-8f,
			  "PQ inverse EOTF(0) == 7.3096e-7, the ST 2084 floor c1^m2 -- NOT zero");
	SE_ASSERT(fabsf(PqInverseEotf(1.0f) - 1.0f) < 1e-5f,
			  "PQ inverse EOTF(1) == 1 (the 10 000-nit peak)");

	// It is the inverse of THAT PqEotf, not of a formula copied from elsewhere:
	// round-trip a ramp through both.
	{
		float worst = 0.0f;
		for (int i = 0; i <= 20; i++)
		{
			const float code = (float)i / 20.0f;
			const float back = PqInverseEotf(VideoTransfer::PqEotf(code));
			const float err  = fabsf(back - code);
			if (err > worst) worst = err;
		}
		char buf[160];
		snprintf(buf, sizeof(buf),
				 "PqInverseEotf round-trips VideoTransfer::PqEotf across a 21-step "
				 "ramp (worst error %.2e)", worst);
		SE_ASSERT(worst < 1e-4f, buf);
	}

	// The one number that ties our pipeline's white to PQ's absolute scale:
	// our 1.0 is 203 nits (BT.2408), so it must encode to the PQ code word for
	// 203 nits -- computed HERE by bisecting PqEotf, never hardcoded.
	{
		const float expected =
			PqCodeForAbsLuminance(VideoTransfer::kSdrReferenceWhiteNits / kPqPeakNits);
		const float got = EncodedSrgbToPq(1.0f, VideoTransfer::kSdrReferenceWhiteNits);
		char buf[192];
		snprintf(buf, sizeof(buf),
				 "EncodedSrgbToPq(1.0, 203) == PQ code for 203 nits "
				 "(expected %.6f, got %.6f)", expected, got);
		SE_ASSERT(fabsf(got - expected) < 1e-4f, buf);

		// AND against the PUBLISHED figure, not only against this repo's own
		// PqEotf. The bisection above is a round trip through ONE function: if
		// m1, c2 or c3 carried a wrong value in BOTH PqEotf and PqInverseEotf,
		// it would still agree with itself (review round 2 -- the same shape of
		// gap round 1 closed for the sRGB curve and left open for PQ).
		// 0.580689 is ST 2084's code word for 203 cd/m2.
		SE_ASSERT(fabsf(got - 0.580689f) < 1e-5f,
				  "203 nits is ST 2084 code 0.580689 -- the PUBLISHED value, "
				  "independent of this repo's own PqEotf");
	}

	SE_ASSERT(fabsf(EncodedSrgbToPq(0.0f, VideoTransfer::kSdrReferenceWhiteNits)
				   - PqInverseEotf(0.0f)) < 1e-9f,
			  "EncodedSrgbToPq(0, 203) lands exactly on the ST 2084 floor "
			  "(7.3096e-7, code word 0 in 10-bit) -- not on a different one");

	// Above-white encodes ABOVE the 203-nit code, monotonically. A PQ path that
	// clamped at reference white would pass every endpoint test above.
	SE_ASSERT(EncodedSrgbToPq(SrgbExtendedEncode(4.0f), VideoTransfer::kSdrReferenceWhiteNits) >
			  EncodedSrgbToPq(1.0f, VideoTransfer::kSdrReferenceWhiteNits),
			  "4x reference white encodes to a HIGHER PQ code than reference white");

	TestCompleted(true, "surface-encoding conversions agree with their closed-form references");
}
