#pragma once

#include "NET_Main.h"
#include "SYS_Threading.h"

#include "CNetClient.h"
#include "CNetGamePackets.h"

#include "CChatHistory.h"
#include "json.hpp"
#include <vector>

using namespace nlohmann;
using namespace std;

class CGuiViewMessages;

class CNetGameClient : public CNetClientCallback
{
public:
	CNetGameClient(int componentId, const char *serverAddress, int serverConnectPort, const char *clientLoginName, vector<u8> passwordHash, CGuiViewMessages *messagesLog, const string &connectionToken = "");
	CNetGameClient(CNetClient *netClient, int componentId, CGuiViewMessages *messagesLog, const string &connectionToken = "");
	void Init(CNetClient *netClient, int componentId);

	CGuiViewMessages *messagesLog;

	CNetClient *netClient;
	CNetGamePackets *netPackets;

	int clientId;

	// Authentication
	string connectionToken;
	bool isAuthenticated;

	// Turn-based round state (from server's roundState broadcasts)
	int roundNumber;
	int currentPlayerClientId;
	vector<int> playerOrder;
	bool isMyTurn;
	string turnPhase;  // "waiting", "playing", "paused"

	// Structured chat storage
	vector<ChatEntry> chatMessages;
	int oldestLoadedIndex;
	int totalChatCount;
	bool hasMoreHistory;
	bool isLoadingHistory;

	void RequestMoreHistory();

	virtual void NetClientCallbackConnected(CNetClient *netClient);
	virtual void NetClientCallbackDisconnected(CNetClient *netClient);
	virtual void NetClientProcessPacket(CNetPacket *packet);

	void IssuePacket(bool isReliable, CNetPacket *packet);

	void SendJson(json sendJson);

	CSlrMutex *mutexLog;
	char logBuf[MAX_STRING_LENGTH];
	CSlrDate *logDate;
	void AddLog(string str);
	void AddLog(const char* fmt, ...);
};
