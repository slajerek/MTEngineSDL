#pragma once

//
// THE ENGINE-WIDE UI SCALE.
//
// Why this is here and not in an app: SDL3 declares PER_MONITOR_AWARE_V2 for
// the process (WIN_InitDPIAwareness), so on Windows and Linux the app is handed
// REAL PHYSICAL PIXELS and one ImGui unit is one of them. On macOS SDL reports
// points and imgui_impl_sdl3 carries the density in DisplayFramebufferScale, so
// one ImGui unit is already a scaled point. That asymmetry is a property of
// THIS ENGINE's SDL and ImGui integration, not of any app, so every app that
// asks "how much must I scale?" has to be told the same answer -- and the
// engine's own legacy views need it too. See docs/hidpi-ui-scaling.md.
//
// WHAT IT DOES NOT DO, and why. It does not make CGuiButton and friends scale
// themselves. A legacy control renders at posX/posY that its parent view
// derived from an ImGui window rect -- already device pixels -- while its
// sizeX/sizeY/fontScale come from constants the CALLER wrote. A constructor
// cannot tell those apart, so auto-scaling would leave positions and sizes in
// different units and double-scale any caller that had already converted. The
// contract is therefore the other way round: a FIXED PIXEL CONSTANT is scaled
// where it is written, with MT_UiScaled(), by the engine for its own controls
// and by each app for its own.
//

class CGuiView;

// The scale in force. 1.0 until MT_SetUiScale() is called, so an app that never
// opts in behaves exactly as it did before this existed.
float MT_GetUiScale();

// A fixed pixel constant, scaled for the display. Use it at the point the
// constant is written: a button's height, a legacy view's font size, a gap.
//
// Do NOT use it on a value already derived from a view's own rect -- that rect
// is in ImGui units and has therefore already grown, so scaling it again
// doubles.
float MT_UiScaled(float v);

// Sets the scale and re-applies it to the live ImGui style. Clamped to the
// MT_kGuiScaleSteps ladder (MT_Theme.h): discrete on purpose, so every rung can
// be pixel-checked and the font atlas cannot grow unbounded.
//
// If a theme owns the style, the theme is re-applied at the new scale rather
// than the style being scaled underneath it -- the theme rebuilds geometry from
// a default ImGuiStyle and scales exactly once, which is the only way that does
// not compound.
void MT_SetUiScale(float scale);

// What the display asks for, snapped to the ladder.
//
// 1.0 on macOS: the OS already scaled, and doing it again would double. 1.0 in
// headless mode: a test suite that asserts on pixel geometry must not move with
// the build machine's monitor. Answers from the main window when there is one
// and from the primary display before it exists, because the engine asks for a
// default window size during VID_Init.
float MT_DetectDisplayUiScale();

// Writes FontScaleMain and, when no theme owns the style, scales the geometry
// table. IDEMPOTENT: it keeps its own copy of the pre-scale geometry and puts
// it back before scaling, because ImGuiStyle::ScaleAllSizes multiplies in place
// -- calling it twice on the same style squares the scale and ImTruncs twice.
//
// Called from VID_FinishStyleChange(), which is the tail of EVERY style change,
// so the scale survives a theme switch, a macOS system-appearance flip and a
// custom-style load without the app having to notice and re-assert it.
void MT_UiScaleApplyToImGuiStyle();
