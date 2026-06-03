#pragma once

#include "CLlamaTask.h"

#include <memory>
#include <mutex>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// CLlamaTaskQueue
//
// FIFO task scheduler — executes one CLlamaTask at a time.  Tasks are added
// with AddTask() and run in order.
//
// Thread safety: all public methods are guarded by an internal mutex.
// Tick() may be called from any thread that is NOT the LLM done-callback
// thread (calling SendPrompt re-entrantly from within GenerateAsync crashes).
// In practice, call Tick() from the main thread every frame.
//
// Singleton — access via Instance().
// ─────────────────────────────────────────────────────────────────────────────
class CLlamaTaskQueue {
public:
	static CLlamaTaskQueue *Instance();

	// Append a new task.  Starts immediately if no other task is active.
	void AddTask(std::unique_ptr<CLlamaTask> task);

	// Remove task at index.  If it is currently in-progress, Cancel() is
	// called first.
	void RemoveTask(int index);

	// Re-run a completed/stopped task.  Resets its status to Queued and
	// moves it to the end of the queue.
	void RestartTask(int index);

	// Stop active task and remove everything.
	void ClearAll();

	// Advance in-progress task and start the next queued task if one finished.
	// Safe to call every frame; must NOT be called from within the LLM done-callback.
	void Tick();

	// Find a task whose GetTargetPtr() matches the given pointer.
	// Returns nullptr if none found.
	CLlamaTask *FindTaskForTarget(void *target) const;

	// Remove a task by its target pointer. If it's in-progress, Cancel() is called first.
	// Returns true if a task was found and removed.
	bool RemoveTaskForTarget(void *target);

	// Currently executing task (status == InProgress), or nullptr.
	CLlamaTask *GetActiveTask() const;

	const std::vector<std::unique_ptr<CLlamaTask>> &GetTasks() const { return tasks; }

private:
	CLlamaTaskQueue() = default;

	mutable std::mutex tasksMutex;
	std::vector<std::unique_ptr<CLlamaTask>> tasks;

	// Start the next queued task if nothing is currently running.
	// Caller must hold tasksMutex.
	void TryStartNextLocked();
};
