#include "CMTTheme.h"
#include "DBG_Log.h"
#include <cmath>
#include <cstring>

// Why this is a TABLE and not "every pair in the token list":
//
//   - TextDisabled at the seed ratio would be indistinguishable from enabled
//     text, which is the entire point of the token. WCAG 1.4.3 exempts
//     inactive components. It gets a 2.0 FLOOR so it stays legible-ish
//     without pretending to be enabled.
//   - BorderSubtle has NO rule. It is decorative; WCAG 1.4.11 governs
//     meaningful boundaries and state indicators, not separators. Forcing it
//     to 3:1 would turn every panel edge into a hard outline and destroy the
//     "calm screen at rest" the design asks for (#5.2).
//   - AccentFocus is judged at 3.0, not at minContrastRatio. A High Contrast
//     accent forced to 7:1 is near-white and stops reading as an accent at
//     all; 3.0 is what WCAG 1.4.11 actually requires of a focus indicator.
//   - BorderStrong is 3.0 for the same clause. In themes with
//     FrameBorderSize == 0 it is still used for window/child/popup borders,
//     so the rule is not vacuous.
//
// TextMuted IS held to the seed ratio. It carries real text (captions, status)
// and the ramp has room: with Surface at L=0.20 the 4.5 floor is L=0.596 and
// neutral[8] lands at L=0.69 unaided.
static const MTThemeContrastRule kRules[] =
{
	{ MTThemeToken_TextPrimary,   MTThemeToken_Surface,        0.f, "WCAG 1.4.3 body text on the page" },
	{ MTThemeToken_TextPrimary,   MTThemeToken_SurfaceRaised,  0.f, "WCAG 1.4.3 body text in a control" },
	{ MTThemeToken_TextPrimary,   MTThemeToken_SurfaceOverlay, 0.f, "WCAG 1.4.3 body text in a popup" },
	{ MTThemeToken_TextSecondary, MTThemeToken_Surface,        0.f, "WCAG 1.4.3 secondary text" },
	{ MTThemeToken_TextSecondary, MTThemeToken_SurfaceRaised,  0.f, "WCAG 1.4.3 secondary text in a control" },
	{ MTThemeToken_TextSecondary, MTThemeToken_SurfaceOverlay, 0.f, "WCAG 1.4.3 secondary text in a popup" },
	{ MTThemeToken_TextMuted,     MTThemeToken_Surface,        0.f, "WCAG 1.4.3 captions and status text" },
	{ MTThemeToken_TextMuted,     MTThemeToken_SurfaceRaised,  0.f, "WCAG 1.4.3 captions inside a control" },
	{ MTThemeToken_TextDisabled,  MTThemeToken_Surface,        2.0f, "floor only -- 1.4.3 exempts inactive components" },
	{ MTThemeToken_BorderStrong,  MTThemeToken_Surface,        3.0f, "WCAG 1.4.11 component boundary" },
	{ MTThemeToken_BorderStrong,  MTThemeToken_SurfaceRaised,  3.0f, "WCAG 1.4.11 component boundary" },
	{ MTThemeToken_AccentFocus,   MTThemeToken_Surface,        3.0f, "WCAG 1.4.11 focus indicator" },
	{ MTThemeToken_AccentFocus,   MTThemeToken_SurfaceRaised,  3.0f, "WCAG 1.4.11 focus indicator" },
};

// Copy an id into the fixed buffer, and SAY SO if it does not fit. Silent
// truncation breaks the documented recovery path -- FindTheme(r.themeId) would
// return NULL forever for that theme, with nothing anywhere explaining why.
static void MT_ThemeCopyId(char *dst, size_t cap, const char *id)
{
	const char *src = id ? id : "";
	if (strlen(src) >= cap)
		LOGError("MT_ThemeResolve: theme id '%s' is longer than %d characters "
		         "and is being truncated -- FindTheme() will not find it again",
		         src, (int)cap - 1);
	snprintf(dst, cap, "%s", src);
}

