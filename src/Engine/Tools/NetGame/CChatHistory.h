#pragma once

#include "SYS_Threading.h"
#include "json.hpp"
#include <string>
#include <vector>
#include <fstream>
#include <ctime>

using namespace std;
using namespace nlohmann;

struct ChatEntry
{
	string player;
	string message;
	int64_t timestamp;
};

class CChatHistory
{
public:
	CChatHistory(const string &filePath);
	~CChatHistory();

	void Append(const string &player, const string &message);
	vector<ChatEntry> GetRecent(int count);
	vector<ChatEntry> GetRange(int startIndex, int count);
	int GetTotalCount();
	size_t GetFileSize();
	const std::string &GetFilePath() const { return filePath; }
	void LoadFromFile();

	static json EntryToJson(const ChatEntry &e);
	static ChatEntry JsonToEntry(const json &j);

private:
	vector<ChatEntry> entries;
	string filePath;
	ofstream appendStream;
	CSlrMutex *mutex;
};
