#include "MT_UiScale.h"

#include "DBG_Log.h"
#include "SYS_Main.h"
#include "VID_Main.h"
#include "MT_Theme.h"
#include "CMTThemeRegistry.h"

#include "imgui.h"

#include <SDL3/SDL.h>
#include <cstring>

static float gUiScale = 1.0f;

// The geometry as it was BEFORE we scaled it. Without this,
// MT_UiScaleApplyToImGuiStyle() is only correct when something else has just
// rebuilt the style from defaults -- true for a theme switch, false for a bare
// re-apply and false for a scale change. ScaleAllSizes multiplies IN PLACE, so
// the second call would square the scale.
static ImGuiStyle gUnscaledStyle;
static bool gHaveUnscaledStyle = false;

// What the last apply left behind, so "is this still the style we produced?"
// can be answered. Deliberately NOT ImGuiStyle::_MainScale: VID_SetImGuiStyle
// resets the geometry fields from a default ImGuiStyle but does NOT reset
// _MainScale, so _MainScale would still read as scaled while the geometry was
// back at 1.0 -- exactly the case this has to catch.
static float gAppliedFramePaddingY = -1.0f;
static float gAppliedScrollbarSize = -1.0f;

float MT_GetUiScale()
{
	return gUiScale;
}

float MT_UiScaled(float v)
{
	return v * gUiScale;
}

float MT_DetectDisplayUiScale()
{
#if defined(MACOS)
	// SDL hands us points here and imgui_impl_sdl3 puts the density in
	// DisplayFramebufferScale, so an ImGui unit is already a scaled point.
	return 1.0f;
#else
	if (gHeadlessMode)
		return 1.0f;

	// The window is the right question once it exists -- PER_MONITOR_AWARE_V2
	// means the answer differs per monitor. Before it exists (the engine asks
	// for a default window size during VID_Init) the primary display is where
	// a first-run window lands anyway.
	float scale = 0.0f;

	SDL_Window *window = VID_GetMainSDLWindow();
	if (window != NULL)
	{
		scale = SDL_GetWindowDisplayScale(window);
	}
	else
	{
		SDL_DisplayID display = SDL_GetPrimaryDisplay();
		if (display != 0)
			scale = SDL_GetDisplayContentScale(display);
	}

	if (!(scale > 0.0f))
	{
		// Not an error before SDL video init, and not worth a warning on every
		// headless or service start.
		return 1.0f;
	}

	return MT_ThemeClampGuiScale(scale);
#endif
}

void MT_UiScaleApplyToImGuiStyle()
{
	if (ImGui::GetCurrentContext() == NULL)
		return;

	ImGuiStyle &style = ImGui::GetStyle();

	CMTThemeRegistry *registry = CMTThemeRegistry::Instance();
	bool themeOwnsStyle = (registry != NULL && registry->HasActiveTheme());

	if (themeOwnsStyle)
	{
		// The theme built this style from a default ImGuiStyle and scaled it
		// exactly once, fonts included (MT_ThemeApplyResolved sets
		// FontScaleMain itself). Touching it here would scale it twice.
		gHaveUnscaledStyle = false;
		return;
	}

	style.FontScaleMain = gUiScale;

	// At 1.0 there is nothing to scale and something to lose: ScaleAllSizes
	// ImTruncs every field even by a factor of one, which would quietly turn
	// IntelliJ's 5.3 rounding and 6.5 spacing into 5 and 6. Scale 1.0 must be
	// byte-identical to the engine without this feature.
	if (gUiScale == 1.0f)
	{
		gHaveUnscaledStyle = false;
		gAppliedFramePaddingY = style.FramePadding.y;
		gAppliedScrollbarSize = style.ScrollbarSize;
		return;
	}

	// A user's own custom style is captured from the live Style Editor and
	// written to disk verbatim, so it was saved with this scale ALREADY baked
	// into every field; scaling it again on the next launch would compound and
	// keep compounding. The user owns that style's geometry; only its text
	// follows the display.
	if (VID_GetDefaultImGuiStyle() == IMGUI_STYLE_CUSTOM)
		return;

	bool styleIsStillOurs = gHaveUnscaledStyle
		&& style.FramePadding.y == gAppliedFramePaddingY
		&& style.ScrollbarSize == gAppliedScrollbarSize;

	if (styleIsStillOurs)
	{
		// Colours and the font/alpha fields are owned by other code (the Style
		// Editor, the font loader, ImGui's DPI path) and may have moved since
		// the snapshot; only the geometry comes back.
		ImVec4 colors[ImGuiCol_COUNT];
		memcpy(colors, style.Colors, sizeof(colors));
		float fontScaleDpi = style.FontScaleDpi;
		float fontSizeBase = style.FontSizeBase;
		float alpha = style.Alpha;
		float disabledAlpha = style.DisabledAlpha;

		style = gUnscaledStyle;

		memcpy(style.Colors, colors, sizeof(colors));
		style.FontScaleDpi = fontScaleDpi;
		style.FontSizeBase = fontSizeBase;
		style.Alpha = alpha;
		style.DisabledAlpha = disabledAlpha;
		style.FontScaleMain = gUiScale;
	}
	else
	{
		gUnscaledStyle = style;
		gHaveUnscaledStyle = true;
	}

	const ImGuiStyle preScale = style;
	style.ScaleAllSizes(gUiScale);
	MT_ThemeSnapBordersFrom(style, preScale);
	MT_ThemeRestoreVanishedDecorations(style, preScale);
	MT_ThemeEnforceImGuiStyleMinimums(style);

	gAppliedFramePaddingY = style.FramePadding.y;
	gAppliedScrollbarSize = style.ScrollbarSize;
}

void MT_SetUiScale(float scale)
{
	scale = MT_ThemeClampGuiScale(scale);
	if (scale == gUiScale)
		return;

	gUiScale = scale;
	LOGM("MT_SetUiScale: %.2f", gUiScale);

	CMTThemeRegistry *registry = CMTThemeRegistry::Instance();
	if (registry != NULL && registry->HasActiveTheme())
	{
		// Let the theme rescale itself: SetActiveTheme rebuilds geometry from a
		// default ImGuiStyle and scales exactly once, which is the only way
		// that neither compounds nor loses precision to ImTrunc.
		registry->SetActiveTheme(registry->GetActiveThemeId(),
								 registry->GetActiveMode(), gUiScale);
		return;
	}

	MT_UiScaleApplyToImGuiStyle();
}
