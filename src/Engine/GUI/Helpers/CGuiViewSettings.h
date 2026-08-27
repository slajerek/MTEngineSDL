#pragma once

#include "CGuiView.h"
#include <initializer_list>
#include <string>
#include <vector>

struct CGuiSettingsSearchTarget
{
    std::string stableId;
    std::string label;
};

struct CGuiSettingsPane
{
    std::string stableId;
    std::string label;
    std::vector<CGuiSettingsSearchTarget> targets;
};

struct CGuiSettingsSearchResult
{
    bool isActive = false;
    int autoSelectPaneIndex = -1;
    std::vector<int> visiblePaneIndices;
    std::vector<std::vector<int>> matchingTargetIndicesByPaneIndex;
};

class CGuiViewSettings : public CGuiView
{
public:
    CGuiViewSettings(const char *name, float posX, float posY, float posZ, float sizeX, float sizeY);
    CGuiViewSettings(const char *name, float posX, float posY, float posZ, float sizeX, float sizeY,
                     const char *titleI18nKey, const char *stableId);
    virtual ~CGuiViewSettings();

    virtual void OpenSettings();
    virtual void CloseSettings();
    bool IsSettingsOpen() const { return settingsIsOpen; }
    const char *GetSettingsSearchQuery() const { return settingsSearchBuf; }
    int GetSettingsSelectedPaneIndex() const { return settingsSelectedPane; }
    bool IsSettingsPaneVisible(int paneIndex) const { return PaneIsVisible(paneIndex); }
    bool IsSettingsTargetVisible(const char *targetStableId) const { return SettingsShouldShowTarget(targetStableId); }

    virtual void RenderImGui() override;
    virtual const char *GetImGuiBeginName() const override;
    virtual bool IsInsideView(float x, float y) override;

    static bool TextMatchesSearch(const char *text, const char *query);
    static void BuildSearchResult(const std::vector<CGuiSettingsPane> &panes,
                                  const char *query,
                                  CGuiSettingsSearchResult &outResult);

protected:
    virtual int SettingsGetPaneCount() const = 0;
    virtual void SettingsBuildPane(int paneIndex, CGuiSettingsPane &outPane) = 0;
    virtual void SettingsRenderPane(int paneIndex) = 0;

    virtual const char *SettingsGetWindowTitle() const;
    virtual const char *SettingsGetCloseLabel() const;
    virtual const char *SettingsGetSearchPlaceholder() const;
    virtual const char *SettingsGetNoResultsLabel() const;

    void SettingsResetContentScrollNextFrame();
    bool SettingsShouldShowTarget(const char *targetStableId) const;
    bool SettingsShouldShowAnyTarget(std::initializer_list<const char *> targetStableIds) const;

    int settingsSelectedPane = 0;
    bool settingsIsOpen = false;
    char settingsSearchBuf[256];

private:
    bool settingsJustOpened = false;
    mutable std::string settingsWindowName;
    std::vector<CGuiSettingsPane> settingsPanes;
    CGuiSettingsSearchResult settingsSearchResult;
    bool settingsSearchWasActive = false;
    bool settingsResetContentScroll = false;

    void RebuildSettingsPanes();
    void UpdateSelectionForSearch(bool searchChanged);
    bool PaneIsVisible(int paneIndex) const;
};
