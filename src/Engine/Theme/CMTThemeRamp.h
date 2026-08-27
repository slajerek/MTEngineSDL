#pragma once
#include "CMTThemeSeed.h"

// 12 steps, 1-based in every comment and in the token table, 0-based in the
// arrays. MT_RAMP_STEPS is the single source of that number: nothing else may
// hardcode 12, and nothing else may hardcode 13 for the inversion.
enum { MT_RAMP_STEPS = 12 };

// Ramp step i (1-based) has OKLab lightness
//     L(i) = lo + (hi - lo) * ((i-1) / (MT_RAMP_STEPS-1)) ^ contrastGamma
// with lo = seed.bgAnchorDark and hi = seed.bgAnchorLight, and chroma clipped
// into sRGB at that lightness. Step 1 is ALWAYS the darkest.
//
// ONE ramp serves both modes. Dark mode reads index i as steps[i-1]; light
// mode reads it as steps[MT_RAMP_STEPS - i]. That is the whole of "light is a
// true twin, not a second palette" -- a token cannot forget about mode,
// because no token knows a colour.
struct CMTThemeRamp
{
	MTOkLch neutral[MT_RAMP_STEPS];
	MTOkLch accent [MT_RAMP_STEPS];
};

// Pure. ~50 microseconds; called once per theme change, never per frame.
CMTThemeRamp MT_ThemeGenerateRamp(const CMTThemeSeed &seed);

// 1-based index -> 0-based array subscript for the given mode. The ONLY place
// the inversion is expressed; every caller goes through it.
//   Dark : step(i) = i - 1
//   Light: step(i) = MT_RAMP_STEPS - i
int MT_ThemeNeutralStepIndex(int oneBasedIndex, MTThemeMode mode);
