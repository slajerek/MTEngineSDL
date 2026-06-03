#include "CSlrMusicPlaylistController.h"
#include "CSlrMusicCueTransitionPlanner.h"
#include "CSlrMusicPlaylistPersistence.h"

#include <algorithm>

CSlrMusicPlaylistController::CSlrMusicPlaylistController()
: rng(0xBADC0DEU)
{
}

void CSlrMusicPlaylistController::SetRandomSeed(uint32_t seed)
{
	rng.seed(seed);
	randomSeed = seed;
	RerandomizeRound();
}

void CSlrMusicPlaylistController::SetLoopEnabled(bool enabled)
{
	loopEnabled = enabled;
	scheduleVersion++;
}

bool CSlrMusicPlaylistController::GetLoopEnabled() const
{
	return loopEnabled;
}

void CSlrMusicPlaylistController::SetRandomEnabled(bool enabled)
{
	if (randomEnabled == enabled)
		return;
	randomEnabled = enabled;
	RerandomizeRound();
}

bool CSlrMusicPlaylistController::GetRandomEnabled() const
{
	return randomEnabled;
}

void CSlrMusicPlaylistController::SetMinDistanceBetweenSameTrack(int k)
{
	if (k < 1)
		k = 1;
	if (minDistanceBetweenSameTrack == k)
		return;
	minDistanceBetweenSameTrack = k;
	RerandomizeRound();
}

int CSlrMusicPlaylistController::GetMinDistanceBetweenSameTrack() const
{
	return minDistanceBetweenSameTrack;
}

uint32_t CSlrMusicPlaylistController::AddTrack(const std::string &path)
{
	CSlrMusicPlaylistTrack track;
	track.id = nextTrackId++;
	track.path = path;
	tracks.push_back(track);
	int newIndex = (int)tracks.size() - 1;

	if (isPlaying && tracks.size() == 1)
	{
		currentTrackIndex = 0;
	}

	RerandomizeRound();

	if (playOnAddEnabled)
	{
		StartPlaybackAtIndex(newIndex);
	}

	return track.id;
}

bool CSlrMusicPlaylistController::RemoveTrackByIndex(int removeIndex)
{
	if (removeIndex < 0 || removeIndex >= (int)tracks.size())
		return false;
	return RemoveTrackById(tracks[removeIndex].id);
}

bool CSlrMusicPlaylistController::RemoveTrackById(uint32_t trackId)
{
	int removeIndex = IndexOfTrackId(trackId);
	if (removeIndex < 0)
		return false;

	bool wasPlaying = isPlaying;
	bool wasCurrent = (currentTrackIndex == removeIndex);

	tracks.erase(tracks.begin() + removeIndex);

	if (tracks.empty())
	{
		isPlaying = false;
		currentTrackIndex = -1;
		randomRound.clear();
		randomRoundPos = -1;
		scheduleVersion++;
		return true;
	}

	if (currentTrackIndex > removeIndex)
	{
		currentTrackIndex--;
	}

	if (randomEnabled)
	{
		RerandomizeRound();
		if (wasPlaying && wasCurrent)
		{
			currentTrackIndex = randomRound.empty() ? 0 : randomRound[0];
			randomRoundPos = randomRound.empty() ? -1 : 0;
			isPlaying = true;
			AppendHistory((uint32_t)currentTrackIndex);
		}
	}
	else
	{
		scheduleVersion++;
		if (wasPlaying && wasCurrent)
		{
			if (currentTrackIndex >= (int)tracks.size())
			{
				if (loopEnabled)
					currentTrackIndex = 0;
				else
				{
					isPlaying = false;
					currentTrackIndex = (int)tracks.size() - 1;
					return true;
				}
			}
			isPlaying = true;
			AppendHistory((uint32_t)currentTrackIndex);
		}
	}

	return true;
}

void CSlrMusicPlaylistController::ClearTracks()
{
	tracks.clear();
	isPlaying = false;
	currentTrackIndex = -1;
	randomRound.clear();
	randomRoundPos = -1;
	playHistoryTrackIds.clear();
	playCountByTrackId.clear();
	scheduleVersion++;
}

