#include "CGuiViewMusicPlaylist.h"

#include "IconsFontAwesome_c.h"
#include "SYS_Threading.h"

#include <algorithm>
#include <cctype>
#include <cinttypes>
#include <cmath>
#include <cstring>
#include <filesystem>

// ─── Async waveform loader ─────────────────────────────────────────────────────

namespace {

std::filesystem::path NormalizeAbsolutePath(const std::string &path)
{
	if (path.empty())
		return std::filesystem::path();

	std::filesystem::path fsPath(path);
	if (fsPath.is_relative())
		fsPath = std::filesystem::absolute(fsPath);
	return fsPath.lexically_normal();
}

bool IsPathWithinRoot(const std::filesystem::path &path, const std::filesystem::path &root)
{
	if (path.empty() || root.empty())
		return false;

	auto rootIt = root.begin();
	auto pathIt = path.begin();
	for (; rootIt != root.end(); ++rootIt, ++pathIt)
	{
		if (pathIt == path.end() || *rootIt != *pathIt)
			return false;
	}

	return true;
}

std::string PathToGenericString(const std::filesystem::path &path)
{
	return path.lexically_normal().generic_string<char>();
}

class CWaveformLoadThread : public CSlrThread
{
public:
	CWaveformLoadThread(CMusicWaveformCache *cache, const std::string &path,
						const std::string &sidecarPath,
						const CMusicWaveformSourceInfo &sourceInfo,
						std::atomic<bool> *isLoadingFlag, bool *loadSucceededFlag)
		: CSlrThread()
		, cache(cache)
		, filePath(path)
		, sidecarPath(sidecarPath)
		, sourceInfo(sourceInfo)
		, isLoadingFlag(isLoadingFlag)
		, loadSucceededFlag(loadSucceededFlag)
	{
		ThreadSetName("WaveformLoad");
	}

	void ThreadRun(void * /*passData*/) override
	{
		bool success = false;
		if (!sidecarPath.empty())
			success = cache->LoadFromBinaryFile(sidecarPath, sourceInfo);
		if (!success)
		{
			success = cache->LoadFromOggFile(filePath, sourceInfo.envelopePointCount);
			if (success && !sidecarPath.empty())
				cache->SaveToBinaryFile(sidecarPath, sourceInfo);
		}
		*loadSucceededFlag = success;
		isLoadingFlag->store(false);
	}

	CMusicWaveformCache *cache;
	std::string filePath;
	std::string sidecarPath;
	CMusicWaveformSourceInfo sourceInfo;
	std::atomic<bool> *isLoadingFlag;
	bool *loadSucceededFlag;
};

} // anonymous namespace

// ─── Static members ─────────────────────────────────────────────────────────────

const char *(*CGuiViewMusicPlaylist::sTranslateLabelFunc)(const char *key) = NULL;

float CGuiViewMusicPlaylist::GetWaveformSpanForZoom(float zoom)
{
	float clampedZoom = std::clamp(zoom, 1.0f, 128.0f);
	float span = 1.0f / clampedZoom;
	if (span > 1.0f)
		span = 1.0f;
	return span;
}

float CGuiViewMusicPlaylist::ClampWaveformCenterForZoom(float centerNorm, float zoom)
{
	float span = GetWaveformSpanForZoom(zoom);
	float minCenter = span * 0.5f;
	float maxCenter = 1.0f - span * 0.5f;
	if (minCenter > maxCenter)
		return 0.5f;
	return std::clamp(centerNorm, minCenter, maxCenter);
}

float CGuiViewMusicPlaylist::PanWaveformCenterForZoom(float centerNorm, float zoom, float wheelDelta)
{
	float span = GetWaveformSpanForZoom(zoom);
	if (span >= 1.0f)
		return 0.5f;

	float panStep = span * 0.15f;
	return ClampWaveformCenterForZoom(centerNorm + wheelDelta * panStep, zoom);
}

float CGuiViewMusicPlaylist::GetWaveformPanWheelDelta(float mouseWheelH, float mouseWheelV, bool shiftHeld, float zoom)
{
	if (zoom <= 1.0f)
		return 0.0f;
	if (mouseWheelH != 0.0f)
		return mouseWheelH;
	if (shiftHeld)
		return mouseWheelV;
	return 0.0f;
}

std::string CGuiViewMusicPlaylist::GetTrackTitleFromPath(const std::string &trackPath)
{
	if (trackPath.empty())
		return std::string();

	std::filesystem::path path(trackPath);
	std::string title = path.stem().generic_string<char>();
	if (!title.empty())
		return title;

	std::string filename = path.filename().generic_string<char>();
	if (!filename.empty())
		return filename;

	return trackPath;
}

// ─── Construction / Destruction ─────────────────────────────────────────────────

CGuiViewMusicPlaylist::CGuiViewMusicPlaylist(const char *name, float posX, float posY, float posZ, float sizeX, float sizeY)
: CGuiView(name, posX, posY, posZ, sizeX, sizeY)
{
	trackExtensions.push_back(new CSlrString("ogg"));
	playlistExtensions.push_back(new CSlrString("json"));
}

CGuiViewMusicPlaylist::~CGuiViewMusicPlaylist()
{
	for (auto &[path, entry] : waveformCacheByPath)
	{
		if (entry)
			CleanupWaveformThread(entry.get());
	}
	waveformCacheByPath.clear();

	for (CSlrString *ext : trackExtensions)
		delete ext;
	trackExtensions.clear();

	for (CSlrString *ext : playlistExtensions)
		delete ext;
	playlistExtensions.clear();
}

// ─── Main render ────────────────────────────────────────────────────────────────

