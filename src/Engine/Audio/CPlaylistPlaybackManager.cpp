#include "CPlaylistPlaybackManager.h"

#include "CSlrFileFromDocuments.h"
#include "CSlrMusicFileOgg.h"
#include "SND_Main.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <unordered_set>

#include "json.hpp"

#include <fstream>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

CPlaylistPlaybackManager::CPlaylistPlaybackManager()
{
}

CPlaylistPlaybackManager::~CPlaylistPlaybackManager()
{
	StopOutgoingCrossfade();
	StopCurrentPlayback();
	for (auto &it : playbackByTrackId)
	{
		if (it.second.music)
			delete it.second.music;
		if (it.second.fileHandle)
			delete it.second.fileHandle;
	}
	playbackByTrackId.clear();
}

void CPlaylistPlaybackManager::SetCrossfadeSplineParams(float cp1x, float cp1y, float cp2x, float cp2y)
{
	splineCp1x = cp1x;
	splineCp1y = cp1y;
	splineCp2x = cp2x;
	splineCp2y = cp2y;
}

void CPlaylistPlaybackManager::GetCrossfadeSplineParams(float &cp1x, float &cp1y, float &cp2x, float &cp2y) const
{
	cp1x = splineCp1x;
	cp1y = splineCp1y;
	cp2x = splineCp2x;
	cp2y = splineCp2y;
}

float CPlaylistPlaybackManager::ClampVolume(float volume) const
{
	if (!std::isfinite(volume))
		return 1.0f;
	return std::clamp(volume, 0.0f, 1.0f);
}

void CPlaylistPlaybackManager::SetMasterVolume(float volume)
{
	masterVolume = ClampVolume(volume);
	if (currentPlaybackMusic)
		currentPlaybackMusic->volume = currentPlaybackBaseVolume * masterVolume;
	if (crossfadeOutgoingMusic)
		crossfadeOutgoingMusic->volume = outgoingPlaybackBaseVolume * masterVolume;
}

void CPlaylistPlaybackManager::SetCurrentPlaybackBaseVolume(float volume)
{
	currentPlaybackBaseVolume = ClampVolume(volume);
	if (currentPlaybackMusic)
		currentPlaybackMusic->volume = currentPlaybackBaseVolume * masterVolume;
}

void CPlaylistPlaybackManager::SetOutgoingPlaybackBaseVolume(float volume)
{
	outgoingPlaybackBaseVolume = ClampVolume(volume);
	if (crossfadeOutgoingMusic)
		crossfadeOutgoingMusic->volume = outgoingPlaybackBaseVolume * masterVolume;
}

// Cubic Bezier easing: maps t [0,1] to eased value [0,1].
// Control points: P0=(0,0), P1=(cp1x,cp1y), P2=(cp2x,cp2y), P3=(1,1).
// Uses Newton's method to solve for the Bezier parameter u given x(u)=t.
float CPlaylistPlaybackManager::CubicBezierEase(float t, float cp1x, float cp1y, float cp2x, float cp2y)
{
	if (t <= 0.0f) return 0.0f;
	if (t >= 1.0f) return 1.0f;

	// Newton's method: solve x(u) = t for u
	float u = t;
	for (int i = 0; i < 8; i++)
	{
		float omu = 1.0f - u;
		float omu2 = omu * omu;
		float u2 = u * u;

		float x = 3.0f * omu2 * u * cp1x + 3.0f * omu * u2 * cp2x + u2 * u;
		float dx = 3.0f * omu2 * cp1x + 6.0f * omu * u * (cp2x - cp1x) + 3.0f * u2 * (1.0f - cp2x);

		if (std::fabs(dx) < 1e-6f) break;
		u -= (x - t) / dx;
		u = std::clamp(u, 0.0f, 1.0f);
	}

	// Evaluate y(u)
	float omu = 1.0f - u;
	float omu2 = omu * omu;
	float u2 = u * u;
	return 3.0f * omu2 * u * cp1y + 3.0f * omu * u2 * cp2y + u2 * u;
}

