#include "CMTThemeRegistry.h"
#include "VID_Main.h"     // VID_ResetImGuiStyle, VID_SetImGuiStyle, VID_HasCustomImGuiStyle
#include "DBG_Log.h"
#include <cstring>
#include <cstdio>

// Index == ImGuiStyleType. Order taken from VID_Main.h, not from taste:
// IMGUI_STYLE_DARK_ALTERNATIVE is 0, which is not where a reader expects it,
// so a list typed out in "natural" order silently mislabels nine of eleven.
static const char *kLegacyStyleNames[] = {
	"Dark Alternative",   // IMGUI_STYLE_DARK_ALTERNATIVE = 0
	"Dark", "Light", "Classic", "IntelliJ", "Photoshop",
	"Corporate Grey", "Corporate Grey 3D", "Nice", "System", "Custom",
};
static_assert(IM_ARRAYSIZE(kLegacyStyleNames) == IMGUI_STYLE_CUSTOM + 1,
              "one label per ImGuiStyleType");

// "legacy:<n>" ids, static so the borrowed pointers in Entry outlive the call.
static char kLegacyIds[IMGUI_STYLE_CUSTOM + 1][16];
static bool kLegacyIdsBuilt = false;

// RAII for the apply guard. The flag lives HERE, in the registry, not in
// VID_SetImGuiStyle: a "am I nested in VID_SetImGuiStyle" flag is set on entry
// and therefore always set when the tail runs, which makes the tail hook dead
// code on the outermost legitimate call.
struct MTThemeApplyGuard
{
	bool *flag;
	explicit MTThemeApplyGuard(bool *f) : flag(f) { *flag = true; }
	~MTThemeApplyGuard() { *flag = false; }
};

CMTThemeRegistry::CMTThemeRegistry()
{
}

CMTThemeRegistry *CMTThemeRegistry::Instance()
{
	static CMTThemeRegistry sInstance;
	return &sInstance;
}

void CMTThemeRegistry::RegisterTheme(const CMTThemeDef &def)
{
	if (def.id == NULL || def.id[0] == '\0')
	{
		LOGError("CMTThemeRegistry::RegisterTheme: refusing a theme with no id");
		return;
	}
	if (strchr(def.id, ':') != NULL)
	{
		LOGError("CMTThemeRegistry::RegisterTheme: refusing id '%s' -- ':' is "
		         "reserved for the 'legacy:<n>' enumeration prefix", def.id);
		return;
	}
	if (registryCallbackDepth > 0)
	{
		LOGError("CMTThemeRegistry::RegisterTheme: called from inside a "
		         "registry-changed callback ('%s') -- refusing rather than "
		         "recursing", def.id);
		return;
	}

	for (size_t i = 0; i < themes.size(); i++)
	{
		if (strcmp(themes[i].id, def.id) == 0)
		{
			LOGD("CMTThemeRegistry: replacing existing theme '%s'", def.id);
			const bool wasActive = (activeIndex == (int)i);
			themes[i] = def;
			FireRegistryChanged();
			// Re-registering is how a host reloads a theme during development.
			// If the reloaded theme is the ACTIVE one, the point of reloading
			// is to see the new seed -- otherwise the old palette stays on
			// screen and the workflow the header advertises does nothing.
			if (wasActive)
				ReapplyActiveTheme();
			return;
		}
	}
	themes.push_back(def);
	FireRegistryChanged();
}

int CMTThemeRegistry::GetThemeCount() const
{
	return (int)themes.size();
}

const CMTThemeDef *CMTThemeRegistry::GetThemeAt(int index) const
{
	if (index < 0 || index >= (int)themes.size()) return NULL;
	return &themes[index];
}

const CMTThemeDef *CMTThemeRegistry::FindTheme(const char *id) const
{
	if (id == NULL) return NULL;
	for (size_t i = 0; i < themes.size(); i++)
		if (strcmp(themes[i].id, id) == 0)
			return &themes[i];
	return NULL;
}

void CMTThemeRegistry::SetLegacyStylePolicy(MTLegacyStylePolicy policy)
{
	legacyPolicy = policy;
}

MTLegacyStylePolicy CMTThemeRegistry::GetLegacyStylePolicy() const
{
	return legacyPolicy;
}

