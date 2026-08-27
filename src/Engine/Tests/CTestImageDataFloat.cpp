#include "CTestImageDataFloat.h"
#include "CImageData.h"
#include "DBG_Log.h"
#include <cmath>
#include <cstdio>

// Local assertion macro over StepCompleted()/TestCompleted(). CTest subclasses
// have no shared one -- each file defines its own; this follows the shape of
// CTestRawDecode.cpp's RD_ASSERT. imgui_test_engine's IM_CHECK is NOT usable
// here: it needs an ImGuiTestContext this test never has.
#define IDF_ASSERT(cond, msg) \
	do { \
		if (!(cond)) { \
			char buf[256]; \
			snprintf(buf, sizeof(buf), "FAIL: %s", (msg)); \
			LOGD("CTestImageDataFloat: %s", buf); \
			TestCompleted(false, buf); \
			return; \
		} \
		StepCompleted(stepNum++, true, (msg)); \
	} while (0)

void CTestImageDataFloat::Run(ITestCallback *callback)
{
	this->callback = callback;
	isRunning = true;
	int stepNum = 1;

	// ---- 1. half round trip, at the values that have to be exact ----------
	//
	// 0.0 and 1.0 are fixed points of any sane conversion and are checked
	// EXACTLY. The midpoint is checked with a tolerance because half has
	// ~1 024 steps between 0.5 and 1.0 -- this test must not encode a
	// precision claim half-float cannot meet.
	IDF_ASSERT(HalfToFloat(FloatToHalf(0.0f)) == 0.0f, "half round trip: 0.0 exact");
	IDF_ASSERT(HalfToFloat(FloatToHalf(1.0f)) == 1.0f, "half round trip: 1.0 exact");
	IDF_ASSERT(fabsf(HalfToFloat(FloatToHalf(0.5f)) - 0.5f) < 1e-6f,
			   "half round trip: 0.5 exact (a power of two)");
	IDF_ASSERT(fabsf(HalfToFloat(FloatToHalf(0.1f)) - 0.1f) < 0.001f,
			   "half round trip: 0.1 within half precision");
	IDF_ASSERT(HalfToFloat(FloatToHalf(-2.0f)) == -2.0f,
			   "half carries NEGATIVE values (out-of-gamut headroom)");
	IDF_ASSERT(HalfToFloat(FloatToHalf(4.0f)) == 4.0f,
			   "half carries 4.0 exactly -- the range that is the whole point");

	// ---- 1b. rounding is TIES-TO-EVEN, like the hardware --------------------
	//
	// Exactly-halfway values are the only place a half conversion can plausibly
	// disagree with IEEE 754, and the obvious implementation (ties away from
	// zero) gets them wrong. Found by differential testing against __fp16;
	// pinned here with the specific values that failed, because "it rounds
	// correctly" is not something a spot check of round numbers can show --
	// every power of two round-trips perfectly under either rule.
	//
	// Each of these is exactly representable as a float sitting precisely
	// between two halves, so the correct answer is the one with an EVEN
	// mantissa.
	{
		struct { float in; u16 expect; } kTies[] = {
			{  0.366333008f, 0x35DC },
			{  1.73095703f,  0x3EEC },
			{  2.64941406f,  0x414C },
			{  4.48632812f,  0x447C },
			{ -0.998291016f, 0xBBFC },
			{ -3.72753906f,  0xC374 },
			{ -5.56445312f,  0xC590 },
			{ -9.23828125f,  0xC89E },
		};
		for (const auto &t : kTies)
		{
			const u16 got = FloatToHalf(t.in);
			if (got != t.expect)
			{
				char buf[160];
				snprintf(buf, sizeof(buf),
						 "FAIL: FloatToHalf(%.9g) = 0x%04X, IEEE ties-to-even says 0x%04X",
						 t.in, (unsigned)got, (unsigned)t.expect);
				TestCompleted(false, buf);
				return;
			}
		}
	}
	IDF_ASSERT(true, "FloatToHalf rounds exact ties TO EVEN, matching the hardware");

	// Every representable half must survive a decode/encode round trip
	// unchanged -- 65 536 values, so this is exhaustive rather than sampled.
	{
		int bad = 0;
		for (u32 bits = 0; bits < 65536u; bits++)
		{
			const u16 h = (u16)bits;
			const u32 exp = (h >> 10) & 0x1Fu;
			if (exp == 0x1Fu) continue;            // Inf/NaN have their own rules
			if (FloatToHalf(HalfToFloat(h)) != h) bad++;
		}
		if (bad != 0)
		{
			char buf[128];
			snprintf(buf, sizeof(buf), "FAIL: %d of 65536 halves failed the round trip", bad);
			TestCompleted(false, buf);
			return;
		}
	}
	IDF_ASSERT(true, "all 65536 finite halves round-trip bit-exactly");

	// ---- 2. the extended sRGB curve ---------------------------------------
	//
	// Sign-symmetric and CONTINUED above 1.0, which is how CoreAnimation
	// defines extended sRGB. If either property is lost, resident float
	// pixels stop meaning what the surface says they mean.
	IDF_ASSERT(fabsf(SrgbExtendedEncode(0.0f)) < 1e-6f, "sRGB encode: 0 -> 0");
	IDF_ASSERT(fabsf(SrgbExtendedEncode(1.0f) - 1.0f) < 1e-5f, "sRGB encode: 1 -> 1");
	IDF_ASSERT(SrgbExtendedEncode(2.0f) > 1.0f,
			   "sRGB encode CONTINUES above 1.0 rather than clamping");
	IDF_ASSERT(SrgbExtendedEncode(-0.5f) < 0.0f,
			   "sRGB encode is SIGN-SYMMETRIC (negatives survive)");
	for (int i = 0; i <= 20; i++)
	{
		const float v = -1.0f + (float)i * 0.25f;      // -1.0 .. 4.0
		const float rt = SrgbExtendedDecode(SrgbExtendedEncode(v));
		if (fabsf(rt - v) > 1e-4f)
		{
			char buf[128];
			snprintf(buf, sizeof(buf), "FAIL: sRGB round trip at %.2f gave %.4f", v, rt);
			TestCompleted(false, buf);
			return;
		}
	}
	IDF_ASSERT(true, "sRGB encode/decode round trips over -1.0 .. 4.0");

	// ---- 3. unorm16 -> half ------------------------------------------------
	CImageData d(2, 1, IMG_TYPE_RGBA_16BIT);
	d.AllocImage(false, true);
	d.SetPixelResultRGBA16Bit(0, 0,     0,     0, 0, 65535);
	d.SetPixelResultRGBA16Bit(1, 0, 65535, 32768, 0, 65535);

	IDF_ASSERT(d.ConvertRGBA16BitToRGBA16F(), "unorm16 -> half converts");
	IDF_ASSERT(d.getImageType() == IMG_TYPE_RGBA_16F, "type is now float");

	float r0, g0, b0, a0, r1, g1, b1, a1;
	IDF_ASSERT(d.GetPixelResultFloat(0, 0, &r0, &g0, &b0, &a0), "float getter works");
	IDF_ASSERT(d.GetPixelResultFloat(1, 0, &r1, &g1, &b1, &a1), "float getter works (px 1)");
	IDF_ASSERT(r0 == 0.0f && a0 == 1.0f, "black stays black, alpha opaque");
	IDF_ASSERT(r1 == 1.0f, "65535 maps exactly to 1.0");
	IDF_ASSERT(fabsf(g1 - 0.5f) < 0.001f, "midpoint within half precision");

	// ---- 4. above-white survives -------------------------------------------
	d.SetPixelResultFloat(0, 0, 2.0f, 3.0f, 4.0f, 1.0f);
	IDF_ASSERT(d.GetPixelResultFloat(0, 0, &r0, &g0, &b0, &a0), "float getter after set");
	IDF_ASSERT(r0 > 1.9f && g0 > 2.9f && b0 > 3.9f, "values above 1.0 are storable");

	// ---- 5. the tone-map -----------------------------------------------------
	//
	// Two properties, and the SDR one is the one that matters: at headroom 1.0
	// the 0..1 body must come out where it does today, or every user without an
	// HDR display sees their photos change. 0.0 and 1.0 are exact fixed points;
	// interior values are checked within 1 LSB because the float path quantises
	// through half (~1 024 steps in 0.5..1.0 against unorm16's 32 768) and the
	// ordered dither adds its own +-0.5 LSB pattern, so bit-identical across a
	// whole image is not achievable. Demanding it would only invite a tolerance
	// invented after the first red.
	{
		CImageData sdr(4, 1, IMG_TYPE_RGBA_16F);
		sdr.AllocImage(false, true);
		// Surface-encoded input (the resident contract), so inputIsLinear=false.
		const float vals[4] = { 0.0f, 0.25f, 0.75f, 1.0f };
		for (int i = 0; i < 4; i++)
			sdr.SetPixelResultFloat(i, 0, SrgbExtendedEncode(vals[i]),
									SrgbExtendedEncode(vals[i]),
									SrgbExtendedEncode(vals[i]), 1.0f);

		IDF_ASSERT(sdr.ConvertRGBA16FToRGBA8(1.0f, false), "tone-map at headroom 1.0");
		IDF_ASSERT(sdr.getImageType() == IMG_TYPE_RGBA, "tone-map yields RGBA8");

		u8 r, g, b, a;
		sdr.GetPixelResultRGBA(0, 0, &r, &g, &b, &a);
		IDF_ASSERT(r == 0, "headroom 1.0: black is EXACTLY black");
		sdr.GetPixelResultRGBA(3, 0, &r, &g, &b, &a);
		IDF_ASSERT(r == 255, "headroom 1.0: white is EXACTLY white");

		for (int i = 1; i <= 2; i++)
		{
			sdr.GetPixelResultRGBA(i, 0, &r, &g, &b, &a);
			const int want = (int)(SrgbExtendedEncode(vals[i]) * 255.0f + 0.5f);
			if (abs((int)r - want) > 1)
			{
				char buf[160];
				snprintf(buf, sizeof(buf),
						 "FAIL: headroom 1.0 interior %.2f gave %d, today's path gives %d",
						 vals[i], (int)r, want);
				TestCompleted(false, buf);
				return;
			}
		}
		IDF_ASSERT(true, "headroom 1.0 interior values match today's path within 1 LSB");
	}

	// ---- 6. headroom buys highlight SEPARATION, not brightness ------------
	//
	// An 8-bit output has no headroom whatever the display does -- 255 is its
	// maximum. What the parameter actually buys is that two DIFFERENT
	// above-white values stay different instead of both flattening to white.
	// That is what a photographer needs from a tone-map: seeing that one
	// specular is hotter than another. Asserting "brighter at higher headroom"
	// would encode the wrong physical model -- higher headroom compresses MORE
	// range into the same 8 bits, so the body darkens.
	{
		u8 a2_h1, a3_h1, a2_h4, a3_h4, g, b, a;
		auto mapOne = [&](float v, float h) -> u8 {
			CImageData d1(1, 1, IMG_TYPE_RGBA_16F);
			d1.AllocImage(false, true);
			d1.SetPixelResultFloat(0, 0, v, v, v, 1.0f);
			d1.ConvertRGBA16FToRGBA8(h, true);
			u8 r; d1.GetPixelResultRGBA(0, 0, &r, &g, &b, &a);
			return r;
		};
		a2_h1 = mapOne(2.0f, 1.0f);  a3_h1 = mapOne(3.0f, 1.0f);
		a2_h4 = mapOne(2.0f, 4.0f);  a3_h4 = mapOne(3.0f, 4.0f);

		IDF_ASSERT(a2_h1 == 255 && a3_h1 == 255,
				   "headroom 1.0: above-white clips to white, exactly as today");
		IDF_ASSERT(a2_h4 < 255 && a3_h4 < 255,
				   "headroom 4.0: above-white no longer pinned at white");
		IDF_ASSERT(a3_h4 > a2_h4,
				   "headroom 4.0: 3.0 stays BRIGHTER than 2.0 -- highlights remain separable");
		IDF_ASSERT(a2_h4 < a2_h1,
				   "headroom 4.0 darkens the body -- the trade that buys the separation");
	}

	// ---- 7. wrong-type calls refuse rather than corrupt -----------------------
	{
		CImageData eight(1, 1, IMG_TYPE_RGBA);
		eight.AllocImage(false, true);
		IDF_ASSERT(!eight.ConvertRGBA16FToRGBA8(2.0f, true),
				   "tone-map refuses a non-float image");
		IDF_ASSERT(!eight.ConvertRGBA16BitToRGBA16F(),
				   "unorm16->half refuses a non-16-bit image");
		float fr, fg, fb, fa;
		IDF_ASSERT(!eight.GetPixelResultFloat(0, 0, &fr, &fg, &fb, &fa),
				   "float getter refuses a non-float image");
	}

	TestCompleted(true, "float image type: storage, curve, conversions and tone-map");
}
