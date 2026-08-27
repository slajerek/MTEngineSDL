#include "MTH_OkLab.h"
#include <cmath>

// Deliberately no other includes: this unit is the numeric substrate of the
// whole theme system and must stay unit-testable in isolation.

static const float kPi = 3.14159265358979323846f;

// --- transfer function -------------------------------------------------

float MTH_SrgbToLinear(float c)
{
	return (c <= 0.04045f) ? (c / 12.92f)
	                       : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

float MTH_LinearToSrgb(float c)
{
	return (c <= 0.0031308f) ? (12.92f * c)
	                         : (1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f);
}

// --- OKLab -------------------------------------------------------------

MTOkLab MTH_LinearRgbToOkLab(const MTLinearRgb &rgb)
{
	float l = 0.4122214708f * rgb.r + 0.5363325363f * rgb.g + 0.0514459929f * rgb.b;
	float m = 0.2119034982f * rgb.r + 0.6806995451f * rgb.g + 0.1073969566f * rgb.b;
	float s = 0.0883024619f * rgb.r + 0.2817188376f * rgb.g + 0.6299787005f * rgb.b;

	// std::cbrt, never powf(x, 1/3): LMS values legitimately go slightly
	// negative for out-of-gamut colours during the chroma search, and powf
	// returns NaN for those.
	float l_ = std::cbrt(l);
	float m_ = std::cbrt(m);
	float s_ = std::cbrt(s);

	MTOkLab lab;
	lab.L = 0.2104542553f * l_ + 0.7936177850f * m_ - 0.0040720468f * s_;
	lab.a = 1.9779984951f * l_ - 2.4285922050f * m_ + 0.4505937099f * s_;
	lab.b = 0.0259040371f * l_ + 0.7827717662f * m_ - 0.8086757660f * s_;
	return lab;
}

MTLinearRgb MTH_OkLabToLinearRgb(const MTOkLab &lab)
{
	float l_ = lab.L + 0.3963377774f * lab.a + 0.2158037573f * lab.b;
	float m_ = lab.L - 0.1055613458f * lab.a - 0.0638541728f * lab.b;
	float s_ = lab.L - 0.0894841775f * lab.a - 1.2914855480f * lab.b;

	float l = l_ * l_ * l_;
	float m = m_ * m_ * m_;
	float s = s_ * s_ * s_;

	MTLinearRgb rgb;
	rgb.r =  4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s;
	rgb.g = -1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s;
	rgb.b = -0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s;
	return rgb;
}

MTOkLch MTH_OkLabToOkLch(const MTOkLab &lab)
{
	MTOkLch lch;
	lch.L = lab.L;
	lch.C = std::sqrt(lab.a * lab.a + lab.b * lab.b);

	// 1e-4, not 1e-7: the measured float residual for an sRGB grey reaches
	// 9.5e-8 at 0.90, which is within 5% of a 1e-7 threshold -- with FMA
	// contraction or on another compiler the margin vanishes and h becomes
	// atan2 noise. 1e-4 chroma is far below anything perceptible.
	if (lch.C < 1e-4f)
	{
		lch.h = 0.0f;
		return lch;
	}

	float h = std::atan2(lab.b, lab.a) * 180.0f / kPi;
	if (h < 0.0f) h += 360.0f;
	lch.h = h;
	return lch;
}

MTOkLab MTH_OkLchToOkLab(const MTOkLch &lch)
{
	MTOkLab lab;
	lab.L = lch.L;
	float rad = lch.h * kPi / 180.0f;
	lab.a = lch.C * std::cos(rad);
	lab.b = lch.C * std::sin(rad);
	return lab;
}

// --- gamut -------------------------------------------------------------

bool MTH_LinearRgbInGamut(const MTLinearRgb &rgb, float eps)
{
	return rgb.r >= -eps && rgb.r <= 1.0f + eps &&
	       rgb.g >= -eps && rgb.g <= 1.0f + eps &&
	       rgb.b >= -eps && rgb.b <= 1.0f + eps;
}

static bool MTH_LchInGamut(float L, float C, float hueDeg)
{
	MTOkLch lch = { L, C, hueDeg };
	return MTH_LinearRgbInGamut(MTH_OkLabToLinearRgb(MTH_OkLchToOkLab(lch)));
}

float MTH_OkLchMaxChroma(float L, float hueDeg)
{
	// Even zero chroma out of gamut means L itself is outside [0,1].
	if (!MTH_LchInGamut(L, 0.0f, hueDeg))
		return 0.0f;

	// 0.5 is comfortably beyond the sRGB gamut's maximum OKLab chroma (~0.32).
	float lo = 0.0f, hi = 0.5f;
	if (MTH_LchInGamut(L, hi, hueDeg))
		return hi;

	for (int i = 0; i < 24; i++)
	{
		float mid = 0.5f * (lo + hi);
		if (MTH_LchInGamut(L, mid, hueDeg)) lo = mid; else hi = mid;
	}
	return lo;
}

MTOkLch MTH_OkLchClipChroma(const MTOkLch &lch)
{
	if (lch.C <= 0.0f)
		return lch;
	if (MTH_LchInGamut(lch.L, lch.C, lch.h))
		return lch;

	MTOkLch out = lch;					// hue preserved -- the whole point
	out.C = MTH_OkLchMaxChroma(lch.L, lch.h);
	return out;
}

// --- output ------------------------------------------------------------

static float MTH_Clamp01(float v)
{
	return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

void MTH_OkLchToSrgbF(const MTOkLch &lch, float outSrgb[3])
{
	MTOkLch clipped = MTH_OkLchClipChroma(lch);
	MTLinearRgb lin = MTH_OkLabToLinearRgb(MTH_OkLchToOkLab(clipped));
	outSrgb[0] = MTH_Clamp01(MTH_LinearToSrgb(MTH_Clamp01(lin.r)));
	outSrgb[1] = MTH_Clamp01(MTH_LinearToSrgb(MTH_Clamp01(lin.g)));
	outSrgb[2] = MTH_Clamp01(MTH_LinearToSrgb(MTH_Clamp01(lin.b)));
}

// --- WCAG 2.x ----------------------------------------------------------

float MTH_WcagLuminance(const MTLinearRgb &linear)
{
	return 0.2126f * linear.r + 0.7152f * linear.g + 0.0722f * linear.b;
}

float MTH_WcagContrast(float y1, float y2)
{
	float hi = y1 > y2 ? y1 : y2;
	float lo = y1 > y2 ? y2 : y1;
	return (hi + 0.05f) / (lo + 0.05f);
}

float MTH_OkLchWcagLuminance(const MTOkLch &lch)
{
	float srgb[3];
	MTH_OkLchToSrgbF(lch, srgb);
	MTLinearRgb lin = { MTH_SrgbToLinear(srgb[0]),
	                    MTH_SrgbToLinear(srgb[1]),
	                    MTH_SrgbToLinear(srgb[2]) };
	return MTH_WcagLuminance(lin);
}

// --- achromatic closed form -------------------------------------------

float MTH_GreyLLighterForContrast(float bgL, float ratio)
{
	float bgY = bgL * bgL * bgL;
	float y = ratio * (bgY + 0.05f) - 0.05f;
	// cbrt handles the negative case (an absurdly small ratio) without NaN;
	// callers test the returned L rather than trusting it.
	return std::cbrt(y);
}

float MTH_GreyLDarkerForContrast(float bgL, float ratio)
{
	float bgY = bgL * bgL * bgL;
	float y = (bgY + 0.05f) / ratio - 0.05f;
	return std::cbrt(y);
}