void CGuiViewMusicPlaylist::RenderImGui()
{
	PreRenderImGui();

	if (ImGui::IsWindowAppearing() && playlistController.IsPlaying())
	{
		int idx = playlistController.GetCurrentTrackIndex();
		if (idx >= 0)
			SelectSingle(idx);
	}

	EnsureInitialPlaylistLoaded();

	if (ImGui::Button(L("music.playlist.new_scratch", "New Scratch")))
	{
		NewScratchPlaylist();
	}
	ImGui::SameLine();
	if (ImGui::Button(L("music.playlist.load_playlist", "Load Playlist...")))
	{
		playlistDialogMode = PlaylistDialogMode::LoadPlaylist;
		CSlrString title(L("music.playlist.load_playlist", "Load Playlist..."));
		CSlrString *defaultFolder = nullptr;
		std::string folder = GetFolderFromPath(currentPlaylistFilePath);
		if (folder.empty())
			folder = !projectRootPath.empty() ? projectRootPath : playlistController.GetLastOpenFolder();
		if (!folder.empty())
			defaultFolder = new CSlrString(folder.c_str());
		SYS_DialogOpenFile(this, &playlistExtensions, defaultFolder, &title);
		if (defaultFolder)
			delete defaultFolder;
	}
	ImGui::SameLine();
	if (ImGui::Button(L("music.playlist.save", "Save")))
	{
		SaveCurrentPlaylist();
	}
	ImGui::SameLine();
	if (ImGui::Button(L("music.playlist.save_as", "Save As...")))
	{
		playlistDialogMode = PlaylistDialogMode::SavePlaylistAs;
		pendingSaveAsPathMode = currentPathMode;
		CSlrString title(L("music.playlist.save_as", "Save As..."));
		CSlrString defaultFileName("music-playlist.json");
		CSlrString *defaultFolder = nullptr;
		std::string folder = GetFolderFromPath(currentPlaylistFilePath);
		if (folder.empty())
			folder = !projectRootPath.empty() ? projectRootPath : playlistController.GetLastOpenFolder();
		if (!folder.empty())
			defaultFolder = new CSlrString(folder.c_str());
		SYS_DialogSaveFile(this, &playlistExtensions, &defaultFileName, defaultFolder, &title);
		if (defaultFolder)
			delete defaultFolder;
	}
	ImGui::SameLine();
	if (ImGui::Button(L("music.playlist.save_to_settings", "Save To Settings")))
	{
		SaveCopyToSettingsScratch();
	}

	bool canUseRelativePaths = CanUseProjectRelativePaths();
	ImGui::SameLine();
	if (!canUseRelativePaths)
		ImGui::BeginDisabled();
	if (ImGui::Button(L("music.playlist.use_relative_paths", "Use Relative Paths")))
	{
		currentPathMode = CSlrMusicPlaylistPathMode::ProjectRelative;
		documentDirty = true;
		UpdateDocumentStatusMessage(L("music.playlist.path_mode_relative", "Path mode set to project-relative"));
	}
	if (!canUseRelativePaths)
		ImGui::EndDisabled();
	ImGui::SameLine();
	if (ImGui::Button(L("music.playlist.use_absolute_paths", "Use Absolute Paths")))
	{
		currentPathMode = CSlrMusicPlaylistPathMode::Absolute;
		documentDirty = true;
		UpdateDocumentStatusMessage(L("music.playlist.path_mode_absolute", "Path mode set to absolute"));
	}

	ImGui::TextWrapped("%s: %s | %s: %s | %s: %s%s",
		L("music.playlist.current_mode", "Current Mode"), GetDocumentModeLabel(),
		L("music.playlist.path_mode", "Path Mode"), GetPathModeLabel(),
		L("music.playlist.current_file", "Current File"),
		currentPlaylistFilePath.empty() ? L("music.playlist.settings_scratch", "settings scratch") : currentPlaylistFilePath.c_str(),
		documentDirty ? " *" : "");
	ImGui::TextWrapped("%s: %s",
		L("music.playlist.project_root", "Project Root"),
		projectRootPath.empty() ? L("music.playlist.none", "(none)") : projectRootPath.c_str());
	if (!documentStatusMessage.empty())
		ImGui::TextWrapped("%s", documentStatusMessage.c_str());

	if (ImGui::Button(L("music.playlist.add_track", "Add Track")))
	{
		playlistDialogMode = PlaylistDialogMode::AddTracks;
		CSlrString title(L("music.playlist.dialog.add_tracks", "Add music tracks"));
		CSlrString *defaultFolder = NULL;
		if (!playlistController.GetLastOpenFolder().empty())
			defaultFolder = new CSlrString(playlistController.GetLastOpenFolder().c_str());

		SYS_DialogOpenFiles(this, &trackExtensions, defaultFolder, &title);
		if (defaultFolder)
			delete defaultFolder;
	}
	ImGui::SameLine();
	if (ImGui::Button(L("music.playlist.remove_selected", "Remove Selected")))
	{
		RemoveSelectedTracks();
	}
	ImGui::SameLine();
	if (ImGui::Button(L("music.playlist.clear_all", "Clear All")))
	{
		playlistController.ClearTracks();
		selectedRows.clear();
		cursorRow = -1;
		anchorRow = -1;
		playbackManager.ClearAllPlayback();
		AutosaveCurrentDocument();
	}
	// ─── Transport controls ─────────────────────────────────────────────────
	ImGui::SameLine();
	ImGui::Text("|");
	ImGui::SameLine();
	if (ImGui::Button(ICON_FA_STEP_BACKWARD "##prev"))
	{
		const auto &tracks = playlistController.GetTracks();
		if (!tracks.empty())
		{
			if (playlistController.IsPlaying())
			{
				playbackManager.RequestCueMatchedPrevTrack(playlistController);
			}
			else
			{
				int ref = cursorRow;
				if (ref < 0) ref = playlistController.GetCurrentTrackIndex();
				if (ref < 0) ref = 0;
				int target = std::max(0, ref - 1);
				playlistController.StartPlaybackAtIndex(target);
				int newIdx = playlistController.GetCurrentTrackIndex();
				if (newIdx >= 0 && newIdx < (int)tracks.size())
					playbackManager.StartTrackPlayback(tracks[newIdx], std::max<int64_t>(0, tracks[newIdx].cueInSample));
			}
			int finalIdx = playlistController.GetCurrentTrackIndex();
			if (finalIdx >= 0 && finalIdx < (int)tracks.size())
				SelectSingle(finalIdx);
		}
	}
	ImGui::SameLine();
	if (playbackManager.IsPaused())
	{
		if (ImGui::Button(ICON_FA_PLAY "##play"))
		{
			playbackManager.ResumePlayback();
		}
	}
	else if (playbackManager.GetCurrentPlaybackMusic())
	{
		if (ImGui::Button(ICON_FA_PAUSE "##pause"))
		{
			playbackManager.PausePlayback();
		}
	}
	else
	{
		if (ImGui::Button(ICON_FA_PLAY "##play"))
		{
			const auto &tracks = playlistController.GetTracks();
			if (!tracks.empty())
			{
				int idx = cursorRow;
				if (idx < 0 || idx >= (int)tracks.size())
					idx = playlistController.GetCurrentTrackIndex();
				if (idx < 0 || idx >= (int)tracks.size())
					idx = 0;
				playlistController.StartPlaybackAtIndex(idx);
				int newIdx = playlistController.GetCurrentTrackIndex();
				if (newIdx >= 0 && newIdx < (int)tracks.size())
				{
					playbackManager.StartTrackPlayback(tracks[newIdx], std::max<int64_t>(0, tracks[newIdx].cueInSample));
					SelectSingle(newIdx);
				}
			}
		}
	}
	ImGui::SameLine();
	if (ImGui::Button(ICON_FA_STOP "##stop"))
	{
		playbackManager.StopCurrentPlayback();
		playlistController.StopPlayback();
	}
	ImGui::SameLine();
	if (ImGui::Button(ICON_FA_STEP_FORWARD "##next"))
	{
		const auto &tracks = playlistController.GetTracks();
		if (!tracks.empty())
		{
			if (playlistController.IsPlaying())
			{
				playbackManager.RequestImmediateNextTrack(playlistController);
			}
			else
			{
				int ref = cursorRow;
				if (ref < 0) ref = playlistController.GetCurrentTrackIndex();
				int target = std::min((int)tracks.size() - 1, ref + 1);
				if (target < 0) target = 0;
				playlistController.StartPlaybackAtIndex(target);
				int newIdx = playlistController.GetCurrentTrackIndex();
				if (newIdx >= 0 && newIdx < (int)tracks.size())
					playbackManager.StartTrackPlayback(tracks[newIdx], std::max<int64_t>(0, tracks[newIdx].cueInSample));
			}
			int finalIdx = playlistController.GetCurrentTrackIndex();
			if (finalIdx >= 0 && finalIdx < (int)tracks.size())
				SelectSingle(finalIdx);
		}
	}

	bool loopEnabled = playlistController.GetLoopEnabled();
	if (ImGui::Checkbox(L("music.playlist.loop", "Loop"), &loopEnabled))
	{
		playlistController.SetLoopEnabled(loopEnabled);
		AutosaveCurrentDocument();
	}
	ImGui::SameLine();
	bool randomEnabled = playlistController.GetRandomEnabled();
	if (ImGui::Checkbox(L("music.playlist.random", "Random"), &randomEnabled))
	{
		playlistController.SetRandomEnabled(randomEnabled);
		AutosaveCurrentDocument();
	}
	ImGui::SameLine();
	bool playOnAddEnabled = playlistController.GetPlayOnAddEnabled();
	if (ImGui::Checkbox(L("music.playlist.play_on_add", "Play On Add"), &playOnAddEnabled))
	{
		playlistController.SetPlayOnAddEnabled(playOnAddEnabled);
		AutosaveCurrentDocument();
	}

	int minDistance = playlistController.GetMinDistanceBetweenSameTrack();
	if (ImGui::SliderInt(L("music.playlist.min_distance_k", "Min Distance K"), &minDistance, 1, 16))
	{
		playlistController.SetMinDistanceBetweenSameTrack(minDistance);
		AutosaveCurrentDocument();
	}

	RenderCrossfadeControls();

	if (ImGui::Button(L("music.playlist.show_randomized_order", "Show Randomized Order")))
	{
		ImGui::OpenPopup("Randomized Order");
	}
	if (ImGui::BeginPopup("Randomized Order"))
	{
		ImGui::TextUnformatted(L("music.playlist.randomized_order", "Randomized Order"));
		ImGui::Separator();
		const auto &order = playlistController.GetRandomRoundOrder();
		const auto &tracks = playlistController.GetTracks();
		for (size_t i = 0; i < order.size(); i++)
		{
			int idx = order[i];
			if (idx >= 0 && idx < (int)tracks.size())
			{
				ImGui::Text("%d. %s", (int)i + 1, GetDisplayTrackPath(tracks[idx].path).c_str());
			}
		}
		ImGui::EndPopup();
	}

	RenderKeyboardNavigation();
	RenderPlaylistTable();
	RenderCueEditor();

	PostRenderImGui();
}