bool CSlrMusicPlaylistController::StartPlaybackAtIndex(int index)
{
	if (index < 0 || index >= (int)tracks.size())
		return false;

	if (randomEnabled)
	{
		RerandomizeRound(index);
		if (randomRound.empty())
			return false;
		currentTrackIndex = randomRound[0];
		randomRoundPos = 0;
	}
	else
	{
		currentTrackIndex = index;
	}

	isPlaying = true;
	AppendHistory((uint32_t)currentTrackIndex);
	return true;
}

bool CSlrMusicPlaylistController::StartPlaybackAtTrackId(uint32_t trackId)
{
	int idx = IndexOfTrackId(trackId);
	if (idx < 0)
		return false;
	return StartPlaybackAtIndex(idx);
}

void CSlrMusicPlaylistController::StopPlayback()
{
	isPlaying = false;
}

bool CSlrMusicPlaylistController::MoveTrack(int fromIndex, int toIndex)
{
	if (fromIndex < 0 || fromIndex >= (int)tracks.size())
		return false;
	if (toIndex < 0 || toIndex >= (int)tracks.size())
		return false;
	if (fromIndex == toIndex)
		return true;

	auto moved = tracks[fromIndex];
	tracks.erase(tracks.begin() + fromIndex);
	tracks.insert(tracks.begin() + toIndex, moved);

	if (currentTrackIndex == fromIndex)
	{
		currentTrackIndex = toIndex;
	}
	else if (fromIndex < currentTrackIndex && toIndex >= currentTrackIndex)
	{
		currentTrackIndex--;
	}
	else if (fromIndex > currentTrackIndex && toIndex <= currentTrackIndex)
	{
		currentTrackIndex++;
	}

	RerandomizeRound();
	return true;
}

bool CSlrMusicPlaylistController::StepToNextTrack()
{
	if (!isPlaying || tracks.empty())
		return false;

	if (randomEnabled)
	{
		return StepRandom();
	}

	int next = FindNextEnabledIndex(currentTrackIndex, loopEnabled);
	if (next < 0)
	{
		isPlaying = false;
		return false;
	}
	currentTrackIndex = next;
	AppendHistory((uint32_t)currentTrackIndex);
	return true;
}

bool CSlrMusicPlaylistController::StepToPrevTrack()
{
	if (!isPlaying || tracks.empty())
		return false;

	if (randomEnabled)
	{
		if (randomRound.empty())
			return false;

		if (randomRoundPos > 0)
		{
			randomRoundPos--;
			currentTrackIndex = randomRound[randomRoundPos];
			AppendHistory((uint32_t)currentTrackIndex);
			return true;
		}

		if (!loopEnabled)
			return false;

		randomRoundPos = (int)randomRound.size() - 1;
		currentTrackIndex = randomRound[randomRoundPos];
		AppendHistory((uint32_t)currentTrackIndex);
		return true;
	}

	int prev = FindPrevEnabledIndex(currentTrackIndex, loopEnabled);
	if (prev < 0)
		return false;
	currentTrackIndex = prev;
	AppendHistory((uint32_t)currentTrackIndex);
	return true;
}

bool CSlrMusicPlaylistController::StepToPrevTrackWithCuePlan(int64_t *incomingStartSampleOut)
{
	if (incomingStartSampleOut)
		*incomingStartSampleOut = 0;

	if (!isPlaying || tracks.empty() || currentTrackIndex < 0 || currentTrackIndex >= (int)tracks.size())
		return false;

	if (!StepToPrevTrack())
		return false;

	if (currentTrackIndex < 0 || currentTrackIndex >= (int)tracks.size())
		return false;

	const CSlrMusicPlaylistTrack &incomingTrack = tracks[currentTrackIndex];
	if (incomingStartSampleOut)
		*incomingStartSampleOut = std::max<int64_t>(0, incomingTrack.cueInSample);

	return true;
}

