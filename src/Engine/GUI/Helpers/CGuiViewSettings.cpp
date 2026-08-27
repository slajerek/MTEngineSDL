#include "CGuiViewSettings.h"
#include "imgui.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <utility>

static std::string CGuiSettingsToLowerAscii(const char *s)
{
    std::string out;
    if (!s)
        return out;

    while (*s)
    {
        unsigned char c = (unsigned char)*s++;
        out.push_back((char)std::tolower(c));
    }
    return out;
}

CGuiViewSettings::CGuiViewSettings(const char *name, float posX, float posY, float posZ, float sizeX, float sizeY)
: CGuiView(name, posX, posY, posZ, sizeX, sizeY)
{
    settingsSearchBuf[0] = 0;
    imGuiExtraWindowFlags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking;
    // Keep engine key/focus routing on the underlying view; ImGui still drives
    // the panel's own widgets. This preserves shortcut routing while the panel
    // is open and only relies on PreRenderImGui for window tagging + bounds.
    imGuiWindowSkipFocusCheck = true;
}

CGuiViewSettings::CGuiViewSettings(const char *name, float posX, float posY, float posZ, float sizeX, float sizeY,
                                   const char *titleI18nKey, const char *stableId)
: CGuiView(name, posX, posY, posZ, sizeX, sizeY, titleI18nKey, stableId)
{
    settingsSearchBuf[0] = 0;
    imGuiExtraWindowFlags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking;
    imGuiWindowSkipFocusCheck = true;
}

CGuiViewSettings::~CGuiViewSettings()
{
}

void CGuiViewSettings::OpenSettings()
{
    settingsIsOpen = true;
    settingsJustOpened = true;
}

// Localized title for the visible label, stable id after "###" so the ImGui
// window identity (and its saved position) doesn't change with the language.
const char *CGuiViewSettings::GetImGuiBeginName() const
{
    const char *title = SettingsGetWindowTitle();
    const char *id = this->name ? this->name : "CGuiViewSettings";
    settingsWindowName = std::string(title ? title : "");
    settingsWindowName += "###";
    settingsWindowName += id;
    return settingsWindowName.c_str();
}

// Claim the whole window rect (title bar + borders included), not just the
// inner content rect the base class reports. This is what lets the mouse-event
// routing stop at the panel: without it, a title-bar drag would fall through to
// the view rendered behind the panel (e.g. a full-screen moving pane).
bool CGuiViewSettings::IsInsideView(float x, float y)
{
    if (!settingsIsOpen || !this->visible)
        return false;
    return x >= windowPosX && x <= windowPosEndX
        && y >= windowPosY && y <= windowPosEndY;
}

void CGuiViewSettings::CloseSettings()
{
    settingsIsOpen = false;
}

bool CGuiViewSettings::TextMatchesSearch(const char *text, const char *query)
{
    if (!query || query[0] == 0)
        return true;
    if (!text || text[0] == 0)
        return false;

    std::string haystack = CGuiSettingsToLowerAscii(text);
    std::string needle = CGuiSettingsToLowerAscii(query);
    return haystack.find(needle) != std::string::npos;
}

void CGuiViewSettings::BuildSearchResult(const std::vector<CGuiSettingsPane> &panes,
                                         const char *query,
                                         CGuiSettingsSearchResult &outResult)
{
    outResult = CGuiSettingsSearchResult{};
    outResult.matchingTargetIndicesByPaneIndex.resize(panes.size());
    outResult.isActive = query && query[0] != 0;

    if (!outResult.isActive)
    {
        outResult.visiblePaneIndices.reserve(panes.size());
        for (int i = 0; i < (int)panes.size(); ++i)
            outResult.visiblePaneIndices.push_back(i);
        return;
    }

    for (int paneIndex = 0; paneIndex < (int)panes.size(); ++paneIndex)
    {
        const CGuiSettingsPane &pane = panes[paneIndex];
        bool paneMatches = TextMatchesSearch(pane.label.c_str(), query);
        std::vector<int> matchingTargets;

        for (int targetIndex = 0; targetIndex < (int)pane.targets.size(); ++targetIndex)
        {
            if (TextMatchesSearch(pane.targets[targetIndex].label.c_str(), query))
                matchingTargets.push_back(targetIndex);
        }

        if (paneMatches || !matchingTargets.empty())
        {
            outResult.visiblePaneIndices.push_back(paneIndex);
            outResult.matchingTargetIndicesByPaneIndex[paneIndex] = std::move(matchingTargets);
            if (outResult.autoSelectPaneIndex < 0)
                outResult.autoSelectPaneIndex = paneIndex;
        }
    }
}

