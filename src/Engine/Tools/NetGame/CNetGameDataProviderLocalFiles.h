#pragma once

#include "CNetGameDataProvider.h"

// File-based implementation of player data persistence.
// Stores player profiles as JSON files in a configurable folder.
// Game-specific subclasses can override CreateProfileInstance()
// to deserialize into their own profile subclass.
class CNetGameDataProviderLocalFiles : public CNetGameDataProvider
{
public:
	CNetGameDataProviderLocalFiles();
	CNetGameDataProviderLocalFiles(string configPath);
	virtual ~CNetGameDataProviderLocalFiles();

	virtual void InitFromHjson(Hjson::Value hjsonRoot);
	virtual void StoreToHjson(Hjson::Value hjsonRoot);

	string playersFolder;
	string gamesFolder;

	virtual bool PlayerExists(string playerName);
	virtual CNetGameUserProfile *LoadPlayerProfile(string playerName);
	virtual bool SavePlayerProfile(CNetGameUserProfile *profile);

	virtual CNetGameUserProfile *PlayerAuthorize(string playerName, vector<u8> hash);
};