bool CPlaylistPlaybackManager::IsOggPath(const std::string &trackPath) const
{
	const size_t dotPos = trackPath.find_last_of('.');
	if (dotPos == std::string::npos)
		return false;

	std::string ext = trackPath.substr(dotPos + 1);
	for (char &ch : ext)
		ch = (char)std::tolower((unsigned char)ch);

	return ext == "ogg";
}

bool CPlaylistPlaybackManager::EnsureTrackPlaybackLoaded(const CSlrMusicPlaylistTrack &track)
{
	SPlaybackEntry &entry = playbackByTrackId[track.id];
	if (entry.attemptedLoad)
		return entry.loadSucceeded;

	entry.attemptedLoad = true;
	entry.loadSucceeded = false;

	if (!IsOggPath(track.path))
		return false;

	entry.fileHandle = new CSlrFileFromDocuments(track.path.c_str(), SLR_FILE_MODE_READ, true);
	if (entry.fileHandle == NULL || !entry.fileHandle->Exists())
	{
		if (entry.fileHandle)
		{
			delete entry.fileHandle;
			entry.fileHandle = NULL;
		}
		return false;
	}

	entry.music = new CSlrMusicFileOgg(track.path.c_str());
	if (!entry.music->Init(entry.fileHandle, true))
	{
		delete entry.music;
		entry.music = NULL;
		delete entry.fileHandle;
		entry.fileHandle = NULL;
		return false;
	}

	entry.music->repeat = false;
	entry.loadSucceeded = true;
	return true;
}

void CPlaylistPlaybackManager::StopCurrentPlayback()
{
	if (currentPlaybackMusic)
	{
		currentPlaybackMusic->Pause();
		SND_RemoveChannel(currentPlaybackMusic);
	}

	currentPlaybackMusic = NULL;
	currentPlaybackTrackId = -1;
	currentPlaybackBaseVolume = 1.0f;
	StopOutgoingCrossfade();
	isPaused = false;
	fadeInActive = false;
	fadeInProgress = 0.0f;
	transitionPending = false;
}

void CPlaylistPlaybackManager::PausePlayback()
{
	bool pausedAny = false;
	if (currentPlaybackMusic && currentPlaybackMusic->IsPlaying())
	{
		currentPlaybackMusic->Pause();
		pausedAny = true;
	}
	if (crossfadeOutgoingMusic && crossfadeOutgoingMusic->IsPlaying())
	{
		crossfadeOutgoingMusic->Pause();
		pausedAny = true;
	}
	if (pausedAny || currentPlaybackMusic != NULL || crossfadeOutgoingMusic != NULL || transitionPending)
		isPaused = true;
}

void CPlaylistPlaybackManager::ResumePlayback()
{
	if (isPaused)
	{
		if (currentPlaybackMusic)
			currentPlaybackMusic->Play();
		if (crossfadeOutgoingMusic)
			crossfadeOutgoingMusic->Play();
		isPaused = false;
	}
}

void CPlaylistPlaybackManager::SeekCurrentTrack(int64_t sampleNum)
{
	if (currentPlaybackMusic)
	{
		if (sampleNum < 0)
			sampleNum = 0;
		currentPlaybackMusic->SeekToSample((u64)sampleNum);
	}
}

int64_t CPlaylistPlaybackManager::GetCurrentSampleNum() const
{
	if (currentPlaybackMusic)
		return (int64_t)currentPlaybackMusic->GetCurrentSampleNum();
	return 0;
}

