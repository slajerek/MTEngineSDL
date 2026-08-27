#include "CTestThemeMath.h"
#include "MTH_OkLab.h"
#include "DBG_Log.h"
#include <cmath>
#include <cstdio>

#define TM_ASSERT(cond, msg) \
	do { \
		if (!(cond)) { \
			char _buf[256]; \
			snprintf(_buf, sizeof(_buf), "FAIL: %s", msg); \
			LOGD("CTestThemeMath: %s", _buf); \
			TestCompleted(false, _buf); \
			return; \
		} \
		StepCompleted(stepNum++, true, msg); \
	} while(0)

static MTOkLab LabOfSrgb(float r, float g, float b)
{
	MTLinearRgb lin = { MTH_SrgbToLinear(r), MTH_SrgbToLinear(g), MTH_SrgbToLinear(b) };
	return MTH_LinearRgbToOkLab(lin);
}

void CTestThemeMath::Run(ITestCallback *cb)
{
	this->callback = cb;
	isRunning = true;
	int stepNum = 1;

	// --- 1. Published reference values -------------------------------------
	// Ottosson's OKLab values for the sRGB primaries. Tolerance 1e-3: these are
	// the PUBLISHED numbers, and if the implementation disagrees by more than
	// that, a matrix coefficient was mistyped. Do not "fix" the test by
	// widening it -- cross-check against https://bottosson.github.io/posts/oklab/
	MTOkLab white = LabOfSrgb(1.f, 1.f, 1.f);
	TM_ASSERT(std::fabs(white.L - 1.0f) < 1e-3f &&
	          std::fabs(white.a) < 1e-3f && std::fabs(white.b) < 1e-3f,
	          "sRGB white -> OKLab (1, 0, 0)");

	MTOkLab black = LabOfSrgb(0.f, 0.f, 0.f);
	TM_ASSERT(std::fabs(black.L) < 1e-3f &&
	          std::fabs(black.a) < 1e-3f && std::fabs(black.b) < 1e-3f,
	          "sRGB black -> OKLab (0, 0, 0)");

	MTOkLab red = LabOfSrgb(1.f, 0.f, 0.f);
	TM_ASSERT(std::fabs(red.L - 0.627955f) < 1e-3f &&
	          std::fabs(red.a - 0.224863f) < 1e-3f &&
	          std::fabs(red.b - 0.125846f) < 1e-3f,
	          "sRGB red -> OKLab (0.62796, 0.22486, 0.12585)");

	MTOkLab green = LabOfSrgb(0.f, 1.f, 0.f);
	TM_ASSERT(std::fabs(green.L - 0.866440f) < 1e-3f &&
	          std::fabs(green.a + 0.233888f) < 1e-3f &&
	          std::fabs(green.b - 0.179498f) < 1e-3f,
	          "sRGB green -> OKLab (0.86644, -0.23389, 0.17950)");

	MTOkLab blue = LabOfSrgb(0.f, 0.f, 1.f);
	TM_ASSERT(std::fabs(blue.L - 0.452014f) < 1e-3f &&
	          std::fabs(blue.a + 0.032457f) < 1e-3f &&
	          std::fabs(blue.b + 0.311528f) < 1e-3f,
	          "sRGB blue -> OKLab (0.45201, -0.03246, -0.31153)");

	// --- 2. Achromatic invariant -------------------------------------------
	// Every neutral must come back with a == b == 0. This is what makes the
	// "chroma stays at zero for colour-judgement themes" promise structural.
	for (int i = 0; i <= 10; i++)
	{
		float v = (float)i / 10.f;
		MTOkLab lab = LabOfSrgb(v, v, v);
		TM_ASSERT(std::fabs(lab.a) < 1e-5f && std::fabs(lab.b) < 1e-5f,
		          "grey ramp stays achromatic in OKLab");
	}

	// --- 3. Round trip ------------------------------------------------------
	float worst = 0.f;
	for (int r = 0; r < 256; r += 17)
		for (int g = 0; g < 256; g += 51)
			for (int b = 0; b < 256; b += 85)
			{
				float sr = r / 255.f, sg = g / 255.f, sb = b / 255.f;
				MTOkLab lab = LabOfSrgb(sr, sg, sb);
				MTLinearRgb back = MTH_OkLabToLinearRgb(lab);
				float er = std::fabs(MTH_LinearToSrgb(back.r) - sr);
				float eg = std::fabs(MTH_LinearToSrgb(back.g) - sg);
				float eb = std::fabs(MTH_LinearToSrgb(back.b) - sb);
				if (er > worst) worst = er;
				if (eg > worst) worst = eg;
				if (eb > worst) worst = eb;
			}
	TM_ASSERT(worst < 1e-4f, "sRGB -> OKLab -> sRGB round trip within 1e-4");

	// --- 4. Lch <-> Lab -----------------------------------------------------
	MTOkLch lch = MTH_OkLabToOkLch(red);
	TM_ASSERT(std::fabs(lch.C - 0.257683f) < 1e-3f &&
	          std::fabs(lch.h - 29.234f) < 0.05f,
	          "red -> OKLCh C 0.25768, h 29.234 deg");
	MTOkLab backLab = MTH_OkLchToOkLab(lch);
	TM_ASSERT(std::fabs(backLab.a - red.a) < 1e-5f &&
	          std::fabs(backLab.b - red.b) < 1e-5f,
	          "OKLab -> OKLCh -> OKLab round trip");

	MTOkLch greyLch = MTH_OkLabToOkLch(LabOfSrgb(0.5f, 0.5f, 0.5f));
	TM_ASSERT(greyLch.C < 1e-4f && greyLch.h == 0.0f,
	          "hue is forced to 0 at zero chroma, never atan2(0,0) noise");

	// --- 5. Gamut clipping --------------------------------------------------
	// A mid-lightness blue at absurd chroma must come back in gamut with its
	// hue intact -- hue preservation is the entire reason we clip chroma
	// rather than clamping channels.
	MTOkLch wild = { 0.55f, 0.60f, 250.f };
	MTOkLch clipped = MTH_OkLchClipChroma(wild);
	TM_ASSERT(clipped.C < wild.C && clipped.C > 0.f, "absurd chroma is reduced");
	TM_ASSERT(std::fabs(clipped.h - wild.h) < 1e-4f, "clipping preserves hue");
	MTLinearRgb clippedLin = MTH_OkLabToLinearRgb(MTH_OkLchToOkLab(clipped));
	TM_ASSERT(MTH_LinearRgbInGamut(clippedLin), "clipped colour is in sRGB gamut");
	MTOkLch tame = { 0.55f, 0.02f, 250.f };
	TM_ASSERT(std::fabs(MTH_OkLchClipChroma(tame).C - tame.C) < 1e-6f,
	          "an in-gamut chroma is returned unchanged");

	// --- 6. WCAG ------------------------------------------------------------
	MTLinearRgb linWhite = { 1.f, 1.f, 1.f };
	MTLinearRgb linBlack = { 0.f, 0.f, 0.f };
	float yw = MTH_WcagLuminance(linWhite), yb = MTH_WcagLuminance(linBlack);
	TM_ASSERT(std::fabs(yw - 1.0f) < 1e-5f, "WCAG luminance of white is 1");
	TM_ASSERT(std::fabs(yb) < 1e-5f, "WCAG luminance of black is 0");
	TM_ASSERT(std::fabs(MTH_WcagContrast(yw, yb) - 21.0f) < 1e-3f,
	          "black on white is 21:1");
	TM_ASSERT(std::fabs(MTH_WcagContrast(yb, yw) - 21.0f) < 1e-3f,
	          "contrast is order-independent");

	// --- 7. The closed form the solver depends on ---------------------------
	// Y(grey with OKLab lightness L) == L^3, exactly. If this ever stops
	// holding, the solver silently degrades to "approximately right", which
	// is exactly the failure mode a contrast guarantee cannot have.
	for (int i = 1; i <= 10; i++)
	{
		float L = (float)i / 10.f;
		MTOkLch g = { L, 0.f, 0.f };
		float y = MTH_OkLchWcagLuminance(g);
		TM_ASSERT(std::fabs(y - L * L * L) < 1e-4f,
		          "achromatic luminance equals L^3");
	}
	// And the inversion round-trips: solve for the L that gives 4.5:1 against
	// a background, then measure what that L actually achieves.
	{
		float bgL = 0.20f;
		float tL = MTH_GreyLLighterForContrast(bgL, 4.5f);
		TM_ASSERT(tL > bgL && tL <= 1.0f, "4.5:1 above L=0.20 is achievable");
		MTOkLch bg = { bgL, 0.f, 0.f }, fg = { tL, 0.f, 0.f };
		float got = MTH_WcagContrast(MTH_OkLchWcagLuminance(fg),
		                             MTH_OkLchWcagLuminance(bg));
		TM_ASSERT(std::fabs(got - 4.5f) < 1e-2f,
		          "MTH_GreyLLighterForContrast hits the requested ratio");

		float dL = MTH_GreyLDarkerForContrast(0.97f, 4.5f);
		TM_ASSERT(dL < 0.97f && dL >= 0.f, "4.5:1 below L=0.97 is achievable");
		MTOkLch lbg = { 0.97f, 0.f, 0.f }, lfg = { dL, 0.f, 0.f };
		float lgot = MTH_WcagContrast(MTH_OkLchWcagLuminance(lfg),
		                              MTH_OkLchWcagLuminance(lbg));
		TM_ASSERT(std::fabs(lgot - 4.5f) < 1e-2f,
		          "MTH_GreyLDarkerForContrast hits the requested ratio");
	}
	// Unachievable requests must be detectable, not silently clamped.
	TM_ASSERT(MTH_GreyLLighterForContrast(0.90f, 21.0f) > 1.0f,
	          "an impossible ratio returns L > 1 rather than clamping");

	TestCompleted(true, "ThemeMath: all steps passed");
}