void CGuiViewMusicPlaylist::TickPlayback(float deltaTime)
{
	EnsureInitialPlaylistLoaded();
	int prevTrackIdx = playlistController.GetCurrentTrackIndex();
	playbackManager.SyncPlayback(playlistController, deltaTime);
	int newTrackIdx = playlistController.GetCurrentTrackIndex();
	if (newTrackIdx != prevTrackIdx && prevTrackIdx >= 0 && cursorRow == prevTrackIdx && newTrackIdx >= 0)
		SelectSingle(newTrackIdx);
}

void CGuiViewMusicPlaylist::SetMusicVolume(float volume)
{
	playbackManager.SetMasterVolume(volume);
}

float CGuiViewMusicPlaylist::GetMusicVolume() const
{
	return playbackManager.GetMasterVolume();
}

// ─── File dialog callbacks ──────────────────────────────────────────────────────

void CGuiViewMusicPlaylist::SystemDialogFileOpenSelected(CSlrString *path)
{
	if (path == NULL)
		return;

	char *cPath = path->GetStdASCII();
	if (cPath == nullptr)
		return;
	std::string selectedPath = cPath;
	delete[] cPath;

	if (playlistDialogMode == PlaylistDialogMode::LoadPlaylist)
	{
		playlistDialogMode = PlaylistDialogMode::None;
		LoadPlaylistFromFile(selectedPath);
		return;
	}

	std::vector<CSlrString *> one;
	one.push_back(path);
	SystemDialogFilesOpenSelected(&one);
}

void CGuiViewMusicPlaylist::SystemDialogFilesOpenSelected(std::vector<CSlrString *> *paths)
{
	if (playlistDialogMode != PlaylistDialogMode::AddTracks)
	{
		playlistDialogMode = PlaylistDialogMode::None;
		return;
	}

	if (paths == NULL || paths->empty())
	{
		playlistDialogMode = PlaylistDialogMode::None;
		return;
	}

	for (CSlrString *path : *paths)
	{
		if (!path)
			continue;
		char *cPath = path->GetStdASCII();
		if (!cPath)
			continue;

		playlistController.AddTrack(cPath);
		playlistController.SetLastOpenFolder(GetFolderFromPath(cPath));
		delete[] cPath;
	}

	playlistDialogMode = PlaylistDialogMode::None;
	AutosaveCurrentDocument();
}

void CGuiViewMusicPlaylist::SystemDialogFileOpenCancelled()
{
	playlistDialogMode = PlaylistDialogMode::None;
}

void CGuiViewMusicPlaylist::SystemDialogFileSaveSelected(CSlrString *path)
{
	if (playlistDialogMode != PlaylistDialogMode::SavePlaylistAs || path == nullptr)
	{
		playlistDialogMode = PlaylistDialogMode::None;
		return;
	}

	char *cPath = path->GetStdASCII();
	if (cPath == nullptr)
	{
		playlistDialogMode = PlaylistDialogMode::None;
		return;
	}

	std::string savePath = cPath;
	delete[] cPath;
	playlistDialogMode = PlaylistDialogMode::None;
	SavePlaylistAs(savePath, pendingSaveAsPathMode);
}

void CGuiViewMusicPlaylist::SystemDialogFileSaveCancelled()
{
	playlistDialogMode = PlaylistDialogMode::None;
}

// ─── Playlist persistence ───────────────────────────────────────────────────────

void CGuiViewMusicPlaylist::EnsureInitialPlaylistLoaded()
{
	if (initialPlaylistResolved)
		return;
	LoadSettingsScratchPlaylist();
	playbackManager.SetPlaybackStateFilePath(GetPlaybackStateFilePath());
}

std::string CGuiViewMusicPlaylist::GetSettingsScratchPlaylistPath() const
{
	std::string path = gPathToSettings ? gPathToSettings : "";
	if (!path.empty() && path.back() != '/' && path.back() != '\\')
		path += "/";
	path += "music-playlist.json";
	return path;
}

std::string CGuiViewMusicPlaylist::GetPlaybackStateFilePath() const
{
	std::string path = gPathToSettings ? gPathToSettings : "";
	if (!path.empty() && path.back() != '/' && path.back() != '\\')
		path += "/";
	path += "music-playback-state.json";
	return path;
}

void CGuiViewMusicPlaylist::SavePlaybackStateNow()
{
	playbackManager.SetPlaybackStateFilePath(GetPlaybackStateFilePath());
	playbackManager.SavePlaybackState(playlistController);
}

void CGuiViewMusicPlaylist::ResumePlaybackIfNeeded()
{
	if (resumePlaybackTriggered)
		return;
	resumePlaybackTriggered = true;

	EnsureInitialPlaylistLoaded();

	playbackManager.SetPlaybackStateFilePath(GetPlaybackStateFilePath());
	SPlaybackResumeState state = playbackManager.LoadPlaybackState();

	if (!state.valid || !state.wasPlaying)
		return;

	const auto &tracks = playlistController.GetTracks();
	if (tracks.empty())
		return;

	// Try to find the saved track in the current playlist
	const CSlrMusicPlaylistTrack *matchedTrack = nullptr;
	int64_t resumeSample = state.samplePosition;

	// Fast path: match by trackId + verify path
	if (state.trackId > 0)
	{
		const CSlrMusicPlaylistTrack *candidate = playlistController.FindTrackById(state.trackId);
		if (candidate && candidate->path == state.trackPath)
			matchedTrack = candidate;
	}

	// Slow path: scan by trackPath
	if (!matchedTrack && !state.trackPath.empty())
	{
		for (const auto &track : tracks)
		{
			if (track.path == state.trackPath)
			{
				matchedTrack = &track;
				break;
			}
		}
	}

	// Fallback: first track in playlist
	if (!matchedTrack)
	{
		matchedTrack = &tracks[0];
		resumeSample = std::max((int64_t)0, matchedTrack->cueInSample);
	}

	// Set playlist controller state to the matched track
	playlistController.StartPlaybackAtTrackId(matchedTrack->id);

	// Restore shuffle state (random round, play history, play counts) from saved session.
	// Must be called AFTER StartPlaybackAtTrackId which rebuilds the shuffle round.
	if (!state.shuffleState.is_null() && state.shuffleState.is_object())
		playlistController.RestoreShuffleState(state.shuffleState);

	// Start playback with fade-in from silence
	playbackManager.StartTrackPlaybackWithFadeIn(*matchedTrack, resumeSample);
}

