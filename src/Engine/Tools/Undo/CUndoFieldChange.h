#pragma once

#include "CUndoAction.h"
#include <utility>

template<typename T>
class CUndoFieldChange : public CUndoAction
{
public:
	CUndoFieldChange(T* field, T oldVal, T newVal)
		: fieldPtr(field), oldValue(std::move(oldVal)), newValue(std::move(newVal)) {}

	void Undo() override { *fieldPtr = oldValue; }
	void Redo() override { *fieldPtr = newValue; }

private:
	T* fieldPtr;
	T oldValue;
	T newValue;
};
