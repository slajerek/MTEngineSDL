#include "CServerGame.h"
#include "DBG_Log.h"
#include "SYS_Main.h"

#include <array>
#include <sstream>
#include <iomanip>

CServerGame::CServerGame()
{
	mapId = -1;
	serverAddress = "localhost";
	serverPort = 0;
	gameServer = NULL;
	processId = 0;
	isRemoteProcess = false;
	status = SUSPENDED;
	isFinishing = false;
	lastActivityTime = time(NULL);
	connectedPlayerCount = 0;
}

CServerGame::~CServerGame()
{
}

void CServerGame::Serialize(json &j)
{
	j["gameId"] = gameId;
	j["mapName"] = mapName;
	j["mapId"] = mapId;
	j["stateFilePath"] = stateFilePath;
	if (!projectPath.empty())
		j["projectPath"] = projectPath;

	json jsonPlayers = json::array();
	for (const auto &slot : playerSlots)
	{
		json jp;
		jp["playerProfileId"] = slot.playerProfileId;
		jp["playerName"] = slot.playerName;
		jp["roleId"] = slot.roleId;
		jp["isActive"] = slot.isActive;
		jsonPlayers.push_back(jp);
	}
	j["players"] = jsonPlayers;
}

bool CServerGame::Deserialize(json &j)
{
	try
	{
		gameId = j["gameId"].get<string>();

		// Backward-compat: read both new and old field names
		if (j.contains("mapName"))
			mapName = j["mapName"].get<string>();
		else if (j.contains("scenarioName"))
			mapName = j["scenarioName"].get<string>();

		if (j.contains("mapId"))
			mapId = j["mapId"].get<int>();
		else if (j.contains("scenarioId"))
			mapId = j["scenarioId"].get<int>();

		stateFilePath = j.value("stateFilePath", "");
		projectPath = j.value("projectPath", "");

		playerSlots.clear();
		if (j.contains("players"))
		{
			for (auto &jp : j["players"])
			{
				PlayerSlot slot;
				slot.playerProfileId = jp["playerProfileId"].get<int>();
				slot.playerName = jp["playerName"].get<string>();

				// Backward-compat: read both new and old field names
				if (jp.contains("roleId"))
					slot.roleId = jp["roleId"].get<int>();
				else if (jp.contains("factionId"))
					slot.roleId = jp["factionId"].get<int>();
				else
					slot.roleId = -1;

				slot.isActive = jp.value("isActive", true);

				playerSlots.push_back(slot);
			}
		}

		status = SUSPENDED;
		serverPort = 0;
		gameServer = NULL;
		connectedPlayerCount = 0;
		lastActivityTime = time(NULL);
		return true;
	}
	catch (const exception &e)
	{
		LOGError("CServerGame::Deserialize: %s", e.what());
		return false;
	}
}

string CServerGame::GenerateGameId()
{
	// UUID v4: 16 random bytes with version/variant bits set.
	std::array<u8, 16> b;
	if (!SYS_SecureRandomBytes(b.data(), b.size()))
	{
		SYS_FatalExit("CServerGame::GenerateGameId: secure RNG failed");
	}

	// Version 4 (0100xxxx)
	b[6] = (u8)((b[6] & 0x0F) | 0x40);
	// Variant (10xxxxxx)
	b[8] = (u8)((b[8] & 0x3F) | 0x80);

	static const char hex[] = "0123456789abcdef";
	string uuid;
	uuid.reserve(36);
	for (size_t i = 0; i < b.size(); i++)
	{
		if (i == 4 || i == 6 || i == 8 || i == 10)
			uuid.push_back('-');
		uuid.push_back(hex[(b[i] >> 4) & 0x0F]);
		uuid.push_back(hex[b[i] & 0x0F]);
	}
	return uuid;
}

string CServerGame::GenerateConnectionToken()
{
	std::array<u8, 32> rnd;
	if (!SYS_SecureRandomBytes(rnd.data(), rnd.size()))
	{
		SYS_FatalExit("CServerGame::GenerateConnectionToken: secure RNG failed");
	}

	static const char hex[] = "0123456789abcdef";
	string token;
	token.resize(rnd.size() * 2);
	for (size_t i = 0; i < rnd.size(); i++)
	{
		token[i * 2 + 0] = hex[(rnd[i] >> 4) & 0x0F];
		token[i * 2 + 1] = hex[rnd[i] & 0x0F];
	}
	return token;
}

int CServerGame::GetActivePlayerCount()
{
	int count = 0;
	for (const auto &slot : playerSlots)
	{
		if (slot.isActive)
			count++;
	}
	return count;
}

string CServerGame::GetLastActivePlayerName()
{
	string lastActive;
	for (const auto &slot : playerSlots)
	{
		if (slot.isActive)
			lastActive = slot.playerName;
	}
	return lastActive;
}

int CServerGame::GetLastActivePlayerProfileId()
{
	int lastActiveId = -1;
	for (const auto &slot : playerSlots)
	{
		if (slot.isActive)
			lastActiveId = slot.playerProfileId;
	}
	return lastActiveId;
}

const char *CServerGame::GetStatusName()
{
	switch (status)
	{
		case STARTING:  return "STARTING";
		case RUNNING:   return "RUNNING";
		case SUSPENDED: return "SUSPENDED";
		case FINISHED:  return "FINISHED";
	}
	return "UNKNOWN";
}