void CPlaylistPlaybackManager::StartTrackPlayback(const CSlrMusicPlaylistTrack &track, int64_t incomingStartSample)
{
	if (!EnsureTrackPlaybackLoaded(track))
		return;

	SPlaybackEntry &entry = playbackByTrackId[track.id];
	if (!entry.loadSucceeded || entry.music == NULL)
		return;

	// Cancel any active fade-in — user is manually changing tracks
	bool wasFadeInActive = fadeInActive;
	fadeInActive = false;
	fadeInProgress = 0.0f;
	transitionPending = false;
	if (wasFadeInActive && currentPlaybackMusic == entry.music)
		SetCurrentPlaybackBaseVolume(1.0f);

	if (currentPlaybackMusic != entry.music)
	{
		bool useOverlap = (crossfadeEnabled || overlapEnabled) && currentPlaybackMusic != NULL && !wasFadeInActive;

		if (useOverlap)
		{
			StopOutgoingCrossfade();
			crossfadeOutgoingMusic = currentPlaybackMusic;
			crossfadeOutgoingTrackId = currentPlaybackTrackId;
			SetOutgoingPlaybackBaseVolume(currentPlaybackBaseVolume);
			crossfadeProgress = 0.0f;
			// Outgoing stays in mixer — ramped down by SyncPlayback (crossfade) or left at 1.0 (overlap)
			currentPlaybackMusic = NULL;
			currentPlaybackTrackId = -1;
			currentPlaybackBaseVolume = 1.0f;
		}
		else
		{
			StopCurrentPlayback();
		}

		SND_AddChannel(entry.music);
		currentPlaybackMusic = entry.music;
		currentPlaybackTrackId = (int)track.id;

		// Crossfade: start incoming at volume 0, SyncPlayback ramps it up
		// Overlap or no transition: start at full volume
		if (crossfadeEnabled && !overlapEnabled && crossfadeOutgoingMusic != NULL)
		{
			SetCurrentPlaybackBaseVolume(0.0f);
		}
		else
		{
			SetCurrentPlaybackBaseVolume(1.0f);
		}
	}

	if (incomingStartSample < 0)
		incomingStartSample = 0;
	entry.music->SeekToSample((u64)incomingStartSample);
	entry.music->Play();
	isPaused = false;
}

void CPlaylistPlaybackManager::StartTrackPlaybackWithFadeIn(const CSlrMusicPlaylistTrack &track, int64_t startSample)
{
	if (!EnsureTrackPlaybackLoaded(track))
		return;

	SPlaybackEntry &entry = playbackByTrackId[track.id];
	if (!entry.loadSucceeded || entry.music == NULL)
		return;

	// Stop any existing playback cleanly (no crossfade)
	StopOutgoingCrossfade();
	StopCurrentPlayback();

	SND_AddChannel(entry.music);
	currentPlaybackMusic = entry.music;
	currentPlaybackTrackId = (int)track.id;

	// Start at volume 0 — SyncPlayback will ramp up via fade-in
	SetCurrentPlaybackBaseVolume(0.0f);
	fadeInActive = true;
	fadeInProgress = 0.0f;

	if (startSample < 0)
		startSample = 0;
	entry.music->SeekToSample((u64)startSample);
	entry.music->Play();
	isPaused = false;
}

void CPlaylistPlaybackManager::SetPlaybackStateFilePath(const std::string &path)
{
	playbackStateFilePath = path;
}

void CPlaylistPlaybackManager::SavePlaybackState(const CSlrMusicPlaylistController &controller)
{
	if (playbackStateFilePath.empty())
		return;

	nlohmann::json j;
	j["version"] = 2;

	bool isCurrentlyPlaying = (currentPlaybackMusic != NULL && !isPaused);
	j["wasPlaying"] = isCurrentlyPlaying;

	if (isCurrentlyPlaying && currentPlaybackTrackId >= 0)
	{
		const CSlrMusicPlaylistTrack *track = controller.FindTrackById((u32)currentPlaybackTrackId);
		if (track)
		{
			j["trackPath"] = track->path;
			j["trackId"] = track->id;
			j["samplePosition"] = (int64_t)currentPlaybackMusic->GetCurrentSampleNum();
		}
	}

	// Persist shuffle state (random round, play history, play counts)
	j["shuffleState"] = controller.SerializeShuffleState();

	std::ofstream out(playbackStateFilePath, std::ios::trunc);
	if (out.is_open())
		out << j.dump(2);
}

SPlaybackResumeState CPlaylistPlaybackManager::LoadPlaybackState()
{
	SPlaybackResumeState state;
	if (playbackStateFilePath.empty())
		return state;

	std::ifstream in(playbackStateFilePath);
	if (!in.is_open())
		return state;

	nlohmann::json j;
	try
	{
		j = nlohmann::json::parse(in);
	}
	catch (...)
	{
		return state;
	}

	state.valid = true;
	state.wasPlaying = j.value("wasPlaying", false);
	state.trackPath = j.value("trackPath", std::string());
	state.samplePosition = j.value<int64_t>("samplePosition", 0LL);
	state.trackId = j.value("trackId", (u32)0);
	if (j.contains("shuffleState") && j["shuffleState"].is_object())
		state.shuffleState = j["shuffleState"];
	return state;
}