const MTThemeContrastRule *MT_ThemeContrastRules(int *outCount)
{
	if (outCount) *outCount = (int)(sizeof(kRules) / sizeof(kRules[0]));
	return kRules;
}

// 1-based neutral ramp index per token. AccentFocus is not here: it comes from
// the ACCENT ramp and is resolved separately (step 2 of the algorithm).
static const int kTokenNeutralIndex[MTThemeToken_COUNT] =
{
	1,   // Surface
	2,   // SurfaceRaised
	3,   // SurfaceOverlay
	5,   // BorderSubtle
	7,   // BorderStrong
	6,   // TextDisabled
	8,   // TextMuted
	10,  // TextSecondary
	12,  // TextPrimary
	0,   // AccentFocus -- accent ramp, see below
};

static bool IsSurfaceToken(MTThemeToken t)
{
	return t == MTThemeToken_Surface ||
	       t == MTThemeToken_SurfaceRaised ||
	       t == MTThemeToken_SurfaceOverlay;
}

static float ContrastOf(const MTOkLch &a, const MTOkLch &b)
{
	return MTH_WcagContrast(MTH_OkLchWcagLuminance(a), MTH_OkLchWcagLuminance(b));
}

// The solver works in float; ImGui draws 8-bit. Landing a token EXACTLY on its
// threshold therefore ships pixels below it: IM_F32_TO_INT8_SAT rounds each
// channel to 1/255, and the quantised pair can come back under. Measured on a
// shipping seed before this margin existed -- High Contrast light,
// TextMuted on SurfaceRaised: 7.0000 in float, 6.9941 once drawn. A theme
// called High Contrast that misses the AAA 7:1 it advertises is exactly the
// kind of quiet failure a generated palette is supposed to make impossible.
//
// 0.4% is comfortably more than one 8-bit step is worth at any lightness and
// far below anything an eye resolves, so it costs nothing visible.
static const float kQuantisationMargin = 1.004f;

// The ratio the SOLVER aims for, given the ratio the rule REQUIRES.
static float SolveTarget(float requiredRatio)
{
	return requiredRatio * kQuantisationMargin;
}

// Move `fg`'s lightness AWAY from `bg`'s until the ratio is met. Returns false
// when the requirement is unattainable inside L in [0,1].
static bool SolveLightnessForContrast(MTOkLch &fg, const MTOkLch &bg, float ratio)
{
	const bool goLighter = (fg.L >= bg.L);

	// Closed form when both are achromatic: Y == L^3 exactly, so the
	// requirement inverts with no search at all.
	if (fg.C < 1e-6f && bg.C < 1e-6f)
	{
		float want = goLighter ? MTH_GreyLLighterForContrast(bg.L, ratio)
		                       : MTH_GreyLDarkerForContrast(bg.L, ratio);
		if (want < 0.0f || want > 1.0f)
			return false;
		if (goLighter ? (want > fg.L) : (want < fg.L))
			fg.L = want;
		return true;
	}

	// Chromatic: bisect on L, re-clipping chroma at every probe, because the
	// maximum in-gamut chroma changes with L and an unclipped probe would
	// measure a colour that cannot be drawn.
	MTOkLch probe = fg;
	probe.L = goLighter ? 1.0f : 0.0f;
	probe = MTH_OkLchClipChroma(probe);
	if (ContrastOf(probe, bg) < ratio)
		return false;                    // not even the extreme is enough

	float lo = fg.L, hi = probe.L;       // lo fails (or barely passes), hi passes
	for (int i = 0; i < 24; i++)
	{
		float mid = 0.5f * (lo + hi);
		MTOkLch m = fg; m.L = mid; m = MTH_OkLchClipChroma(m);
		if (ContrastOf(m, bg) >= ratio) hi = mid; else lo = mid;
	}
	MTOkLch out = fg; out.L = hi; out = MTH_OkLchClipChroma(out);
	fg = out;
	return true;
}

