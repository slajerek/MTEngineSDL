#include "CTestThemeRamp.h"
#include "CMTThemeRamp.h"
#include "CMTTheme.h"
#include "DBG_Log.h"
#include <cmath>
#include <cstdio>

#define TR_ASSERT(cond, msg) \
	do { \
		if (!(cond)) { \
			char _buf[256]; \
			snprintf(_buf, sizeof(_buf), "FAIL: %s", msg); \
			LOGD("CTestThemeRamp: %s", _buf); \
			TestCompleted(false, _buf); \
			return; \
		} \
		StepCompleted(stepNum++, true, msg); \
	} while(0)

void CTestThemeRamp::Run(ITestCallback *cb)
{
	this->callback = cb;
	isRunning = true;
	int stepNum = 1;

	CMTThemeSeed seed;             // defaults: achromatic, 0.20 -> 0.97, gamma 1
	CMTThemeRamp ramp = MT_ThemeGenerateRamp(seed);

	TR_ASSERT(std::fabs(ramp.neutral[0].L - 0.20f) < 1e-5f,
	          "neutral step 1 sits on bgAnchorDark");
	TR_ASSERT(std::fabs(ramp.neutral[MT_RAMP_STEPS-1].L - 0.97f) < 1e-5f,
	          "neutral step 12 sits on bgAnchorLight");

	for (int i = 1; i < MT_RAMP_STEPS; i++)
		TR_ASSERT(ramp.neutral[i].L > ramp.neutral[i-1].L + 1e-4f,
		          "neutral ramp is strictly increasing");
	for (int i = 1; i < MT_RAMP_STEPS; i++)
		TR_ASSERT(ramp.accent[i].L > ramp.accent[i-1].L + 1e-4f,
		          "accent ramp is strictly increasing");

	for (int i = 0; i < MT_RAMP_STEPS; i++)
	{
		TR_ASSERT(ramp.neutral[i].C < 1e-6f,
		          "an achromatic seed produces a strictly achromatic ramp");
		TR_ASSERT(ramp.neutral[i].h == 0.0f,
		          "achromatic steps store hue 0, so seeds with different "
		          "neutralHue give bit-identical ramps");
	}

	// Gamut: every generated step must survive conversion without clamping.
	for (int i = 0; i < MT_RAMP_STEPS; i++)
	{
		MTLinearRgb lin = MTH_OkLabToLinearRgb(MTH_OkLchToOkLab(ramp.accent[i]));
		TR_ASSERT(MTH_LinearRgbInGamut(lin), "every accent step is in sRGB gamut");
	}
	// ... and the top accent step must actually have been clipped, or the
	// clipping code is dead and nobody would notice.
	CMTThemeSeed vivid; vivid.accentChroma = 0.20f; vivid.accentHue = 250.f;
	CMTThemeRamp vr = MT_ThemeGenerateRamp(vivid);
	TR_ASSERT(vr.accent[MT_RAMP_STEPS-1].C < vivid.accentChroma - 1e-3f,
	          "a vivid accent is chroma-clipped at the light end");

	// Gamma redistributes without moving the anchors.
	CMTThemeSeed g2 = seed; g2.contrastGamma = 2.0f;
	CMTThemeRamp r2 = MT_ThemeGenerateRamp(g2);
	TR_ASSERT(std::fabs(r2.neutral[0].L - 0.20f) < 1e-5f &&
	          std::fabs(r2.neutral[MT_RAMP_STEPS-1].L - 0.97f) < 1e-5f,
	          "gamma leaves both anchors fixed");
	TR_ASSERT(r2.neutral[5].L < ramp.neutral[5].L - 1e-3f,
	          "gamma > 1 pushes middle steps toward the dark anchor");

	// Inversion.
	TR_ASSERT(MT_ThemeNeutralStepIndex(1, MTThemeMode_Dark) == 0 &&
	          MT_ThemeNeutralStepIndex(12, MTThemeMode_Dark) == 11,
	          "dark mode maps 1..12 to 0..11");
	TR_ASSERT(MT_ThemeNeutralStepIndex(1, MTThemeMode_Light) == 11 &&
	          MT_ThemeNeutralStepIndex(12, MTThemeMode_Light) == 0,
	          "light mode inverts: Surface is the lightest step");

	// Swapped anchors must be corrected, not obeyed.
	CMTThemeSeed sw = seed; sw.bgAnchorDark = 0.97f; sw.bgAnchorLight = 0.20f;
	CMTThemeRamp swr = MT_ThemeGenerateRamp(sw);
	TR_ASSERT(swr.neutral[0].L < swr.neutral[MT_RAMP_STEPS-1].L,
	          "swapped anchors are corrected -- step 1 is always the darkest");

	// Determinism: same seed, same bytes. The whole "machine-checkable"
	// argument collapses without this.
	CMTThemeRamp again = MT_ThemeGenerateRamp(seed);
	for (int i = 0; i < MT_RAMP_STEPS; i++)
		TR_ASSERT(again.neutral[i].L == ramp.neutral[i].L &&
		          again.accent[i].C  == ramp.accent[i].C,
		          "ramp generation is deterministic");

	TestCompleted(true, "ThemeRamp: all steps passed");
}

