#include "CGuiViewResourceManagerImGui.h"

#include "IconsFontAwesome_c.h"
#include "imgui.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>

#if defined(_WIN32)
#include <shellapi.h>
#endif

CGuiViewResourceManagerImGui::CGuiViewResourceManagerImGui(const char *name, float posX, float posY, float sizeX, float sizeY)
: CGuiView(name, posX, posY, -1, sizeX, sizeY)
{
	visible = false;
	pathFilter[0] = 0;
	stateFilter = 0;
	cacheOnly = false;
	sortDescending = true;
	sortColumn = 2;
}

CGuiViewResourceManagerImGui::~CGuiViewResourceManagerImGui()
{
}

const char *CGuiViewResourceManagerImGui::GetStateFilterLabel(int value) const
{
	switch (value)
	{
		case 1: return "Loaded";
		case 2: return "Loading";
		case 3: return "Deallocated";
		case 4: return "Evicting";
		case 5: return "Error";
		default: return "All";
	}
}

bool CGuiViewResourceManagerImGui::PassesFilters(const RES_ResourceSnapshot &snapshot) const
{
	if (cacheOnly && snapshot.cacheKey == 0)
		return false;

	if (pathFilter[0] != 0 && strstr(snapshot.path.c_str(), pathFilter) == NULL)
		return false;

	if (stateFilter == 0)
		return true;
	if (stateFilter == 1) return snapshot.state == RESOURCE_STATE_LOADED;
	if (stateFilter == 2) return snapshot.state == RESOURCE_STATE_LOADING;
	if (stateFilter == 3) return snapshot.state == RESOURCE_STATE_DEALLOCATED;
	if (stateFilter == 4) return snapshot.state == RESOURCE_STATE_EVICTING;
	if (stateFilter == 5) return snapshot.state == RESOURCE_STATE_ERROR;
	return true;
}

void CGuiViewResourceManagerImGui::ApplySort()
{
	std::sort(snapshotBuf.begin(), snapshotBuf.end(), [this](const RES_ResourceSnapshot &a, const RES_ResourceSnapshot &b) {
		auto cmp = [this](auto left, auto right) {
			return sortDescending ? (left > right) : (left < right);
		};

		switch (sortColumn)
		{
			case 0: return cmp(a.path, b.path);
			case 1: return cmp(a.stateName, b.stateName);
			case 3: return cmp(a.loadingSize, b.loadingSize);
			case 4: return cmp(a.activatedTime, b.activatedTime);
			case 5: return cmp(a.priority, b.priority);
			case 6: return cmp(a.isActive, b.isActive);
			case 7: return cmp(a.cacheKey != 0, b.cacheKey != 0);
			case 8: return cmp(a.hashCode, b.hashCode);
			default: return cmp(a.idleSize, b.idleSize);
		}
	});
}

const char *CGuiViewResourceManagerImGui::FormatBytes(u64 bytes, char *buf, size_t bufSize)
{
	const double oneKb = 1024.0;
	const double oneMb = oneKb * 1024.0;
	if (bytes >= (u64)oneMb)
		snprintf(buf, bufSize, "%.1f MB", (double)bytes / oneMb);
	else if (bytes >= (u64)oneKb)
		snprintf(buf, bufSize, "%.1f KB", (double)bytes / oneKb);
	else
		snprintf(buf, bufSize, "%llu B", bytes);
	return buf;
}

const char *CGuiViewResourceManagerImGui::FormatTimeAgo(u64 activatedTime, char *buf, size_t bufSize)
{
	if (activatedTime == 0)
	{
		snprintf(buf, bufSize, "never");
		return buf;
	}

	u64 now = SYS_GetCurrentTimeInMillis();
	u64 diff = now > activatedTime ? (now - activatedTime) : 0;
	if (diff < 1000)
		snprintf(buf, bufSize, "%llums ago", diff);
	else if (diff < 60000)
		snprintf(buf, bufSize, "%llus ago", diff / 1000);
	else
		snprintf(buf, bufSize, "%llum ago", diff / 60000);
	return buf;
}

std::string CGuiViewResourceManagerImGui::GetDisplayName(const std::string &path)
{
	if (path.empty())
		return std::string();

	std::filesystem::path fsPath(path);
	std::string name = fsPath.filename().string();
	return name.empty() ? path : name;
}

bool CGuiViewResourceManagerImGui::OpenPathInFileManager(const std::string &path)
{
	if (path.empty())
		return false;

	std::filesystem::path fsPath(path);
	std::string targetPath = path;

#if defined(_WIN32)
	std::string command = "explorer.exe /select,\"" + fsPath.string() + "\"";
	return std::system(command.c_str()) == 0;
#elif defined(__APPLE__)
	std::string command = "open -R \"" + fsPath.string() + "\"";
	return std::system(command.c_str()) == 0;
#else
	std::string folderPath = fsPath.has_parent_path() ? fsPath.parent_path().string() : fsPath.string();
	std::string command = "xdg-open \"" + folderPath + "\"";
	return std::system(command.c_str()) == 0;
#endif
}