MTThemeResolved MT_ThemeResolve(const CMTThemeDef &def, MTThemeMode mode)
{
	MTThemeResolved r;
	MT_ThemeCopyId(r.themeId, sizeof(r.themeId), def.id);
	r.source = def.source;
	r.mode = mode;
	r.override = def.override;
	r.outOfContract = false;
	for (int t = 0; t < MTThemeToken_COUNT; t++)
		r.solved[t] = false;

	CMTThemeRamp ramp = MT_ThemeGenerateRamp(def.seed);
	const float seedRatio = def.seed.minContrastRatio;

	// 1. Seed every token from its base ramp index.
	for (int t = 0; t < MTThemeToken_COUNT; t++)
	{
		if (t == MTThemeToken_AccentFocus) continue;
		int idx = MT_ThemeNeutralStepIndex(kTokenNeutralIndex[t], mode);
		r.lch[t] = ramp.neutral[idx];
	}

	int ruleCount = 0;
	const MTThemeContrastRule *rules = MT_ThemeContrastRules(&ruleCount);

	// 2. AccentFocus first: no text rule depends on it, and it depends on the
	//    surfaces. Start at accent index 9 (dark) / 5 (light) and step AWAY
	//    from the surface until EVERY accent rule is met.
	//
	//    Deviation from the plan's step 2, found by running it: the plan said
	//    "until the 3.0 rule against Surface is met", but the table carries
	//    TWO accent rules -- against Surface and against SurfaceRaised -- and
	//    step 3 skips AccentFocus entirely. In light mode SurfaceRaised is
	//    DARKER than Surface, so an accent that clears Surface at 3.0 sits at
	//    2.69 against SurfaceRaised, and nothing downstream would fix it: the
	//    palette came out of the resolver permanently out of contract. Walking
	//    until all accent rules pass keeps the plan's actual intent ("the
	//    accent step nearest the seed's target that satisfies its rules") and
	//    generalises to any accent rule added later.
	{
		const int startOneBased = (mode == MTThemeMode_Dark) ? 9 : 5;
		int idx = MT_ThemeNeutralStepIndex(startOneBased, mode);
		r.lch[MTThemeToken_AccentFocus] = ramp.accent[idx];

		const int step = (mode == MTThemeMode_Dark) ? +1 : -1;
		for (int guard = 0; guard <= MT_RAMP_STEPS; guard++)
		{
			bool allMet = true;
			for (int i = 0; i < ruleCount; i++)
			{
				if (rules[i].fg != MTThemeToken_AccentFocus) continue;
				float want = rules[i].ratio > 0.f ? rules[i].ratio : seedRatio;
				if (ContrastOf(r.lch[MTThemeToken_AccentFocus], r.lch[rules[i].bg]) < SolveTarget(want))
				{
					allMet = false;
					break;
				}
			}
			if (allMet) break;

			int next = idx + step;
			if (next < 0 || next >= MT_RAMP_STEPS)
			{
				r.outOfContract = true;
				LOGError("MT_ThemeResolve: theme '%s' (%s): AccentFocus cannot meet "
				         "its rules against the surfaces -- WCAG 1.4.11 focus indicator",
				         r.themeId, mode == MTThemeMode_Dark ? "dark" : "light");
				break;
			}
			idx = next;
			r.lch[MTThemeToken_AccentFocus] = ramp.accent[idx];
			r.solved[MTThemeToken_AccentFocus] = true;
		}
	}

	// 3. Every rule in table order.
	for (int i = 0; i < ruleCount; i++)
	{
		// AccentFocus was resolved in step 2, by stepping along the accent
		// ramp. Bisecting it here would move it off the step that was
		// deliberately chosen as "nearest the seed's target".
		if (rules[i].fg == MTThemeToken_AccentFocus) continue;

		float want = rules[i].ratio > 0.f ? rules[i].ratio : seedRatio;
		MTOkLch &fg = r.lch[rules[i].fg];
		const MTOkLch &bg = r.lch[rules[i].bg];
		if (ContrastOf(fg, bg) >= SolveTarget(want)) continue;

		// Never move a Surface token: surfaces are the theme's identity, and
		// moving one silently re-tunes every other pair.
		if (IsSurfaceToken(rules[i].fg))
		{
			r.outOfContract = true;
			LOGError("MT_ThemeResolve: theme '%s' (%s): rule would move a surface "
			         "token (%s) -- refusing; %s", r.themeId,
			         mode == MTThemeMode_Dark ? "dark" : "light",
			         MT_ThemeTokenName(rules[i].fg), rules[i].why);
			continue;
		}

		MTOkLch before = fg;
		if (!SolveLightnessForContrast(fg, bg, SolveTarget(want)))
		{
			r.outOfContract = true;
			LOGError("MT_ThemeResolve: theme '%s' (%s): %s on %s cannot reach "
			         "%.2f:1 inside L in [0,1] -- %s", r.themeId,
			         mode == MTThemeMode_Dark ? "dark" : "light",
			         MT_ThemeTokenName(rules[i].fg), MT_ThemeTokenName(rules[i].bg),
			         want, rules[i].why);
			continue;
		}
		if (fg.L != before.L)
			r.solved[rules[i].fg] = true;
	}

	// 4. Prominence ordering. d(t) = |L(t) - L(Surface)| must be
	//    non-increasing across Primary > Secondary > Muted > Disabled. Raise
	//    the MORE prominent token; never lower the less prominent one, which
	//    could break a rule already satisfied.
	{
		const float sL = r.lch[MTThemeToken_Surface].L;
		const bool lighterThanSurface = (r.lch[MTThemeToken_TextPrimary].L >= sL);
		const MTThemeToken order[4] = {
			MTThemeToken_TextPrimary, MTThemeToken_TextSecondary,
			MTThemeToken_TextMuted,   MTThemeToken_TextDisabled
		};
		// Walk from the least prominent upward so a raise propagates.
		for (int i = 2; i >= 0; i--)
		{
			float dMore = std::fabs(r.lch[order[i]].L     - sL);
			float dLess = std::fabs(r.lch[order[i + 1]].L - sL);
			if (dMore >= dLess) continue;

			float wantL = lighterThanSurface ? (sL + dLess) : (sL - dLess);
			if (wantL < 0.0f || wantL > 1.0f)
			{
				r.outOfContract = true;
				LOGError("MT_ThemeResolve: theme '%s' (%s): %s cannot be raised to "
				         "keep the text prominence ordering (needs L %.3f)",
				         r.themeId, mode == MTThemeMode_Dark ? "dark" : "light",
				         MT_ThemeTokenName(order[i]), wantL);
				continue;
			}
			r.lch[order[i]].L = wantL;
			r.lch[order[i]] = MTH_OkLchClipChroma(r.lch[order[i]]);
			r.solved[order[i]] = true;
		}

		// Re-check every rule once: raising a token can only increase its
		// distance from Surface, but it may still have crossed a
		// SurfaceRaised/SurfaceOverlay pairing the other way.
		for (int i = 0; i < ruleCount; i++)
		{
			float want = rules[i].ratio > 0.f ? rules[i].ratio : seedRatio;
			if (ContrastOf(r.lch[rules[i].fg], r.lch[rules[i].bg]) < want - 0.005f)
			{
				r.outOfContract = true;
				LOGError("MT_ThemeResolve: theme '%s' (%s): %s on %s regressed to "
				         "below %.2f:1 while restoring prominence ordering -- %s",
				         r.themeId, mode == MTThemeMode_Dark ? "dark" : "light",
				         MT_ThemeTokenName(rules[i].fg), MT_ThemeTokenName(rules[i].bg),
				         want, rules[i].why);
			}
		}
	}

	// 5. Convert.
	for (int t = 0; t < MTThemeToken_COUNT; t++)
	{
		float srgb[3];
		MTH_OkLchToSrgbF(r.lch[t], srgb);
		r.color[t] = ImVec4(srgb[0], srgb[1], srgb[2], 1.0f);
	}
	return r;
}

