#pragma once

// Perceptual colour maths for the theme system. Björn Ottosson's OKLab
// (https://bottosson.github.io/posts/oklab/) plus the WCAG 2.x contrast
// definition. Deliberately free of ImGui and of every engine header: an error
// here contaminates all palettes at once, so it must be unit-testable alone.
//
// Conventions:
//   - "linear" means linear-light sRGB primaries, nominally [0,1] but allowed
//     out of range so out-of-gamut colours can be detected rather than clipped
//     silently.
//   - "srgb" means gamma-encoded sRGB in [0,1].
//   - hue is in DEGREES, [0,360). At C == 0 hue is meaningless, so it is
//     PINNED rather than left to atan2(0,0)'s noise. Two cases, and they
//     differ: MTH_OkLabToOkLch has no caller-supplied hue and must return
//     h == 0 exactly; functions that BUILD an MTOkLch from a requested hue
//     (the ramp, the solver) keep the requested hue, so a round trip through
//     them does not silently forget it.

struct MTLinearRgb { float r, g, b; };
struct MTOkLab     { float L, a, b; };
struct MTOkLch     { float L, C, h; };

// --- transfer function -------------------------------------------------
float MTH_SrgbToLinear(float c);
float MTH_LinearToSrgb(float c);

// --- OKLab -------------------------------------------------------------
MTOkLab     MTH_LinearRgbToOkLab(const MTLinearRgb &rgb);
MTLinearRgb MTH_OkLabToLinearRgb(const MTOkLab &lab);
MTOkLch     MTH_OkLabToOkLch(const MTOkLab &lab);
MTOkLab     MTH_OkLchToOkLab(const MTOkLch &lch);

// --- gamut -------------------------------------------------------------
// eps of 1e-4 (not 0) because the round trip's own error is ~1e-6 and a
// hairline overshoot must not cost a whole binary-search step.
bool  MTH_LinearRgbInGamut(const MTLinearRgb &rgb, float eps = 1e-4f);
// Largest chroma at this L and hue that stays in sRGB. 24 bisection steps
// (~6e-8 resolution); returns 0 when even C == 0 is out of gamut, which can
// only happen for L outside [0,1].
float MTH_OkLchMaxChroma(float L, float hueDeg);
// Same lch with C reduced to MTH_OkLchMaxChroma when needed; h preserved.
MTOkLch MTH_OkLchClipChroma(const MTOkLch &lch);

// --- output ------------------------------------------------------------
// Gamut-clips, converts, and clamps each channel to [0,1]. The clamp is a
// belt-and-braces final guard, not the gamut strategy: clipping chroma first
// preserves hue, clamping channels does not.
void MTH_OkLchToSrgbF(const MTOkLch &lch, float outSrgb[3]);

// --- WCAG 2.x ----------------------------------------------------------
// Relative luminance of a LINEAR rgb triple: 0.2126 R + 0.7152 G + 0.0722 B.
float MTH_WcagLuminance(const MTLinearRgb &linear);
// (max+0.05) / (min+0.05). Order-independent; result is >= 1.
float MTH_WcagContrast(float y1, float y2);
// Convenience: gamut-clip, convert, clamp, then luminance. This is the ONLY
// luminance a token may be judged by, because it is the luminance of the
// colour that will actually be drawn (post-clip, post-clamp), not of the
// idealised OKLCh value.
float MTH_OkLchWcagLuminance(const MTOkLch &lch);

// --- achromatic closed form -------------------------------------------
// For a neutral (C == 0) colour, linear R = G = B = L^3 exactly, so the WCAG
// luminance of a grey with OKLab lightness L is exactly L^3. That makes the
// contrast requirement invertible in closed form, with no search:
//
//   ratio = (Lt^3 + 0.05) / (Lb^3 + 0.05)     for text lighter than background
//
// Both functions return the required OKLab L. A result outside [0,1] means the
// requirement is UNACHIEVABLE against that background -- callers must test,
// not clamp silently.
float MTH_GreyLLighterForContrast(float bgL, float ratio);
float MTH_GreyLDarkerForContrast(float bgL, float ratio);
