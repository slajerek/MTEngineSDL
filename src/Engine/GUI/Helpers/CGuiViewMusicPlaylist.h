#pragma once

#include "CGuiView.h"
#include "CSystemFileDialogCallback.h"
#include "CMusicWaveformCache.h"
#include "CSlrMusicCueTransitionPlanner.h"
#include "CSlrMusicPlaylistController.h"
#include "CPlaylistPlaybackManager.h"
#include "SYS_FileSystem.h"

#include <atomic>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

class CSlrThread;

class CGuiViewMusicPlaylist : public CGuiView, public CSystemFileDialogCallback
{
public:
	enum class CMusicPlaylistDocumentMode
	{
		SettingsScratch,
		ExternalFile,
	};

	static void SetTranslateLabelFunc(const char *(*fn)(const char *key))
	{
		sTranslateLabelFunc = fn;
	}

	static float GetWaveformSpanForZoom(float zoom);
	static float ClampWaveformCenterForZoom(float centerNorm, float zoom);
	static float PanWaveformCenterForZoom(float centerNorm, float zoom, float wheelDelta);
	static float GetWaveformPanWheelDelta(float mouseWheelH, float mouseWheelV, bool shiftHeld, float zoom);
	static std::string GetTrackTitleFromPath(const std::string &trackPath);

	CGuiViewMusicPlaylist(const char *name, float posX, float posY, float posZ, float sizeX, float sizeY);
	virtual ~CGuiViewMusicPlaylist();

	virtual void RenderImGui() override;

	virtual void SystemDialogFileOpenSelected(CSlrString *path) override;
	virtual void SystemDialogFilesOpenSelected(std::vector<CSlrString *> *paths) override;
	virtual void SystemDialogFileOpenCancelled() override;
	virtual void SystemDialogFileSaveSelected(CSlrString *path) override;
	virtual void SystemDialogFileSaveCancelled() override;

	void SetProjectRootPath(const std::string &path);
	bool LoadSettingsScratchPlaylist();
	bool SaveSettingsScratchPlaylist();
	bool SaveCopyToSettingsScratch();
	bool LoadPlaylistFromFile(const std::string &path);
	bool SaveCurrentPlaylist();
	bool SavePlaylistAs(const std::string &path, CSlrMusicPlaylistPathMode pathMode);
	void NewScratchPlaylist();
	bool HasExternalPlaylistFile() const;
	const std::string &GetCurrentPlaylistFilePath() const;
	CSlrMusicPlaylistPathMode GetCurrentPathMode() const;
	CMusicPlaylistDocumentMode GetCurrentDocumentMode() const;
	bool TryGetCurrentPlaylistPathRelativeToProjectRoot(std::string &outPath) const;
	void TickPlayback(float deltaTime);
	void ResumePlaybackIfNeeded();
	void SavePlaybackStateNow();
	void SetMusicVolume(float volume);
	float GetMusicVolume() const;

private:
	enum class PlaylistDialogMode
	{
		None,
		LoadPlaylist,
		SavePlaylistAs,
		AddTracks,
	};

	void EnsureInitialPlaylistLoaded();
	std::string GetSettingsScratchPlaylistPath() const;
	std::string GetPlaybackStateFilePath() const;
	void AutosaveCurrentDocument();
	void ApplyPlaybackSettingsFromController();
	void UpdateDocumentStatusMessage(const char *message);
	std::optional<std::string> GetProjectRelativePath(const std::string &absolutePath) const;
	bool CanUseProjectRelativePaths(std::string *reasonOut = nullptr) const;
	const char *GetDocumentModeLabel() const;
	const char *GetPathModeLabel() const;
	std::string GetDisplayTrackPath(const std::string &trackPath) const;
	std::string GetFolderFromPath(const std::string &path) const;

	bool IsSelected(int idx) const;
	void SelectSingle(int idx);
	void ToggleSelection(int idx);
	void SelectRange(int from, int to);
	void RemoveSelectedTracks();

	void RenderPlaylistTable();
	void RenderKeyboardNavigation();
	void RenderCrossfadeControls();

	struct SWaveformCacheEntry
	{
		CMusicWaveformCache cache;
		CSlrThread *loadThread = nullptr;
		std::atomic<bool> isLoading{false};
		bool attemptedLoad = false;
		bool loadSucceeded = false;
	};

	SWaveformCacheEntry *GetWaveformEntry(const std::string &trackPath);
	std::string GetWaveformSidecarPath(const std::string &trackPath) const;
	bool GetWaveformSourceInfo(const std::string &trackPath, CMusicWaveformSourceInfo &outInfo) const;
	void EvictLruWaveformEntries();
	void CleanupWaveformThread(SWaveformCacheEntry *entry);
	void ResetWaveformViewForTrack(int trackId, float centerNorm);
	void RenderWaveformPanel(const CMusicWaveformCache &cache, int64_t maxSample, int64_t cueIn, int64_t cueOut);
	void RenderCueEditor();

	const char *L(const char *key, const char *fallback) const;

private:
	static const char *(*sTranslateLabelFunc)(const char *key);
	static const int kMaxWaveformCacheEntries = 20;

	CSlrMusicPlaylistController playlistController;
	CPlaylistPlaybackManager playbackManager;
	std::map<std::string, std::unique_ptr<SWaveformCacheEntry>> waveformCacheByPath;
	std::list<std::string> waveformLruOrder;
	std::list<CSlrString *> trackExtensions;
	std::list<CSlrString *> playlistExtensions;
	std::unordered_set<int> selectedRows;
	int cursorRow = -1;
	int anchorRow = -1;
	int64_t cueCursorSample = 0;
	float waveformZoom = 1.0f;
	float waveformCenterNorm = 0.5f;
	int waveformViewTrackId = -1;
	bool initialPlaylistResolved = false;
	bool documentDirty = false;
	bool resumePlaybackTriggered = false;
	PlaylistDialogMode playlistDialogMode = PlaylistDialogMode::None;
	CSlrMusicPlaylistPathMode pendingSaveAsPathMode = CSlrMusicPlaylistPathMode::Absolute;
	CMusicPlaylistDocumentMode currentDocumentMode = CMusicPlaylistDocumentMode::SettingsScratch;
	CSlrMusicPlaylistPathMode currentPathMode = CSlrMusicPlaylistPathMode::Absolute;
	std::string projectRootPath;
	std::string currentPlaylistFilePath;
	std::string documentStatusMessage;
};
