#include "DBG_Log.h"
#include "CNetGameDataProviderLocalFiles.h"
#include "CNetGameUserProfile.h"
#include "SYS_FileSystem.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include "json.hpp"

using namespace nlohmann;
using namespace std;
using namespace std::filesystem;

CNetGameDataProviderLocalFiles::CNetGameDataProviderLocalFiles()
: CNetGameDataProvider()
{
}

CNetGameDataProviderLocalFiles::CNetGameDataProviderLocalFiles(string configPath)
: CNetGameDataProvider()
{
	Init(configPath);
}

CNetGameDataProviderLocalFiles::~CNetGameDataProviderLocalFiles()
{
}

void CNetGameDataProviderLocalFiles::InitFromHjson(Hjson::Value hjsonRoot)
{
	playersFolder = hjsonRoot["playersFolder"].to_string();
	gamesFolder = hjsonRoot["gamesFolder"].to_string();

	// Resolve relative paths against app root (gPathToResources)
	namespace fs = std::filesystem;
	if (!fs::path(playersFolder).is_absolute())
		playersFolder = string(gPathToResources) + playersFolder;
	if (!fs::path(gamesFolder).is_absolute())
		gamesFolder = string(gPathToResources) + gamesFolder;

	// Create data directories if they don't exist
	SYS_CreateFolder(playersFolder.c_str());
	SYS_CreateFolder(gamesFolder.c_str());

	LOGM("CNetGameDataProviderLocalFiles config:");
	LOGM("..playersFolder=%s", playersFolder.c_str());
	LOGM("..gamesFolder=%s", gamesFolder.c_str());
}

void CNetGameDataProviderLocalFiles::StoreToHjson(Hjson::Value hjsonRoot)
{
}

// returns player profile when authorized or NULL when failed
CNetGameUserProfile *CNetGameDataProviderLocalFiles::PlayerAuthorize(string playerName, vector<u8> hash)
{
	LOGD("CNetGameDataProviderLocalFiles::PlayerAuthorize: playerName=%s", playerName.c_str());
	CNetGameUserProfile *playerProfile = LoadPlayerProfile(playerName);
	if (playerProfile == NULL)
		return NULL;

	bool didUpgrade = false;
	if (playerProfile->CheckPassword(hash, &didUpgrade))
	{
		if (didUpgrade)
			SavePlayerProfile(playerProfile);
		return playerProfile;
	}

	delete playerProfile;
	return NULL;
}

bool CNetGameDataProviderLocalFiles::PlayerExists(string playerName)
{
	LOGD("CNetGameDataProviderLocalFiles::PlayerExists");

	path basePath = playersFolder;
	path fileName = Utf8StringToLowercase(playerName) + ".json";
	path fullPath = basePath / fileName;
	LOGD("CNetGameDataProviderLocalFiles::PlayerExists: fullPath=%s", fullPath.string().c_str());
	if (filesystem::exists(fullPath))
	{
		return true;
	}
	return false;
}

CNetGameUserProfile *CNetGameDataProviderLocalFiles::LoadPlayerProfile(string playerName)
{
	LOGD("CNetGameDataProviderLocalFiles::LoadPlayerProfile");

	path basePath = playersFolder;
	path fileName = Utf8StringToLowercase(playerName) + ".json";
	path fullPath = basePath / fileName;
	LOGD("CNetGameDataProviderLocalFiles::LoadPlayerProfile: fullPath=%s", fullPath.string().c_str());
	ifstream file(fullPath);
	if (!file.is_open())
	{
		LOGError("CNetGameDataProviderLocalFiles: file not found player=%s", playerName.c_str());
		return NULL;
	}

	json j;
	try
	{
		file >> j;
	} catch (const exception& e) {
		LOGError("CNetGameDataProviderLocalFiles: broken player file=%s error=%s", playerName.c_str(), e.what());
		return NULL;
	}

	CNetGameUserProfile *playerProfile = CreateProfileInstance();
	if (!playerProfile->Deserialize(j))
	{
		delete playerProfile;
		return NULL;
	}

	return playerProfile;
}

bool CNetGameDataProviderLocalFiles::SavePlayerProfile(CNetGameUserProfile *playerProfile)
{
	LOGD("CNetGameDataProviderLocalFiles::SavePlayerProfile");

	path basePath = playersFolder;
	path fileName = Utf8StringToLowercase(playerProfile->name) + ".json";
	path fullPath = basePath / fileName;
	LOGD("CNetGameDataProviderLocalFiles::SavePlayerProfile: fullPath=%s", fullPath.string().c_str());
	ofstream outFile(fullPath);
	if (!outFile.is_open())
	{
		LOGError("CNetGameDataProviderLocalFiles::SavePlayerProfile: Failed to open file for writing: %s", playerProfile->name.c_str());
		return false;
	}

	json j;
	playerProfile->Serialize(j);

	// Write JSON to file with pretty formatting
	outFile << j.dump(4);  // indent = 4 spaces

	return true;
}
