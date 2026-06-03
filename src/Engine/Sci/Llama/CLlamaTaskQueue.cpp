#include "CLlamaTaskQueue.h"
#include "DBG_Log.h"

#include <mutex>

CLlamaTaskQueue *CLlamaTaskQueue::Instance()
{
	static CLlamaTaskQueue sInstance;
	return &sInstance;
}

void CLlamaTaskQueue::AddTask(std::unique_ptr<CLlamaTask> task)
{
	task->onFinished = [this]() {
		// Will be picked up by Tick() on the next frame.
	};
	std::lock_guard<std::mutex> lock(tasksMutex);
	tasks.push_back(std::move(task));
	TryStartNextLocked();
}

void CLlamaTaskQueue::RemoveTask(int index)
{
	std::lock_guard<std::mutex> lock(tasksMutex);
	if (index < 0 || index >= (int)tasks.size())
		return;

	auto &task = tasks[index];
	if (task->GetTaskStatus() == CLlamaTask::ETaskStatus::InProgress)
		task->Cancel();

	tasks.erase(tasks.begin() + index);

	// If we removed the active task, start the next one.
	TryStartNextLocked();
}

void CLlamaTaskQueue::RestartTask(int index)
{
	std::lock_guard<std::mutex> lock(tasksMutex);
	if (index < 0 || index >= (int)tasks.size())
		return;

	auto &task = tasks[index];
	auto status = task->GetTaskStatus();
	if (status != CLlamaTask::ETaskStatus::Completed &&
	    status != CLlamaTask::ETaskStatus::Stopped &&
	    status != CLlamaTask::ETaskStatus::Error)
		return;

	// Move task to the end of the queue and reset its state.
	auto moved = std::move(tasks[index]);
	tasks.erase(tasks.begin() + index);

	moved->Restart();
	moved->onFinished = [this]() {};
	tasks.push_back(std::move(moved));
	TryStartNextLocked();
}

void CLlamaTaskQueue::ClearAll()
{
	std::lock_guard<std::mutex> lock(tasksMutex);
	for (auto &task : tasks)
	{
		if (task->GetTaskStatus() == CLlamaTask::ETaskStatus::InProgress)
			task->Cancel();
	}
	tasks.clear();
}

void CLlamaTaskQueue::Tick()
{
	// Snapshot the active task pointer under the lock, then call Tick()
	// outside the lock — task->Tick() may call SendPrompt() which must not
	// run while the queue lock is held (avoids potential re-entrancy issues).
	CLlamaTask *active = nullptr;
	{
		std::lock_guard<std::mutex> lock(tasksMutex);
		for (auto &task : tasks)
		{
			if (task->GetTaskStatus() == CLlamaTask::ETaskStatus::InProgress)
			{
				active = task.get();
				break;
			}
		}
	}

	if (active)
	{
		// If the active task's prerequisite (e.g. model) disappeared while
		// it was running, cancel and requeue it so it retries when ready.
		if (!active->CanExecute())
		{
			active->Cancel();
			active->Restart(); // resets status → Queued
		}
		else
		{
			active->Tick();

			// Check if the active task finished during Tick.
			auto status = active->GetTaskStatus();
			if (status == CLlamaTask::ETaskStatus::Completed ||
			    status == CLlamaTask::ETaskStatus::Stopped ||
			    status == CLlamaTask::ETaskStatus::Error)
			{
				std::lock_guard<std::mutex> lock(tasksMutex);
				TryStartNextLocked();
				return;
			}
		}
	}

	// Always attempt to start the next queued task — this covers the case
	// where no task is active yet (e.g. model just became available).
	{
		std::lock_guard<std::mutex> lock(tasksMutex);
		TryStartNextLocked();
	}
}

CLlamaTask *CLlamaTaskQueue::FindTaskForTarget(void *target) const
{
	if (!target)
		return nullptr;
	std::lock_guard<std::mutex> lock(tasksMutex);
	// Prefer active/queued tasks — skip completed/stopped/error entries so
	// a finished task does not shadow a newly queued one for the same target.
	for (auto &task : tasks)
	{
		if (task->GetTargetPtr() != target)
			continue;
		auto s = task->GetTaskStatus();
		if (s == CLlamaTask::ETaskStatus::Queued ||
		    s == CLlamaTask::ETaskStatus::InProgress)
			return task.get();
	}
	return nullptr;
}

bool CLlamaTaskQueue::RemoveTaskForTarget(void *target)
{
	if (!target)
		return false;
	std::lock_guard<std::mutex> lock(tasksMutex);
	for (int i = 0; i < (int)tasks.size(); i++)
	{
		if (tasks[i]->GetTargetPtr() == target)
		{
			// Can't call RemoveTask (would deadlock). Inline the logic.
			auto &task = tasks[i];
			if (task->GetTaskStatus() == CLlamaTask::ETaskStatus::InProgress)
				task->Cancel();
			tasks.erase(tasks.begin() + i);
			TryStartNextLocked();
			return true;
		}
	}
	return false;
}

CLlamaTask *CLlamaTaskQueue::GetActiveTask() const
{
	std::lock_guard<std::mutex> lock(tasksMutex);
	for (auto &task : tasks)
	{
		if (task->GetTaskStatus() == CLlamaTask::ETaskStatus::InProgress)
			return task.get();
	}
	return nullptr;
}

void CLlamaTaskQueue::TryStartNextLocked()
{
	// Caller must hold tasksMutex.
	for (auto &task : tasks)
	{
		if (task->GetTaskStatus() == CLlamaTask::ETaskStatus::InProgress)
			return; // something already running

		if (task->GetTaskStatus() == CLlamaTask::ETaskStatus::Queued)
		{
			if (!task->CanExecute())
				return; // not ready yet — wait for next Tick
			LOGD("CLlamaTaskQueue: starting task '%s'", task->GetDescription().c_str());
			task->Execute();
			return;
		}
	}
}