bool CSlrMusicPlaylistController::StepToNextTrackWithCuePlan(int64_t currentOutgoingSample, int64_t *startDelaySamplesOut, int64_t *incomingStartSampleOut)
{
	if (startDelaySamplesOut)
		*startDelaySamplesOut = 0;
	if (incomingStartSampleOut)
		*incomingStartSampleOut = 0;

	if (!isPlaying || tracks.empty() || currentTrackIndex < 0 || currentTrackIndex >= (int)tracks.size())
		return false;

	const CSlrMusicPlaylistTrack outgoingTrack = tracks[currentTrackIndex];
	const int64_t outgoingCueOut = (outgoingTrack.cueOutSample >= 0) ? outgoingTrack.cueOutSample : currentOutgoingSample;

	if (!StepToNextTrack())
		return false;

	if (currentTrackIndex < 0 || currentTrackIndex >= (int)tracks.size())
		return false;

	const CSlrMusicPlaylistTrack incomingTrack = tracks[currentTrackIndex];
	CSlrMusicCueTransitionPlan plan = CSlrMusicCueTransitionPlanner::PlanTransition(currentOutgoingSample, outgoingCueOut, incomingTrack.cueInSample);

	if (startDelaySamplesOut)
		*startDelaySamplesOut = plan.startDelaySamples;
	if (incomingStartSampleOut)
		*incomingStartSampleOut = plan.incomingStartSample;

	return true;
}

bool CSlrMusicPlaylistController::IsPlaying() const
{
	return isPlaying;
}

int CSlrMusicPlaylistController::GetCurrentTrackIndex() const
{
	return currentTrackIndex;
}

uint32_t CSlrMusicPlaylistController::GetCurrentTrackId() const
{
	if (currentTrackIndex < 0 || currentTrackIndex >= (int)tracks.size())
		return 0;
	return tracks[currentTrackIndex].id;
}

uint32_t CSlrMusicPlaylistController::GetScheduleVersion() const
{
	return scheduleVersion;
}

int CSlrMusicPlaylistController::GetTrackCount() const
{
	return (int)tracks.size();
}

const std::vector<int> &CSlrMusicPlaylistController::GetRandomRoundOrder() const
{
	return randomRound;
}

const std::vector<CSlrMusicPlaylistTrack> &CSlrMusicPlaylistController::GetTracks() const
{
	return tracks;
}

void CSlrMusicPlaylistController::SetPlayOnAddEnabled(bool enabled)
{
	playOnAddEnabled = enabled;
	scheduleVersion++;
}

bool CSlrMusicPlaylistController::GetPlayOnAddEnabled() const
{
	return playOnAddEnabled;
}

void CSlrMusicPlaylistController::SetLastOpenFolder(const std::string &folder)
{
	lastOpenFolder = folder;
	scheduleVersion++;
}

const std::string &CSlrMusicPlaylistController::GetLastOpenFolder() const
{
	return lastOpenFolder;
}

bool CSlrMusicPlaylistController::SetTrackCueById(uint32_t trackId, int64_t cueInSample, int64_t cueOutSample)
{
	for (auto &track : tracks)
	{
		if (track.id == trackId)
		{
			track.cueInSample = std::max<int64_t>(0, cueInSample);
			track.cueOutSample = cueOutSample;
			scheduleVersion++;
			return true;
		}
	}
	return false;
}

const CSlrMusicPlaylistTrack *CSlrMusicPlaylistController::FindTrackById(uint32_t trackId) const
{
	for (const auto &track : tracks)
	{
		if (track.id == trackId)
			return &track;
	}
	return nullptr;
}

void CSlrMusicPlaylistController::SetCrossfadeEnabled(bool enabled)
{
	crossfadeEnabled = enabled;
	scheduleVersion++;
}

bool CSlrMusicPlaylistController::GetCrossfadeEnabled() const
{
	return crossfadeEnabled;
}

void CSlrMusicPlaylistController::SetCrossfadeDurationMs(int ms)
{
	if (ms < 20) ms = 20;
	crossfadeDurationMs = ms;
	scheduleVersion++;
}

int CSlrMusicPlaylistController::GetCrossfadeDurationMs() const
{
	return crossfadeDurationMs;
}

void CSlrMusicPlaylistController::SetOverlapEnabled(bool enabled)
{
	overlapEnabled = enabled;
	scheduleVersion++;
}

bool CSlrMusicPlaylistController::GetOverlapEnabled() const
{
	return overlapEnabled;
}

void CSlrMusicPlaylistController::SetCrossfadeSplineEnabled(bool enabled)
{
	crossfadeSplineEnabled = enabled;
	scheduleVersion++;
}

bool CSlrMusicPlaylistController::GetCrossfadeSplineEnabled() const
{
	return crossfadeSplineEnabled;
}

