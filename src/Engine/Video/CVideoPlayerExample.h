// CVideoPlayerExample.h — Minimal example: play a fullscreen cutscene with callback
//
// This file shows the simplest possible way to use CVideoPlayer + CViewCutscene
// for a fullscreen video cutscene in your game. Copy and adapt to your needs.
//
// Usage (from anywhere that has access to viewMain):
//
//   #include "CVideoPlayerExample.h"
//
//   // Play intro video, then show main menu:
//   VideoPlayerExample::PlayCutscene("assets/videos/intro.webm", []() {
//       ShowMainMenu();  // your callback — runs when video ends or is skipped
//   });
//
//   // Play with custom settings:
//   VideoPlayerExample::PlayCutscene("assets/videos/boss_intro.webm", []() {
//       StartBossFight();
//   }, 1.0f, 0.3f, false);  // 1s fade in, 0.3s fade out, no skip allowed
//

#pragma once

#include <functional>

class CViewLightHeroesMain;

namespace VideoPlayerExample
{

// Play a fullscreen cutscene video.
//
// Parameters:
//   videoPath     — path to .webm file (relative to working directory or absolute)
//   onFinished    — called when video ends naturally or is skipped by user
//   fadeIn        — fade-in duration in seconds (default 0.5)
//   fadeOut       — fade-out duration in seconds (default 0.5)
//   allowSkip    — if true, any key press skips the video (default true)
//
// The video renders fullscreen with letterboxing, audio plays through the engine mixer,
// and ImGui content can be rendered on top if needed.
//
inline void PlayCutscene(const char *videoPath,
						 std::function<void()> onFinished = nullptr,
						 float fadeIn = 0.5f,
						 float fadeOut = 0.5f,
						 bool allowSkip = true);

// Stop any currently playing cutscene immediately (no fade out).
inline void StopCutscene();

} // namespace VideoPlayerExample


// ============================================================================
// IMPLEMENTATION (inline — include this header where you need it)
// ============================================================================

// These headers are needed only for the implementation:
#include "CViewLightHeroesMain.h"
#include "CViewCutscene.h"

extern CViewLightHeroesMain *viewMain;

namespace VideoPlayerExample
{

inline void PlayCutscene(const char *videoPath,
						 std::function<void()> onFinished,
						 float fadeIn,
						 float fadeOut,
						 bool allowSkip)
{
	if (!viewMain || !viewMain->viewCutscene)
		return;

	CViewCutscene *cs = viewMain->viewCutscene;

	// Configure fade and skip
	cs->fadeInDuration = fadeIn;
	cs->fadeOutDuration = fadeOut;
	cs->allowSkip = allowSkip;

	// Set completion callback
	cs->onCutsceneFinished = onFinished;

	// Show the cutscene view and start playback
	cs->SetVisible(true);
	cs->PlayCutscene(videoPath);
}

inline void StopCutscene()
{
	if (!viewMain || !viewMain->viewCutscene)
		return;

	viewMain->viewCutscene->StopCutscene();
	viewMain->viewCutscene->SetVisible(false);
}

} // namespace VideoPlayerExample
