#include "CTestThemeApply.h"
#include "MT_Theme.h"
#include "CMTThemeRegistry.h"
#include "VID_Main.h"
#include <cstring>
#include "DBG_Log.h"
#include <cmath>
#include <cstdio>

#define TS_ASSERT(cond, msg) \
	do { \
		if (!(cond)) { \
			char _b[320]; \
			snprintf(_b, sizeof(_b), "FAIL: %s", msg); \
			LOGD("CTestThemeScale: %s", _b); \
			TestCompleted(false, _b); \
			return; \
		} \
		StepCompleted(stepNum++, true, msg); \
	} while(0)

void CTestThemeScale::Teardown()
{
	if (hasSavedStyle && ImGui::GetCurrentContext() != NULL)
		ImGui::GetStyle() = savedStyle;
	hasSavedStyle = false;
	// Essential now that MT_ThemeApplyResolved PUBLISHES: CTestThemeRegistry
	// runs after this one and its first assertion is that no palette is
	// active.
	MT_ThemeClearActiveResolved();
}

void CTestThemeScale::Run(ITestCallback *cb)
{
	this->callback = cb;
	isRunning = true;
	int stepNum = 1;

	if (ImGui::GetCurrentContext() == NULL)
	{
		TestCompleted(false, "no ImGui context");
		return;
	}
	savedStyle = ImGui::GetStyle();
	hasSavedStyle = true;

	// Scale idempotence -- the reason this test exists. ScaleAllSizes is
	// cumulative (`_MainScale *= scale_factor`) and lossy (ImTrunc), so
	// applying a theme twice at the same scale must still produce the same
	// style, and applying it at scale A then scale B must equal applying it
	// at B from scratch.
	CMTThemeDef def; def.id = "t"; def.label = "t";
	MTThemeResolved r = MT_ThemeResolve(def, MTThemeMode_Dark);

	MT_ThemeApplyResolved(r, 1.0f);
	ImGuiStyle once = ImGui::GetStyle();
	MT_ThemeApplyResolved(r, 1.0f);
	ImGuiStyle twice = ImGui::GetStyle();
	TS_ASSERT(once.WindowPadding.x == twice.WindowPadding.x &&
	          once.FramePadding.y  == twice.FramePadding.y  &&
	          once.ScrollbarSize   == twice.ScrollbarSize   &&
	          once.IndentSpacing   == twice.IndentSpacing,
	          "applying the same theme twice is idempotent");

	MT_ThemeApplyResolved(r, 1.50f);
	ImGuiStyle a = ImGui::GetStyle();
	MT_ThemeApplyResolved(r, 1.00f);
	MT_ThemeApplyResolved(r, 1.50f);
	ImGuiStyle b = ImGui::GetStyle();
	TS_ASSERT(a.WindowPadding.x == b.WindowPadding.x &&
	          a.ItemSpacing.y   == b.ItemSpacing.y   &&
	          a.ScrollbarSize   == b.ScrollbarSize,
	          "scale is never applied on top of an already-scaled style");

	// Scale actually scales.
	MT_ThemeApplyResolved(r, 1.0f);
	float pad1 = ImGui::GetStyle().WindowPadding.x;
	MT_ThemeApplyResolved(r, 2.0f);
	float pad2 = ImGui::GetStyle().WindowPadding.x;
	TS_ASSERT(pad2 > pad1 * 1.8f, "guiScale 2.0 roughly doubles padding");

	// Borders stay hairlines at every step -- and at the VENDORED 19293 this
	// loop is the live regression test for that, not a formality.
	//
	// ScaleAllSizes DOES scale border sizes now: imgui.cpp ImTruncs NINE
	// *BorderSize fields -- Window, Child, Popup, Frame, Image, Tab, TabBar,
	// DragDropTarget, SeparatorText. It did not at 19259, and these comments
	// still said 19259 months after the upgrade.
	//
	// MT_ThemeSnapBordersFrom() -- the TWO-argument form, called from
	// MT_ThemeApplyResolved -- is what puts them back. Not MT_ThemeSnapBorders:
	// the one-argument form has no production caller, because from a
	// post-scale value alone it cannot tell a border truncated to nothing from
	// one the theme deliberately switched off.
	for (int i = 0; i < MT_kGuiScaleStepCount; i++)
	{
		MT_ThemeApplyResolved(r, MT_kGuiScaleSteps[i]);
		const ImGuiStyle &s = ImGui::GetStyle();
		TS_ASSERT(s.WindowBorderSize == 1.0f && s.ChildBorderSize == 1.0f &&
		          s.PopupBorderSize == 1.0f,
		          "1px borders stay exactly 1px at every scale step");
		TS_ASSERT(s.FrameBorderSize == 0.0f,
		          "a zero border stays zero -- snapping never invents a border");

		// THE THREE THAT WERE NOT RESTORED, and which no assertion here
		// covered until S-6 found them by review.
		//
		// ScaleAllSizes truncates NINE *BorderSize fields at 19293;
		// MT_ThemeSnapBordersFrom restored six. TabBarBorderSize defaults to
		// 1.0 and ImTrunc(1.0 * s) is 0 at 0.25, 0.50, 0.80 AND 0.90 -- four of
		// the twelve rungs -- so the tab-bar focus separator vanished for
		// anyone running at 80% or 90% UI scale, with a green suite. Asserting
		// the DEFAULTS here rather than "non-zero" is deliberate: it also
		// catches the opposite failure, a border silently thickening at 300%.
		char bmsg[192];
		snprintf(bmsg, sizeof(bmsg),
		         "scale %.2f: TabBarBorderSize %.3f == 1.0 (the tab focus separator)",
		         MT_kGuiScaleSteps[i], s.TabBarBorderSize);
		TS_ASSERT(s.TabBarBorderSize == 1.0f, bmsg);
		snprintf(bmsg, sizeof(bmsg),
		         "scale %.2f: DragDropTargetBorderSize %.3f == 2.0",
		         MT_kGuiScaleSteps[i], s.DragDropTargetBorderSize);
		TS_ASSERT(s.DragDropTargetBorderSize == 2.0f, bmsg);
		snprintf(bmsg, sizeof(bmsg),
		         "scale %.2f: SeparatorTextBorderSize %.3f == 3.0",
		         MT_kGuiScaleSteps[i], s.SeparatorTextBorderSize);
		TS_ASSERT(s.SeparatorTextBorderSize == 3.0f, bmsg);
		// ImageBorderSize defaults to 0 and must STAY 0 -- the "never invent a
		// border" half of the contract, on a field that is now in the list.
		TS_ASSERT(s.ImageBorderSize == 0.0f,
		          "ImageBorderSize stays 0 -- in the snap list, but never conjured");

		// AND THE DECORATIONS THAT ARE NOT BORDERS.
		//
		// The first fix of the vanishing-border bug sorted by field NAME and so
		// missed four more fields with the identical symptom at the identical
		// scale rungs. These do NOT get restored to their pre-scale value -- a
		// docking separator is a hit target and should grow with the UI -- they
		// are only rescued from reaching zero. So the assertion is ">= 1", not
		// "== default": it must hold at 25% and at 300% alike.
		snprintf(bmsg, sizeof(bmsg),
		         "scale %.2f: TabBarOverlineSize %.3f >= 1 (the selected-tab focus overline)",
		         MT_kGuiScaleSteps[i], s.TabBarOverlineSize);
		TS_ASSERT(s.TabBarOverlineSize >= 1.0f, bmsg);
		snprintf(bmsg, sizeof(bmsg),
		         "scale %.2f: TreeLinesSize %.3f >= 1", MT_kGuiScaleSteps[i], s.TreeLinesSize);
		TS_ASSERT(s.TreeLinesSize >= 1.0f, bmsg);
		snprintf(bmsg, sizeof(bmsg),
		         "scale %.2f: DockingSeparatorSize %.3f >= 1", MT_kGuiScaleSteps[i], s.DockingSeparatorSize);
		TS_ASSERT(s.DockingSeparatorSize >= 1.0f, bmsg);
		snprintf(bmsg, sizeof(bmsg),
		         "scale %.2f: ColorMarkerSize %.3f >= 1", MT_kGuiScaleSteps[i], s.ColorMarkerSize);
		TS_ASSERT(s.ColorMarkerSize >= 1.0f, bmsg);
		snprintf(bmsg, sizeof(bmsg),
		         "scale %.2f: InputTextCursorSize %.3f >= 1", MT_kGuiScaleSteps[i], s.InputTextCursorSize);
		TS_ASSERT(s.InputTextCursorSize >= 1.0f, bmsg);
		snprintf(bmsg, sizeof(bmsg),
		         "scale %.2f: SeparatorSize %.3f >= 1", MT_kGuiScaleSteps[i], s.SeparatorSize);
		TS_ASSERT(s.SeparatorSize >= 1.0f, bmsg);

		// IMGUI'S OWN NewFrame PRECONDITIONS, checked here because failing
		// them is a hard abort one frame later and the assert fires inside
		// ImGui with no clue which style field did it.
		//
		// This is not hypothetical: WindowBorderHoverPadding defaults to 4.0,
		// ScaleAllSizes TRUNCATES, and ImTrunc(4.0 * 0.10) is 0 -- so the
		// moment the scale ladder gained a 10% step, selecting it killed the
		// process. And because the scale persists, the next launch applied it
		// again before the first frame and the app could not start.
		char pre[160];
		snprintf(pre, sizeof(pre),
		         "scale %.2f: WindowBorderHoverPadding %.3f > 0 (ImGui asserts this in NewFrame)",
		         MT_kGuiScaleSteps[i], s.WindowBorderHoverPadding);
		TS_ASSERT(s.WindowBorderHoverPadding > 0.0f, pre);
		snprintf(pre, sizeof(pre),
		         "scale %.2f: WindowMinSize %.1fx%.1f >= 1 (ImGui asserts this in NewFrame)",
		         MT_kGuiScaleSteps[i], s.WindowMinSize.x, s.WindowMinSize.y);
		TS_ASSERT(s.WindowMinSize.x >= 1.0f && s.WindowMinSize.y >= 1.0f, pre);
	}

	// FontScaleMain carries the scale; FontSizeBase and FontScaleDpi are the
	// font loader's and ImGui's, and must survive a theme apply untouched.
	ImGui::GetStyle().FontSizeBase = 18.0f;
	ImGui::GetStyle().FontScaleDpi = 2.0f;
	MT_ThemeApplyResolved(r, 1.25f);
	TS_ASSERT(ImGui::GetStyle().FontScaleMain == 1.25f,
	          "guiScale lands on FontScaleMain");
	TS_ASSERT(ImGui::GetStyle().FontSizeBase == 18.0f,
	          "FontSizeBase survives a theme apply");
	TS_ASSERT(ImGui::GetStyle().FontScaleDpi == 2.0f,
	          "FontScaleDpi survives a theme apply");
	ImGui::GetStyle().FontScaleDpi = 1.0f;

	// MT_ThemeSnapBorders directly, as a unit test of the helper itself.
	//
	// This used to say "the loop above cannot exercise it: ScaleAllSizes
	// provably never touches border fields at 19259". The 19259 REASONING is
	// dead -- at the vendored 19293 ScaleAllSizes truncates nine border fields
	// -- but the CONCLUSION still holds, for a different reason:
	// MT_ThemeApplyResolved calls MT_ThemeSnapBordersFrom(), the two-argument
	// form, so the loop above covers THAT one and never reaches this one.
	//
	// So this block is still the ONLY coverage of MT_ThemeSnapBorders, which
	// today has no production caller at all. (Review round 1 of S-6 rewrote
	// this comment to say the opposite; round 2 caught it. Getting it wrong in
	// this direction is worse than the stale version was: it invites someone to
	// delete the only test of a library function on the belief that the loop
	// covers it.) Feed it the values it exists for.
	{
		// ALL NINE FIELDS, not the four this probe used to set. The list grew
		// from six to nine during S-6 and the probe did not follow, so deleting
		// any of the five it ignored would have failed nothing -- in the ONLY
		// direct test of a function with no production caller. That is the
		// project's recurring "production writes it, only tests read it" shape
		// with the test half half-written.
		ImGuiStyle probe;
		probe.WindowBorderSize         = 1.35f;  // what a future ImGui that DID
		probe.ChildBorderSize          = 0.40f;  // scale borders would hand us
		probe.FrameBorderSize          = 0.0f;
		probe.PopupBorderSize          = 2.60f;
		probe.ImageBorderSize          = 0.0f;
		probe.TabBorderSize            = 0.05f;
		probe.TabBarBorderSize         = 0.40f;
		probe.DragDropTargetBorderSize = 2.60f;
		probe.SeparatorTextBorderSize  = 1.50f;
		MT_ThemeSnapBorders(probe);
		TS_ASSERT(probe.WindowBorderSize == 1.0f, "1.35 snaps to a 1px hairline");
		TS_ASSERT(probe.ChildBorderSize  == 1.0f, "a sub-pixel border is raised to 1, not erased");
		TS_ASSERT(probe.FrameBorderSize  == 0.0f, "a zero border stays zero -- snapping never invents one");
		TS_ASSERT(probe.PopupBorderSize  == 3.0f, "2.60 rounds to 3");
		TS_ASSERT(probe.ImageBorderSize          == 0.0f, "image border: zero stays zero");
		TS_ASSERT(probe.TabBorderSize            == 1.0f, "tab border: 0.05 is raised to a hairline, not erased");
		TS_ASSERT(probe.TabBarBorderSize         == 1.0f, "tab-bar border: 0.40 is raised to a hairline");
		TS_ASSERT(probe.DragDropTargetBorderSize == 3.0f, "drag-drop target border: 2.60 rounds to 3");
		TS_ASSERT(probe.SeparatorTextBorderSize  == 2.0f, "separator-text border: 1.50 rounds to 2");
	}

	// Clamping.
	// 0.01, not 0.10: 10% became a real step when the ladder was widened, so
	// the old probe was asserting an exact match rather than a clamp.
	TS_ASSERT(MT_ThemeClampGuiScale(0.01f) == MT_kGuiScaleSteps[0],
	          "an absurdly small scale clamps to the first step");
	TS_ASSERT(MT_ThemeClampGuiScale(9.0f) == MT_kGuiScaleSteps[MT_kGuiScaleStepCount-1],
	          "an absurdly large scale clamps to the last step");
	TS_ASSERT(MT_ThemeClampGuiScale(1.13f) == 1.10f,
	          "an off-step scale snaps to the nearest step");

	// The override is data, not a branch: a theme carrying FrameBorderSize 1
	// gets it, and its role remap actually lands.
	CMTThemeDef hc; hc.id = "hc"; hc.label = "hc";
	hc.seed.minContrastRatio = 7.0f;
	hc.override.frameBorderSize = MTOptFloat(1.0f);
	MTThemeRoleRemap remap; remap.col = ImGuiCol_Border;
	remap.token = MTThemeToken_BorderStrong;
	hc.override.roles.push_back(remap);
	MTThemeResolved hr = MT_ThemeResolve(hc, MTThemeMode_Dark);
	MT_ThemeApplyResolved(hr, 1.0f);
	TS_ASSERT(ImGui::GetStyle().FrameBorderSize == 1.0f,
	          "the geometry override reaches the applied style");
	ImVec4 border = ImGui::GetStyle().Colors[ImGuiCol_Border];
	ImVec4 strong = hr.color[MTThemeToken_BorderStrong];
	TS_ASSERT(border.x == strong.x && border.y == strong.y && border.z == strong.z,
	          "the role remap points ImGuiCol_Border at BorderStrong");

	// ...and the boundary the remap exists for actually clears 3:1.
	{
		MTOkLch bs = hr.lch[MTThemeToken_BorderStrong];
		MTOkLch sr = hr.lch[MTThemeToken_SurfaceRaised];
		float got = MTH_WcagContrast(MTH_OkLchWcagLuminance(bs),
		                             MTH_OkLchWcagLuminance(sr));
		TS_ASSERT(got >= 3.0f - 0.005f,
		          "High Contrast's component boundary meets WCAG 1.4.11 3:1");
	}

	// --- Task 8: typography role sizes ---------------------------------
	ImGui::GetStyle().FontSizeBase = 18.0f;
	TS_ASSERT(MT_ThemeFontSize(MTThemeFont_Body) == 18.0f,
	          "Body is FontSizeBase exactly");
	TS_ASSERT(std::fabs(MT_ThemeFontSize(MTThemeFont_Display) - 22.5f) < 1e-4f,
	          "Display is 1.25x FontSizeBase");
	TS_ASSERT(std::fabs(MT_ThemeFontSize(MTThemeFont_Label) - 15.75f) < 1e-4f,
	          "Label is 0.875x FontSizeBase");
	// The role size must NOT include guiScale -- FontScaleMain applies it once,
	// inside ImGui. Compounding here is the classic double-scale bug.
	MT_ThemeApplyResolved(r, 2.0f);
	ImGui::GetStyle().FontSizeBase = 18.0f;
	TS_ASSERT(MT_ThemeFontSize(MTThemeFont_Body) == 18.0f,
	          "role sizes are pre-scale: guiScale is applied by FontScaleMain, once");

	TestCompleted(true, "ThemeScale: all steps passed");
}