const char *CGuiViewSettings::SettingsGetWindowTitle() const
{
    return name;
}

const char *CGuiViewSettings::SettingsGetCloseLabel() const
{
    return "Close";
}

const char *CGuiViewSettings::SettingsGetSearchPlaceholder() const
{
    return "Search...";
}

const char *CGuiViewSettings::SettingsGetNoResultsLabel() const
{
    return "No settings found";
}

void CGuiViewSettings::RebuildSettingsPanes()
{
    settingsPanes.clear();
    int paneCount = SettingsGetPaneCount();
    if (paneCount < 0)
        paneCount = 0;
    settingsPanes.resize((size_t)paneCount);
    for (int i = 0; i < paneCount; ++i)
        SettingsBuildPane(i, settingsPanes[i]);
}

bool CGuiViewSettings::PaneIsVisible(int paneIndex) const
{
    return std::find(settingsSearchResult.visiblePaneIndices.begin(),
                     settingsSearchResult.visiblePaneIndices.end(),
                     paneIndex) != settingsSearchResult.visiblePaneIndices.end();
}

void CGuiViewSettings::UpdateSelectionForSearch(bool searchChanged)
{
    if (settingsSearchResult.visiblePaneIndices.empty())
        return;

    if (settingsSearchResult.isActive)
    {
        if (searchChanged || !PaneIsVisible(settingsSelectedPane))
        {
            settingsSelectedPane = settingsSearchResult.autoSelectPaneIndex;
            SettingsResetContentScrollNextFrame();
        }
        return;
    }

    if (settingsSearchWasActive)
    {
        settingsSelectedPane = 0;
        SettingsResetContentScrollNextFrame();
    }
    else if (!PaneIsVisible(settingsSelectedPane))
    {
        settingsSelectedPane = settingsSearchResult.visiblePaneIndices[0];
        SettingsResetContentScrollNextFrame();
    }
}

void CGuiViewSettings::SettingsResetContentScrollNextFrame()
{
    settingsResetContentScroll = true;
}

bool CGuiViewSettings::SettingsShouldShowTarget(const char *targetStableId) const
{
    if (!settingsSearchResult.isActive)
        return true;
    if (settingsSelectedPane < 0 || settingsSelectedPane >= (int)settingsPanes.size())
        return false;
    if (!targetStableId)
        return false;
    if (settingsSelectedPane >= (int)settingsSearchResult.matchingTargetIndicesByPaneIndex.size())
        return false;

    const std::vector<int> &matches = settingsSearchResult.matchingTargetIndicesByPaneIndex[settingsSelectedPane];
    if (matches.empty())
        return true;

    const CGuiSettingsPane &pane = settingsPanes[settingsSelectedPane];
    for (int targetIndex : matches)
    {
        if (targetIndex >= 0 && targetIndex < (int)pane.targets.size() &&
            pane.targets[targetIndex].stableId == targetStableId)
            return true;
    }
    return false;
}

bool CGuiViewSettings::SettingsShouldShowAnyTarget(std::initializer_list<const char *> targetStableIds) const
{
    for (const char *targetStableId : targetStableIds)
        if (SettingsShouldShowTarget(targetStableId))
            return true;
    return false;
}

