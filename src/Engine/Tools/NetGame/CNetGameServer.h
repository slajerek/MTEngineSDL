#pragma once

#include "NET_Main.h"
#include "SYS_Threading.h"

#include "CNetServer.h"
#include "CNetGamePackets.h"
#include "ILogSink.h"
#include "json.hpp"
#include <set>
#include <map>
#include <vector>
#include <mutex>

using namespace std;

class CChatHistory;
class CServerGamesManagerBase;
class CLobbyLink;

using namespace nlohmann;

// Generic base class for a multiplayer game server.
// Handles authentication, chat, tokens, lobby link, logging.
// Game-specific subclasses override virtual methods for game state management.
class CNetGameServer : public CNetServerCallback
{
public:
	CNetGameServer(json serverConfig, int serverPort);
	virtual ~CNetGameServer();

	void Init(CNetServer *netServer);

	json config;
	CNetServer *netServer;
	CNetGamePackets *netPackets;

	virtual void NetServerCallbackClientConnected(CNetClientData *clientData);
	virtual void NetServerCallbackClientDisconnected(CNetClientData *clientData);
	virtual void NetServerProcessPacket(CNetPacket *packet);
	virtual void NetServerLogic(CNetServer *netServer);

	void SendJson(CNetClientData *clientData, nlohmann::json sendJson);
	void BroadcastJson(nlohmann::json sendJson);
	void DisconnectWithError(CNetClientData *clientData, const char *format, ...);

	// Logging — uses ILogSink if set, otherwise LOGD
	ILogSink *logSink;

	CSlrMutex *mutexLog;
	char logBuf[MAX_STRING_LENGTH];
	CSlrDate *logDate;
	void AddLog(const char *fmt, ...);
	void AddLog(string str);

	int serverPort;

	// Game orchestration
	string gameId;
	CServerGamesManagerBase *manager;
	CLobbyLink *lobbyLink;

	// Security: connection tokens (protected by mutexTokens for cross-thread access)
	std::mutex mutexTokens;
	map<int, string> expectedTokens;         // clientId -> token
	map<string, int> clientIdByClientName;   // clientName -> clientId (for auth lookup)
	set<int> authenticatedClients;           // clientIds that passed token check
	time_t tokenCreationTime;

	// Security: player whitelist
	set<int> allowedPlayers;                 // clientIds

	// Security: token expiry — tokens older than this are rejected (seconds, 0 = no expiry)
	static const int TOKEN_EXPIRY_SECONDS = 300;  // 5 minutes

	// Security: rate limiting per client
	static const int MAX_PACKETS_PER_SECOND = 60;
	static const int MAX_PACKET_SIZE = 65536;  // 64KB

	// Per-client packet rate tracking
	struct ClientRateInfo {
		int packetCount;
		time_t windowStart;
	};
	map<CNetClientData*, ClientRateInfo> clientPacketRates;

	// Chat history (persistent)
	CChatHistory *chatHistory;

	// Connected player tracking
	int connectedPlayerCount;

	// Set expected tokens from lobby via CLobbyLink
	void SetExpectedTokens(const map<int, string> &tokens, const map<string, int> &nameToId, time_t creationTime);

	// Lifecycle
	void SaveState();
	void Shutdown();

	// Periodic save — checkpoint every periodicSaveIntervalSeconds (0 = disabled)
	int periodicSaveIntervalSeconds;
	time_t lastPeriodicSaveTime;

	// --- Virtual hooks for game-specific behavior ---
	// Called from SaveState() — serialize game state to disk
	virtual void OnSaveState() {}
	// Called when a client successfully authenticates
	virtual void OnPlayerAuthenticated(CNetClientData *clientData) {}
	// Called for non-standard packet actions; return true if handled
	virtual bool OnCustomPacket(CNetClientData *clientData, const string &action, json &j) { return false; }
	// Called when a player connects/disconnects to update game tracking (legacy in-process mode)
	virtual void OnUpdateGameTracking(int connectedPlayerCount) {}
	// Called when a player is deactivated (left the game). Override for turn system adjustments.
	virtual void OnPlayerDeactivated(int clientId) {}

	// Protocol version check — override to provide game-specific version info.
	// Returns true if version is acceptable, false if rejected.
	// Default implementation accepts all versions (no check).
	virtual bool CheckProtocolVersion(const string &clientVersion, string &serverVersion, string &rejectReason) { return true; }

	// Admission gate — called during clientHello after version check.
	// Override to reject clients during maintenance. Fill reasonCode, messageKey,
	// localizedText, fallbackText for the reject payload.
	// Default implementation admits all clients.
	virtual bool CheckAdmission(const string &clientLocale, string &reasonCode,
								string &messageKey, string &localizedText, string &fallbackText) { return true; }

protected:
	bool isShuttingDown;

	// Deferred auth queue — replaces std::thread().detach() race
	struct PendingAuth {
		json authJson;
		CNetClientData *clientData;
		time_t queuedAt;
	};
	std::mutex pendingAuthMutex;
	vector<PendingAuth> pendingAuthQueue;
	void ProcessPendingAuth();
};