void CMTThemeRegistry::EnumerateEntries(std::vector<Entry> &out, bool devBuild) const
{
	out.clear();                       // documented in the header; callers reuse one vector

	for (size_t i = 0; i < themes.size(); i++)
	{
		Entry e;
		e.id = themes[i].id;
		e.label = themes[i].label;
		e.isLegacyStyle = false;
		e.legacyStyleType = -1;
		e.supportsLight = themes[i].seed.supportsLight;
		out.push_back(e);
	}

	bool showLegacy = (legacyPolicy == MTLegacyStylePolicy::Show) ||
	                  (legacyPolicy == MTLegacyStylePolicy::DevOnly && devBuild);
	if (!showLegacy)
		return;

	if (!kLegacyIdsBuilt)
	{
		for (int i = 0; i <= (int)IMGUI_STYLE_CUSTOM; i++)
			snprintf(kLegacyIds[i], sizeof(kLegacyIds[i]), "legacy:%d", i);
		kLegacyIdsBuilt = true;
	}

	for (int i = 0; i <= (int)IMGUI_STYLE_CUSTOM; i++)
	{
		// CUSTOM only exists when the user has saved one. The registry asks
		// rather than taking it as an argument -- this file already includes
		// VID_Main.h.
		if (i == (int)IMGUI_STYLE_CUSTOM && !VID_HasCustomImGuiStyle())
			continue;
		Entry e;
		e.id = kLegacyIds[i];
		e.label = kLegacyStyleNames[i];
		e.isLegacyStyle = true;
		e.legacyStyleType = i;
		// Legacy styles are not mode-aware. A host filtering its light slot on
		// this field must ALSO filter on isLegacyStyle.
		e.supportsLight = true;
		out.push_back(e);
	}
}

bool CMTThemeRegistry::ResolveActive(const CMTThemeDef &def, MTThemeMode mode,
                                     float guiScale, MTThemeResolved &out)
{
	if (mode == MTThemeMode_Light && !def.seed.supportsLight &&
	    def.source == CMTThemeSource::Ramp)
	{
		LOGD("CMTThemeRegistry: theme '%s' does not support light mode", def.id);
		return false;
	}

	if (def.source == CMTThemeSource::ImportedImGuiStyle)
	{
		// Apply the underlying style first so there IS something to sample.
		// The guard makes VID_SetImGuiStyle's tail hook a no-op for the
		// duration, so this cannot recurse.
		MTThemeApplyGuard guard(&applyingTheme);
		VID_SetImGuiStyle((ImGuiStyleType)def.importedStyleType);
		out = MT_ThemeResolveImported(def, mode, ImGui::GetStyle());
		return true;
	}

	out = MT_ThemeResolve(def, mode);
	if (out.outOfContract)
	{
		LOGError("CMTThemeRegistry: refusing to activate '%s' -- the resolved "
		         "palette is out of contract; fix the seed", def.id);
		return false;
	}
	return true;
}

bool CMTThemeRegistry::SetActiveTheme(const char *id, MTThemeMode mode, float guiScale)
{
	if (themeChangedDepth > 0)
	{
		// An onThemeChanged callback that activates a theme would recurse
		// without bound -- a stack overflow rather than a refusal. Say no.
		LOGError("CMTThemeRegistry::SetActiveTheme('%s') called from inside an "
		         "onThemeChanged callback -- refusing rather than recursing",
		         id ? id : "(null)");
		return false;
	}

	const CMTThemeDef *found = FindTheme(id);
	if (found == NULL)
	{
		LOGD("CMTThemeRegistry::SetActiveTheme: unknown id '%s'", id ? id : "(null)");
		return false;
	}
	// COPY, for the same reason ReapplyActiveTheme copies: ResolveActive calls
	// VID_SetImGuiStyle for an imported theme, and anything that reaches host
	// code able to RegisterTheme would reallocate the vector under us.
	const CMTThemeDef def = *found;

	MTThemeResolved resolved;
	if (!ResolveActive(def, mode, guiScale, resolved))
		return false;                 // a failed switch changes NOTHING

	bool applied = false;
	{
		MTThemeApplyGuard guard(&applyingTheme);
		applied = MT_ThemeApplyResolved(resolved, guiScale);
	}
	if (!applied)
	{
		// No ImGui context. Reporting success here would leave
		// HasActiveTheme() true while MT_ThemeGetActiveResolved() is NULL --
		// the one disagreement this class promises cannot happen.
		LOGError("CMTThemeRegistry::SetActiveTheme('%s'): the palette could not "
		         "be applied; leaving the previous selection untouched", def.id);
		return false;
	}

	for (size_t i = 0; i < themes.size(); i++)
		if (strcmp(themes[i].id, id) == 0)
			activeIndex = (int)i;
	activeMode = mode;
	// The CLAMPED scale, which is what was actually applied. Storing the raw
	// value makes GetActiveGuiScale() report a scale that is not on screen.
	activeGuiScale = MT_ThemeClampGuiScale(guiScale);

	FireThemeChanged();
	return true;
}

bool CMTThemeRegistry::HasActiveTheme() const
{
	return activeIndex >= 0 && activeIndex < (int)themes.size();
}

const char *CMTThemeRegistry::GetActiveThemeId() const
{
	return HasActiveTheme() ? themes[activeIndex].id : NULL;
}

MTThemeMode CMTThemeRegistry::GetActiveMode() const
{
	return activeMode;
}

float CMTThemeRegistry::GetActiveGuiScale() const
{
	return activeGuiScale;
}

