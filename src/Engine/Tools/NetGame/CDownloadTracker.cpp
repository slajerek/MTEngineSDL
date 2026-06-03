#include "CDownloadTracker.h"

CDownloadTracker::CDownloadTracker()
	: nextId(1)
{
}

int CDownloadTracker::Register(const std::string &label)
{
	std::lock_guard<std::mutex> lock(mutex);
	SDownloadEntry entry;
	entry.id = nextId++;
	entry.label = label;
	entry.active = true;
	entry.progress.isSyncing = true;
	entries.push_back(entry);
	return entry.id;
}

void CDownloadTracker::Update(int id, const SDownloadProgress &progress)
{
	std::lock_guard<std::mutex> lock(mutex);
	SDownloadEntry *e = FindEntry(id);
	if (e)
		e->progress = progress;
}

void CDownloadTracker::Complete(int id)
{
	std::lock_guard<std::mutex> lock(mutex);
	SDownloadEntry *e = FindEntry(id);
	if (e)
	{
		e->progress.isComplete = true;
		e->progress.isSyncing = false;
		e->active = false;
	}
}

void CDownloadTracker::Fail(int id, const std::string &error)
{
	std::lock_guard<std::mutex> lock(mutex);
	SDownloadEntry *e = FindEntry(id);
	if (e)
	{
		e->progress.hasError = true;
		e->progress.errorMessage = error;
		e->progress.isSyncing = false;
		e->active = false;
	}
}

std::vector<SDownloadEntry> CDownloadTracker::GetActive() const
{
	std::lock_guard<std::mutex> lock(mutex);
	std::vector<SDownloadEntry> result;
	for (const auto &e : entries)
		if (e.active || e.progress.hasError)
			result.push_back(e);
	return result;
}

bool CDownloadTracker::HasAnyActive() const
{
	std::lock_guard<std::mutex> lock(mutex);
	for (const auto &e : entries)
		if (e.active)
			return true;
	return false;
}

SDownloadEntry *CDownloadTracker::FindEntry(int id)
{
	for (auto &e : entries)
		if (e.id == id)
			return &e;
	return nullptr;
}
