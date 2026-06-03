#pragma once

#include "NET_Main.h"
#include "SYS_Threading.h"

#include "CNetClient.h"
#include "CNetGamePackets.h"
#include "CNetLobbyClientCallback.h"
#include "CChatHistory.h"

#include "json.hpp"
#include <map>
#include <list>
#include <vector>

using namespace nlohmann;
using namespace std;

class CGuiViewMessages;

// Info about an active game the player is part of
struct CActiveGameInfo
{
	string gameId;
	string mapName;
	string status;
	vector<string> playerNames;
	json extraData;  // game-specific extra fields from server (e.g. playerDetails)
};

class CNetLobbyClient : public CNetClientCallback
{
public:
	CNetLobbyClient(int componentId, const char *serverAddress, int serverConnectPort, string clientLoginName, vector<u8> passwordHash, CGuiViewMessages *messagesLog);
	CNetLobbyClient(CNetClient *netClient, int componentId, CGuiViewMessages *messagesLog);
	void Init(CNetClient *netClient, int componentId);

	CGuiViewMessages *messagesLog;

	CNetClient *netClient;
	CNetGamePackets *netPackets;

	int clientId;

	virtual void NetClientProcessPacket(CNetPacket *packet);
	virtual void NetClientCallbackConnected(CNetClient *netClient);
	virtual void NetClientCallbackDisconnected(CNetClient *netClient);
	virtual void NetClientCallbackNotAuthorized(CNetClient *netClient);

	void IssuePacket(bool isReliable, CNetPacket *packet);

	void SendJson(json sendJson);

	CSlrMutex *mutexLog;
	char logBuf[MAX_STRING_LENGTH];
	CSlrDate *logDate;
	void AddLog(string str);
	void AddLog(const char* fmt, ...);

	int joinableGameId;

	// Active games this player is part of (received from server)
	vector<CActiveGameInfo> activeGames;

	// Structured chat storage (replaces AddLog-only chat)
	std::vector<ChatEntry> chatMessages;
	int oldestLoadedIndex = 0;
	int totalChatCount = 0;
	bool hasMoreHistory = false;
	bool isLoadingHistory = false;

	void SendChatMessage(const std::string &msg);
	void RequestMoreChatHistory();
	void ClearChatState();

	// Callbacks for test automation and event notification
	list<CNetLobbyClientCallback *> netLobbyClientCallbacks;
	void AddNetLobbyClientCallback(CNetLobbyClientCallback *callback);
	void RemoveNetLobbyClientCallback(CNetLobbyClientCallback *callback);

protected:
	// Virtual hooks for game-specific packet handling.
	// Override these in subclass to handle game-specific data.
	virtual void OnJoinableGamesPacket(json &j) {}
	virtual void OnPlayerProfilePacket(json &j) {}

	// Virtual hooks for game-specific state changes
	virtual void OnGameStarting() {}
	virtual void OnStartGameFailed(const string &error) {}
	virtual void OnGameCancelled() {}
	virtual void OnDisconnectedFromLobby() {}
};