void CSlrMusicPlaylistController::SetCrossfadeSplineCp1(float x, float y)
{
	crossfadeSplineCp1x = x;
	crossfadeSplineCp1y = y;
}

void CSlrMusicPlaylistController::SetCrossfadeSplineCp2(float x, float y)
{
	crossfadeSplineCp2x = x;
	crossfadeSplineCp2y = y;
}

void CSlrMusicPlaylistController::GetCrossfadeSplineCp1(float &x, float &y) const
{
	x = crossfadeSplineCp1x;
	y = crossfadeSplineCp1y;
}

void CSlrMusicPlaylistController::GetCrossfadeSplineCp2(float &x, float &y) const
{
	x = crossfadeSplineCp2x;
	y = crossfadeSplineCp2y;
}

bool CSlrMusicPlaylistController::SaveToFile(const char *filePath) const
{
	return SaveToFile(filePath, CSlrMusicPlaylistSaveOptions());
}

bool CSlrMusicPlaylistController::SaveToFile(const char *filePath, const CSlrMusicPlaylistSaveOptions &options) const
{
	return CSlrMusicPlaylistPersistence::SaveToFile(*this, filePath, options);
}

bool CSlrMusicPlaylistController::LoadFromFile(const char *filePath)
{
	return LoadFromFile(filePath, CSlrMusicPlaylistLoadOptions(), nullptr);
}

bool CSlrMusicPlaylistController::LoadFromFile(const char *filePath,
					   const CSlrMusicPlaylistLoadOptions &options,
					   CSlrMusicPlaylistDocumentInfo *infoOut)
{
	return CSlrMusicPlaylistPersistence::LoadFromFile(*this, filePath, options, infoOut);
}

int CSlrMusicPlaylistController::IndexOfTrackId(uint32_t trackId) const
{
	for (int i = 0; i < (int)tracks.size(); i++)
	{
		if (tracks[i].id == trackId)
			return i;
	}
	return -1;
}

int CSlrMusicPlaylistController::FindNextEnabledIndex(int startIndex, bool wrap) const
{
	int n = (int)tracks.size();
	if (n == 0) return -1;
	for (int step = 1; step <= n; step++)
	{
		int i = startIndex + step;
		if (i >= n)
		{
			if (!wrap) return -1;
			i -= n;
		}
		if (tracks[i].enabled) return i;
	}
	return -1;
}

int CSlrMusicPlaylistController::FindPrevEnabledIndex(int startIndex, bool wrap) const
{
	int n = (int)tracks.size();
	if (n == 0) return -1;
	for (int step = 1; step <= n; step++)
	{
		int i = startIndex - step;
		if (i < 0)
		{
			if (!wrap) return -1;
			i += n;
		}
		if (tracks[i].enabled) return i;
	}
	return -1;
}

void CSlrMusicPlaylistController::AppendHistory(uint32_t trackIndex)
{
	if (trackIndex >= tracks.size())
		return;
	uint32_t trackId = tracks[trackIndex].id;
	playHistoryTrackIds.push_back(trackId);
	playCountByTrackId[trackId]++;
	const int keep = std::max(minDistanceBetweenSameTrack * 4, 16);
	if ((int)playHistoryTrackIds.size() > keep)
	{
		playHistoryTrackIds.erase(playHistoryTrackIds.begin(),
								 playHistoryTrackIds.begin() + ((int)playHistoryTrackIds.size() - keep));
	}
}

int CSlrMusicPlaylistController::GetPlayCountForTrack(uint32_t trackId) const
{
	auto it = playCountByTrackId.find(trackId);
	return (it != playCountByTrackId.end()) ? it->second : 0;
}

const std::map<uint32_t, int> &CSlrMusicPlaylistController::GetPlayCounts() const
{
	return playCountByTrackId;
}

bool CSlrMusicPlaylistController::RespectsDistance(uint32_t trackId, const std::vector<uint32_t> &sequence, int requiredDistance) const
{
	if (requiredDistance <= 1)
		return true;
	int start = (int)sequence.size() - (requiredDistance - 1);
	if (start < 0)
		start = 0;
	for (int i = start; i < (int)sequence.size(); i++)
	{
		if (sequence[i] == trackId)
			return false;
	}
	return true;
}

