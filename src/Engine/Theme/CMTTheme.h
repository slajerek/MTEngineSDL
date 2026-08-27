#pragma once
#include "CMTThemeRamp.h"

// The resolved output of a (theme, mode) pair: opaque sRGB for every semantic
// token, plus enough provenance to explain a colour in a log line.
//
// Tokens are ALWAYS opaque. Alpha belongs to draw sites and to app-side domain
// tokens (the EXIF scrim, hover tints); an alpha-carrying engine token would
// composite differently depending on what happens to be underneath, which is
// exactly the property a contrast guarantee cannot have.
struct MTThemeResolved
{
	// The theme's ID, COPIED -- deliberately not a pointer to the def. The
	// registry stores defs in a container whose elements move when another
	// theme is registered, and MT_ThemeApplyResolved copies this struct into a
	// module-static slot, so a borrowed pointer dangles the moment the
	// registry changes. Callers needing the def look it up:
	// CMTThemeRegistry::Instance()->FindTheme(r.themeId).
	char               themeId[64] = {0};
	CMTThemeSource     source = CMTThemeSource::Ramp;
	MTThemeMode        mode = MTThemeMode_Dark;

	// The def's override, COPIED for the same reason themeId is copied. It has
	// to travel with the resolved palette: MT_ThemeApplyResolved is what turns
	// an override into an ImGuiStyle, it is handed only this struct, and the
	// theme it came from need not be registered anywhere (CTestThemeScale
	// resolves and applies a def it builds on the stack). Looking it up by id
	// instead would work only for registered themes and would silently do
	// nothing for every other caller -- the failure mode being "High Contrast
	// has no borders", with no error anywhere.
	MTThemeOverride    override;

	MTOkLch lch  [MTThemeToken_COUNT];   // post-solve, post-clip
	ImVec4  color[MTThemeToken_COUNT];   // sRGB, alpha always 1.0

	// True when the solver had to move a token to meet its rule. Purely
	// diagnostic -- the palette is valid either way.
	bool    solved[MTThemeToken_COUNT];

	// Set when a rule could NOT be met even at L = 0 or L = 1. The palette is
	// then out of contract and the theme's author must fix the seed. Never
	// silently tolerated: MT_ThemeResolve LOGErrors, the registry refuses to
	// activate the theme, and CTestThemeContrast fails.
	bool    outOfContract = false;

	ImVec4 Get(MTThemeToken t) const { return color[t]; }
};

// One contrast requirement. `ratio == 0` means "use seed.minContrastRatio";
// any other value is an absolute threshold that the seed does not scale.
struct MTThemeContrastRule
{
	MTThemeToken fg;
	MTThemeToken bg;
	float        ratio;
	const char  *why;      // cited in the failure message; WCAG clause or design ref
};

// The rule table, exposed so CTestThemeContrast checks exactly what the solver
// enforces rather than a second, drifting copy of it.
const MTThemeContrastRule *MT_ThemeContrastRules(int *outCount);

// Pure: touches no ImGui state, allocates nothing, ~50 us.
MTThemeResolved MT_ThemeResolve(const CMTThemeDef &def, MTThemeMode mode);

// For CMTThemeSource::ImportedImGuiStyle. `style` is the ImGuiStyle the legacy
// style produced; tokens are sampled from it (see the mapping in the .cpp).
MTThemeResolved MT_ThemeResolveImported(const CMTThemeDef &def,
                                        MTThemeMode mode,
                                        const ImGuiStyle &style);