void CGuiViewResourceManagerImGui::RenderImGui()
{
	PreRenderImGui();

	RES_DebugSnapshotResources(snapshotBuf);

	int loadingCount = 0;
	int deallocatedCount = 0;
	int errorCount = 0;
	for (const RES_ResourceSnapshot &snapshot : snapshotBuf)
	{
		if (snapshot.cacheKey == 0)
			continue;
		if (snapshot.state == RESOURCE_STATE_LOADING)
			loadingCount++;
		else if (snapshot.state == RESOURCE_STATE_DEALLOCATED)
			deallocatedCount++;
		else if (snapshot.state == RESOURCE_STATE_ERROR)
			errorCount++;
	}

	ImGui::Text("Budget: %.1f MB", (double)RES_CacheGetBudgetBytes() / (1024.0 * 1024.0));
	ImGui::SameLine();
	ImGui::Text("Total used: %.1f MB", (double)RES_CacheGetTotalUsedBytes() / (1024.0 * 1024.0));
	ImGui::SameLine();
	ImGui::Text("All entries: %d", (int)snapshotBuf.size());

	ImGui::Text("Cache used: %.1f MB", (double)RES_CacheGetCacheUsedBytes() / (1024.0 * 1024.0));
	ImGui::SameLine();
	ImGui::Text("Cache loaded: %d / %d", RES_CacheGetCacheLoadedCount(), RES_CacheGetCacheEntryCount());
	ImGui::SameLine();
	ImGui::Text("Loading: %d", loadingCount);
	ImGui::SameLine();
	ImGui::Text("Deallocated: %d", deallocatedCount);
	ImGui::SameLine();
	ImGui::Text("Error: %d", errorCount);

	if (ImGui::Button("Force GC"))
		RES_CacheForceEvictLRU(RES_CacheGetCacheUsedBytes() / 2);
	ImGui::SameLine();
	if (ImGui::Button("Clear All Cached"))
		ImGui::OpenPopup("ConfirmClearAllCached");
	if (ImGui::BeginPopupModal("ConfirmClearAllCached", NULL, ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::Text("Deallocate all currently loaded cache entries?");
		if (ImGui::Button("Clear"))
		{
			RES_CacheClearAll();
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel"))
			ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}

	ImGui::InputText("Path filter", pathFilter, sizeof(pathFilter));
	ImGui::SameLine();
	if (ImGui::BeginCombo("State", GetStateFilterLabel(stateFilter)))
	{
		for (int i = 0; i <= 5; i++)
		{
			bool selected = (stateFilter == i);
			if (ImGui::Selectable(GetStateFilterLabel(i), selected))
				stateFilter = i;
			if (selected)
				ImGui::SetItemDefaultFocus();
		}
		ImGui::EndCombo();
	}
	ImGui::SameLine();
	ImGui::Checkbox("Cache only", &cacheOnly);

	if (ImGui::BeginTable("ResourceManagerTable", 10, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Sortable, ImVec2(0, 0)))
	{
		ImGui::TableSetupColumn("Name");
		ImGui::TableSetupColumn("State");
		ImGui::TableSetupColumn("Idle Size", ImGuiTableColumnFlags_DefaultSort);
		ImGui::TableSetupColumn("Loading Size");
		ImGui::TableSetupColumn("Last Touch");
		ImGui::TableSetupColumn("Priority");
		ImGui::TableSetupColumn("Active");
		ImGui::TableSetupColumn("Cache");
		ImGui::TableSetupColumn("Hash");
		ImGui::TableSetupColumn("Action");
		ImGui::TableHeadersRow();

		if (ImGuiTableSortSpecs *sorts = ImGui::TableGetSortSpecs())
		{
			if (sorts->SpecsCount > 0)
			{
				sortColumn = sorts->Specs[0].ColumnIndex;
				sortDescending = (sorts->Specs[0].SortDirection != ImGuiSortDirection_Ascending);
			}
		}

		ApplySort();
		for (const RES_ResourceSnapshot &snapshot : snapshotBuf)
		{
			if (!PassesFilters(snapshot))
				continue;

			char idleBuf[64];
			char loadingBuf[64];
			char touchBuf[64];

			ImGui::PushID((int)snapshot.hashCode);
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			std::string displayName = GetDisplayName(snapshot.path);
			ImGui::TextUnformatted(displayName.c_str());
			if (ImGui::BeginItemTooltip())
			{
				ImGui::PushTextWrapPos(500.0f);
				ImGui::TextUnformatted(snapshot.path.c_str());
				ImGui::PopTextWrapPos();
				if (!snapshot.path.empty())
				{
					if (ImGui::SmallButton(ICON_FA_FOLDER_OPEN))
						OpenPathInFileManager(snapshot.path);
					if (ImGui::IsItemHovered())
						ImGui::SetTooltip("Open in file manager");
				}
				ImGui::EndTooltip();
			}

			ImGui::TableSetColumnIndex(1);
			ImGui::TextUnformatted(snapshot.stateName.c_str());

			ImGui::TableSetColumnIndex(2);
			ImGui::TextUnformatted(FormatBytes(snapshot.idleSize, idleBuf, sizeof(idleBuf)));

			ImGui::TableSetColumnIndex(3);
			ImGui::TextUnformatted(FormatBytes(snapshot.loadingSize, loadingBuf, sizeof(loadingBuf)));

			ImGui::TableSetColumnIndex(4);
			ImGui::TextUnformatted(FormatTimeAgo(snapshot.activatedTime, touchBuf, sizeof(touchBuf)));

			ImGui::TableSetColumnIndex(5);
			ImGui::Text("%d", snapshot.priority);

			ImGui::TableSetColumnIndex(6);
			ImGui::TextUnformatted(snapshot.isActive ? "Yes" : "No");

			ImGui::TableSetColumnIndex(7);
			ImGui::TextUnformatted(snapshot.cacheKey != 0 ? "Yes" : "No");

			ImGui::TableSetColumnIndex(8);
			ImGui::Text("%llu", snapshot.hashCode);

			ImGui::TableSetColumnIndex(9);
			if (snapshot.state == RESOURCE_STATE_ERROR && snapshot.cacheKey != 0)
			{
				if (ImGui::Button("Retry"))
					RES_CacheRetry(snapshot.path.c_str(), snapshot.cacheLinearScaling);
			}

			ImGui::PopID();
		}

		ImGui::EndTable();
	}

	PostRenderImGui();
}
