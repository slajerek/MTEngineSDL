#pragma once

#include <string>

class CUndoAction
{
public:
	virtual ~CUndoAction() = default;
	virtual void Undo() = 0;
	virtual void Redo() = 0;
	virtual std::string GetDescription() { return ""; }
};
