#include "CChatHistory.h"
#include "DBG_Log.h"
#include <sstream>
#include <algorithm>
#include <filesystem>

CChatHistory::CChatHistory(const string &filePath)
{
	this->filePath = filePath;
	mutex = new CSlrMutex("CChatHistoryMutex");
}

CChatHistory::~CChatHistory()
{
	if (appendStream.is_open())
		appendStream.close();
	delete mutex;
}

void CChatHistory::LoadFromFile()
{
	mutex->Lock();

	entries.clear();

	ifstream inFile(filePath);
	if (!inFile.is_open())
	{
		LOGD("CChatHistory::LoadFromFile: no file at %s, starting fresh", filePath.c_str());
		mutex->Unlock();
		return;
	}

	string line;
	while (getline(inFile, line))
	{
		if (line.empty()) continue;

		try
		{
			json j = json::parse(line);
			entries.push_back(JsonToEntry(j));
		}
		catch (const exception &e)
		{
			LOGWarning("CChatHistory::LoadFromFile: skipping corrupt line: %s", e.what());
		}
	}

	inFile.close();

	LOGM("CChatHistory::LoadFromFile: loaded %d entries from %s", (int)entries.size(), filePath.c_str());

	// Open append stream for future writes
	appendStream.open(filePath, ios::app);
	if (!appendStream.is_open())
	{
		LOGError("CChatHistory::LoadFromFile: failed to open append stream for %s", filePath.c_str());
	}

	mutex->Unlock();
}

void CChatHistory::Append(const string &player, const string &message)
{
	mutex->Lock();

	ChatEntry entry;
	entry.player = player;
	entry.message = message;
	entry.timestamp = (int64_t)time(NULL);

	entries.push_back(entry);

	// Write to file
	if (appendStream.is_open())
	{
		appendStream << EntryToJson(entry).dump() << "\n";
		appendStream.flush();
	}
	else
	{
		// Try to open if not yet open
		appendStream.open(filePath, ios::app);
		if (appendStream.is_open())
		{
			appendStream << EntryToJson(entry).dump() << "\n";
			appendStream.flush();
		}
		else
		{
			LOGError("CChatHistory::Append: failed to write to %s", filePath.c_str());
		}
	}

	mutex->Unlock();
}

vector<ChatEntry> CChatHistory::GetRecent(int count)
{
	mutex->Lock();
	if (count <= 0)
	{
		mutex->Unlock();
		return {};
	}

	int total = (int)entries.size();
	int start = max(0, total - count);
	vector<ChatEntry> result(entries.begin() + start, entries.end());

	mutex->Unlock();
	return result;
}

vector<ChatEntry> CChatHistory::GetRange(int startIndex, int count)
{
	mutex->Lock();
	if (count <= 0)
	{
		mutex->Unlock();
		return {};
	}

	int total = (int)entries.size();
	if (startIndex < 0) startIndex = 0;
	if (startIndex >= total)
	{
		mutex->Unlock();
		return {};
	}

	int64_t end64 = (int64_t)startIndex + (int64_t)count;
	int endIndex = (end64 > (int64_t)total) ? total : (int)end64;
	vector<ChatEntry> result(entries.begin() + startIndex, entries.begin() + endIndex);

	mutex->Unlock();
	return result;
}

int CChatHistory::GetTotalCount()
{
	mutex->Lock();
	int count = (int)entries.size();
	mutex->Unlock();
	return count;
}

size_t CChatHistory::GetFileSize()
{
	try {
		if (std::filesystem::exists(filePath))
			return (size_t)std::filesystem::file_size(filePath);
	} catch (...) {}
	return 0;
}

json CChatHistory::EntryToJson(const ChatEntry &e)
{
	json j;
	j["p"] = e.player;
	j["m"] = e.message;
	j["t"] = e.timestamp;
	return j;
}

ChatEntry CChatHistory::JsonToEntry(const json &j)
{
	ChatEntry e;
	e.player = j.value("p", "");
	e.message = j.value("m", "");
	e.timestamp = j.value("t", (int64_t)0);
	return e;
}