// --- imported (Legacy) -------------------------------------------------

static ImVec4 CompositeOver(const ImVec4 &fg, const ImVec4 &bg)
{
	float a = fg.w;
	return ImVec4(fg.x * a + bg.x * (1.0f - a),
	              fg.y * a + bg.y * (1.0f - a),
	              fg.z * a + bg.z * (1.0f - a),
	              1.0f);
}

MTThemeResolved MT_ThemeResolveImported(const CMTThemeDef &def,
                                        MTThemeMode mode,
                                        const ImGuiStyle &style)
{
	MTThemeResolved r;
	MT_ThemeCopyId(r.themeId, sizeof(r.themeId), def.id);
	r.source = def.source;
	r.mode = mode;
	r.override = def.override;
	r.outOfContract = false;
	for (int t = 0; t < MTThemeToken_COUNT; t++)
		r.solved[t] = false;

	const ImVec4 black = ImVec4(0.f, 0.f, 0.f, 1.f);
	ImVec4 surface = CompositeOver(style.Colors[ImGuiCol_WindowBg], black);

	r.color[MTThemeToken_Surface]        = surface;
	r.color[MTThemeToken_SurfaceRaised]  = CompositeOver(style.Colors[ImGuiCol_FrameBg], surface);
	r.color[MTThemeToken_SurfaceOverlay] = CompositeOver(style.Colors[ImGuiCol_PopupBg], surface);
	r.color[MTThemeToken_BorderSubtle]   = CompositeOver(style.Colors[ImGuiCol_Border], surface);

	ImVec4 borderOpaque = style.Colors[ImGuiCol_Border];
	borderOpaque.w = 1.0f;
	r.color[MTThemeToken_BorderStrong]   = borderOpaque;

	ImVec4 text = style.Colors[ImGuiCol_Text];
	text.w = 1.0f;
	r.color[MTThemeToken_TextPrimary]    = text;

	ImVec4 sec = style.Colors[ImGuiCol_Text]; sec.w = 0.75f;
	r.color[MTThemeToken_TextSecondary]  = CompositeOver(sec, surface);
	ImVec4 mut = style.Colors[ImGuiCol_Text]; mut.w = 0.55f;
	r.color[MTThemeToken_TextMuted]      = CompositeOver(mut, surface);

	ImVec4 dis = style.Colors[ImGuiCol_TextDisabled];
	r.color[MTThemeToken_TextDisabled]   = CompositeOver(dis, surface);

	// The app's existing convention: CFilmStrip.cpp already reads
	// ImGuiCol_SliderGrab as "the accent".
	ImVec4 accent = style.Colors[ImGuiCol_SliderGrab];
	r.color[MTThemeToken_AccentFocus]    = CompositeOver(accent, surface);

	// Fill lch[] so diagnostics and tests can read a consistent struct, but
	// run NO solver: Legacy is an instrument for looking at somebody else's
	// palette, and silently "fixing" its contrast would destroy the one
	// property that makes it useful.
	for (int t = 0; t < MTThemeToken_COUNT; t++)
	{
		MTLinearRgb lin = { MTH_SrgbToLinear(r.color[t].x),
		                    MTH_SrgbToLinear(r.color[t].y),
		                    MTH_SrgbToLinear(r.color[t].z) };
		r.lch[t] = MTH_OkLabToOkLch(MTH_LinearRgbToOkLab(lin));
		r.color[t].w = 1.0f;
	}
	return r;
}