int64_t CPlaylistPlaybackManager::GetTrackEffectiveCueOut(const CSlrMusicPlaylistTrack &track) const
{
	if (track.cueOutSample >= 0)
		return track.cueOutSample;

	auto it = playbackByTrackId.find(track.id);
	if (it == playbackByTrackId.end())
		return -1;

	const SPlaybackEntry &entry = it->second;
	if (!entry.loadSucceeded || entry.music == NULL)
		return -1;

	const u64 lenSamples = entry.music->GetLengthSamples();
	if (lenSamples == 0)
		return -1;

	return (int64_t)(lenSamples - 1);
}

void CPlaylistPlaybackManager::CleanupPlaybackCache(const CSlrMusicPlaylistController &controller)
{
	std::unordered_set<u32> liveTrackIds;
	liveTrackIds.reserve(controller.GetTracks().size());
	for (const CSlrMusicPlaylistTrack &track : controller.GetTracks())
		liveTrackIds.insert(track.id);

	for (auto it = playbackByTrackId.begin(); it != playbackByTrackId.end(); )
	{
		if (liveTrackIds.count(it->first))
		{
			++it;
			continue;
		}

		if (currentPlaybackTrackId == (int)it->first)
			StopCurrentPlayback();

		if (crossfadeOutgoingTrackId == (int)it->first)
			StopOutgoingCrossfade();

		if (it->second.music)
			delete it->second.music;
		if (it->second.fileHandle)
			delete it->second.fileHandle;

		it = playbackByTrackId.erase(it);
	}

	if (transitionPending)
	{
		if (controller.FindTrackById(transitionPendingTrackId) == NULL ||
			controller.FindTrackById((u32)transitionOutgoingTrackId) == NULL)
		{
			transitionPending = false;
		}
	}
}

void CPlaylistPlaybackManager::ClearAllPlayback()
{
	StopOutgoingCrossfade();
	StopCurrentPlayback();

	for (auto &it : playbackByTrackId)
	{
		if (it.second.music)
			delete it.second.music;
		if (it.second.fileHandle)
			delete it.second.fileHandle;
	}
	playbackByTrackId.clear();

	transitionPending = false;
}

std::string CPlaylistPlaybackManager::RetryTrackLoad(u32 trackId, const CSlrMusicPlaylistController &controller)
{
	std::string trackPath;

	const CSlrMusicPlaylistTrack *track = controller.FindTrackById(trackId);
	if (track)
		trackPath = track->path;

	auto it = playbackByTrackId.find(trackId);
	if (it != playbackByTrackId.end())
	{
		if (currentPlaybackTrackId == (int)trackId)
			StopCurrentPlayback();

		if (it->second.music)
			delete it->second.music;
		if (it->second.fileHandle)
			delete it->second.fileHandle;

		playbackByTrackId.erase(it);
	}

	return trackPath;
}

bool CPlaylistPlaybackManager::IsTrackLoadFailed(u32 trackId) const
{
	auto it = playbackByTrackId.find(trackId);
	if (it == playbackByTrackId.end())
		return false;

	return it->second.attemptedLoad && !it->second.loadSucceeded;
}