void CSlrMusicPlaylistController::BuildRandomRound(int preferredStartIndex)
{
	randomRound.clear();
	randomRoundPos = -1;
	if (tracks.empty())
		return;

	std::vector<int> remaining;
	remaining.reserve(tracks.size());
	for (int i = 0; i < (int)tracks.size(); i++)
		if (tracks[i].enabled)
			remaining.push_back(i);

	std::vector<uint32_t> sequence = playHistoryTrackIds;

	// Sort candidates by play count (ascending) so least-played are picked first.
	// Within same play count, shuffle randomly for variety.
	auto pickCandidate = [&](int requiredDistance) -> int {
		if (remaining.empty())
			return -1;
		// Partition by play count: sort ascending, then shuffle within same-count groups
		std::sort(remaining.begin(), remaining.end(), [&](int a, int b) {
			int ca = GetPlayCountForTrack(tracks[a].id);
			int cb = GetPlayCountForTrack(tracks[b].id);
			return ca < cb;
		});
		// Shuffle within same-count groups
		int groupStart = 0;
		while (groupStart < (int)remaining.size())
		{
			int groupCount = GetPlayCountForTrack(tracks[remaining[groupStart]].id);
			int groupEnd = groupStart;
			while (groupEnd < (int)remaining.size() && GetPlayCountForTrack(tracks[remaining[groupEnd]].id) == groupCount)
				groupEnd++;
			std::shuffle(remaining.begin() + groupStart, remaining.begin() + groupEnd, rng);
			groupStart = groupEnd;
		}
		for (int idx : remaining)
		{
			uint32_t id = tracks[idx].id;
			if (RespectsDistance(id, sequence, requiredDistance))
				return idx;
		}
		return -1;
	};

	auto moveToRoundAndErase = [&](int trackIndex) {
		randomRound.push_back(trackIndex);
		sequence.push_back(tracks[trackIndex].id);
		remaining.erase(std::remove(remaining.begin(), remaining.end(), trackIndex), remaining.end());
	};

	if (preferredStartIndex >= 0 && preferredStartIndex < (int)tracks.size()
		&& tracks[preferredStartIndex].enabled)
	{
		moveToRoundAndErase(preferredStartIndex);
	}

	while (!remaining.empty())
	{
		int candidate = -1;
		for (int req = minDistanceBetweenSameTrack; req >= 1; req--)
		{
			candidate = pickCandidate(req);
			if (candidate >= 0)
				break;
		}

		if (candidate < 0)
			candidate = remaining.front();

		moveToRoundAndErase(candidate);
	}
}

void CSlrMusicPlaylistController::RerandomizeRound(int preferredStartIndex)
{
	scheduleVersion++;
	if (!randomEnabled)
	{
		randomRound.clear();
		randomRoundPos = -1;
		return;
	}
	BuildRandomRound(preferredStartIndex);
}

nlohmann::json CSlrMusicPlaylistController::SerializeShuffleState() const
{
	using json = nlohmann::json;
	json j;

	// Shuffle round
	json roundArr = json::array();
	for (int idx : randomRound)
		roundArr.push_back(idx);
	j["randomRound"] = roundArr;
	j["randomRoundPos"] = randomRoundPos;

	// Play history (for distance constraint)
	json histArr = json::array();
	for (uint32_t id : playHistoryTrackIds)
		histArr.push_back(id);
	j["playHistory"] = histArr;

	// Play counts per track
	json countsObj = json::object();
	for (const auto &kv : playCountByTrackId)
		countsObj[std::to_string(kv.first)] = kv.second;
	j["playCounts"] = countsObj;

	return j;
}

void CSlrMusicPlaylistController::RestoreShuffleState(const nlohmann::json &j)
{
	// Restore shuffle round
	randomRound.clear();
	if (j.contains("randomRound") && j["randomRound"].is_array())
	{
		for (const auto &el : j["randomRound"])
		{
			int idx = el.get<int>();
			if (idx >= 0 && idx < (int)tracks.size())
				randomRound.push_back(idx);
		}
	}
	randomRoundPos = j.value("randomRoundPos", -1);

	// Clamp position
	if (randomRoundPos >= (int)randomRound.size())
		randomRoundPos = (int)randomRound.size() - 1;

	// Restore play history
	playHistoryTrackIds.clear();
	if (j.contains("playHistory") && j["playHistory"].is_array())
	{
		for (const auto &el : j["playHistory"])
			playHistoryTrackIds.push_back(el.get<uint32_t>());
	}

	// Restore play counts
	playCountByTrackId.clear();
	if (j.contains("playCounts") && j["playCounts"].is_object())
	{
		for (auto &[key, val] : j["playCounts"].items())
		{
			uint32_t trackId = (uint32_t)std::stoul(key);
			playCountByTrackId[trackId] = val.get<int>();
		}
	}
}

