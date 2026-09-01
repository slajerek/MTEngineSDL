#pragma once
#include "MT_Theme.h"
#include <vector>

// Registration, selection and change notification. A singleton because there
// is exactly one ImGui style and exactly one active theme; making it an
// instance would only invite two of them.
//
// BACKWARD COMPATIBILITY IS THE POINT: a host that never touches this class
// behaves exactly as it did before TS-1. Nothing here runs unless a theme is
// registered and activated.
class CMTThemeRegistry
{
public:
	static CMTThemeRegistry *Instance();

	// --- registration ---------------------------------------------------
	// Copies the def (including the override vector). `id` and `label` are
	// BORROWED pointers and must outlive the registry -- string literals or
	// static storage, never std::string temporaries. Re-registering an
	// existing id replaces it and LOGDs; that is how a host reloads a theme
	// during development, not an error.
	void RegisterTheme(const CMTThemeDef &def);

	int                GetThemeCount() const;
	const CMTThemeDef *GetThemeAt(int index) const;     // NULL when out of range
	const CMTThemeDef *FindTheme(const char *id) const; // NULL when unknown

	// --- legacy policy (design #6.2) ------------------------------------
	// Governs ONLY whether legacy ImGuiStyleType values appear in
	// EnumerateEntries. Default Show, so a host that never calls this sees
	// every style listed -- costless future-proofing, and unobservable to
	// c64d and the game app today because neither consumes an enumeration.
	void                SetLegacyStylePolicy(MTLegacyStylePolicy policy);
	MTLegacyStylePolicy GetLegacyStylePolicy() const;

	struct Entry
	{
		const char *id;             // theme id, or "legacy:<n>" for a legacy style
		const char *label;
		bool        isLegacyStyle;
		int         legacyStyleType; // ImGuiStyleType, or -1
		bool        supportsLight;
	};
	// Registered themes first, in registration order, then legacy styles if
	// the policy allows. `devBuild` decides what DevOnly means; the engine
	// cannot know the host's build flavour, so the host passes it.
	// IMGUI_STYLE_CUSTOM is listed only when a saved custom style exists; the
	// registry asks VID_HasCustomImGuiStyle() itself.
	//
	// CLEARS `out` first. Say so here and do it there: the natural reading of
	// an `out` vector is "appends", CTestThemeRegistry calls this four times
	// with one vector, and an appending implementation makes the third
	// assertion fail on otherwise-correct code.
	void EnumerateEntries(std::vector<Entry> &out, bool devBuild) const;

	// --- active theme ---------------------------------------------------
	// Resolves, applies and fires onThemeChanged. Returns false and changes
	// NOTHING when the id is unknown, when the mode is unsupported by the
	// theme, or when the resolve came back outOfContract -- an out-of-contract
	// palette must never reach a user's screen.
	bool SetActiveTheme(const char *id, MTThemeMode mode, float guiScale);

	bool         HasActiveTheme() const;
	const char  *GetActiveThemeId() const;     // NULL when none
	MTThemeMode  GetActiveMode() const;
	float        GetActiveGuiScale() const;

	// Re-resolve and re-apply the current selection. Used by the VID hook
	// after the engine re-applies a style, and by a scale change. No-op with
	// no active theme -- which is what keeps every other host untouched.
	// MUST dispatch on def.source exactly as SetActiveTheme does -- they share
	// one private ResolveActive(). Without that, the first style re-apply or
	// scale change after activating an imported theme converts it into a
	// generated ramp.
	void ReapplyActiveTheme();

	// Back to "engine style only". Clears the active selection FIRST, then
	// calls MT_ThemeClearActiveResolved() -- so HasActiveTheme() == false and
	// MT_ThemeGetActiveResolved() == NULL can never disagree -- and only then
	// VID_ResetImGuiStyle().
	void ClearActiveTheme();

	// True while the registry is inside its own VID_SetImGuiStyle /
	// VID_ResetImGuiStyle call. VID_SetImGuiStyle's tail consults this to
	// decide whether to fire ReapplyActiveTheme.
	bool IsApplyingTheme() const;

	// Removes one theme by id. Returns false if the id was not registered.
	// Clears the active selection first if that theme was active.
	//
	// This exists for tests, but it is deliberately surgical rather than a
	// reset-everything: the registry is a process-wide singleton, and by the
	// time the suite runs the host has ALREADY registered its own themes.
	// CTestThemeRegistry must remove exactly what it added.
	bool UnregisterTheme(const char *id);

	// --- OS appearance (design #6.4) ------------------------------------
	// Fired when the engine detects the OS light/dark appearance changing,
	// BEFORE ReapplyActiveTheme runs. Distinct from onThemeChanged, and it has
	// to be: ReapplyActiveTheme re-resolves the CURRENT selection, so it can
	// re-apply a theme but can never switch from the host's dark slot to its
	// light slot. Only the host knows it has two slots.
	typedef void (*MTSystemAppearanceCallback)(MTThemeMode osMode, void *userData);
	void AddSystemAppearanceChangedCallback(MTSystemAppearanceCallback cb, void *userData);
	void RemoveSystemAppearanceChangedCallback(MTSystemAppearanceCallback cb, void *userData);
	// PUBLIC, not internal: VID_RefreshSystemVisualState calls it from
	// VID_Main.cpp and CTestThemeRegistry drives it from a third TU.
	void NotifySystemAppearanceChanged(MTThemeMode osMode);

	// --- registry composition changed -----------------------------------
	// Fired by RegisterTheme and UnregisterTheme, i.e. when the SET of themes
	// changes rather than which one is active. TS-3 caches a resolved palette
	// per theme for its swatch cards and has no other correct invalidation
	// trigger: activation-only notification misses both addition and
	// re-registration in place.
	typedef void (*MTThemeRegistryChangedCallback)(void *userData);
	void AddRegistryChangedCallback(MTThemeRegistryChangedCallback cb, void *userData);
	void RemoveRegistryChangedCallback(MTThemeRegistryChangedCallback cb, void *userData);

	// --- change notification (design #6.4) ------------------------------
	typedef void (*MTThemeChangedCallback)(void *userData);
	void AddThemeChangedCallback(MTThemeChangedCallback cb, void *userData);
	void RemoveThemeChangedCallback(MTThemeChangedCallback cb, void *userData);

private:
	CMTThemeRegistry();

	// The one path SetActiveTheme and ReapplyActiveTheme share, so the
	// source dispatch cannot drift between them.
	bool ResolveActive(const CMTThemeDef &def, MTThemeMode mode, float guiScale,
	                   MTThemeResolved &out);

	std::vector<CMTThemeDef> themes;
	int                      activeIndex = -1;
	MTThemeMode              activeMode = MTThemeMode_Dark;
	float                    activeGuiScale = 1.0f;
	MTLegacyStylePolicy      legacyPolicy = MTLegacyStylePolicy::Show;
	bool                     applyingTheme = false;
	// DEPTH counters, not bools. A bool is cleared by the INNER dispatch when
	// callbacks nest (register -> callback -> unregister -> its own dispatch),
	// which defeats the guard exactly when it is needed.
	int                      registryCallbackDepth = 0;
	int                      themeChangedDepth = 0;

	std::vector<std::pair<MTThemeChangedCallback, void *> >          themeChangedCbs;
	std::vector<std::pair<MTThemeRegistryChangedCallback, void *> >  registryChangedCbs;
	std::vector<std::pair<MTSystemAppearanceCallback, void *> >      appearanceCbs;

	void FireThemeChanged();
	void FireRegistryChanged();
};