bool CPlaylistPlaybackManager::RequestCueMatchedNextTrack(CSlrMusicPlaylistController &controller)
{
	if (!controller.IsPlaying())
		return false;

	int64_t currentSample = 0;
	if (currentPlaybackMusic)
		currentSample = (int64_t)currentPlaybackMusic->GetCurrentSampleNum();

	int64_t startDelaySamples = 0;
	int64_t incomingStartSample = 0;
	if (!controller.StepToNextTrackWithCuePlan(currentSample, &startDelaySamples, &incomingStartSample))
		return false;

	const u32 nextTrackId = controller.GetCurrentTrackId();
	const CSlrMusicPlaylistTrack *nextTrack = controller.FindTrackById(nextTrackId);
	if (nextTrack == NULL)
		return false;

	if (startDelaySamples > 0 && currentPlaybackMusic != NULL)
	{
		transitionPending = true;
		transitionPendingTrackId = nextTrackId;
		transitionPendingIncomingStartSample = incomingStartSample;
		transitionOutgoingTrackId = currentPlaybackTrackId;

		const CSlrMusicPlaylistTrack *outgoingTrack = controller.FindTrackById((u32)currentPlaybackTrackId);
		transitionOutgoingCueOutSample = outgoingTrack ? GetTrackEffectiveCueOut(*outgoingTrack) : -1;
		if (transitionOutgoingCueOutSample < 0)
			transitionOutgoingCueOutSample = currentSample + startDelaySamples;
		return true;
	}

	transitionPending = false;
	StartTrackPlayback(*nextTrack, incomingStartSample);
	return true;
}

bool CPlaylistPlaybackManager::RequestImmediateNextTrack(CSlrMusicPlaylistController &controller)
{
	if (!controller.IsPlaying())
		return false;

	int64_t currentSample = 0;
	if (currentPlaybackMusic)
		currentSample = (int64_t)currentPlaybackMusic->GetCurrentSampleNum();

	int64_t startDelaySamples = 0;
	int64_t incomingStartSample = 0;
	if (!controller.StepToNextTrackWithCuePlan(currentSample, &startDelaySamples, &incomingStartSample))
		return false;

	const u32 nextTrackId = controller.GetCurrentTrackId();
	const CSlrMusicPlaylistTrack *nextTrack = controller.FindTrackById(nextTrackId);
	if (nextTrack == NULL)
		return false;

	transitionPending = false;
	StartTrackPlayback(*nextTrack, incomingStartSample);
	return true;
}

bool CPlaylistPlaybackManager::RequestCueMatchedPrevTrack(CSlrMusicPlaylistController &controller)
{
	if (!controller.IsPlaying())
		return false;

	int64_t incomingStartSample = 0;
	if (!controller.StepToPrevTrackWithCuePlan(&incomingStartSample))
		return false;

	const u32 prevTrackId = controller.GetCurrentTrackId();
	const CSlrMusicPlaylistTrack *prevTrack = controller.FindTrackById(prevTrackId);
	if (prevTrack == NULL)
		return false;

	transitionPending = false;
	StartTrackPlayback(*prevTrack, incomingStartSample);
	return true;
}

void CPlaylistPlaybackManager::StopOutgoingCrossfade()
{
	if (crossfadeOutgoingMusic)
	{
		crossfadeOutgoingMusic->Pause();
		SND_RemoveChannel(crossfadeOutgoingMusic);
		crossfadeOutgoingMusic = NULL;
	}
	crossfadeOutgoingTrackId = -1;
	outgoingPlaybackBaseVolume = 1.0f;
	crossfadeProgress = 0.0f;
}