#define TR_ASSERT(cond, msg) \
	do { \
		if (!(cond)) { \
			char _b[320]; \
			snprintf(_b, sizeof(_b), "FAIL: %s", msg); \
			LOGD("CTestThemeRegistry: %s", _b); \
			TestCompleted(false, _b); \
			return; \
		} \
		StepCompleted(stepNum++, true, msg); \
	} while(0)

static const char *kIdA = "test-a";
static const char *kIdB = "test-b-dark-only";
static const char *kIdC = "test-c";
static const char *kIdL = "test-legacy";

// Runs on failure as well as success, which is the point: without it a failed
// assertion would leave this test's themes registered and the host's
// selection dropped, and every later test would run in the wrong palette.
void CTestThemeRegistry::Teardown()
{
	CMTThemeRegistry *reg = CMTThemeRegistry::Instance();
	reg->SetLegacyStylePolicy(MTLegacyStylePolicy::Show);
	reg->UnregisterTheme(kIdA);
	reg->UnregisterTheme(kIdB);
	reg->UnregisterTheme(kIdC);
	reg->UnregisterTheme(kIdL);
	if (needsRestore)
	{
		if (hadTheme && savedId[0] != '\0')
			reg->SetActiveTheme(savedId, (MTThemeMode)savedMode, savedScale);
		else
			reg->ClearActiveTheme();
		needsRestore = false;
	}
}

