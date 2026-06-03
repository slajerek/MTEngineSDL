#pragma once

#include <functional>
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
// CLlamaTask
//
// Abstract base class for any LLM-driven background task that can be queued
// in CLlamaTaskQueue.  Derive from this class and implement the virtual
// methods to define concrete task types (translation, text generation, etc.).
//
// Lifecycle:
//   1. Created in ETaskStatus::Queued state.
//   2. Queue calls Execute() when the task reaches the front — status becomes
//      InProgress.
//   3. Task calls Tick() from the main thread each frame while running.
//   4. When work is done the task sets status to Completed and calls
//      onFinished (so the queue can advance).  Similarly for Cancel/Error.
// ─────────────────────────────────────────────────────────────────────────────
class CLlamaTask {
public:
	enum class ETaskStatus {
		Queued,
		InProgress,
		Completed,
		Stopped,
		Error
	};

	virtual ~CLlamaTask() = default;

	// Human-readable one-line description, e.g. "Translate Army 'Knight'".
	virtual std::string GetDescription() const = 0;

	// Start executing the task.  Called by the queue when this task reaches
	// the front.  Implementation must set status to InProgress.
	virtual void Execute() = 0;

	// Request graceful cancellation.  Implementation must set status to
	// Stopped and call onFinished.
	virtual void Cancel() = 0;

	// Main-thread per-frame pump (e.g. deferred pipeline advancement).
	virtual void Tick() = 0;

	// Live preview — streaming LLM output for display in hover tooltips.
	virtual std::string GetLiveBuffer() const { return ""; }
	virtual std::string GetLiveStepLabel() const { return ""; }

	// Opaque pointer identifying the target object (e.g. CLocalizedName*).
	// Used by the queue to find tasks associated with a particular editor item.
	virtual void *GetTargetPtr() const { return nullptr; }

	// Returns true if the task is ready to start (e.g. required resources are
	// available).  Called by the queue each frame before attempting Execute().
	// Default: always ready.  Override to gate on model availability etc.
	virtual bool CanExecute() const { return true; }

	// Reset task state so it can be re-executed.  Default resets status to
	// Queued; derived classes should override to clear internal state.
	virtual void Restart() { taskStatus = ETaskStatus::Queued; }

	ETaskStatus GetTaskStatus() const { return taskStatus; }

	// Called by the task when it finishes (completed, stopped, or error).
	// The queue wires this up to advance to the next task.
	std::function<void()> onFinished;

protected:
	void SetTaskStatus(ETaskStatus s) { taskStatus = s; }
	ETaskStatus taskStatus = ETaskStatus::Queued;
};
