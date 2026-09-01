#pragma once

#include "MTH_OkLab.h"
#include "imgui.h"
#include <vector>

// ---------------------------------------------------------------------------
// A theme is NOT a palette. It is ~9 numbers from which two 12-step OKLCh
// ramps are generated, plus an optional sparse override. Nine palettes of ~25
// colours would be 200-odd hand-tuned numbers that drift apart on first edit;
// generated palettes are consistent by construction and machine-checkable.
// ---------------------------------------------------------------------------

enum MTThemeMode : int
{
	MTThemeMode_Dark = 0,
	MTThemeMode_Light = 1
};
// NOTE: there is no MTThemeMode_System here on purpose. "Follow the OS" is a
// host-level policy that resolves to Dark or Light before it reaches the
// engine; the engine only ever renders a resolved mode. The resolution lives
// in VID_ResolveImGuiStyle / the host's settings, exactly as it does today.

struct CMTThemeSeed
{
	float neutralHue      = 0.0f;    // degrees; meaningless when neutralChroma == 0
	float neutralChroma   = 0.000f;  // 0.000 .. 0.015. Zero for any theme meant
	                                 // for colour judgement (design #2.1).
	float accentHue       = 250.0f;  // degrees
	float accentChroma    = 0.090f;  // pre-gamut-clip target
	float contrastGamma   = 1.0f;    // step distribution: L(i) = lo + (hi-lo)*t^gamma
	float bgAnchorDark    = 0.20f;   // OKLab L of ramp step 1  (= Surface in dark mode)
	float bgAnchorLight   = 0.97f;   // OKLab L of ramp step 12 (= Surface in light mode)
	float minContrastRatio = 4.5f;   // 4.5 normally, 7.0 for High Contrast
	bool  supportsLight   = true;    // Midnight sets false
};

// ---------------------------------------------------------------------------
// Semantic tokens. A draw site asks for a ROLE, never a colour, and never a
// ramp index. Ramp indices below are 1-BASED and are the DARK-mode reading;
// light mode reads neutral index i as ramp step (13 - i) -- see CMTTheme.
// ---------------------------------------------------------------------------

enum MTThemeToken : int
{
	MTThemeToken_Surface = 0,     // neutral[ 1]  window/page background
	MTThemeToken_SurfaceRaised,   // neutral[ 2]  controls, frames, table rows
	MTThemeToken_SurfaceOverlay,  // neutral[ 3]  popups, menus, tooltips
	MTThemeToken_BorderSubtle,    // neutral[ 5]  decorative separators
	MTThemeToken_BorderStrong,    // neutral[ 7]  meaningful component boundaries
	MTThemeToken_TextDisabled,    // neutral[ 6]
	MTThemeToken_TextMuted,       // neutral[ 8]
	MTThemeToken_TextSecondary,   // neutral[10]
	MTThemeToken_TextPrimary,     // neutral[12]
	MTThemeToken_AccentFocus,     // accent [ 9] dark / accent[5] light, then solved
	MTThemeToken_COUNT
};

// Declared AND defined inline here: this header has no .cpp, so an
// out-of-line definition would have no home. inline, not static, so every
// translation unit shares one definition.
inline const char *MT_ThemeTokenName(MTThemeToken token)
{
	switch (token)
	{
	case MTThemeToken_Surface:        return "Surface";
	case MTThemeToken_SurfaceRaised:  return "SurfaceRaised";
	case MTThemeToken_SurfaceOverlay: return "SurfaceOverlay";
	case MTThemeToken_BorderSubtle:   return "BorderSubtle";
	case MTThemeToken_BorderStrong:   return "BorderStrong";
	case MTThemeToken_TextDisabled:   return "TextDisabled";
	case MTThemeToken_TextMuted:      return "TextMuted";
	case MTThemeToken_TextSecondary:  return "TextSecondary";
	case MTThemeToken_TextPrimary:    return "TextPrimary";
	case MTThemeToken_AccentFocus:    return "AccentFocus";
	default:                          return "?";
	}
}

// ---------------------------------------------------------------------------
// Optional per-theme override. DATA on the theme, never an `if (theme == ...)`
// branch in the apply path. Exactly one theme needs it today (High Contrast),
// but the hook is general so the next need does not force a redesign.
// ---------------------------------------------------------------------------

struct MTOptFloat
{
	bool  set   = false;
	float value = 0.0f;
	MTOptFloat() {}
	MTOptFloat(float v) : set(true), value(v) {}
};

struct MTThemeRoleRemap
{
	// Initialised, unlike a POD: this struct reads as one, and a host that
	// fills `col` and forgets `token` would otherwise index the 10-element
	// colour array with whatever was on the stack.
	ImGuiCol     col   = 0;
	MTThemeToken token = MTThemeToken_Surface;
};

struct MTThemeOverride
{
	// Geometry deltas, applied AFTER the shared geometry table and BEFORE
	// scaling. Only the fields a theme has ever needed are exposed; adding one
	// is a two-line change here plus one in MT_ThemeApplyOverride.
	MTOptFloat frameBorderSize;
	MTOptFloat windowBorderSize;
	MTOptFloat childBorderSize;
	MTOptFloat popupBorderSize;
	MTOptFloat frameRounding;
	MTOptFloat windowRounding;

	// Colour-role remaps, applied after the base role table. High Contrast
	// uses this to point ImGuiCol_Border at BorderStrong: a 1px border drawn
	// in BorderSubtle is not an accessible boundary, and geometry alone cannot
	// say so (design #5.2 vs WCAG 1.4.11).
	std::vector<MTThemeRoleRemap> roles;
};

// ---------------------------------------------------------------------------
// Where a theme's tokens come from.
// ---------------------------------------------------------------------------

enum class CMTThemeSource
{
	Ramp,                // generated from the seed -- every shipping theme
	ImportedImGuiStyle   // sampled from a legacy ImGuiStyleType (diagnostic)
};

struct CMTThemeDef
{
	// Stable ASCII slug persisted in settings. NEVER an enum ordinal: adding a
	// sixth theme must not renumber the other five (design #7.3).
	const char *id = "";
	// Display label. Hosts pass an i18n key or a resolved string; the engine
	// only stores and returns the pointer, so it must outlive the registry
	// (a string literal or a static buffer -- not a std::string temporary).
	const char *label = "";

	CMTThemeSource source = CMTThemeSource::Ramp;
	CMTThemeSeed   seed;
	MTThemeOverride override;

	// Only meaningful when source == ImportedImGuiStyle: the ImGuiStyleType to
	// apply and sample. Stored as int so this header does not depend on
	// VID_Main.h (which depends on SDL).
	int importedStyleType = -1;
};

// ---------------------------------------------------------------------------
// Legacy-style visibility policy (design #6.2). Governs ONE thing: whether the
// 11 existing ImGuiStyleType values appear in the registry's enumeration. It
// does not, and must not, change how VID_SetImGuiStyle behaves.
// ---------------------------------------------------------------------------

enum class MTLegacyStylePolicy
{
	Show,      // default -- hosts that never call the registry are unaffected
	DevOnly,   // visible in development builds only
	Hidden     // the photo app release
};
