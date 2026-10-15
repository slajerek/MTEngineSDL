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

	// The action the next PerformUndo() would undo, or nullptr. Lets a caller
	// that groups bursts of edits (a text editor coalescing typed characters)
	// extend its own last action in place instead of pushing one per key.
	CUndoAction *PeekUndo() const;

private:
	std::vector<std::unique_ptr<CUndoAction>> history;
	int currentIndex = -1;
};
