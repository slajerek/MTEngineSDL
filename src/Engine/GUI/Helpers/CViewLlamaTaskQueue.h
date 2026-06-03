#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// CViewLlamaTaskQueue
//
// ImGui window that displays the CLlamaTaskQueue as a list of tasks with
// status, delete/restart buttons, and a live preview on hover.
//
// Call Render() every frame from the main render loop.
// Toggle visibility via sOpen.
// ─────────────────────────────────────────────────────────────────────────────
class CViewLlamaTaskQueue {
public:
	static bool sOpen;

	// Call from main render loop each frame (always, even when window is hidden).
	// Ticks the task queue so in-progress tasks advance regardless of UI visibility.
	static void Tick();

	// Call from main render loop each frame.
	static void Render();
};
