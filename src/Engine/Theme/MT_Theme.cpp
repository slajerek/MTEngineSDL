#include "MT_Theme.h"
#include "VID_Main.h"       // VID_ApplyViewportStyleOverrides -- same library
#include "DBG_Log.h"
#include <cmath>

// Spell the type out, or MSVC greets you with an unresolved external.
//
// The ladder is dense where people actually live (80%-200%, one comfortable
// step apart) and sparse at the ends, which are there for different reasons:
// 250% and 300% for high-DPI panels and for reading at a distance, and
// 25%/50% for fitting far more of the UI on screen at once.
//
// THERE WAS A 10% STEP AND IT WAS REMOVED, which is worth recording because
// the reason is not "too small to be useful". At 10% an 18px base font is
// under 2px, so the combo box you would use to LEAVE 10% cannot be read
// either -- and the setting persists, so the state survived restarts. A
// control that can be entered and not exited is not a small setting, it is a
// one-way door, and 25% is the lowest rung that is still operable.
//
// The two guards that trap grew are deliberately kept, because they are about
// the ladder being changeable rather than about 10% specifically:
// MT_ThemeEnforceImGuiStyleMinimums (ScaleAllSizes truncates fields ImGui
// asserts positive) and PC_ClampGuiScaleForLoad on the app side. Both are
// no-ops at 25% today. That is the point -- they cost nothing now and they
// mean the next person to add a smaller rung does not repeat this.
const float MT_kGuiScaleSteps[MT_kGuiScaleStepCount] =
	{ 0.25f, 0.50f,
	  0.80f, 0.90f, 1.00f, 1.10f, 1.25f, 1.50f, 1.75f, 2.00f,
	  2.50f, 3.00f };

float MT_ThemeClampGuiScale(float scale)
{
	int best = 0;
	float bestD = std::fabs(scale - MT_kGuiScaleSteps[0]);
	for (int i = 1; i < MT_kGuiScaleStepCount; i++)
	{
		float d = std::fabs(scale - MT_kGuiScaleSteps[i]);
		if (d < bestD) { bestD = d; best = i; }
	}
	return MT_kGuiScaleSteps[best];
}

void MT_ThemeApplyGeometry(ImGuiStyle &style)
{
	// Spacing: every value is a step on the 2px scale. The rule that does the
	// work is that the gap BETWEEN groups exceeds the gap WITHIN a group; a
	// discrete scale makes that hard to break by accident.
	style.WindowPadding     = ImVec2(12, 12);   // space-5
	style.FramePadding      = ImVec2(10,  6);
	style.ItemSpacing       = ImVec2( 8,  6);   // space-4, space-3
	style.ItemInnerSpacing  = ImVec2( 6,  4);   // space-3, space-2
	style.CellPadding       = ImVec2( 8,  4);   // space-4, space-2
	style.IndentSpacing     = 16.0f;            // space-6
	style.ScrollbarSize     = 12.0f;            // space-5
	style.GrabMinSize       = 10.0f;

	// Rounding 3-4px, not 12. Heavy rounding reads as consumer software and
	// costs vertices: ImGui tessellates corners and the filmstrip draws
	// hundreds of rects per frame.
	style.WindowRounding    = 4.0f;
	style.ChildRounding     = 4.0f;
	style.FrameRounding     = 3.0f;
	style.PopupRounding     = 4.0f;
	style.TabRounding       = 3.0f;
	style.GrabRounding      = 3.0f;

	// FrameBorderSize 0 is deliberate: controls separate from the background
	// by SURFACE CONTRAST, not outline, and a border appears on FOCUS only,
	// in the accent. Calm at rest, unambiguous focus -- which in a
	// keyboard-driven app is a feature, not decoration. High Contrast
	// overrides this to 1 (WCAG 1.4.11).
	style.FrameBorderSize   = 0.0f;
	style.WindowBorderSize  = 1.0f;
	style.ChildBorderSize   = 1.0f;
	style.PopupBorderSize   = 1.0f;
	style.TabBorderSize     = 0.0f;
}