bool CSlrMusicPlaylistController::StepRandom()
{
	if (randomRound.empty())
	{
		BuildRandomRound(currentTrackIndex);
		if (randomRound.empty())
		{
			isPlaying = false;
			return false;
		}
		currentTrackIndex = randomRound[0];
		randomRoundPos = 0;
		AppendHistory((uint32_t)currentTrackIndex);
		return true;
	}

	if (randomRoundPos + 1 < (int)randomRound.size())
	{
		randomRoundPos++;
		currentTrackIndex = randomRound[randomRoundPos];
		AppendHistory((uint32_t)currentTrackIndex);
		return true;
	}

	if (!loopEnabled)
	{
		isPlaying = false;
		return false;
	}

	BuildRandomRound(-1);
	if (randomRound.empty())
	{
		isPlaying = false;
		return false;
	}

	randomRoundPos = 0;
	currentTrackIndex = randomRound[0];
	AppendHistory((uint32_t)currentTrackIndex);
	scheduleVersion++;
	return true;
}

int CSlrMusicPlaylistController::CountEnabledTracks() const
{
	int count = 0;
	for (const auto &t : tracks)
		if (t.enabled) count++;
	return count;
}

int CSlrMusicPlaylistController::FindFirstEnabledIndex() const
{
	for (int i = 0; i < (int)tracks.size(); i++)
		if (tracks[i].enabled) return i;
	return -1;
}

bool CSlrMusicPlaylistController::WasAutoStoppedDueToNoEnabled() const
{
	return wasAutoStoppedDueToNoEnabled;
}

CSlrMusicPlaylistEnableResult CSlrMusicPlaylistController::SetTrackEnabledById(uint32_t trackId, bool enabled)
{
	int idx = IndexOfTrackId(trackId);
	if (idx < 0)
		return CSlrMusicPlaylistEnableResult::NoChange;
	if (tracks[idx].enabled == enabled)
		return CSlrMusicPlaylistEnableResult::NoChange;

	tracks[idx].enabled = enabled;
	scheduleVersion++;

	// Case A: disabling the currently-playing track.
	if (!enabled && isPlaying && idx == currentTrackIndex)
	{
		if (randomEnabled)
		{
			RerandomizeRound(-1);   // rebuild round from remaining enabled
		}
		int enabledCount = CountEnabledTracks();
		if (enabledCount == 0)
		{
			isPlaying = false;
			wasAutoStoppedDueToNoEnabled = true;
			return CSlrMusicPlaylistEnableResult::ToggledStopAutoFlagSet;
		}

		int next;
		if (randomEnabled)
		{
			next = randomRound.empty() ? FindFirstEnabledIndex() : randomRound[0];
			randomRoundPos = randomRound.empty() ? -1 : 0;
		}
		else
		{
			next = FindNextEnabledIndex(idx, loopEnabled);
			if (next < 0)
				next = FindFirstEnabledIndex();   // fallback: wrap regardless of loop so playback survives
		}
		currentTrackIndex = next;
		AppendHistory((uint32_t)currentTrackIndex);
		return CSlrMusicPlaylistEnableResult::ToggledSkipCurrent;
	}

	// Case B: enabling any track while auto-stopped.
	if (enabled && wasAutoStoppedDueToNoEnabled)
	{
		wasAutoStoppedDueToNoEnabled = false;
		currentTrackIndex = idx;
		isPlaying = true;
		if (randomEnabled)
			RerandomizeRound(idx);
		AppendHistory((uint32_t)currentTrackIndex);
		return CSlrMusicPlaylistEnableResult::ToggledResumeFromStop;
	}

	// Case C: disabling a non-current track while playing — keep random round consistent.
	if (!enabled && randomEnabled)
	{
		// Rebuild to drop the disabled track from the pool without replaying history.
		RerandomizeRound(-1);
	}

	return CSlrMusicPlaylistEnableResult::Toggled;
}
