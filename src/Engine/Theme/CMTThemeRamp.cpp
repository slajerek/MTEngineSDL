#include "CMTThemeRamp.h"
#include "DBG_Log.h"
#include <cmath>

static float MTH_ClampF(float v, float lo, float hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

// Clamp one seed field and say so once. A seed is author data, but it arrives
// from a host, and a silently absurd ramp is worse than a loud one.
static float ClampSeedField(float v, float lo, float hi, const char *name)
{
	float c = MTH_ClampF(v, lo, hi);
	if (c != v)
		LOGError("MT_ThemeGenerateRamp: seed.%s = %f clamped to %f", name, v, c);
	return c;
}

CMTThemeRamp MT_ThemeGenerateRamp(const CMTThemeSeed &seedIn)
{
	CMTThemeSeed seed = seedIn;
	seed.contrastGamma    = ClampSeedField(seed.contrastGamma,   0.25f, 4.0f,  "contrastGamma");
	seed.bgAnchorDark     = ClampSeedField(seed.bgAnchorDark,    0.0f,  1.0f,  "bgAnchorDark");
	seed.bgAnchorLight    = ClampSeedField(seed.bgAnchorLight,   0.0f,  1.0f,  "bgAnchorLight");
	seed.neutralChroma    = ClampSeedField(seed.neutralChroma,   0.0f,  0.05f, "neutralChroma");
	seed.accentChroma     = ClampSeedField(seed.accentChroma,    0.0f,  0.4f,  "accentChroma");
	seed.minContrastRatio = ClampSeedField(seed.minContrastRatio, 1.0f, 21.0f, "minContrastRatio");

	float lo = seed.bgAnchorDark;
	float hi = seed.bgAnchorLight;
	if (lo > hi)
	{
		// Step 1 being the darkest is the invariant the mode inversion rests
		// on. Obeying a descending seed would invert light and dark
		// everywhere at once, which is not a thing a caller can want.
		LOGError("MT_ThemeGenerateRamp: bgAnchorDark (%f) > bgAnchorLight (%f), swapping", lo, hi);
		float t = lo; lo = hi; hi = t;
	}

	// An achromatic seed must store hue 0, not seed.neutralHue: two seeds that
	// differ only in a hue nobody can see must produce bit-identical ramps.
	const bool  neutralIsAchromatic = (seed.neutralChroma <= 0.0f);
	const float neutralHue = neutralIsAchromatic ? 0.0f : seed.neutralHue;

	CMTThemeRamp ramp;
	for (int i = 0; i < MT_RAMP_STEPS; i++)
	{
		float t = (float)i / (float)(MT_RAMP_STEPS - 1);
		float L = lo + (hi - lo) * std::pow(t, seed.contrastGamma);

		MTOkLch n = { L, seed.neutralChroma, neutralHue };
		ramp.neutral[i] = MTH_OkLchClipChroma(n);
		if (neutralIsAchromatic)
		{
			ramp.neutral[i].C = 0.0f;
			ramp.neutral[i].h = 0.0f;
		}

		// Same L range for the accent, so accent[i] and neutral[i] share a
		// lightness and every contrast rule stated for one holds for the
		// other. Clipping matters most at the ends: at L 0.97 an 0.09-chroma
		// blue is far out of gamut, and an unclipped value would come back
		// clamped and hue-shifted.
		MTOkLch a = { L, seed.accentChroma, seed.accentHue };
		ramp.accent[i] = MTH_OkLchClipChroma(a);
	}
	return ramp;
}

int MT_ThemeNeutralStepIndex(int oneBasedIndex, MTThemeMode mode)
{
	int i = oneBasedIndex;
	if (i < 1) i = 1;
	if (i > MT_RAMP_STEPS) i = MT_RAMP_STEPS;
	return (mode == MTThemeMode_Light) ? (MT_RAMP_STEPS - i) : (i - 1);
}