static float SnapOneBorder(float v)
{
	if (v <= 0.0f) return 0.0f;            // never invent a border
	float r = std::floor(v + 0.5f);
	return r < 1.0f ? 1.0f : r;            // never erase one either
}

// NO PRODUCTION CALLER as of S-6 -- MT_ThemeApplyResolved uses the two-argument
// form below, which is the only one that works now that ScaleAllSizes
// truncates. Kept because this is a library surface and "no app calls it" is
// not grounds for deletion, and kept IN STEP with the list below so the two
// cannot disagree about what a border is.
void MT_ThemeSnapBorders(ImGuiStyle &style)
{
	style.WindowBorderSize         = SnapOneBorder(style.WindowBorderSize);
	style.ChildBorderSize          = SnapOneBorder(style.ChildBorderSize);
	style.PopupBorderSize          = SnapOneBorder(style.PopupBorderSize);
	style.FrameBorderSize          = SnapOneBorder(style.FrameBorderSize);
	style.ImageBorderSize          = SnapOneBorder(style.ImageBorderSize);
	style.TabBorderSize            = SnapOneBorder(style.TabBorderSize);
	style.TabBarBorderSize         = SnapOneBorder(style.TabBarBorderSize);
	style.DragDropTargetBorderSize = SnapOneBorder(style.DragDropTargetBorderSize);
	style.SeparatorTextBorderSize  = SnapOneBorder(style.SeparatorTextBorderSize);
}

// The same snap, but told what each border was BEFORE scaling.
//
// ImGui 1.93.0 changed ScaleAllSizes to multiply the border sizes, which 1.92.6
// did not. That broke the one-argument form above at small scales and it broke
// it SILENTLY-looking: ImTrunc(1.0f * 0.25f) is 0, and SnapOneBorder is right to
// leave a 0 alone -- "never invent a border" -- because from the post-scale
// value alone it cannot tell a border truncated to nothing from one the theme
// deliberately switched off. So every 1px border vanished at 25% and 50%.
//
// Reading the pre-scale value removes the ambiguity instead of guessing: a
// border the theme asked for stays visible at every scale, a border it did not
// ask for is never conjured. This is also robust to upstream changing its mind
// again about whether ScaleAllSizes touches borders at all.
void MT_ThemeSnapBordersFrom(ImGuiStyle &style, const ImGuiStyle &preScale)
{
	// THIS LIST IS THE *BorderSize FAMILY, restored to their pre-scale values
	// because the theme's look is hairlines that stay one pixel at every UI
	// scale. At the vendored 1.93.0 (19293), ScaleAllSizes ImTruncs exactly
	// nine of them and all nine are here.
	//
	// BUT DO NOT DIFF ON THAT AXIS ON THE NEXT UPGRADE. "Fields whose name ends
	// in BorderSize" is a naming convention, not the defect class. The defect
	// class is: **any field ScaleAllSizes truncates, whose value is small
	// enough to reach 0 at a small scale, and whose consumer treats 0 as "do
	// not draw"**. Sorting by name is how the first fix of this bug landed
	// having missed four more fields with exactly the same symptom at exactly
	// the same scale rungs -- see MT_ThemeRestoreVanishedDecorations() below,
	// which handles the ones that must keep SCALING rather than be restored.
	//
	// It carried SIX for a while, and the three it was missing were a live
	// defect found by review during S-6, not by any test:
	//   * TabBarBorderSize defaults to 1.0, and ImTrunc(1.0 * s) is 0 at s =
	//     0.25, 0.50, 0.80 AND 0.90 -- four of the twelve rungs in
	//     MT_kGuiScaleSteps, two of them thoroughly ordinary UI scales. The
	//     tab-bar separator that "takes on the tab active color to denote
	//     focus" simply disappeared for anyone running at 80% or 90%.
	//   * DragDropTargetBorderSize defaults to 2.0 -> gone at 0.25.
	//   * ImageBorderSize defaults to 0.0, so it was harmless -- but a theme
	//     that sets it would have hit the same thing, and a list that is
	//     "complete except where the default happens to be zero" is not a list
	//     anybody can check.
	struct SBorder { float ImGuiStyle::*field; };
	static const SBorder kBorders[] = {
		{ &ImGuiStyle::WindowBorderSize },
		{ &ImGuiStyle::ChildBorderSize },
		{ &ImGuiStyle::PopupBorderSize },
		{ &ImGuiStyle::FrameBorderSize },
		{ &ImGuiStyle::ImageBorderSize },
		{ &ImGuiStyle::TabBorderSize },
		{ &ImGuiStyle::TabBarBorderSize },
		{ &ImGuiStyle::DragDropTargetBorderSize },
		{ &ImGuiStyle::SeparatorTextBorderSize },
	};
	// Borders are RESTORED to their pre-scale value, not scaled. That is the
	// theme's long-standing look -- hairlines that stay one pixel at every UI
	// scale -- and under 1.92.6 it came for free because ScaleAllSizes did not
	// touch them. Reproducing it explicitly keeps the upgrade from silently
	// changing the appearance: without this, 300% scale would draw 3px borders
	// and 25% would draw none.
	//
	// Whether borders SHOULD thicken with UI scale is a design question, not an
	// upgrade question. If it is ever reopened, this is the one place to change.
	for (const SBorder &b : kBorders)
	{
		const float before = preScale.*(b.field);
		style.*(b.field) = SnapOneBorder(before);
	}
}

