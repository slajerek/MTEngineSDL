#pragma once

#include "json.hpp"

#include <cstdint>
#include <map>
#include <random>
#include <string>
#include <vector>

enum class CSlrMusicPlaylistEnableResult
{
	NoChange,
	Toggled,
	ToggledSkipCurrent,
	ToggledStopAutoFlagSet,
	ToggledResumeFromStop
};

struct CSlrMusicPlaylistTrack
{
	uint32_t id = 0;
	std::string path;

	// Cue defaults cover full track when not set by user.
	int64_t cueInSample = 0;
	int64_t cueOutSample = -1;

	bool enabled = true;
};

enum class CSlrMusicPlaylistPathMode
{
	Absolute,
	ProjectRelative,
};

struct CSlrMusicPlaylistSaveOptions
{
	CSlrMusicPlaylistPathMode pathMode = CSlrMusicPlaylistPathMode::Absolute;
	std::string projectRootPath;
};

struct CSlrMusicPlaylistLoadOptions
{
	std::string projectRootPath;
};

struct CSlrMusicPlaylistDocumentInfo
{
	int version = 2;
	CSlrMusicPlaylistPathMode pathMode = CSlrMusicPlaylistPathMode::Absolute;
};

class CSlrMusicPlaylistPersistence;

class CSlrMusicPlaylistController
{
public:
	CSlrMusicPlaylistController();

	void SetRandomSeed(uint32_t seed);

	void SetLoopEnabled(bool enabled);
	bool GetLoopEnabled() const;

	void SetRandomEnabled(bool enabled);
	bool GetRandomEnabled() const;

	void SetMinDistanceBetweenSameTrack(int k);
	int GetMinDistanceBetweenSameTrack() const;

	uint32_t AddTrack(const std::string &path);
	bool RemoveTrackByIndex(int removeIndex);
	bool RemoveTrackById(uint32_t trackId);
	void ClearTracks();

	bool StartPlaybackAtIndex(int index);
	bool StartPlaybackAtTrackId(uint32_t trackId);
	void StopPlayback();

	bool MoveTrack(int fromIndex, int toIndex);
	bool StepToNextTrack();
	bool StepToPrevTrack();
	bool StepToNextTrackWithCuePlan(int64_t currentOutgoingSample, int64_t *startDelaySamplesOut, int64_t *incomingStartSampleOut);
	bool StepToPrevTrackWithCuePlan(int64_t *incomingStartSampleOut);

	bool IsPlaying() const;
	int GetCurrentTrackIndex() const;
	uint32_t GetCurrentTrackId() const;
	uint32_t GetScheduleVersion() const;
	int GetTrackCount() const;

	const std::vector<int> &GetRandomRoundOrder() const;
	const std::vector<CSlrMusicPlaylistTrack> &GetTracks() const;

	void SetPlayOnAddEnabled(bool enabled);
	bool GetPlayOnAddEnabled() const;

	void SetLastOpenFolder(const std::string &folder);
	const std::string &GetLastOpenFolder() const;

	bool SetTrackCueById(uint32_t trackId, int64_t cueInSample, int64_t cueOutSample);
	const CSlrMusicPlaylistTrack *FindTrackById(uint32_t trackId) const;

	void SetCrossfadeEnabled(bool enabled);
	bool GetCrossfadeEnabled() const;
	void SetCrossfadeDurationMs(int ms);
	int GetCrossfadeDurationMs() const;

	void SetOverlapEnabled(bool enabled);
	bool GetOverlapEnabled() const;

	void SetCrossfadeSplineEnabled(bool enabled);
	bool GetCrossfadeSplineEnabled() const;
	void SetCrossfadeSplineCp1(float x, float y);
	void SetCrossfadeSplineCp2(float x, float y);
	void GetCrossfadeSplineCp1(float &x, float &y) const;
	void GetCrossfadeSplineCp2(float &x, float &y) const;

	bool SaveToFile(const char *filePath) const;
	bool SaveToFile(const char *filePath, const CSlrMusicPlaylistSaveOptions &options) const;
	bool LoadFromFile(const char *filePath);
	bool LoadFromFile(const char *filePath, const CSlrMusicPlaylistLoadOptions &options,
					 CSlrMusicPlaylistDocumentInfo *infoOut = nullptr);

	// Shuffle state persistence (saved in playback-state file, not playlist file)
	nlohmann::json SerializeShuffleState() const;
	void RestoreShuffleState(const nlohmann::json &j);

	// Play count tracking
	int GetPlayCountForTrack(uint32_t trackId) const;
	const std::map<uint32_t, int> &GetPlayCounts() const;

	int CountEnabledTracks() const;
	int FindFirstEnabledIndex() const;   // returns -1 if none
	bool WasAutoStoppedDueToNoEnabled() const;
	CSlrMusicPlaylistEnableResult SetTrackEnabledById(uint32_t trackId, bool enabled);

private:
	friend class CSlrMusicPlaylistPersistence;

	int IndexOfTrackId(uint32_t trackId) const;
	int FindNextEnabledIndex(int startIndex, bool wrap) const;
	int FindPrevEnabledIndex(int startIndex, bool wrap) const;
	void AppendHistory(uint32_t trackIndex);
	bool RespectsDistance(uint32_t trackId, const std::vector<uint32_t> &sequence, int requiredDistance) const;
	void BuildRandomRound(int preferredStartIndex);
	void RerandomizeRound(int preferredStartIndex = -1);
	bool StepRandom();

private:
	std::vector<CSlrMusicPlaylistTrack> tracks;
	std::vector<int> randomRound;
	int randomRoundPos = -1;

	std::vector<uint32_t> playHistoryTrackIds;
	std::map<uint32_t, int> playCountByTrackId;

	bool loopEnabled = false;
	bool randomEnabled = false;
	int minDistanceBetweenSameTrack = 1;
	bool playOnAddEnabled = false;
	bool crossfadeEnabled = false;
	int crossfadeDurationMs = 2000;
	bool overlapEnabled = false;
	bool crossfadeSplineEnabled = false;
	float crossfadeSplineCp1x = 0.42f;
	float crossfadeSplineCp1y = 0.0f;
	float crossfadeSplineCp2x = 0.58f;
	float crossfadeSplineCp2y = 1.0f;
	std::string lastOpenFolder;

	int currentTrackIndex = -1;
	bool isPlaying = false;
	bool wasAutoStoppedDueToNoEnabled = false;

	uint32_t nextTrackId = 1;
	uint32_t scheduleVersion = 1;
	uint32_t randomSeed = 0xBADC0DEU;
	std::mt19937 rng;
};
