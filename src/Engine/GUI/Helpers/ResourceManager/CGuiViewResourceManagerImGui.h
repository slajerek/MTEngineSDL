#pragma once

#include "CGuiView.h"
#include "RES_ResourceManager.h"

#include <string>
#include <vector>

class CGuiViewResourceManagerImGui : public CGuiView
{
public:
	CGuiViewResourceManagerImGui(const char *name, float posX, float posY, float sizeX, float sizeY);
	virtual ~CGuiViewResourceManagerImGui();

	virtual void RenderImGui() override;

private:
	std::vector<RES_ResourceSnapshot> snapshotBuf;
	char pathFilter[256];
	int stateFilter;
	bool cacheOnly;
	bool sortDescending;
	int sortColumn;

	bool PassesFilters(const RES_ResourceSnapshot &snapshot) const;
	void ApplySort();
	const char *GetStateFilterLabel(int value) const;
	static const char *FormatBytes(u64 bytes, char *buf, size_t bufSize);
	static const char *FormatTimeAgo(u64 activatedTime, char *buf, size_t bufSize);
	static std::string GetDisplayName(const std::string &path);
	static bool OpenPathInFileManager(const std::string &path);
};