// ---------------------------------------------------------------------------
// Decorations that ScaleAllSizes truncates to NOTHING at small UI scales.
//
// DIFFERENT FROM THE BORDER LIST ABOVE, deliberately. A border is restored to
// its pre-scale value because a hairline should stay a hairline. These are
// thicknesses that SHOULD grow with the UI -- a docking separator is a hit
// target, a colour marker is a glyph -- so they keep their scaled value and are
// only rescued from disappearing.
//
// The rule is "was visible before scaling, is invisible after": that
// distinguishes a decoration ImTrunc destroyed from one a theme deliberately
// switched off, which the post-scale value alone cannot. Same reasoning that
// forced MT_ThemeSnapBordersFrom to take the pre-scale style.
//
// WHY THESE FOUR, at 1.93.0 (19293):
//   TabBarOverlineSize    1.0 -> 0 at 0.25, 0.50, 0.80 AND 0.90. The selected
//                         tab's focus overline. Docking is on engine-wide and
//                         DockNodeUpdateTabBar always requests it, so at 80% or
//                         90% UI scale every docked app lost it silently.
//   TreeLinesSize         1.0 -> same four rungs. ImGuiTreeNodeFlags_DrawLines.
//   DockingSeparatorSize  2.0 -> 0 at 0.25.
//   ColorMarkerSize       3.0 -> 0 at 0.25.
// InputTextCursorSize and SeparatorSize are also truncated, and ImGui happens
// to clamp both downstream -- included anyway, because "safe because something
// else clamps it" is a fact about ImGui's internals that an upgrade can change
// without telling us, and the rescue costs one comparison.
void MT_ThemeRestoreVanishedDecorations(ImGuiStyle &style, const ImGuiStyle &preScale)
{
	struct SDecoration { float ImGuiStyle::*field; };
	static const SDecoration kDecorations[] = {
		{ &ImGuiStyle::TabBarOverlineSize },
		{ &ImGuiStyle::TreeLinesSize },
		{ &ImGuiStyle::ColorMarkerSize },
		{ &ImGuiStyle::DockingSeparatorSize },
		{ &ImGuiStyle::InputTextCursorSize },
		{ &ImGuiStyle::SeparatorSize },
	};
	for (const SDecoration &d : kDecorations)
	{
		if (preScale.*(d.field) > 0.0f && style.*(d.field) < 1.0f)
			style.*(d.field) = 1.0f;
	}
}

