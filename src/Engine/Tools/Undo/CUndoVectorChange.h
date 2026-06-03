#pragma once

#include "CUndoAction.h"
#include <vector>

template<typename T>
class CUndoVectorInsert : public CUndoAction
{
public:
	CUndoVectorInsert(std::vector<T>* vec, T element, int index)
		: vec(vec), element(std::move(element)), index(index) {}

	void Undo() override { vec->erase(vec->begin() + index); }
	void Redo() override { vec->insert(vec->begin() + index, element); }

private:
	std::vector<T>* vec;
	T element;
	int index;
};

template<typename T>
class CUndoVectorErase : public CUndoAction
{
public:
	CUndoVectorErase(std::vector<T>* vec, T element, int index)
		: vec(vec), element(std::move(element)), index(index) {}

	void Undo() override { vec->insert(vec->begin() + index, element); }
	void Redo() override { vec->erase(vec->begin() + index); }

private:
	std::vector<T>* vec;
	T element;
	int index;
};
