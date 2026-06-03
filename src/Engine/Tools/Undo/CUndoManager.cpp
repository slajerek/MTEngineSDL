#include "CUndoManager.h"
#include "DBG_Log.h"

CUndoManager::CUndoManager()
{
}

CUndoManager::~CUndoManager()
{
	Clear();
}

void CUndoManager::Push(std::unique_ptr<CUndoAction> action)
{
	// Truncate redo tail
	if (currentIndex + 1 < (int)history.size())
	{
		history.erase(history.begin() + currentIndex + 1, history.end());
	}

	history.push_back(std::move(action));
	currentIndex = (int)history.size() - 1;
}

void CUndoManager::PerformUndo()
{
	if (!CanUndo()) return;

	history[currentIndex]->Undo();
	currentIndex--;

	LOGD("CUndoManager::PerformUndo: index=%d size=%d", currentIndex, (int)history.size());
}

void CUndoManager::PerformRedo()
{
	if (!CanRedo()) return;

	currentIndex++;
	history[currentIndex]->Redo();

	LOGD("CUndoManager::PerformRedo: index=%d size=%d", currentIndex, (int)history.size());
}

bool CUndoManager::CanUndo() const
{
	return currentIndex >= 0;
}

bool CUndoManager::CanRedo() const
{
	return currentIndex + 1 < (int)history.size();
}

void CUndoManager::Clear()
{
	history.clear();
	currentIndex = -1;
}
