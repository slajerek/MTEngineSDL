#include "CViewLlamaTaskQueue.h"
#include "Sci/Llama/CLlamaTaskQueue.h"
#include "imgui.h"

#include <algorithm>
#include <string>

using namespace ImGui;

bool CViewLlamaTaskQueue::sOpen = false;

// Status color helpers
static ImVec4 StatusColor(CLlamaTask::ETaskStatus status)
{
	switch (status)
	{
		case CLlamaTask::ETaskStatus::Queued:     return ImVec4(0.6f, 0.6f, 0.6f, 1.0f); // gray
		case CLlamaTask::ETaskStatus::InProgress: return ImVec4(1.0f, 0.85f, 0.0f, 1.0f); // yellow
		case CLlamaTask::ETaskStatus::Completed:  return ImVec4(0.2f, 0.9f, 0.2f, 1.0f);  // green
		case CLlamaTask::ETaskStatus::Stopped:    return ImVec4(0.9f, 0.5f, 0.1f, 1.0f);  // orange
		case CLlamaTask::ETaskStatus::Error:      return ImVec4(0.9f, 0.2f, 0.2f, 1.0f);  // red
	}
	return ImVec4(1, 1, 1, 1);
}

static const char *StatusLabel(CLlamaTask::ETaskStatus status)
{
	switch (status)
	{
		case CLlamaTask::ETaskStatus::Queued:     return "Queued";
		case CLlamaTask::ETaskStatus::InProgress: return "Running";
		case CLlamaTask::ETaskStatus::Completed:  return "Done";
		case CLlamaTask::ETaskStatus::Stopped:    return "Stopped";
		case CLlamaTask::ETaskStatus::Error:      return "Error";
	}
	return "?";
}

// Render the live preview floating window for a given task near a UI element.
static void RenderLivePreview(CLlamaTask *task, const char *windowId)
{
	ImVec2 itemMin = GetItemRectMin();
	ImVec2 itemMax = GetItemRectMax();
	ImVec2 winSize = ImVec2(440.0f, 280.0f);
	ImVec2 display = GetIO().DisplaySize;

	// Place above the item; if no room, place below
	float posX = std::max(0.0f, std::min(itemMin.x, display.x - winSize.x - 4.0f));
	float posY = itemMin.y - winSize.y - 4.0f;
	if (posY < 0.0f)
	{
		posY = itemMax.y + 4.0f;
		if (posY + winSize.y > display.y)
			posY = display.y - winSize.y - 4.0f;
	}

	SetNextWindowPos(ImVec2(posX, posY), ImGuiCond_Always);
	SetNextWindowSize(winSize, ImGuiCond_Always);
	SetNextWindowBgAlpha(0.93f);

	BeginTooltip();
	TextDisabled("%s", task->GetLiveStepLabel().c_str());
	Separator();
	BeginChild("##live_text", ImVec2(0, 0), false,
	           ImGuiWindowFlags_HorizontalScrollbar);
	std::string liveText = task->GetLiveBuffer();
	// Split on \x01 delimiter: text before = thinking (white), after = answer (light yellow).
	auto sep = liveText.find('\x01');
	if (sep == std::string::npos)
	{
		// No answer yet — all thinking
		TextWrapped("%s", liveText.c_str());
	}
	else
	{
		std::string thinking = liveText.substr(0, sep);
		std::string answer   = liveText.substr(sep + 1);
		if (!thinking.empty())
			TextWrapped("%s", thinking.c_str());
		if (!answer.empty())
		{
			if (!thinking.empty())
				Spacing();
			TextColored(ImVec4(1.0f, 1.0f, 0.6f, 1.0f), "%s", answer.c_str());
		}
	}
	SetScrollHereY(1.0f);
	EndChild();
	EndTooltip();
}

void CViewLlamaTaskQueue::Tick()
{
	CLlamaTaskQueue::Instance()->Tick();
}

void CViewLlamaTaskQueue::Render()
{
	if (!sOpen)
		return;

	CLlamaTaskQueue *queue = CLlamaTaskQueue::Instance();

	SetNextWindowSize(ImVec2(480, 320), ImGuiCond_FirstUseEver);
	if (!Begin("AI Tasks", &sOpen))
	{
		End();
		return;
	}

	const auto &tasks = queue->GetTasks();

	if (tasks.empty())
	{
		TextDisabled("No tasks in queue.");
		End();
		return;
	}

	// Clear All button
	if (SmallButton("Clear All"))
		queue->ClearAll();

	Separator();

	int removeIdx   = -1;
	int restartIdx  = -1;

	for (int i = 0; i < (int)tasks.size(); i++)
	{
		auto &task   = tasks[i];
		auto status  = task->GetTaskStatus();

		PushID(i);

		// Status label (colored)
		TextColored(StatusColor(status), "%-8s", StatusLabel(status));
		SameLine();

		// Description
		Text("%s", task->GetDescription().c_str());

		// Hover on in-progress tasks shows live preview
		if (status == CLlamaTask::ETaskStatus::InProgress && IsItemHovered())
		{
			char previewId[64];
			snprintf(previewId, sizeof(previewId), "##task_preview_%d", i);
			RenderLivePreview(task.get(), previewId);
		}

		// [X] is always anchored at the right edge.
		// [R] (for done tasks) sits immediately to the left of [X].
		const float xBtnW = CalcTextSize("[X]").x + GetStyle().FramePadding.x * 2.0f;
		const float rBtnW = CalcTextSize("[R]").x + GetStyle().FramePadding.x * 2.0f;
		const float spacing = GetStyle().ItemSpacing.x;
		const float xPos = GetWindowWidth() - xBtnW - GetStyle().ScrollbarSize;

		bool hasDoneButtons = (status == CLlamaTask::ETaskStatus::Completed ||
		                       status == CLlamaTask::ETaskStatus::Stopped ||
		                       status == CLlamaTask::ETaskStatus::Error);

		if (hasDoneButtons)
		{
			SameLine(xPos - rBtnW - spacing);
			if (SmallButton("[R]"))
				restartIdx = i;
			if (IsItemHovered())
				SetTooltip("Restart this task");
		}

		SameLine(xPos);
		if (SmallButton("[X]"))
			removeIdx = i;
		if (IsItemHovered())
		{
			if (status == CLlamaTask::ETaskStatus::InProgress)
				SetTooltip("Stop and remove");
			else
				SetTooltip("Remove");
		}

		PopID();
	}

	// Apply deferred operations (avoid modifying vector during iteration)
	if (restartIdx >= 0)
		queue->RestartTask(restartIdx);
	if (removeIdx >= 0)
		queue->RemoveTask(removeIdx);

	End();
}