void CMTThemeRegistry::ReapplyActiveTheme()
{
	if (!HasActiveTheme()) return;

	// Copy: ResolveActive may call VID_SetImGuiStyle for an imported theme,
	// and a host callback fired from there could in principle re-register.
	CMTThemeDef def = themes[activeIndex];
	MTThemeResolved resolved;
	if (!ResolveActive(def, activeMode, activeGuiScale, resolved))
		return;

	bool applied = false;
	{
		MTThemeApplyGuard guard(&applyingTheme);
		applied = MT_ThemeApplyResolved(resolved, activeGuiScale);
	}
	if (!applied) return;
	FireThemeChanged();
}

void CMTThemeRegistry::ClearActiveTheme()
{
	// Order matters: clear the selection BEFORE resetting the style, or
	// VID_SetImGuiStyle's tail hook re-applies the theme we just cleared.
	activeIndex = -1;
	MT_ThemeClearActiveResolved();

	{
		MTThemeApplyGuard guard(&applyingTheme);
		VID_ResetImGuiStyle();
	}
	FireThemeChanged();
}

bool CMTThemeRegistry::IsApplyingTheme() const
{
	return applyingTheme;
}

bool CMTThemeRegistry::UnregisterTheme(const char *id)
{
	if (id == NULL) return false;

	// Find first, and do not hold the index across ClearActiveTheme():
	// that fires onThemeChanged, and a callback registering or unregistering
	// would shift the container under a held index, erasing the wrong entry.
	int idx = -1;
	for (size_t i = 0; i < themes.size(); i++)
		if (strcmp(themes[i].id, id) == 0) { idx = (int)i; break; }
	if (idx < 0) return false;

	if (activeIndex == idx)
		ClearActiveTheme();

	// Re-find: the callback above may have moved things.
	idx = -1;
	for (size_t i = 0; i < themes.size(); i++)
		if (strcmp(themes[i].id, id) == 0) { idx = (int)i; break; }
	if (idx < 0) return true;          // a callback already removed it

	themes.erase(themes.begin() + idx);
	if (activeIndex > idx)
		activeIndex--;                 // indices after the removed one shift down
	FireRegistryChanged();
	return true;
}

// --- callbacks ---------------------------------------------------------

template <typename V, typename F>
static bool AddUniqueCallback(V &vec, F cb, void *userData)
{
	if (cb == NULL) return false;
	for (size_t i = 0; i < vec.size(); i++)
		if (vec[i].first == cb && vec[i].second == userData)
			return false;              // idempotent: hosts register more than once
	vec.push_back(std::make_pair(cb, userData));
	return true;
}

template <typename V, typename F>
static void RemoveCallback(V &vec, F cb, void *userData)
{
	for (size_t i = 0; i < vec.size(); i++)
		if (vec[i].first == cb && vec[i].second == userData)
		{
			vec.erase(vec.begin() + i);
			return;
		}
}

void CMTThemeRegistry::AddThemeChangedCallback(MTThemeChangedCallback cb, void *userData)
{
	AddUniqueCallback(themeChangedCbs, cb, userData);
}

void CMTThemeRegistry::RemoveThemeChangedCallback(MTThemeChangedCallback cb, void *userData)
{
	RemoveCallback(themeChangedCbs, cb, userData);
}

void CMTThemeRegistry::AddRegistryChangedCallback(MTThemeRegistryChangedCallback cb, void *userData)
{
	AddUniqueCallback(registryChangedCbs, cb, userData);
}

void CMTThemeRegistry::RemoveRegistryChangedCallback(MTThemeRegistryChangedCallback cb, void *userData)
{
	RemoveCallback(registryChangedCbs, cb, userData);
}

void CMTThemeRegistry::AddSystemAppearanceChangedCallback(MTSystemAppearanceCallback cb, void *userData)
{
	AddUniqueCallback(appearanceCbs, cb, userData);
}

void CMTThemeRegistry::RemoveSystemAppearanceChangedCallback(MTSystemAppearanceCallback cb, void *userData)
{
	RemoveCallback(appearanceCbs, cb, userData);
}

void CMTThemeRegistry::NotifySystemAppearanceChanged(MTThemeMode osMode)
{
	// Copy: a callback may remove itself.
	std::vector<std::pair<MTSystemAppearanceCallback, void *> > copy = appearanceCbs;
	for (size_t i = 0; i < copy.size(); i++)
		copy[i].first(osMode, copy[i].second);
}

void CMTThemeRegistry::FireThemeChanged()
{
	themeChangedDepth++;
	std::vector<std::pair<MTThemeChangedCallback, void *> > copy = themeChangedCbs;
	for (size_t i = 0; i < copy.size(); i++)
		copy[i].first(copy[i].second);
	themeChangedDepth--;
}

void CMTThemeRegistry::FireRegistryChanged()
{
	registryCallbackDepth++;
	std::vector<std::pair<MTThemeRegistryChangedCallback, void *> > copy = registryChangedCbs;
	for (size_t i = 0; i < copy.size(); i++)
		copy[i].first(copy[i].second);
	registryCallbackDepth--;
}
