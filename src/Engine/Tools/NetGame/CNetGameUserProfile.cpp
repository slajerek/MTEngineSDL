#include "CNetGameUserProfile.h"
#include "DBG_Log.h"
#include "SYS_Crypto.h"
#include "SYS_SecureRandom.h"
#include <iomanip>
#include <sstream>
#include <cstring>

using namespace std;

CNetGameUserProfile::CNetGameUserProfile()
{
	this->profileId = -1;
	netClientData = NULL;
	gamesWon = 0;
	gamesLost = 0;
	passwordHashVersion = 0;
	passwordHashIterations = 0;
}

CNetGameUserProfile::~CNetGameUserProfile()
{
}

vector<uint8_t> CNetGameUserProfile::PasswordHashStringToHashBytes(const string &hex)
{
	if (hex.length() % 2 != 0)
		throw invalid_argument("Hex string has odd length");

	vector<uint8_t> bytes;
	bytes.reserve(hex.length() / 2);

	for (size_t i = 0; i < hex.length(); i += 2)
	{
		auto hexByte = hex.substr(i, 2);
		if (!isxdigit(hexByte[0]) || !isxdigit(hexByte[1]))
			throw invalid_argument("Invalid hex digit");

		uint8_t byte = static_cast<uint8_t>(stoul(hexByte, nullptr, 16));
		bytes.push_back(byte);
	}

	return bytes;
}

string CNetGameUserProfile::PasswordHashBytesToHashString(const vector<u8> &hash)
{
	ostringstream oss;
	for (int i = 0; i < hash.size(); ++i)
	{
		oss << hex << setw(2) << setfill('0') << (int)hash[i];
	}
	return oss.str();
}

void CNetGameUserProfile::Serialize(json &jsonRoot)
{
	jsonRoot["id"] = profileId;
	jsonRoot["name"] = name;
	jsonRoot["passwordHash"] = PasswordHashBytesToHashString(passwordHash);

	if (passwordHashVersion >= 1)
	{
		jsonRoot["passwordHashVersion"] = passwordHashVersion;
		jsonRoot["passwordIterations"] = passwordHashIterations;
		jsonRoot["passwordSalt"] = PasswordHashBytesToHashString(passwordSalt);
	}

	json jsonGameIds = nlohmann::json::array();
	for (const string &gameId : joinedGameIds)
	{
		jsonGameIds.push_back(gameId);
	}
	jsonRoot["joinedGameIds"] = jsonGameIds;
	jsonRoot["gamesWon"] = gamesWon;
	jsonRoot["gamesLost"] = gamesLost;
}

bool CNetGameUserProfile::Deserialize(json &jsonRoot)
{
	if (!jsonRoot.contains("id") || !jsonRoot.contains("name") || !jsonRoot.contains("passwordHash"))
	{
		LOGError("CNetGameUserProfile::Deserialize: missing required field(s)");
		return false;
	}

	profileId = jsonRoot["id"];
	name = jsonRoot["name"];

	passwordHashVersion = (u8)jsonRoot.value("passwordHashVersion", 0);
	passwordHashIterations = (u32)jsonRoot.value("passwordIterations", 0);
	passwordSalt.clear();

	string passwordHashStr = jsonRoot["passwordHash"];
	passwordHash = PasswordHashStringToHashBytes(passwordHashStr);

	if (passwordHashVersion == 0)
	{
		// Legacy SHA256(password)
		passwordHashIterations = 0;
		passwordSalt.clear();
	}
	else if (passwordHashVersion == 1)
	{
		if (!jsonRoot.contains("passwordSalt"))
		{
			LOGError("CNetGameUserProfile::Deserialize: v1 missing passwordSalt");
			return false;
		}
		string passwordSaltStr = jsonRoot.value("passwordSalt", "");
		passwordSalt = PasswordHashStringToHashBytes(passwordSaltStr);
		if (passwordSalt.empty() || passwordHashIterations == 0)
		{
			LOGError("CNetGameUserProfile::Deserialize: v1 invalid salt/iterations");
			return false;
		}
	}
	else
	{
		LOGError("CNetGameUserProfile::Deserialize: unknown passwordHashVersion=%d", (int)passwordHashVersion);
		return false;
	}

	joinedGameIds.clear();
	if (jsonRoot.contains("joinedGameIds"))
	{
		json jsonGameIds = jsonRoot["joinedGameIds"];
		for (const auto &gameId : jsonGameIds)
		{
			string gameIdStr = gameId.get<string>();
			joinedGameIds.push_back(gameIdStr);
		}
	}

	gamesWon = jsonRoot.value("gamesWon", 0);
	gamesLost = jsonRoot.value("gamesLost", 0);

	return true;
}

void CNetGameUserProfile::SetPassword(const vector<u8> &passwordProof)
{
	// Default: store a salted KDF verifier.
	const u8 kVersion = 1;
	const u32 kDefaultIterations = NETGAME_PBKDF2_DEFAULT_ITERATIONS;
	const size_t kSaltLen = 16;
	const size_t kVerifierLen = 32;

	passwordHashVersion = kVersion;
	passwordHashIterations = kDefaultIterations;

	passwordSalt.resize(kSaltLen);
	if (!SYS_SecureRandomBytes(passwordSalt.data(), kSaltLen))
	{
		LOGError("CNetGameUserProfile::SetPassword: secure RNG failed");
		return;
	}

	passwordHash = SYS_Pbkdf2HmacSha256(passwordProof, passwordSalt, passwordHashIterations, kVerifierLen);
}

bool CNetGameUserProfile::CheckPassword(const vector<u8> &passwordProof, bool *outDidUpgradeToV1)
{
	if (outDidUpgradeToV1)
		*outDidUpgradeToV1 = false;

	if (passwordHashVersion == 0)
	{
		// Legacy verifier: exact proof match (but constant-time).
		bool ok = SYS_ConstantTimeEquals(passwordHash, passwordProof);
		if (ok)
		{
			// Transparent upgrade to v1 in-memory.
			SetPassword(passwordProof);
			if (outDidUpgradeToV1)
				*outDidUpgradeToV1 = true;
		}
		return ok;
	}
	else if (passwordHashVersion == 1)
	{
		if (passwordSalt.empty() || passwordHashIterations == 0)
			return false;
		std::vector<u8> derived = SYS_Pbkdf2HmacSha256(passwordProof, passwordSalt, passwordHashIterations, passwordHash.size());
		return SYS_ConstantTimeEquals(passwordHash, derived);
	}

	return false;
}