void CPlaylistPlaybackManager::SyncPlayback(CSlrMusicPlaylistController &controller, float deltaTime)
{
	// Thread safety: runs exclusively on main UI thread.
	// Audio subsystem calls are internally mutex-protected by gSoundEngine.
	CleanupPlaybackCache(controller);

	if (!controller.IsPlaying() || controller.GetTrackCount() == 0)
	{
		StopCurrentPlayback();
		return;
	}

	// Pause freezes all transition state; resume continues from the same
	// fade/crossfade/cue-transition progress.
	if (isPaused)
		return;

	// Handle fade-in from silence (resume playback)
	if (fadeInActive && currentPlaybackMusic != NULL)
	{
		float durationSec = (float)crossfadeDurationMs / 1000.0f;
		if (durationSec <= 0.0f)
			durationSec = 0.001f;
		if (deltaTime < 0.0f)
			deltaTime = 0.0f;
		fadeInProgress += deltaTime / durationSec;
		if (fadeInProgress >= 1.0f)
		{
			fadeInProgress = 1.0f;
			fadeInActive = false;
			SetCurrentPlaybackBaseVolume(1.0f);
		}
		else
		{
			float t = fadeInProgress;
			if (crossfadeSplineEnabled)
				t = CubicBezierEase(t, splineCp1x, splineCp1y, splineCp2x, splineCp2y);

			SetCurrentPlaybackBaseVolume(sinf((float)(t * M_PI * 0.5)));
		}
	}

	// Handle outgoing track (crossfade ramp or overlap cleanup)
	if (crossfadeOutgoingMusic != NULL)
	{
		if (overlapEnabled)
		{
			// Overlap mode: both at full volume, remove outgoing when it stops playing
			if (!crossfadeOutgoingMusic->IsPlaying())
				StopOutgoingCrossfade();
		}
		else if (crossfadeProgress < 1.0f)
		{
			// Crossfade mode: equal-power ramp with optional Bezier shaping
			float durationSec = (float)crossfadeDurationMs / 1000.0f;
			if (durationSec <= 0.0f)
				durationSec = 0.001f;
			if (deltaTime < 0.0f)
				deltaTime = 0.0f;
			crossfadeProgress += deltaTime / durationSec;
			if (crossfadeProgress >= 1.0f)
			{
				crossfadeProgress = 1.0f;
				StopOutgoingCrossfade();
				if (currentPlaybackMusic)
					SetCurrentPlaybackBaseVolume(1.0f);
			}
			else
			{
				// Apply optional Bezier easing to shape the crossfade curve
				float t = crossfadeProgress;
				if (crossfadeSplineEnabled)
					t = CubicBezierEase(t, splineCp1x, splineCp1y, splineCp2x, splineCp2y);

				// Equal-power crossfade (sin/cos): maintains constant perceived loudness
				float inVol = sinf((float)(t * M_PI * 0.5));
				float outVol = cosf((float)(t * M_PI * 0.5));
				SetOutgoingPlaybackBaseVolume(outVol);
				if (currentPlaybackMusic)
					SetCurrentPlaybackBaseVolume(inVol);
			}
		}
	}

	if (transitionPending)
	{
		if (currentPlaybackMusic == NULL || currentPlaybackTrackId != transitionOutgoingTrackId)
		{
			transitionPending = false;
		}
		else
		{
			const int64_t currentSample = (int64_t)currentPlaybackMusic->GetCurrentSampleNum();
			if (!currentPlaybackMusic->IsPlaying() ||
				(transitionOutgoingCueOutSample >= 0 && currentSample >= transitionOutgoingCueOutSample))
			{
				const CSlrMusicPlaylistTrack *nextTrack = controller.FindTrackById(transitionPendingTrackId);
				if (nextTrack)
					StartTrackPlayback(*nextTrack, transitionPendingIncomingStartSample);
				transitionPending = false;
			}
			else
			{
				return;
			}
		}
	}

	const int currentIdx = controller.GetCurrentTrackIndex();
	const std::vector<CSlrMusicPlaylistTrack> &tracks = controller.GetTracks();
	if (currentIdx < 0 || currentIdx >= (int)tracks.size())
	{
		StopCurrentPlayback();
		return;
	}

	const CSlrMusicPlaylistTrack &track = tracks[currentIdx];
	if (currentPlaybackTrackId != (int)track.id)
	{
		StartTrackPlayback(track, std::max((int64_t)0, track.cueInSample));
		return;
	}

	if (currentPlaybackMusic == NULL)
		return;

	if (!currentPlaybackMusic->IsPlaying())
	{
		if (!RequestCueMatchedNextTrack(controller))
			StopCurrentPlayback();
		return;
	}

	const int64_t cueOut = GetTrackEffectiveCueOut(track);
	if (cueOut >= 0)
	{
		const int64_t currentSample = (int64_t)currentPlaybackMusic->GetCurrentSampleNum();
		if (currentSample >= cueOut)
			RequestCueMatchedNextTrack(controller);
	}

	// Periodic playback state save
	if (!playbackStateFilePath.empty() && currentPlaybackMusic != NULL && !isPaused)
	{
		periodicSaveTimer += deltaTime;
		if (periodicSaveTimer >= 30.0f)
		{
			SavePlaybackState(controller);
			periodicSaveTimer = 0.0f;
		}
	}
}
