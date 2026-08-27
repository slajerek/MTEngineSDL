#pragma once
#include "CMTTheme.h"

// The discrete UI scale steps (design #5.5). Discrete, not a slider: every
// value can be tested and pixel-checked, and it bounds font-atlas growth.
//
// The COUNT is an enum, not `extern const int`: TS-3 uses it as an array bound
// (`char labels[MT_kGuiScaleStepCount][8]`), and an extern const int with no
// visible initialiser is not an integral constant expression -- clang and gcc
// accept the VLA as an extension, MSVC rejects it outright (C2131). Same
// reason MT_RAMP_STEPS is an enum.
enum { MT_kGuiScaleStepCount = 12 };           // 0.25 .. 3.00
extern const float MT_kGuiScaleSteps[MT_kGuiScaleStepCount];
float MT_ThemeClampGuiScale(float scale);      // snaps to the nearest step

// Writes the shared geometry table (design #5.1 / #5.2) into `style` at scale
// 1.0. Colours untouched. Public so a host can build a style without a theme.
void MT_ThemeApplyGeometry(ImGuiStyle &style);

// Borders to integers, minimum 1 where non-zero. At 19259 ScaleAllSizes does
// not scale border sizes at all, so this is a guard against a future ImGui
// adding them to that list -- not a fix for a defect that exists today.
void MT_ThemeSnapBorders(ImGuiStyle &style);
// Snap borders knowing what they were BEFORE ScaleAllSizes. Needed since
// ImGui 1.93.0, which scales border sizes (1.92.6 did not) -- post-scale a
// 1px border reads as 0 at 25% and is indistinguishable from a border the
// theme switched off deliberately.
void MT_ThemeSnapBordersFrom(ImGuiStyle &style, const ImGuiStyle &preScale);

// Rescue decorations ScaleAllSizes truncated to nothing at a small UI scale.
// Needs the PRE-SCALE style to tell "ImTrunc destroyed it" from "the theme
// switched it off". See the .cpp for which fields and why they are not simply
// added to the border list.
void MT_ThemeRestoreVanishedDecorations(ImGuiStyle &style, const ImGuiStyle &preScale);

// Restore the minimums ImGui ASSERTS on in NewFrame but does not enforce in
// ScaleAllSizes. Two fields today (WindowMinSize, WindowBorderHoverPadding);
// see the definition for why this is a real crash and not a theoretical one.
// Must run AFTER ScaleAllSizes.
void MT_ThemeEnforceImGuiStyleMinimums(ImGuiStyle &style);

// Build a complete ImGuiStyle from resolved tokens. ALWAYS from a
// default-constructed ImGuiStyle, ALWAYS scaled exactly once. Never scales an
// already-scaled style: ScaleAllSizes does `_MainScale *= scale_factor` and
// ImTruncs every field, so applying it twice both compounds and loses
// precision. The failure mode is "the UI grows every time you open Settings",
// which is rarely traced back to its cause. Guarded by CTestThemeScale.
//
// FontSizeBase and FontScaleDpi are PRESERVED from the live style -- they are
// owned by the font loader and by ImGui's DPI path respectively, and a fresh
// ImGuiStyle would zero them. So are Alpha and DisabledAlpha, which hosts
// animate; everything else in ImGuiStyle comes from the geometry table.
//
// PUBLISHES: this function stores a COPY of `resolved` as the active palette,
// so MT_ThemeGetActiveResolved() returns it afterwards. That is load-bearing
// for TS-4's detector, which applies a deliberately shifted palette that no
// registry entry produces and then asks the app-side token layer to recompute
// against it. Callers that apply a palette without wanting to publish it must
// restore the previous one -- MT_ThemeClearActiveResolved() clears it.
// Self-assignment-safe: `resolved` may alias the published palette (TS-1
// Task 10 and TS-4's detector both do exactly that).
// Returns FALSE and applies nothing when there is no ImGui context. Callers
// must not report success on a false return: the registry would otherwise sit
// at HasActiveTheme() == true while MT_ThemeGetActiveResolved() == NULL, which
// is exactly the disagreement CMTThemeRegistry promises cannot happen, and
// every app-side domain token would resolve against NULL.
bool MT_ThemeApplyResolved(const MTThemeResolved &resolved, float guiScale);

// The resolved tokens currently applied, or NULL when none has been.
// App-side domain tokens (design #3.4) resolve against this. Returns NULL in
// every host that never registers a theme, which is how "the engine gets
// machinery, the app gets content" stays true.
const MTThemeResolved *MT_ThemeGetActiveResolved();
void MT_ThemeClearActiveResolved();            // back to NULL; does not touch ImGuiStyle

// --- typography roles (TS-6 does the visible work) ----------------------
// Here because TS-4 and TS-5 migrate draw sites that want a role-relative
// size and must not invent their own multiplier constants in the meantime.
enum MTThemeFont : int
{
	MTThemeFont_Display = 0,   // 1.25x -- pane and section headings
	MTThemeFont_Body,          // 1.00x -- everything by default
	MTThemeFont_Label,         // 0.875x -- captions
	MTThemeFont_Mono,          // 1.00x -- numerals that must not jitter
	MTThemeFont_COUNT
};

float MT_ThemeFontMultiplier(MTThemeFont role);
// style.FontSizeBase * multiplier. Pass the RESULT to PushFont(NULL, size) --
// never ImGui::GetFontSize(), which already has the global factors applied and
// would compound them (imgui.h:534).
float MT_ThemeFontSize(MTThemeFont role);
