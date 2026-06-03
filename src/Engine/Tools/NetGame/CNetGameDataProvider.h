#pragma once

#include "SYS_Defs.h"
#include "CFeatureConfig.h"
#include <string>
#include <vector>

using namespace std;

class CNetGameUserProfile;

// Generic base class for player data persistence and authentication.
// Handles player authorization, profile CRUD, and password hashing.
// Game-specific subclasses override CreateProfileInstance() to return
// their own profile subclass (e.g., CPlayerProfile).
class CNetGameDataProvider : public CFeatureConfig
{
public:
	CNetGameDataProvider();
	virtual ~CNetGameDataProvider();
	void Init(string configPath);

	CFeatureConfig *config;
	virtual void InitFromHjson(Hjson::Value hjsonRoot);
	virtual void StoreToHjson(Hjson::Value hjsonRoot);

	// Password hashing (SHA256)
	virtual vector<u8> PasswordToHash(string password);

	// Player authorization — returns profile when OK, NULL when not authorized
	virtual CNetGameUserProfile *PlayerAuthorize(string playerName, vector<u8> hash);
	virtual bool PlayerExists(string playerName);
	virtual CNetGameUserProfile *CreatePlayerProfileData(string playerName, string password);
	virtual CNetGameUserProfile *CreateAndSavePlayerProfile(string playerName, string password);
	virtual CNetGameUserProfile *LoadPlayerProfile(string playerName);
	virtual bool SavePlayerProfile(CNetGameUserProfile *profile);

	// Factory method — override to create game-specific profile subclass
	virtual CNetGameUserProfile *CreateProfileInstance();
};
