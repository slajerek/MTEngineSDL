#pragma once

#include "CUndoAction.h"
#include <map>

template<typename K, typename V>
class CUndoMapInsert : public CUndoAction
{
public:
	CUndoMapInsert(std::map<K, V>* mapPtr, K key, V value)
		: mapPtr(mapPtr), key(std::move(key)), value(std::move(value)) {}

	void Undo() override { mapPtr->erase(key); }
	void Redo() override { (*mapPtr)[key] = value; }

private:
	std::map<K, V>* mapPtr;
	K key;
	V value;
};

template<typename K, typename V>
class CUndoMapErase : public CUndoAction
{
public:
	CUndoMapErase(std::map<K, V>* mapPtr, K key, V value)
		: mapPtr(mapPtr), key(std::move(key)), value(std::move(value)) {}

	void Undo() override { (*mapPtr)[key] = value; }
	void Redo() override { mapPtr->erase(key); }

private:
	std::map<K, V>* mapPtr;
	K key;
	V value;
};