void CGuiViewSettings::RenderImGui()
{
    if (!settingsIsOpen)
        return;

    // PreRenderImGui wires the window's close (X) button to &this->visible and
    // only renders while visible, so keep it in sync with the open state.
    this->visible = true;

    // Center the panel on the frame it opens. PreRenderImGui otherwise restores
    // the last user position (or the FirstUseEver default at posX/posY).
    if (settingsJustOpened)
    {
        const ImVec2 disp = ImGui::GetIO().DisplaySize;
        const ImVec2 vpPos = ImGui::GetMainViewport()->Pos;
        const float w = sizeX > 0.0f ? sizeX : 720.0f;
        const float h = sizeY > 0.0f ? sizeY : 480.0f;
        imGuiForceThisFrameNewPositionAbsolute = true;
        thisFrameNewAbsPosX = vpPos.x + (disp.x - w) * 0.5f;
        thisFrameNewAbsPosY = vpPos.y + (disp.y - h) * 0.5f;
        settingsJustOpened = false;
    }

    ImGui::SetNextWindowSizeConstraints(ImVec2(480, 320), ImVec2(FLT_MAX, FLT_MAX));

    // PreRenderImGui does the ImGui::Begin (tagging the window with userData and
    // maintaining view bounds so the panel participates in mouse-event routing).
    PreRenderImGui();

    RebuildSettingsPanes();

    const bool wasActiveBeforeBuild = settingsSearchWasActive;
    CGuiViewSettings::BuildSearchResult(settingsPanes, settingsSearchBuf, settingsSearchResult);

    const float footerH = ImGui::GetFrameHeightWithSpacing();
    const float sidebarW = 140.0f;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;

    ImGui::BeginChild("##settings_sidebar", ImVec2(sidebarW, -footerH), ImGuiChildFlags_Borders);
    for (int paneIndex : settingsSearchResult.visiblePaneIndices)
    {
        const char *label = settingsPanes[paneIndex].label.c_str();
        if (ImGui::Selectable(label, settingsSelectedPane == paneIndex))
        {
            settingsSelectedPane = paneIndex;
            SettingsResetContentScrollNextFrame();
        }
    }
    ImGui::EndChild();

    ImGui::SameLine(0.0f, spacing);

    ImGui::BeginChild("##settings_content", ImVec2(0.0f, -footerH), ImGuiChildFlags_None);
    if (settingsResetContentScroll)
    {
        ImGui::SetScrollY(0.0f);
        settingsResetContentScroll = false;
    }
    if (settingsSearchResult.visiblePaneIndices.empty())
    {
        ImGui::TextDisabled("%s", SettingsGetNoResultsLabel());
    }
    else if (settingsSelectedPane >= 0 && settingsSelectedPane < (int)settingsPanes.size())
    {
        SettingsRenderPane(settingsSelectedPane);
    }
    ImGui::EndChild();

    const float btnW = 120.0f;
    const float searchW = std::max(160.0f, ImGui::GetContentRegionAvail().x - btnW - spacing);
    ImGui::SetNextItemWidth(searchW);
    bool searchChanged = ImGui::InputTextWithHint("##settings_search",
                                                   SettingsGetSearchPlaceholder(),
                                                   settingsSearchBuf,
                                                   sizeof(settingsSearchBuf));
    ImGui::SameLine();
    if (ImGui::Button(SettingsGetCloseLabel(), ImVec2(btnW, 0)))
        CloseSettings();

    if (searchChanged || wasActiveBeforeBuild != settingsSearchResult.isActive)
    {
        CGuiViewSettings::BuildSearchResult(settingsPanes, settingsSearchBuf, settingsSearchResult);
        UpdateSelectionForSearch(searchChanged);
    }
    settingsSearchWasActive = settingsSearchResult.isActive;

    // PostRenderImGui does the matching ImGui::End().
    PostRenderImGui();

    // The window's title-bar close (X) button clears this->visible.
    if (!this->visible)
        CloseSettings();
}