void CGuiViewMusicPlaylist::ApplyPlaybackSettingsFromController()
{
	playbackManager.SetCrossfadeEnabled(playlistController.GetCrossfadeEnabled());
	playbackManager.SetCrossfadeDurationMs(playlistController.GetCrossfadeDurationMs());
	playbackManager.SetOverlapEnabled(playlistController.GetOverlapEnabled());
	playbackManager.SetCrossfadeSplineEnabled(playlistController.GetCrossfadeSplineEnabled());

	float cx1, cy1, cx2, cy2;
	playlistController.GetCrossfadeSplineCp1(cx1, cy1);
	playlistController.GetCrossfadeSplineCp2(cx2, cy2);
	playbackManager.SetCrossfadeSplineParams(cx1, cy1, cx2, cy2);
}

void CGuiViewMusicPlaylist::UpdateDocumentStatusMessage(const char *message)
{
	documentStatusMessage = message != nullptr ? message : std::string();
}

void CGuiViewMusicPlaylist::SetProjectRootPath(const std::string &path)
{
	projectRootPath = path;
	if (!projectRootPath.empty())
	{
		projectRootPath = PathToGenericString(NormalizeAbsolutePath(projectRootPath));
		if (!projectRootPath.empty() && projectRootPath.back() != '/')
			projectRootPath += "/";
	}
	UpdateDocumentStatusMessage(nullptr);
}

bool CGuiViewMusicPlaylist::LoadSettingsScratchPlaylist()
{
	CSlrMusicPlaylistLoadOptions options;
	options.projectRootPath = projectRootPath;
	playlistController.LoadFromFile(GetSettingsScratchPlaylistPath().c_str(), options, nullptr);
	currentDocumentMode = CMusicPlaylistDocumentMode::SettingsScratch;
	currentPlaylistFilePath.clear();
	currentPathMode = CSlrMusicPlaylistPathMode::Absolute;
	initialPlaylistResolved = true;
	documentDirty = false;
	playbackManager.ClearAllPlayback();
	ApplyPlaybackSettingsFromController();
	UpdateDocumentStatusMessage(nullptr);
	return true;
}

bool CGuiViewMusicPlaylist::SaveSettingsScratchPlaylist()
{
	CSlrMusicPlaylistSaveOptions options;
	options.pathMode = CSlrMusicPlaylistPathMode::Absolute;
	bool saved = playlistController.SaveToFile(GetSettingsScratchPlaylistPath().c_str(), options);
	if (saved)
	{
		if (currentDocumentMode == CMusicPlaylistDocumentMode::SettingsScratch)
			documentDirty = false;
		UpdateDocumentStatusMessage(L("music.playlist.saved_settings", "Saved to settings scratch"));
	}
	return saved;
}

bool CGuiViewMusicPlaylist::SaveCopyToSettingsScratch()
{
	bool saved = SaveSettingsScratchPlaylist();
	if (saved && currentDocumentMode == CMusicPlaylistDocumentMode::ExternalFile)
		UpdateDocumentStatusMessage(L("music.playlist.saved_to_settings_copy", "Saved a copy to settings scratch"));
	return saved;
}

bool CGuiViewMusicPlaylist::LoadPlaylistFromFile(const std::string &path)
{
	CSlrMusicPlaylistLoadOptions options;
	options.projectRootPath = projectRootPath;
	CSlrMusicPlaylistDocumentInfo info;
	if (!playlistController.LoadFromFile(path.c_str(), options, &info))
	{
		UpdateDocumentStatusMessage(L("music.playlist.load_failed", "Failed to load playlist file"));
		return false;
	}

	currentDocumentMode = CMusicPlaylistDocumentMode::ExternalFile;
	currentPlaylistFilePath = path;
	currentPathMode = info.pathMode;
	initialPlaylistResolved = true;
	documentDirty = false;
	selectedRows.clear();
	cursorRow = -1;
	anchorRow = -1;
	playbackManager.ClearAllPlayback();
	ApplyPlaybackSettingsFromController();
	UpdateDocumentStatusMessage(L("music.playlist.loaded_file", "Loaded playlist file"));
	return true;
}

bool CGuiViewMusicPlaylist::SaveCurrentPlaylist()
{
	if (currentDocumentMode == CMusicPlaylistDocumentMode::ExternalFile && !currentPlaylistFilePath.empty())
	{
		CSlrMusicPlaylistSaveOptions options;
		options.pathMode = currentPathMode;
		options.projectRootPath = projectRootPath;
		bool saved = playlistController.SaveToFile(currentPlaylistFilePath.c_str(), options);
		if (saved)
		{
			documentDirty = false;
			UpdateDocumentStatusMessage(L("music.playlist.saved_file", "Saved playlist file"));
		}
		else
		{
			UpdateDocumentStatusMessage(L("music.playlist.save_failed", "Failed to save playlist file"));
		}
		return saved;
	}

	return SaveSettingsScratchPlaylist();
}

bool CGuiViewMusicPlaylist::SavePlaylistAs(const std::string &path, CSlrMusicPlaylistPathMode pathMode)
{
	CSlrMusicPlaylistSaveOptions options;
	options.pathMode = pathMode;
	options.projectRootPath = projectRootPath;
	if (!playlistController.SaveToFile(path.c_str(), options))
	{
		UpdateDocumentStatusMessage(L("music.playlist.save_failed", "Failed to save playlist file"));
		return false;
	}

	currentDocumentMode = CMusicPlaylistDocumentMode::ExternalFile;
	currentPlaylistFilePath = path;
	currentPathMode = pathMode;
	initialPlaylistResolved = true;
	documentDirty = false;
	UpdateDocumentStatusMessage(L("music.playlist.saved_file", "Saved playlist file"));
	return true;
}

void CGuiViewMusicPlaylist::NewScratchPlaylist()
{
	playlistController.ClearTracks();
	selectedRows.clear();
	cursorRow = -1;
	anchorRow = -1;
	playbackManager.ClearAllPlayback();
	currentDocumentMode = CMusicPlaylistDocumentMode::SettingsScratch;
	currentPlaylistFilePath.clear();
	currentPathMode = CSlrMusicPlaylistPathMode::Absolute;
	initialPlaylistResolved = true;
	documentDirty = false;
	SaveSettingsScratchPlaylist();
}

bool CGuiViewMusicPlaylist::HasExternalPlaylistFile() const
{
	return currentDocumentMode == CMusicPlaylistDocumentMode::ExternalFile && !currentPlaylistFilePath.empty();
}

const std::string &CGuiViewMusicPlaylist::GetCurrentPlaylistFilePath() const
{
	return currentPlaylistFilePath;
}

CSlrMusicPlaylistPathMode CGuiViewMusicPlaylist::GetCurrentPathMode() const
{
	return currentPathMode;
}

CGuiViewMusicPlaylist::CMusicPlaylistDocumentMode CGuiViewMusicPlaylist::GetCurrentDocumentMode() const
{
	return currentDocumentMode;
}

void CGuiViewMusicPlaylist::AutosaveCurrentDocument()
{
	if (!SaveCurrentPlaylist())
		documentDirty = true;
}

std::optional<std::string> CGuiViewMusicPlaylist::GetProjectRelativePath(const std::string &absolutePath) const
{
	if (projectRootPath.empty() || absolutePath.empty())
		return std::nullopt;

	std::filesystem::path rootPath = NormalizeAbsolutePath(projectRootPath);
	std::filesystem::path targetPath = NormalizeAbsolutePath(absolutePath);
	if (!IsPathWithinRoot(targetPath, rootPath))
		return std::nullopt;

	std::filesystem::path relativePath = targetPath.lexically_relative(rootPath);
	if (relativePath.empty())
		return std::nullopt;

	return PathToGenericString(relativePath);
}

bool CGuiViewMusicPlaylist::TryGetCurrentPlaylistPathRelativeToProjectRoot(std::string &outPath) const
{
	auto relativePath = GetProjectRelativePath(currentPlaylistFilePath);
	if (!relativePath.has_value())
		return false;
	outPath = *relativePath;
	return true;
}

