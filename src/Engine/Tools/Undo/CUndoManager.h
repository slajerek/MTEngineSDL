#pragma once

#include "CUndoAction.h"
#include <vector>
#include <memory>

class CUndoManager
{
public:
	CUndoManager();
	~CUndoManager();

	void Push(std::unique_ptr<CUndoAction> action);
	void PerformUndo();
	void PerformRedo();
	bool CanUndo() const;
	bool CanRedo() const;
	void Clear();

private:
	std::vector<std::unique_ptr<CUndoAction>> history;
	int currentIndex = -1;
};