// ImGui ASSERTS these in NewFrame and does NOT enforce them in ScaleAllSizes,
// which truncates. That combination is a hard crash, not a cosmetic problem:
//
//     WindowBorderHoverPadding defaults to 4.0, and ImTrunc(4.0 * 0.10) is 0,
//     while NewFrame asserts it is > 0 -- "Required otherwise cannot resize
//     from borders". Selecting a 10% UI scale therefore aborted the process on
//     the very next frame. Worse, the scale PERSISTS, so the next launch
//     applied it again before the first frame and the app could not start at
//     all until settings.hjson was edited by hand.
//
// Enumerated rather than guessed: these are exactly the fields that
// ScaleAllSizes multiplies AND NewFrame asserts positive at ImGui 19259.
// WindowMinSize (32) survives down to 3%, so it has never been the one to
// break -- it is here because the NEXT time this bites, it should bite
// nothing.
//
// Clamping up rather than refusing the scale is right: the alternative is to
// forbid small scales, and a 1px hit-target at 10% is a perfectly reasonable
// thing to have when the whole UI is a tenth of its size.
void MT_ThemeEnforceImGuiStyleMinimums(ImGuiStyle &style)
{
	if (style.WindowMinSize.x < 1.0f) style.WindowMinSize.x = 1.0f;
	if (style.WindowMinSize.y < 1.0f) style.WindowMinSize.y = 1.0f;
	if (style.WindowBorderHoverPadding < 1.0f) style.WindowBorderHoverPadding = 1.0f;
}

struct MTRoleBinding { ImGuiCol col; MTThemeToken token; float alpha; };