bool CGuiViewMusicPlaylist::CanUseProjectRelativePaths(std::string *reasonOut) const
{
	if (projectRootPath.empty())
	{
		if (reasonOut != nullptr)
			*reasonOut = L("music.playlist.no_project_root", "Project root is not set");
		return false;
	}

	for (const auto &track : playlistController.GetTracks())
	{
		if (!GetProjectRelativePath(track.path).has_value())
		{
			if (reasonOut != nullptr)
				*reasonOut = track.path;
			return false;
		}
	}

	return true;
}

const char *CGuiViewMusicPlaylist::GetDocumentModeLabel() const
{
	return currentDocumentMode == CMusicPlaylistDocumentMode::ExternalFile
		? L("music.playlist.external_file", "external file")
		: L("music.playlist.settings_scratch", "settings scratch");
}

const char *CGuiViewMusicPlaylist::GetPathModeLabel() const
{
	return currentPathMode == CSlrMusicPlaylistPathMode::ProjectRelative
		? L("music.playlist.project_relative", "project-relative")
		: L("music.playlist.absolute", "absolute");
}

std::string CGuiViewMusicPlaylist::GetDisplayTrackPath(const std::string &trackPath) const
{
	if (currentPathMode == CSlrMusicPlaylistPathMode::ProjectRelative)
	{
		auto relativePath = GetProjectRelativePath(trackPath);
		if (relativePath.has_value())
			return *relativePath;
		return trackPath;
	}

	return GetTrackTitleFromPath(trackPath);
}

std::string CGuiViewMusicPlaylist::GetFolderFromPath(const std::string &path) const
{
	size_t slashPos = path.find_last_of("/\\");
	if (slashPos == std::string::npos)
		return std::string();
	return path.substr(0, slashPos);
}

// ─── Selection (unordered_set) ──────────────────────────────────────────────────

bool CGuiViewMusicPlaylist::IsSelected(int idx) const
{
	return selectedRows.count(idx) > 0;
}

void CGuiViewMusicPlaylist::SelectSingle(int idx)
{
	selectedRows.clear();
	selectedRows.insert(idx);
	anchorRow = idx;
	cursorRow = idx;
}

void CGuiViewMusicPlaylist::ToggleSelection(int idx)
{
	auto it = selectedRows.find(idx);
	if (it == selectedRows.end())
		selectedRows.insert(idx);
	else
		selectedRows.erase(it);
	cursorRow = idx;
	if (anchorRow < 0)
		anchorRow = idx;
}

void CGuiViewMusicPlaylist::SelectRange(int from, int to)
{
	selectedRows.clear();
	if (from > to)
		std::swap(from, to);
	for (int i = from; i <= to; i++)
		selectedRows.insert(i);
	cursorRow = to;
}

void CGuiViewMusicPlaylist::RemoveSelectedTracks()
{
	if (selectedRows.empty())
		return;

	std::vector<int> sorted(selectedRows.begin(), selectedRows.end());
	std::sort(sorted.begin(), sorted.end());

	for (auto it = sorted.rbegin(); it != sorted.rend(); ++it)
		playlistController.RemoveTrackByIndex(*it);

	selectedRows.clear();
	cursorRow = -1;
	anchorRow = -1;
	playbackManager.CleanupPlaybackCache(playlistController);
	AutosaveCurrentDocument();
}

// ─── Playlist table ─────────────────────────────────────────────────────────────

void CGuiViewMusicPlaylist::RenderPlaylistTable()
{
	const auto &tracks = playlistController.GetTracks();

	if (ImGui::BeginTable("MusicPlaylistTable", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, 280)))
	{
		ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 40.0f);
		ImGui::TableSetupColumn("##enabled_header", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
		ImGui::TableSetupColumn(L("music.playlist.track", "Track"), ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn(L("music.playlist.cue_in", "Cue In"), ImGuiTableColumnFlags_WidthFixed, 80.0f);
		ImGui::TableSetupColumn(L("music.playlist.cue_out", "Cue Out"), ImGuiTableColumnFlags_WidthFixed, 80.0f);
		ImGui::TableHeadersRow();

		for (int i = 0; i < (int)tracks.size(); i++)
		{
			const auto &track = tracks[i];
			bool selected = IsSelected(i);
			bool isCurrent = playlistController.IsPlaying() && playlistController.GetCurrentTrackIndex() == i;
			bool dimmed = !track.enabled && !isCurrent;

			ImGui::TableNextRow();

			// Col 0: track number
			ImGui::TableSetColumnIndex(0);
			if (isCurrent) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.2f, 0.95f, 0.3f, 1.0f));
			else if (dimmed) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
			ImGui::Text("%d", i + 1);
			if (isCurrent || dimmed) ImGui::PopStyleColor();

			// Col 1: enable checkbox
			ImGui::TableSetColumnIndex(1);
			{
				bool enabled = track.enabled;
				ImGui::PushID((int)track.id);
				if (ImGui::Checkbox("##enabled", &enabled))
				{
					CSlrMusicPlaylistEnableResult res = playlistController.SetTrackEnabledById(track.id, enabled);
					switch (res)
					{
						case CSlrMusicPlaylistEnableResult::ToggledSkipCurrent:
						{
							int newIdx = playlistController.GetCurrentTrackIndex();
							if (newIdx >= 0 && newIdx < (int)tracks.size())
							{
								playbackManager.RequestImmediateNextTrack(playlistController);
							}
							break;
						}
						case CSlrMusicPlaylistEnableResult::ToggledStopAutoFlagSet:
							playbackManager.ClearAllPlayback();
							break;
						case CSlrMusicPlaylistEnableResult::ToggledResumeFromStop:
						{
							int newIdx = playlistController.GetCurrentTrackIndex();
							if (newIdx >= 0 && newIdx < (int)tracks.size())
							{
								playbackManager.StartTrackPlayback(tracks[newIdx],
									std::max<int64_t>(0, tracks[newIdx].cueInSample));
							}
							break;
						}
						case CSlrMusicPlaylistEnableResult::Toggled:
						case CSlrMusicPlaylistEnableResult::NoChange:
							break;
					}
					AutosaveCurrentDocument();
				}
				ImGui::PopID();
			}

			// Col 2: track name / selectable
			ImGui::TableSetColumnIndex(2);
			if (dimmed) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
			ImGuiSelectableFlags sflags = ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick;
			std::string displayPath = GetDisplayTrackPath(track.path);
			if (ImGui::Selectable((displayPath + "##track_" + std::to_string(track.id)).c_str(), selected, sflags))
			{
				ImGuiIO &io = ImGui::GetIO();
				if (io.KeyShift && anchorRow >= 0)
					SelectRange(anchorRow, i);
				else if (io.KeyCtrl)
					ToggleSelection(i);
				else
					SelectSingle(i);

				if (!io.KeyShift && !io.KeyCtrl)
				{
					playlistController.StartPlaybackAtIndex(i);
					int newIdx = playlistController.GetCurrentTrackIndex();
					if (newIdx >= 0 && newIdx < (int)tracks.size())
						playbackManager.StartTrackPlayback(tracks[newIdx], std::max<int64_t>(0, tracks[newIdx].cueInSample));
				}
			}
			if (dimmed) ImGui::PopStyleColor();

			if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
			{
				ImGui::SetDragDropPayload("MUSIC_TRACK_ROW", &i, sizeof(int));
				ImGui::Text("Move: %s", GetDisplayTrackPath(track.path).c_str());
				ImGui::EndDragDropSource();
			}

			if (ImGui::BeginDragDropTarget())
			{
				if (const ImGuiPayload *payload = ImGui::AcceptDragDropPayload("MUSIC_TRACK_ROW"))
				{
					int fromIndex = *(const int *)payload->Data;
					if (playlistController.MoveTrack(fromIndex, i))
						AutosaveCurrentDocument();
				}
				ImGui::EndDragDropTarget();
			}

			if (playbackManager.IsTrackLoadFailed(track.id))
			{
				ImGui::SameLine();
				if (ImGui::SmallButton(("Retry##retry_" + std::to_string(track.id)).c_str()))
				{
					std::string retryPath = playbackManager.RetryTrackLoad(track.id, playlistController);
					if (!retryPath.empty())
					{
						waveformLruOrder.remove(retryPath);
						auto mapIt = waveformCacheByPath.find(retryPath);
						if (mapIt != waveformCacheByPath.end())
						{
							if (mapIt->second)
								CleanupWaveformThread(mapIt->second.get());
							waveformCacheByPath.erase(mapIt);
						}
					}
				}
			}

			// Col 3: Cue In
			ImGui::TableSetColumnIndex(3);
			if (isCurrent) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.2f, 0.95f, 0.3f, 1.0f));
			else if (dimmed) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
			ImGui::Text("%" PRId64, track.cueInSample);
			if (isCurrent || dimmed) ImGui::PopStyleColor();

			// Col 4: Cue Out
			ImGui::TableSetColumnIndex(4);
			if (isCurrent) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.2f, 0.95f, 0.3f, 1.0f));
			else if (dimmed) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
			ImGui::Text("%" PRId64, track.cueOutSample);
			if (isCurrent || dimmed) ImGui::PopStyleColor();
		}

		ImGui::EndTable();
	}
}

