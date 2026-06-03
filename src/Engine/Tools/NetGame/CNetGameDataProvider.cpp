#include "SYS_Main.h"
#include "CNetGameDataProvider.h"
#include "CNetGameUserProfile.h"
#include "sgSHA256.h"
using namespace std;

CNetGameDataProvider::CNetGameDataProvider()
{
}

CNetGameDataProvider::~CNetGameDataProvider()
{
}

void CNetGameDataProvider::Init(string configPath)
{
	InitConfigFromPath(configPath);
	if (featureConfigErrorText[0] != 0)
	{
		SYS_FatalExit("CNetGameDataProvider failed: %s", featureConfigErrorText);
	}
}

void CNetGameDataProvider::InitFromHjson(Hjson::Value hjsonRoot)
{
}

void CNetGameDataProvider::StoreToHjson(Hjson::Value hjsonRoot)
{
}

vector<u8> CNetGameDataProvider::PasswordToHash(string password)
{
	sgSHA256 sha;
	sha.update(password.c_str());
	array<uint8_t, 32> digest = sha.digest();
	vector<uint8_t> passwordHash(digest.begin(), digest.end());
	return passwordHash;
}

CNetGameUserProfile *CNetGameDataProvider::PlayerAuthorize(string playerName, vector<u8> hash)
{
	LOGError("CNetGameDataProvider::PlayerAuthorize");
	return NULL;
}

bool CNetGameDataProvider::PlayerExists(string playerName)
{
	return false;
}

CNetGameUserProfile *CNetGameDataProvider::CreateProfileInstance()
{
	return new CNetGameUserProfile();
}

CNetGameUserProfile *CNetGameDataProvider::CreatePlayerProfileData(string playerName, string password)
{
	CNetGameUserProfile *profile = CreateProfileInstance();
	profile->name = playerName;
	vector<u8> hash = PasswordToHash(password);
	profile->SetPassword(hash);
	return profile;
}

CNetGameUserProfile *CNetGameDataProvider::CreateAndSavePlayerProfile(string playerName, string password)
{
	if (PlayerExists(playerName))
		return NULL;

	CNetGameUserProfile *profile = CreatePlayerProfileData(playerName, password);
	SavePlayerProfile(profile);
	return profile;
}

CNetGameUserProfile *CNetGameDataProvider::LoadPlayerProfile(string playerName)
{
	return NULL;
}

bool CNetGameDataProvider::SavePlayerProfile(CNetGameUserProfile *profile)
{
	return false;
}