#define TC_ASSERT(cond, msg) \
	do { \
		if (!(cond)) { \
			char _b[320]; \
			snprintf(_b, sizeof(_b), "FAIL: %s", msg); \
			LOGD("CTestThemeContrast: %s", _b); \
			TestCompleted(false, _b); \
			return; \
		} \
		StepCompleted(stepNum++, true, msg); \
	} while(0)

// The WCAG luminance of a colour after ImGui's 8-bit quantisation -- i.e. of
// the pixels a user actually sees, not of the float the solver worked in.
static float QuantisedLuminance(const ImVec4 &c)
{
	float q[3];
	for (int i = 0; i < 3; i++)
	{
		float v = (&c.x)[i];
		if (v < 0.0f) v = 0.0f;
		if (v > 1.0f) v = 1.0f;
		q[i] = (float)(int)(v * 255.0f + 0.5f) / 255.0f;
	}
	MTLinearRgb lin = { MTH_SrgbToLinear(q[0]), MTH_SrgbToLinear(q[1]),
	                    MTH_SrgbToLinear(q[2]) };
	return MTH_WcagLuminance(lin);
}

void CTestThemeContrast::Run(ITestCallback *cb)
{
	this->callback = cb;
	isRunning = true;
	int stepNum = 1;

	struct SeedCase { const char *name; CMTThemeSeed seed; };
	SeedCase cases[5];
	cases[0].name = "neutral-default";  // achromatic, 0.20..0.97, 4.5
	cases[1].name = "high-contrast";  cases[1].seed.minContrastRatio = 7.0f;
	cases[2].name = "midnight";       cases[2].seed.bgAnchorDark = 0.08f;
	                                  cases[2].seed.supportsLight = false;
	cases[3].name = "warm";           cases[3].seed.neutralChroma = 0.012f;
	                                  cases[3].seed.neutralHue = 70.f;
	cases[4].name = "steep-gamma";    cases[4].seed.contrastGamma = 2.2f;

	int ruleCount = 0;
	const MTThemeContrastRule *rules = MT_ThemeContrastRules(&ruleCount);
	TC_ASSERT(ruleCount > 0, "the rule table is non-empty");

	for (int c = 0; c < 5; c++)
	{
		for (int m = 0; m < 2; m++)
		{
			MTThemeMode mode = (MTThemeMode)m;
			if (mode == MTThemeMode_Light && !cases[c].seed.supportsLight)
				continue;             // Midnight has no light palette by design

			CMTThemeDef def;
			def.id = cases[c].name;
			def.label = cases[c].name;
			def.seed = cases[c].seed;
			MTThemeResolved r = MT_ThemeResolve(def, mode);

			char msg[320];
			snprintf(msg, sizeof(msg), "%s/%s resolves within contract",
			         cases[c].name, mode == MTThemeMode_Dark ? "dark" : "light");
			TC_ASSERT(!r.outOfContract, msg);

			for (int i = 0; i < ruleCount; i++)
			{
				float want = rules[i].ratio > 0.f ? rules[i].ratio
				                                  : cases[c].seed.minContrastRatio;
				float got = MTH_WcagContrast(MTH_OkLchWcagLuminance(r.lch[rules[i].fg]),
				                             MTH_OkLchWcagLuminance(r.lch[rules[i].bg]));
				snprintf(msg, sizeof(msg), "%s/%s: %s on %s is %.2f:1 (need %.2f) -- %s",
				         cases[c].name, mode == MTThemeMode_Dark ? "dark" : "light",
				         MT_ThemeTokenName(rules[i].fg), MT_ThemeTokenName(rules[i].bg),
				         got, want, rules[i].why);
				// 0.005 slack absorbs float rounding in the closed form; it is
				// two orders of magnitude below any perceptible difference.
				TC_ASSERT(got >= want - 0.005f, msg);

				// AND the same rule on the colour that is actually DRAWN.
				// ImGui quantises to 8 bits per channel, so a token solved to
				// land exactly on its threshold ships pixels below it -- High
				// Contrast light measured 7.0000 in float and 6.9941 on
				// screen before the solver gained its quantisation margin.
				// This is the assertion that makes the margin real rather
				// than a comment.
				float qFg = QuantisedLuminance(r.color[rules[i].fg]);
				float qBg = QuantisedLuminance(r.color[rules[i].bg]);
				float qGot = MTH_WcagContrast(qFg, qBg);
				snprintf(msg, sizeof(msg), "%s/%s: %s on %s is %.4f:1 AS DRAWN "
				         "(8-bit), need %.2f -- %s",
				         cases[c].name, mode == MTThemeMode_Dark ? "dark" : "light",
				         MT_ThemeTokenName(rules[i].fg), MT_ThemeTokenName(rules[i].bg),
				         qGot, want, rules[i].why);
				TC_ASSERT(qGot >= want, msg);
			}

			// Prominence ordering survived the solve.
			float sL = r.lch[MTThemeToken_Surface].L;
			float dPri = std::fabs(r.lch[MTThemeToken_TextPrimary].L   - sL);
			float dSec = std::fabs(r.lch[MTThemeToken_TextSecondary].L - sL);
			float dMut = std::fabs(r.lch[MTThemeToken_TextMuted].L     - sL);
			float dDis = std::fabs(r.lch[MTThemeToken_TextDisabled].L  - sL);
			TC_ASSERT(dPri >= dSec - 1e-4f && dSec >= dMut - 1e-4f &&
			          dMut >= dDis - 1e-4f,
			          "text prominence ordering survives the contrast solve");

			// Every token is opaque and in range.
			for (int t = 0; t < MTThemeToken_COUNT; t++)
			{
				TC_ASSERT(r.color[t].w == 1.0f, "every engine token is opaque");
				TC_ASSERT(r.color[t].x >= 0.f && r.color[t].x <= 1.f &&
				          r.color[t].y >= 0.f && r.color[t].y <= 1.f &&
				          r.color[t].z >= 0.f && r.color[t].z <= 1.f,
				          "every token channel is inside [0,1]");
			}
		}
	}

	// The solver must actually be load-bearing somewhere, or it is untested
	// scaffolding: High Contrast in LIGHT mode is the case the design predicts
	// needs a nudge (dark-on-light and light-on-dark are not symmetric).
	{
		CMTThemeDef hc; hc.id = "hc"; hc.label = "hc";
		hc.seed.minContrastRatio = 7.0f;
		MTThemeResolved r = MT_ThemeResolve(hc, MTThemeMode_Light);
		TC_ASSERT(r.solved[MTThemeToken_TextMuted],
		          "High Contrast light needed the solver for TextMuted");
	}

	// An impossible seed must be REPORTED, not quietly approximated.
	{
		CMTThemeDef bad; bad.id = "impossible"; bad.label = "impossible";
		bad.seed.bgAnchorDark = 0.60f;      // mid-grey surface
		bad.seed.bgAnchorLight = 0.99f;
		bad.seed.minContrastRatio = 21.0f;  // unattainable against mid-grey
		MTThemeResolved r = MT_ThemeResolve(bad, MTThemeMode_Dark);
		TC_ASSERT(r.outOfContract,
		          "an unattainable seed sets outOfContract instead of clamping");
	}

	TestCompleted(true, "ThemeContrast: all steps passed");
}