// alpha < 1 on the translucent roles below (docking preview, text selection,
// both drag-drop roles, both nav-windowing roles, the modal dim, and
// BorderShadow at 0) is what ImGui itself expects there. Everything else is
// opaque, per the token contract.
static const MTRoleBinding kRoles[] =
{
	{ ImGuiCol_Text,                  MTThemeToken_TextPrimary,     1.00f },
	{ ImGuiCol_TextDisabled,          MTThemeToken_TextDisabled,    1.00f },
	{ ImGuiCol_WindowBg,              MTThemeToken_Surface,         1.00f },
	// TRANSPARENT, as all three stock styles have it. ChildBg is filled
	// unconditionally for every BeginChild, so an opaque Surface would paint a
	// Surface rectangle inside popups (SurfaceOverlay) and over Header
	// highlights alike. Transparent means a child inherits whatever it sits
	// on, which is right in every one of those cases; a child that wants its
	// own ground pushes one (CExifOverlay does).
	{ ImGuiCol_ChildBg,               MTThemeToken_Surface,         0.00f },
	{ ImGuiCol_PopupBg,               MTThemeToken_SurfaceOverlay,  1.00f },
	{ ImGuiCol_Border,                MTThemeToken_BorderSubtle,    1.00f },
	{ ImGuiCol_BorderShadow,          MTThemeToken_Surface,         0.00f },
	{ ImGuiCol_FrameBg,               MTThemeToken_SurfaceRaised,   1.00f },
	{ ImGuiCol_FrameBgHovered,        MTThemeToken_SurfaceOverlay,  1.00f },
	{ ImGuiCol_FrameBgActive,         MTThemeToken_BorderSubtle,    1.00f },
	{ ImGuiCol_TitleBg,               MTThemeToken_Surface,         1.00f },
	{ ImGuiCol_TitleBgActive,         MTThemeToken_SurfaceRaised,   1.00f },
	{ ImGuiCol_TitleBgCollapsed,      MTThemeToken_Surface,         1.00f },
	{ ImGuiCol_MenuBarBg,             MTThemeToken_SurfaceRaised,   1.00f },
	{ ImGuiCol_ScrollbarBg,           MTThemeToken_Surface,         1.00f },
	{ ImGuiCol_ScrollbarGrab,         MTThemeToken_BorderSubtle,    1.00f },
	{ ImGuiCol_ScrollbarGrabHovered,  MTThemeToken_BorderStrong,    1.00f },
	{ ImGuiCol_ScrollbarGrabActive,   MTThemeToken_TextMuted,       1.00f },
	// NEW IN IMGUI 1.93.0. The checkbox's own background when checked --
	// upstream's own styles make it a lerp from FrameBg toward FrameBgHovered
	// ("otherwise use FrameBg", imgui.h), so it is a RAISED SURFACE and not an
	// accent: the tick is the accent, and painting the box behind it accent
	// too would leave AccentFocus on AccentFocus. Bound to the same token as
	// FrameBgHovered, which is where upstream's 65% lerp lands.
	{ ImGuiCol_CheckboxSelectedBg,    MTThemeToken_SurfaceOverlay,  1.00f },
	{ ImGuiCol_CheckMark,             MTThemeToken_AccentFocus,     1.00f },
	{ ImGuiCol_SliderGrab,            MTThemeToken_AccentFocus,     1.00f },
	{ ImGuiCol_SliderGrabActive,      MTThemeToken_AccentFocus,     1.00f },
	{ ImGuiCol_Button,                MTThemeToken_SurfaceRaised,   1.00f },
	{ ImGuiCol_ButtonHovered,         MTThemeToken_SurfaceOverlay,  1.00f },
	{ ImGuiCol_ButtonActive,          MTThemeToken_BorderSubtle,    1.00f },
	{ ImGuiCol_Header,                MTThemeToken_SurfaceRaised,   1.00f },
	{ ImGuiCol_HeaderHovered,         MTThemeToken_SurfaceOverlay,  1.00f },
	{ ImGuiCol_HeaderActive,          MTThemeToken_BorderSubtle,    1.00f },
	{ ImGuiCol_Separator,             MTThemeToken_BorderSubtle,    1.00f },
	{ ImGuiCol_SeparatorHovered,      MTThemeToken_BorderStrong,    1.00f },
	{ ImGuiCol_SeparatorActive,       MTThemeToken_AccentFocus,     1.00f },
	{ ImGuiCol_ResizeGrip,            MTThemeToken_BorderSubtle,    1.00f },
	{ ImGuiCol_ResizeGripHovered,     MTThemeToken_BorderStrong,    1.00f },
	{ ImGuiCol_ResizeGripActive,      MTThemeToken_AccentFocus,     1.00f },
	{ ImGuiCol_Tab,                   MTThemeToken_Surface,         1.00f },
	{ ImGuiCol_TabHovered,            MTThemeToken_SurfaceOverlay,  1.00f },
	{ ImGuiCol_TabSelected,           MTThemeToken_SurfaceRaised,   1.00f },
	{ ImGuiCol_TabSelectedOverline,   MTThemeToken_AccentFocus,     1.00f },
	{ ImGuiCol_TabDimmed,             MTThemeToken_Surface,         1.00f },
	{ ImGuiCol_TabDimmedSelected,     MTThemeToken_SurfaceRaised,   1.00f },
	{ ImGuiCol_DockingPreview,        MTThemeToken_AccentFocus,     0.55f },
	{ ImGuiCol_DockingEmptyBg,        MTThemeToken_Surface,         1.00f },
	{ ImGuiCol_PlotLines,             MTThemeToken_TextSecondary,   1.00f },
	{ ImGuiCol_PlotLinesHovered,      MTThemeToken_AccentFocus,     1.00f },
	{ ImGuiCol_PlotHistogram,         MTThemeToken_AccentFocus,     1.00f },
	{ ImGuiCol_PlotHistogramHovered,  MTThemeToken_TextPrimary,     1.00f },
	{ ImGuiCol_TableHeaderBg,         MTThemeToken_SurfaceRaised,   1.00f },
	{ ImGuiCol_TableBorderStrong,     MTThemeToken_BorderStrong,    1.00f },
	{ ImGuiCol_TableBorderLight,      MTThemeToken_BorderSubtle,    1.00f },
	{ ImGuiCol_TableRowBg,            MTThemeToken_Surface,         1.00f },
	{ ImGuiCol_TableRowBgAlt,         MTThemeToken_SurfaceRaised,   1.00f },
	{ ImGuiCol_TextSelectedBg,        MTThemeToken_AccentFocus,     0.35f },
	{ ImGuiCol_DragDropTarget,        MTThemeToken_AccentFocus,     0.90f },
	{ ImGuiCol_NavCursor,             MTThemeToken_AccentFocus,     1.00f },
	{ ImGuiCol_NavWindowingHighlight, MTThemeToken_AccentFocus,     0.70f },
	// The two DIM roles are bound to TextPrimary, NOT Surface. A dim veil has
	// to move the page AWAY from its own surface: in light mode Surface is the
	// LIGHTEST token, so dimming with it washes the page toward white while
	// the modal on top (SurfaceOverlay) is darker -- the separation inverted.
	// ImGui's own styles do the same thing by hand: (0.8,0.8,0.8) in the dark
	// style, (0.2,0.2,0.2) in the light one. TextPrimary is the far end of the
	// ramp from Surface in BOTH modes, which is that rule expressed once.
	{ ImGuiCol_NavWindowingDimBg,     MTThemeToken_TextPrimary,     0.20f },
	{ ImGuiCol_ModalWindowDimBg,      MTThemeToken_TextPrimary,     0.35f },
	// The six roles below complete the enum. They are easy to miss -- none of
	// them exists in pre-1.92 style tables that a table like this gets copied
	// from -- and missing one is invisible until a user hits it, because an
	// unassigned role keeps whatever ImGuiStyle's default ctor left there,
	// which is StyleColorsDark. In light mode that means a dark-theme text
	// cursor and dark-theme hyperlinks.
	{ ImGuiCol_InputTextCursor,           MTThemeToken_TextPrimary,   1.00f },
	{ ImGuiCol_TabDimmedSelectedOverline, MTThemeToken_BorderSubtle,  1.00f },
	{ ImGuiCol_TextLink,                  MTThemeToken_AccentFocus,   1.00f },
	{ ImGuiCol_TreeLines,                 MTThemeToken_BorderSubtle,  1.00f },
	{ ImGuiCol_DragDropTargetBg,          MTThemeToken_AccentFocus,   0.20f },
	{ ImGuiCol_UnsavedMarker,             MTThemeToken_TextSecondary, 1.00f },
};