// ─── Keyboard navigation ────────────────────────────────────────────────────────

void CGuiViewMusicPlaylist::RenderKeyboardNavigation()
{
	if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
		return;
	if (ImGui::GetIO().WantTextInput)
		return;

	int numTracks = playlistController.GetTrackCount();
	if (numTracks <= 0)
		return;

	if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
	{
		if (cursorRow < 0) cursorRow = 0;
		else cursorRow = std::max(0, cursorRow - 1);
		SelectSingle(cursorRow);
	}
	if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
	{
		if (cursorRow < 0) cursorRow = 0;
		else cursorRow = std::min(numTracks - 1, cursorRow + 1);
		SelectSingle(cursorRow);
	}
	if (ImGui::IsKeyPressed(ImGuiKey_Enter) && cursorRow >= 0)
	{
		playlistController.StartPlaybackAtIndex(cursorRow);
	}
	if (ImGui::IsKeyPressed(ImGuiKey_Delete))
	{
		RemoveSelectedTracks();
	}
}

// ─── Crossfade controls ─────────────────────────────────────────────────────────

void CGuiViewMusicPlaylist::RenderCrossfadeControls()
{
	bool crossfadeEnabled = playlistController.GetCrossfadeEnabled();
	if (ImGui::Checkbox(L("music.playlist.crossfade", "Crossfade"), &crossfadeEnabled))
	{
		playlistController.SetCrossfadeEnabled(crossfadeEnabled);
		playbackManager.SetCrossfadeEnabled(crossfadeEnabled);
		AutosaveCurrentDocument();
	}

	if (crossfadeEnabled)
	{
		ImGui::SameLine();
		int crossfadeDurationMs = playlistController.GetCrossfadeDurationMs();
		ImGui::SetNextItemWidth(200.0f);
		if (ImGui::SliderInt(L("music.playlist.crossfade_ms", "Crossfade (ms)"), &crossfadeDurationMs, 20, 10000))
		{
			playlistController.SetCrossfadeDurationMs(crossfadeDurationMs);
			playbackManager.SetCrossfadeDurationMs(crossfadeDurationMs);
			AutosaveCurrentDocument();
		}

		// Spline easing option
		ImGui::SameLine();
		bool splineEnabled = playlistController.GetCrossfadeSplineEnabled();
		if (ImGui::Checkbox(L("music.playlist.spline", "Spline"), &splineEnabled))
		{
			playlistController.SetCrossfadeSplineEnabled(splineEnabled);
			playbackManager.SetCrossfadeSplineEnabled(splineEnabled);
			AutosaveCurrentDocument();
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("%s", L("music.playlist.spline_tooltip",
				"Shape the equal-power crossfade with a Bezier curve"));

		if (splineEnabled)
		{
			float cp1x, cp1y, cp2x, cp2y;
			playlistController.GetCrossfadeSplineCp1(cp1x, cp1y);
			playlistController.GetCrossfadeSplineCp2(cp2x, cp2y);

			bool changed = false;
			ImGui::SetNextItemWidth(140.0f);
			changed |= ImGui::DragFloat("CP1 X", &cp1x, 0.01f, 0.0f, 1.0f, "%.2f");
			ImGui::SameLine();
			ImGui::SetNextItemWidth(140.0f);
			changed |= ImGui::DragFloat("CP1 Y", &cp1y, 0.01f, -0.5f, 1.5f, "%.2f");
			ImGui::SameLine();
			ImGui::SetNextItemWidth(140.0f);
			changed |= ImGui::DragFloat("CP2 X", &cp2x, 0.01f, 0.0f, 1.0f, "%.2f");
			ImGui::SameLine();
			ImGui::SetNextItemWidth(140.0f);
			changed |= ImGui::DragFloat("CP2 Y", &cp2y, 0.01f, -0.5f, 1.5f, "%.2f");

			if (changed)
			{
				playlistController.SetCrossfadeSplineCp1(cp1x, cp1y);
				playlistController.SetCrossfadeSplineCp2(cp2x, cp2y);
				playbackManager.SetCrossfadeSplineParams(cp1x, cp1y, cp2x, cp2y);
				AutosaveCurrentDocument();
			}

			// Draw curve preview
			ImVec2 previewPos = ImGui::GetCursorScreenPos();
			float previewW = 200.0f;
			float previewH = 80.0f;
			ImGui::InvisibleButton("SplinePreview", ImVec2(previewW, previewH));
			ImDrawList *dl = ImGui::GetWindowDrawList();
			ImVec2 pMin = previewPos;
			ImVec2 pMax(previewPos.x + previewW, previewPos.y + previewH);
			dl->AddRectFilled(pMin, pMax, IM_COL32(20, 24, 32, 255));
			dl->AddRect(pMin, pMax, IM_COL32(60, 72, 92, 255));

			// Draw equal-power curves: incoming (green) and outgoing (red)
			const int steps = 64;
			for (int i = 0; i < steps; i++)
			{
				float t0 = (float)i / (float)steps;
				float t1 = (float)(i + 1) / (float)steps;
				float e0 = CPlaylistPlaybackManager::CubicBezierEase(t0, cp1x, cp1y, cp2x, cp2y);
				float e1 = CPlaylistPlaybackManager::CubicBezierEase(t1, cp1x, cp1y, cp2x, cp2y);

				float inV0 = sinf(e0 * 3.14159265f * 0.5f);
				float inV1 = sinf(e1 * 3.14159265f * 0.5f);
				float outV0 = cosf(e0 * 3.14159265f * 0.5f);
				float outV1 = cosf(e1 * 3.14159265f * 0.5f);

				float x0 = pMin.x + t0 * previewW;
				float x1 = pMin.x + t1 * previewW;

				// Incoming (green)
				dl->AddLine(ImVec2(x0, pMax.y - inV0 * previewH),
							ImVec2(x1, pMax.y - inV1 * previewH), IM_COL32(80, 255, 130, 220));
				// Outgoing (red)
				dl->AddLine(ImVec2(x0, pMax.y - outV0 * previewH),
							ImVec2(x1, pMax.y - outV1 * previewH), IM_COL32(255, 100, 80, 220));
			}
		}
	}

	ImGui::SameLine();
	bool overlapEnabled = playlistController.GetOverlapEnabled();
	if (ImGui::Checkbox(L("music.playlist.overlap", "Overlap"), &overlapEnabled))
	{
		playlistController.SetOverlapEnabled(overlapEnabled);
		playbackManager.SetOverlapEnabled(overlapEnabled);
		AutosaveCurrentDocument();
	}
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", L("music.playlist.overlap_tooltip", "Both tracks play at full volume during transition (no fade)"));
}

// ─── Waveform cache (LRU + async) ──────────────────────────────────────────────

CGuiViewMusicPlaylist::SWaveformCacheEntry *CGuiViewMusicPlaylist::GetWaveformEntry(const std::string &trackPath)
{
	auto &ptr = waveformCacheByPath[trackPath];
	bool isNew = (ptr == nullptr);

	if (isNew)
	{
		ptr = std::make_unique<SWaveformCacheEntry>();
		waveformLruOrder.push_back(trackPath);
		EvictLruWaveformEntries();
	}
	else
	{
		waveformLruOrder.remove(trackPath);
		waveformLruOrder.push_back(trackPath);
	}

	SWaveformCacheEntry *entry = ptr.get();

	if (entry->loadThread && !entry->isLoading.load())
	{
		delete entry->loadThread;
		entry->loadThread = nullptr;
	}

	if (!entry->attemptedLoad)
	{
		entry->attemptedLoad = true;
		entry->isLoading.store(true);
		CMusicWaveformSourceInfo sourceInfo;
		std::string sidecarPath;
		if (GetWaveformSourceInfo(trackPath, sourceInfo))
			sidecarPath = GetWaveformSidecarPath(trackPath);

		CWaveformLoadThread *thread = new CWaveformLoadThread(
			&entry->cache, trackPath, sidecarPath, sourceInfo, &entry->isLoading, &entry->loadSucceeded);
		entry->loadThread = thread;
		SYS_StartThread(thread);
	}

	return entry;
}

std::string CGuiViewMusicPlaylist::GetWaveformSidecarPath(const std::string &trackPath) const
{
	if (trackPath.empty())
		return std::string();

	std::filesystem::path path(trackPath);
	if (path.has_extension())
		path.replace_extension();
	std::string sidecarPath = PathToGenericString(path);
	sidecarPath += ".waveform.bin";
	return sidecarPath;
}

bool CGuiViewMusicPlaylist::GetWaveformSourceInfo(const std::string &trackPath, CMusicWaveformSourceInfo &outInfo) const
{
	if (trackPath.empty())
		return false;

	std::error_code ec;
	const auto fileSize = std::filesystem::file_size(trackPath, ec);
	if (ec)
		return false;
	const auto writeTime = std::filesystem::last_write_time(trackPath, ec);
	if (ec)
		return false;

	outInfo.sourceFileSize = static_cast<uint64_t>(fileSize);
	outInfo.sourceFileMtime = static_cast<uint64_t>(writeTime.time_since_epoch().count());
	outInfo.envelopePointCount = 8192;
	return true;
}

void CGuiViewMusicPlaylist::EvictLruWaveformEntries()
{
	while ((int)waveformCacheByPath.size() > kMaxWaveformCacheEntries)
	{
		bool found = false;
		for (auto it = waveformLruOrder.begin(); it != waveformLruOrder.end(); ++it)
		{
			auto mapIt = waveformCacheByPath.find(*it);
			if (mapIt != waveformCacheByPath.end() && mapIt->second && !mapIt->second->isLoading.load())
			{
				CleanupWaveformThread(mapIt->second.get());
				waveformCacheByPath.erase(mapIt);
				waveformLruOrder.erase(it);
				found = true;
				break;
			}
		}
		if (!found)
			break;
	}
}

void CGuiViewMusicPlaylist::CleanupWaveformThread(SWaveformCacheEntry *entry)
{
	if (!entry || !entry->loadThread)
		return;

	while (entry->isLoading.load())
	{
		// Wait for thread to finish — typically fast (file I/O only)
	}
	delete entry->loadThread;
	entry->loadThread = nullptr;
}

void CGuiViewMusicPlaylist::ResetWaveformViewForTrack(int trackId, float centerNorm)
{
	if (waveformViewTrackId == trackId)
		return;

	waveformViewTrackId = trackId;
	waveformZoom = 1.0f;
	waveformCenterNorm = ClampWaveformCenterForZoom(centerNorm, waveformZoom);
}

// ─── Waveform panel rendering ───────────────────────────────────────────────────

void CGuiViewMusicPlaylist::RenderWaveformPanel(const CMusicWaveformCache &cache, int64_t maxSample, int64_t cueIn, int64_t cueOut)
{
	ImVec2 canvasPos = ImGui::GetCursorScreenPos();
	ImVec2 canvasSize(ImGui::GetContentRegionAvail().x, 120.0f);
	if (canvasSize.x < 32.0f)
		canvasSize.x = 32.0f;

	ImGui::InvisibleButton("MusicWaveformCanvas", canvasSize, ImGuiButtonFlags_MouseButtonLeft);
	bool hovered = ImGui::IsItemHovered();
	if (hovered)
	{
		ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelX);
		ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
	}
	ImDrawList *drawList = ImGui::GetWindowDrawList();

	ImVec2 pMin = canvasPos;
	ImVec2 pMax(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y);
	drawList->AddRectFilled(pMin, pMax, IM_COL32(14, 18, 28, 255));
	drawList->AddRect(pMin, pMax, IM_COL32(60, 72, 92, 255));

	ImGuiIO &io = ImGui::GetIO();
	float panWheelDelta = hovered ? GetWaveformPanWheelDelta(io.MouseWheelH, io.MouseWheel, io.KeyShift, waveformZoom) : 0.0f;
	if (panWheelDelta != 0.0f)
	{
		waveformCenterNorm = PanWaveformCenterForZoom(waveformCenterNorm, waveformZoom, panWheelDelta);
	}
	else
	{
		float mouseWheel = hovered ? io.MouseWheel : 0.0f;
		if (mouseWheel != 0.0f)
		{
			float oldZoom = std::max(waveformZoom, 1.0f);
			float oldSpan = GetWaveformSpanForZoom(oldZoom);
			float oldCenter = ClampWaveformCenterForZoom(waveformCenterNorm, oldZoom);
			float oldStart = oldCenter - oldSpan * 0.5f;

			float pointerNormInCanvas = (io.MousePos.x - canvasPos.x) / canvasSize.x;
			pointerNormInCanvas = std::clamp(pointerNormInCanvas, 0.0f, 1.0f);
			float anchorNorm = oldStart + oldSpan * pointerNormInCanvas;

			float zoomFactor = (mouseWheel > 0.0f) ? 1.2f : (1.0f / 1.2f);
			float newZoom = std::clamp(waveformZoom * zoomFactor, 1.0f, 128.0f);
			float newSpan = GetWaveformSpanForZoom(newZoom);
			float newStart = anchorNorm - pointerNormInCanvas * newSpan;

			waveformZoom = newZoom;
			waveformCenterNorm = ClampWaveformCenterForZoom(newStart + newSpan * 0.5f, waveformZoom);
		}
	}

	float zoom = std::max(waveformZoom, 1.0f);
	float span = GetWaveformSpanForZoom(zoom);
	waveformCenterNorm = ClampWaveformCenterForZoom(waveformCenterNorm, zoom);
	float start = waveformCenterNorm - span * 0.5f;

	std::vector<CMusicWaveformRenderPoint> points;
	size_t pixelWidth = (size_t)std::max(1, (int)canvasSize.x);
	if (cache.BuildRenderWindow(pixelWidth, waveformCenterNorm, waveformZoom, points))
	{
		for (size_t i = 0; i < points.size(); i++)
		{
			float x = canvasPos.x + (float)i;
			float yMin = canvasPos.y + (1.0f - (points[i].minValue * 0.5f + 0.5f)) * canvasSize.y;
			float yMax = canvasPos.y + (1.0f - (points[i].maxValue * 0.5f + 0.5f)) * canvasSize.y;
			drawList->AddLine(ImVec2(x, yMin), ImVec2(x, yMax), IM_COL32(98, 213, 255, 235));
		}
	}

	float centerY = canvasPos.y + canvasSize.y * 0.5f;
	drawList->AddLine(ImVec2(canvasPos.x, centerY), ImVec2(canvasPos.x + canvasSize.x, centerY), IM_COL32(120, 140, 170, 120));

	auto drawMarker = [&](float norm, ImU32 color) {
		if (norm < start || norm > start + span)
			return;
		float x = canvasPos.x + ((norm - start) / span) * canvasSize.x;
		drawList->AddLine(ImVec2(x, canvasPos.y), ImVec2(x, canvasPos.y + canvasSize.y), color, 2.0f);
	};

	if (maxSample > 0)
	{
		auto sampleToNorm = [maxSample](int64_t s) -> float {
			return (float)((double)s / (double)maxSample);
		};
		drawMarker(sampleToNorm(cueIn), IM_COL32(80, 255, 130, 230));
		drawMarker(sampleToNorm(cueOut), IM_COL32(255, 120, 80, 230));
		drawMarker(sampleToNorm(cueCursorSample), IM_COL32(255, 255, 120, 210));

		// Playback position cursor (white, thicker)
		int playingTrackId = playbackManager.GetCurrentPlaybackTrackId();
		const auto &tracks = playlistController.GetTracks();
		if (cursorRow >= 0 && cursorRow < (int)tracks.size() && playingTrackId == (int)tracks[cursorRow].id)
		{
			int64_t playbackSample = playbackManager.GetCurrentSampleNum();
			float playNorm = sampleToNorm(playbackSample);
			if (playNorm >= start && playNorm <= start + span)
			{
				float px = canvasPos.x + ((playNorm - start) / span) * canvasSize.x;
				drawList->AddLine(ImVec2(px, canvasPos.y), ImVec2(px, canvasPos.y + canvasSize.y), IM_COL32(255, 255, 255, 240), 2.0f);
			}
		}
	}

	if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && maxSample > 0)
	{
		float localX = (ImGui::GetIO().MousePos.x - canvasPos.x) / canvasSize.x;
		localX = std::clamp(localX, 0.0f, 1.0f);
		float clickedNorm = start + localX * span;
		clickedNorm = std::clamp(clickedNorm, 0.0f, 1.0f);
		int64_t clickedSample = (int64_t)std::round((double)clickedNorm * (double)maxSample);
		cueCursorSample = clickedSample;

		// If this track is currently playing, seek playback to clicked position
		int playingTrackId = playbackManager.GetCurrentPlaybackTrackId();
		const auto &tracks = playlistController.GetTracks();
		if (cursorRow >= 0 && cursorRow < (int)tracks.size() && playingTrackId == (int)tracks[cursorRow].id)
		{
			playbackManager.SeekCurrentTrack(clickedSample);
		}
	}

	ImGui::Text("Zoom: %.2fx", waveformZoom);
}

