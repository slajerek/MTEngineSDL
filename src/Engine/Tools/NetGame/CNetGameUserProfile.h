#pragma once

#include "SYS_Defs.h"
#include "json.hpp"
#include <string>
#include <vector>
#include <list>

using namespace std;
using namespace nlohmann;

class CNetClientData;

static constexpr u32 NETGAME_PBKDF2_DEFAULT_ITERATIONS = 100000;

// Minimal engine-side user profile for multiplayer games.
// Game-specific subclasses add UI, file dialogs, faction selection, etc.
class CNetGameUserProfile
{
public:
	CNetGameUserProfile();
	virtual ~CNetGameUserProfile();

	CNetClientData *netClientData;

	int profileId;
	string name;

	// Stored password verifier (NOT the password and not necessarily the network proof).
	//
	// Version 0 (legacy): passwordHash = SHA256(password)
	// Version 1:          passwordHash = PBKDF2-HMAC-SHA256(passwordProof, salt, iterations)
	//                    where passwordProof is what the client sends over the wire.
	u8 passwordHashVersion;
	u32 passwordHashIterations;
	vector<u8> passwordSalt;
	vector<u8> passwordHash;

	// list of game ids player is involved in
	list<string> joinedGameIds;

	// Win/loss stats
	int gamesWon;
	int gamesLost;

	vector<uint8_t> PasswordHashStringToHashBytes(const string &hex);
	string PasswordHashBytesToHashString(const vector<u8> &hash);

	virtual void Serialize(json &jsonRoot);
	virtual bool Deserialize(json &jsonRoot);

	// Sets password for storage. Input is the network password proof (typically SHA256(password)).
	// This will store a salted KDF by default.
	void SetPassword(const vector<u8> &passwordProof);

	// Checks provided network password proof against stored verifier.
	// If legacy verifier matches, it auto-upgrades to v1 in-memory (caller should SavePlayerProfile).
	bool CheckPassword(const vector<u8> &passwordProof, bool *outDidUpgradeToV1 = NULL);
};