// The published palette. A COPY, not a pointer: the caller's MTThemeResolved
// is usually a stack temporary.
static MTThemeResolved sActiveResolved;
static bool            sHasActiveResolved = false;

const MTThemeResolved *MT_ThemeGetActiveResolved()
{
	return sHasActiveResolved ? &sActiveResolved : NULL;
}

void MT_ThemeClearActiveResolved()
{
	sHasActiveResolved = false;
}

static void MT_ThemeApplyOverride(ImGuiStyle &style, const MTThemeOverride &ov)
{
	if (ov.frameBorderSize.set)  style.FrameBorderSize  = ov.frameBorderSize.value;
	if (ov.windowBorderSize.set) style.WindowBorderSize = ov.windowBorderSize.value;
	if (ov.childBorderSize.set)  style.ChildBorderSize  = ov.childBorderSize.value;
	if (ov.popupBorderSize.set)  style.PopupBorderSize  = ov.popupBorderSize.value;
	if (ov.frameRounding.set)    style.FrameRounding    = ov.frameRounding.value;
	if (ov.windowRounding.set)   style.WindowRounding   = ov.windowRounding.value;
}

bool MT_ThemeApplyResolved(const MTThemeResolved &resolved, float guiScale)
{
	if (ImGui::GetCurrentContext() == NULL)
	{
		LOGError("MT_ThemeApplyResolved: no ImGui context -- refusing, and "
		         "saying so. Silently returning would leave the caller "
		         "believing a theme is live while the published palette is "
		         "still NULL.");
		return false;
	}

	// Self-assignment-safe: TS-1 Task 10 and TS-4's detector both pass in the
	// palette this function published last time.
	MTThemeResolved local = resolved;

	ImGuiStyle &live = ImGui::GetStyle();

	// Owned by other systems; a fresh ImGuiStyle would zero them.
	const float savedFontSizeBase = live.FontSizeBase;
	const float savedFontScaleDpi = live.FontScaleDpi;
	const float savedAlpha        = live.Alpha;
	const float savedDisabled     = live.DisabledAlpha;

	// ALWAYS from a default-constructed style, so the scale below is applied
	// exactly once to unscaled values. ScaleAllSizes is cumulative
	// (`_MainScale *= factor`) and lossy (ImTrunc): scaling an already-scaled
	// style makes the UI grow every time it is applied.
	ImGuiStyle style;
	MT_ThemeApplyGeometry(style);

	// The override travels ON the resolved palette (see CMTTheme.h): applying
	// it is what this function is for, and the theme need not be registered.
	MT_ThemeApplyOverride(style, local.override);

	// Colours. Coverage is asserted by walking the table, not by counting it:
	// a count assert passes happily with one role duplicated and another
	// missing, which is the mistake that actually happens.
	bool seen[ImGuiCol_COUNT] = {};
	for (int i = 0; i < (int)(sizeof(kRoles) / sizeof(kRoles[0])); i++)
	{
		const MTRoleBinding &b = kRoles[i];
		ImVec4 c = local.color[b.token];
		c.w = b.alpha;
		style.Colors[b.col] = c;
		seen[b.col] = true;
	}
	for (int c = 0; c < ImGuiCol_COUNT; c++)
	{
		if (!seen[c])
		{
			LOGError("MT_ThemeApplyResolved: ImGuiCol_ index %d (%s) has no role "
			         "binding -- it keeps StyleColorsDark's value and will look "
			         "wrong in light mode", c, ImGui::GetStyleColorName(c));
			IM_ASSERT(false && "kRoles does not cover every ImGuiCol_");
			break;
		}
	}

	// Per-theme role remaps, after the base table.
	{
		for (size_t i = 0; i < local.override.roles.size(); i++)
		{
			const MTThemeRoleRemap &rm = local.override.roles[i];
			// BOTH fields are range-checked. MTThemeRoleRemap reads as a POD,
			// so a host filling only `col` is a plausible mistake, and an
			// unchecked token indexes a 10-element array out of bounds.
			if (rm.col < 0 || rm.col >= ImGuiCol_COUNT) continue;
			if (rm.token < 0 || rm.token >= MTThemeToken_COUNT)
			{
				LOGError("MT_ThemeApplyResolved: role remap for ImGuiCol_ %d has "
				         "an out-of-range token (%d) -- skipping", (int)rm.col,
				         (int)rm.token);
				continue;
			}
			ImVec4 c = local.color[rm.token];
			c.w = style.Colors[rm.col].w;      // keep the role's expected alpha
			style.Colors[rm.col] = c;
		}
	}

	const float scale = MT_ThemeClampGuiScale(guiScale);
	const ImGuiStyle preScale = style;   // borders are snapped against THIS
	style.ScaleAllSizes(scale);
	MT_ThemeSnapBordersFrom(style, preScale);
	// And the decorations that keep their scaled size but must not vanish.
	MT_ThemeRestoreVanishedDecorations(style, preScale);
	// AFTER scaling: ScaleAllSizes truncates, and two of the fields it
	// truncates are ones ImGui asserts positive on the next NewFrame.
	MT_ThemeEnforceImGuiStyleMinimums(style);

	style.FontSizeBase  = savedFontSizeBase;
	style.FontScaleDpi  = savedFontScaleDpi;
	style.FontScaleMain = scale;               // the one place guiScale reaches fonts
	style.Alpha         = savedAlpha;
	style.DisabledAlpha = savedDisabled;

	live = style;

	sActiveResolved    = local;
	sHasActiveResolved = true;

	// Keeps square, opaque platform windows across a theme apply -- the same
	// fix-up VID_SetImGuiStyle re-applies. Without it, MT_ThemeApplyResolved's
	// fresh style (WindowRounding 4) would undo it the moment a theme is
	// active.
	VID_ApplyViewportStyleOverrides();
	return true;
}

// --- typography roles --------------------------------------------------

float MT_ThemeFontMultiplier(MTThemeFont role)
{
	switch (role)
	{
	case MTThemeFont_Display: return 1.25f;
	case MTThemeFont_Body:    return 1.00f;
	case MTThemeFont_Label:   return 0.875f;
	case MTThemeFont_Mono:    return 1.00f;
	default:                  return 1.00f;
	}
}

float MT_ThemeFontSize(MTThemeFont role)
{
	if (ImGui::GetCurrentContext() == NULL)
		return 0.0f;
	// FontSizeBase, NOT GetFontSize(): the latter already has FontScaleMain
	// and FontScaleDpi applied, and multiplying again is the classic
	// double-scale bug.
	return ImGui::GetStyle().FontSizeBase * MT_ThemeFontMultiplier(role);
}