// ─── Cue editor ─────────────────────────────────────────────────────────────────

void CGuiViewMusicPlaylist::RenderCueEditor()
{
	const auto &tracks = playlistController.GetTracks();
	if (cursorRow < 0 || cursorRow >= (int)tracks.size())
		return;

	const auto &track = tracks[cursorRow];
	ImGui::Separator();
	ImGui::TextUnformatted(L("music.playlist.cue_editor", "Cue Editor"));
	ImGui::TextWrapped("%s: %s", L("music.playlist.track", "Track"), GetDisplayTrackPath(track.path).c_str());

	SWaveformCacheEntry *waveformEntry = GetWaveformEntry(track.path);
	bool waveformReady = waveformEntry && waveformEntry->loadSucceeded && !waveformEntry->isLoading.load();
	size_t waveformFrameCount = waveformReady ? waveformEntry->cache.GetSourceFrameCount() : 0;
	int64_t maxSample = waveformFrameCount > 0 ? (int64_t)waveformFrameCount - 1 : 200000LL;
	if (maxSample < 0)
		maxSample = 0;

	int64_t cueIn = std::max<int64_t>(0, track.cueInSample);
	if (cueIn > maxSample)
		cueIn = maxSample;

	int64_t cueOut = track.cueOutSample >= 0 ? track.cueOutSample : maxSample;
	if (cueOut < cueIn)
		cueOut = cueIn;
	if (cueOut > maxSample)
		cueOut = maxSample;

	if (cueCursorSample < 0 || cueCursorSample > maxSample)
		cueCursorSample = cueIn;

	float cursorNorm = maxSample > 0 ? (float)((double)cueCursorSample / (double)maxSample) : 0.5f;
	ResetWaveformViewForTrack((int)track.id, cursorNorm);

	{
		int64_t sliderMin = 0;
		int64_t sliderMax = maxSample;
		if (ImGui::SliderScalar(L("music.playlist.cursor_sample", "Cursor Sample"), ImGuiDataType_S64, &cueCursorSample, &sliderMin, &sliderMax))
		{
			if (maxSample > 0)
				waveformCenterNorm = ClampWaveformCenterForZoom((float)((double)cueCursorSample / (double)maxSample), waveformZoom);
		}
	}

	if (waveformEntry && waveformEntry->isLoading.load())
	{
		ImGui::TextUnformatted(L("music.playlist.loading_waveform", "Loading waveform..."));
	}
	else if (waveformReady)
	{
		RenderWaveformPanel(waveformEntry->cache, maxSample, cueIn, cueOut);
	}
	else
	{
		ImGui::TextUnformatted("Waveform unavailable for this track");
	}

	if (ImGui::InputScalar(L("music.playlist.cue_in", "Cue In"), ImGuiDataType_S64, &cueIn))
	{
		cueIn = std::clamp<int64_t>(cueIn, 0, maxSample);
		if (cueOut < cueIn)
			cueOut = cueIn;
		playlistController.SetTrackCueById(track.id, cueIn, cueOut);
		AutosaveCurrentDocument();
	}
	if (ImGui::InputScalar(L("music.playlist.cue_out", "Cue Out"), ImGuiDataType_S64, &cueOut))
	{
		cueOut = std::clamp<int64_t>(cueOut, cueIn, maxSample);
		playlistController.SetTrackCueById(track.id, cueIn, cueOut);
		AutosaveCurrentDocument();
	}

	if (ImGui::Button(L("music.playlist.set_cue_in", "Set CUE IN")))
	{
		cueCursorSample = std::clamp<int64_t>(cueCursorSample, 0, maxSample);
		playlistController.SetTrackCueById(track.id, cueCursorSample, cueOut);
		AutosaveCurrentDocument();
	}
	ImGui::SameLine();
	if (ImGui::Button(L("music.playlist.set_cue_out", "Set CUE OUT")))
	{
		cueCursorSample = std::clamp<int64_t>(cueCursorSample, cueIn, maxSample);
		playlistController.SetTrackCueById(track.id, cueIn, cueCursorSample);
		AutosaveCurrentDocument();
	}

	int currentIdx = playlistController.GetCurrentTrackIndex();
	if (playlistController.IsPlaying() && currentIdx >= 0 && currentIdx < (int)tracks.size() && currentIdx != cursorRow)
	{
		const auto &outgoing = tracks[currentIdx];
		int64_t outgoingCueOut = outgoing.cueOutSample >= 0 ? outgoing.cueOutSample : cueCursorSample;
		CSlrMusicCueTransitionPlan plan = CSlrMusicCueTransitionPlanner::PlanTransition(
			cueCursorSample,
			outgoingCueOut,
			track.cueInSample);
		ImGui::Text("%s: delay=%" PRId64 " samples, incoming start offset=%" PRId64,
			L("music.playlist.transition", "Transition"),
			plan.startDelaySamples, plan.incomingStartSample);
	}
}

// ─── Localization helper ────────────────────────────────────────────────────────

const char *CGuiViewMusicPlaylist::L(const char *key, const char *fallback) const
{
	if (sTranslateLabelFunc != NULL)
	{
		const char *translated = sTranslateLabelFunc(key);
		if (translated != NULL && strcmp(translated, key) != 0)
			return translated;
	}
	return fallback;
}