void CTestThemeRegistry::Run(ITestCallback *cb)
{
	this->callback = cb;
	isRunning = true;
	int stepNum = 1;

	CMTThemeRegistry *reg = CMTThemeRegistry::Instance();
	TR_ASSERT(reg != NULL, "registry instance exists");

	// Snapshot whatever the host has active, and restore it in Teardown().
	// THIS TEST MUST NOT ASSERT "no theme is active by default": from TS-2
	// onward the photo app activates a theme in MT_PostInit, which runs BEFORE
	// the suite. An absolute no-theme assertion is true today and becomes a
	// guaranteed failure one stage later.
	hadTheme = reg->HasActiveTheme();
	savedId[0] = '\0';
	if (hadTheme && reg->GetActiveThemeId() != NULL)
		snprintf(savedId, sizeof(savedId), "%s", reg->GetActiveThemeId());
	savedMode  = (int)reg->GetActiveMode();
	savedScale = reg->GetActiveGuiScale();
	needsRestore = true;

	// The invariant that DOES hold at every stage.
	reg->ClearActiveTheme();
	MT_ThemeClearActiveResolved();
	TR_ASSERT(!reg->HasActiveTheme(), "ClearActiveTheme leaves no active theme");
	TR_ASSERT(MT_ThemeGetActiveResolved() == NULL,
	          "MT_ThemeGetActiveResolved is NULL with no active theme");

	CMTThemeDef a; a.id = kIdA; a.label = "Test A";
	CMTThemeDef b; b.id = kIdB; b.label = "Test B"; b.seed.supportsLight = false;
	int before = reg->GetThemeCount();
	reg->RegisterTheme(a);
	reg->RegisterTheme(b);
	TR_ASSERT(reg->GetThemeCount() == before + 2, "two themes registered");
	TR_ASSERT(reg->FindTheme(kIdA) != NULL, "lookup by id works");
	TR_ASSERT(reg->FindTheme("nope") == NULL, "unknown id returns NULL");

	// Ids are strings, not ordinals: registering a third must not disturb the
	// first two (design #7.3). Deliberately NOT a pointer comparison -- the
	// registry's storage may move elements on growth.
	CMTThemeDef c; c.id = kIdC; c.label = "Test C";
	reg->RegisterTheme(c);
	TR_ASSERT(strcmp(reg->FindTheme(kIdA)->id, kIdA) == 0,
	          "registering a new theme does not renumber existing ones");

	// Bad ids are refused, not stored.
	int n = reg->GetThemeCount();
	CMTThemeDef bad; bad.id = "legacy:3"; bad.label = "bad";
	reg->RegisterTheme(bad);
	TR_ASSERT(reg->GetThemeCount() == n, "an id containing ':' is refused");

	// Activation.
	TR_ASSERT(reg->SetActiveTheme(kIdA, MTThemeMode_Dark, 1.0f),
	          "activating a registered theme succeeds");
	TR_ASSERT(reg->HasActiveTheme() && strcmp(reg->GetActiveThemeId(), kIdA) == 0,
	          "the active theme is the one we set");
	TR_ASSERT(MT_ThemeGetActiveResolved() != NULL,
	          "MT_ThemeGetActiveResolved is non-NULL once a theme is active");
	TR_ASSERT(MT_ThemeGetActiveResolved()->mode == MTThemeMode_Dark,
	          "the resolved mode is the one we asked for");

	// Refusals leave the previous selection intact -- a failed switch must
	// never leave the UI half-themed.
	TR_ASSERT(!reg->SetActiveTheme("nope", MTThemeMode_Dark, 1.0f),
	          "activating an unknown id fails");
	TR_ASSERT(strcmp(reg->GetActiveThemeId(), kIdA) == 0,
	          "a failed activation leaves the previous theme active");
	TR_ASSERT(!reg->SetActiveTheme(kIdB, MTThemeMode_Light, 1.0f),
	          "a dark-only theme refuses light mode");
	TR_ASSERT(strcmp(reg->GetActiveThemeId(), kIdA) == 0,
	          "the refused light activation changed nothing");

	// Callbacks.
	static int sFired = 0;
	struct H { static void Cb(void *ud) { (*(int*)ud)++; } };
	reg->AddThemeChangedCallback(&H::Cb, &sFired);
	sFired = 0;
	reg->SetActiveTheme(kIdC, MTThemeMode_Dark, 1.0f);
	TR_ASSERT(sFired == 1, "onThemeChanged fires once on a successful switch");
	sFired = 0;
	reg->SetActiveTheme("nope", MTThemeMode_Dark, 1.0f);
	TR_ASSERT(sFired == 0, "onThemeChanged does not fire on a failed switch");
	sFired = 0;
	reg->ReapplyActiveTheme();
	TR_ASSERT(sFired == 1, "ReapplyActiveTheme fires the callback");
	reg->RemoveThemeChangedCallback(&H::Cb, &sFired);
	sFired = 0;
	reg->SetActiveTheme(kIdA, MTThemeMode_Dark, 1.0f);
	TR_ASSERT(sFired == 0, "a removed callback stops firing");

	// Legacy policy affects enumeration only.
	std::vector<CMTThemeRegistry::Entry> entries;
	reg->SetLegacyStylePolicy(MTLegacyStylePolicy::Hidden);
	reg->EnumerateEntries(entries, /*devBuild*/ true);
	int legacyHidden = 0;
	for (size_t i = 0; i < entries.size(); i++) if (entries[i].isLegacyStyle) legacyHidden++;
	TR_ASSERT(legacyHidden == 0, "Hidden lists no legacy styles");

	reg->SetLegacyStylePolicy(MTLegacyStylePolicy::Show);
	reg->EnumerateEntries(entries, true);
	int legacyShown = 0;
	for (size_t i = 0; i < entries.size(); i++) if (entries[i].isLegacyStyle) legacyShown++;
	TR_ASSERT(legacyShown > 0, "Show lists legacy styles");
	TR_ASSERT(legacyShown == 10 || legacyShown == 11,
	          "Show lists the 11 engine styles, minus CUSTOM when no custom "
	          "style is saved on this machine");

	// CUSTOM's presence tracks VID_HasCustomImGuiStyle() rather than an
	// argument, so assert the RELATIONSHIP, not a number.
	bool sawCustom = false;
	for (size_t i = 0; i < entries.size(); i++)
		if (entries[i].isLegacyStyle && entries[i].legacyStyleType == (int)IMGUI_STYLE_CUSTOM)
			sawCustom = true;
	TR_ASSERT(sawCustom == VID_HasCustomImGuiStyle(),
	          "CUSTOM is listed exactly when a custom style exists on disk");

	reg->SetLegacyStylePolicy(MTLegacyStylePolicy::DevOnly);
	reg->EnumerateEntries(entries, /*devBuild*/ false);
	int legacyRelease = 0;
	for (size_t i = 0; i < entries.size(); i++) if (entries[i].isLegacyStyle) legacyRelease++;
	TR_ASSERT(legacyRelease == 0, "DevOnly hides legacy styles in a release build");

	// An imported theme resolves by SAMPLING, not by generating a ramp.
	{
		CMTThemeDef leg; leg.id = kIdL; leg.label = "Test Legacy";
		leg.source = CMTThemeSource::ImportedImGuiStyle;
		leg.importedStyleType = (int)IMGUI_STYLE_DARK;
		reg->RegisterTheme(leg);
		TR_ASSERT(reg->SetActiveTheme(kIdL, MTThemeMode_Dark, 1.0f),
		          "an imported theme activates");
		// StyleColorsDark's WindowBg is (0.06, 0.06, 0.06, 0.94). Surface is
		// opaque, so compare RGB only.
		const MTThemeResolved *rr = MT_ThemeGetActiveResolved();
		TR_ASSERT(rr != NULL && std::fabs(rr->color[MTThemeToken_Surface].x - 0.06f) < 0.01f,
		          "an imported theme SAMPLES the stock style rather than generating a ramp");
	}

	// Leave the engine exactly as we found it. By id, never a wipe: the host
	// has already registered its own themes into this same singleton.
	reg->ClearActiveTheme();
	reg->SetLegacyStylePolicy(MTLegacyStylePolicy::Show);
	reg->UnregisterTheme(kIdA);
	reg->UnregisterTheme(kIdB);
	reg->UnregisterTheme(kIdC);
	reg->UnregisterTheme(kIdL);
	TR_ASSERT(reg->GetThemeCount() == before,
	          "the registry is left exactly as it was found");

	TestCompleted(true, "ThemeRegistry: all steps passed");
}
