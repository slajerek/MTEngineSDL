#pragma once

#include "SYS_Defs.h"
#include "CSlrMusicPlaylistController.h"
#include "json.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <unordered_set>

class CSlrMusicFileOgg;
class CSlrFileFromDocuments;

struct SPlaybackResumeState
{
	bool wasPlaying = false;
	std::string trackPath;
	int64_t samplePosition = 0;
	u32 trackId = 0;
	bool valid = false;
	nlohmann::json shuffleState;  // shuffle round, play history, play counts
};

class CPlaylistPlaybackManager
{
public:
	CPlaylistPlaybackManager();
	~CPlaylistPlaybackManager();

	// Call every frame to advance playback state; runtime is UI-independent.
	void SyncPlayback(CSlrMusicPlaylistController &controller, float deltaTime);

	// Request cue-matched transition to next/previous track.
	// RequestCueMatchedNextTrack may defer via transitionPending to align outgoing cue-out
	// with incoming cue-in — use only for auto-advance in SyncPlayback.
	bool RequestCueMatchedNextTrack(CSlrMusicPlaylistController &controller);
	bool RequestCueMatchedPrevTrack(CSlrMusicPlaylistController &controller);

	// User-initiated immediate skip: advance controller and start playback now,
	// ignoring cue-out alignment. Use for NEXT button.
	bool RequestImmediateNextTrack(CSlrMusicPlaylistController &controller);

	// Start playback of a specific track at a given sample offset.
	void StartTrackPlayback(const CSlrMusicPlaylistTrack &track, int64_t incomingStartSample);

	// Stop current playback and remove from mixer.
	void StopCurrentPlayback();

	// Pause/resume current playback.
	void PausePlayback();
	void ResumePlayback();

	// Seek the currently playing track to a sample position.
	void SeekCurrentTrack(int64_t sampleNum);

	// Remove cached entries for tracks no longer in the playlist.
	void CleanupPlaybackCache(const CSlrMusicPlaylistController &controller);

	// Clear all cached playback entries and stop everything.
	void ClearAllPlayback();

	// Retry loading a failed track. Returns the track path so caller can also clear waveform cache.
	std::string RetryTrackLoad(u32 trackId, const CSlrMusicPlaylistController &controller);

	// Get effective cue-out sample for a track (explicit or track length).
	int64_t GetTrackEffectiveCueOut(const CSlrMusicPlaylistTrack &track) const;

	// Check if a track's load was attempted and failed.
	bool IsTrackLoadFailed(u32 trackId) const;

	// Query current state
	int GetCurrentPlaybackTrackId() const { return currentPlaybackTrackId; }
	CSlrMusicFileOgg *GetCurrentPlaybackMusic() const { return currentPlaybackMusic; }
	CSlrMusicFileOgg *GetCrossfadeOutgoingMusic() const { return crossfadeOutgoingMusic; }
	bool IsTransitionPending() const { return transitionPending; }
	bool IsPaused() const { return isPaused; }
	int64_t GetCurrentSampleNum() const;
	void SetMasterVolume(float volume);
	float GetMasterVolume() const { return masterVolume; }

	// Start track with fade-in from silence (for resume playback)
	void StartTrackPlaybackWithFadeIn(const CSlrMusicPlaylistTrack &track, int64_t startSample);
	bool IsFadeInActive() const { return fadeInActive; }

	// Playback state persistence
	void SetPlaybackStateFilePath(const std::string &path);
	void SavePlaybackState(const CSlrMusicPlaylistController &controller);
	SPlaybackResumeState LoadPlaybackState();

	// Crossfade configuration
	void SetCrossfadeEnabled(bool enabled) { crossfadeEnabled = enabled; }
	bool GetCrossfadeEnabled() const { return crossfadeEnabled; }
	void SetCrossfadeDurationMs(int ms) { crossfadeDurationMs = ms; }
	int GetCrossfadeDurationMs() const { return crossfadeDurationMs; }

	// Overlap mode: both tracks play at full volume, outgoing plays to its end
	void SetOverlapEnabled(bool enabled) { overlapEnabled = enabled; }
	bool GetOverlapEnabled() const { return overlapEnabled; }

	// Spline easing for crossfade (Bezier curve shaping applied before equal-power sin/cos)
	void SetCrossfadeSplineEnabled(bool enabled) { crossfadeSplineEnabled = enabled; }
	bool GetCrossfadeSplineEnabled() const { return crossfadeSplineEnabled; }
	void SetCrossfadeSplineParams(float cp1x, float cp1y, float cp2x, float cp2y);
	void GetCrossfadeSplineParams(float &cp1x, float &cp1y, float &cp2x, float &cp2y) const;

	// Cubic Bezier easing: maps t [0,1] to eased value [0,1] via control points
	static float CubicBezierEase(float t, float cp1x, float cp1y, float cp2x, float cp2y);

private:
	struct SPlaybackEntry
	{
		CSlrMusicFileOgg *music = NULL;
		CSlrFileFromDocuments *fileHandle = NULL;
		bool attemptedLoad = false;
		bool loadSucceeded = false;
	};

	bool EnsureTrackPlaybackLoaded(const CSlrMusicPlaylistTrack &track);
	bool IsOggPath(const std::string &trackPath) const;
	void StopOutgoingCrossfade();
	float ClampVolume(float volume) const;
	void SetCurrentPlaybackBaseVolume(float volume);
	void SetOutgoingPlaybackBaseVolume(float volume);

	std::map<u32, SPlaybackEntry> playbackByTrackId;
	int currentPlaybackTrackId = -1;
	CSlrMusicFileOgg *currentPlaybackMusic = NULL;
	float masterVolume = 1.0f;
	float currentPlaybackBaseVolume = 1.0f;
	float outgoingPlaybackBaseVolume = 1.0f;

	// Transition state
	bool transitionPending = false;
	u32 transitionPendingTrackId = 0;
	int64_t transitionPendingIncomingStartSample = 0;
	int transitionOutgoingTrackId = -1;
	int64_t transitionOutgoingCueOutSample = -1;

	bool isPaused = false;
	bool overlapEnabled = false;
	bool crossfadeSplineEnabled = false;
	float splineCp1x = 0.42f, splineCp1y = 0.0f;
	float splineCp2x = 0.58f, splineCp2y = 1.0f;

	// Crossfade state
	bool crossfadeEnabled = false;
	int crossfadeDurationMs = 2000;
	CSlrMusicFileOgg *crossfadeOutgoingMusic = NULL;
	int crossfadeOutgoingTrackId = -1;
	float crossfadeProgress = 0.0f;

	// Fade-in from silence (resume playback)
	bool fadeInActive = false;
	float fadeInProgress = 0.0f;

	// Periodic playback state save
	float periodicSaveTimer = 0.0f;
	std::string playbackStateFilePath;
};
