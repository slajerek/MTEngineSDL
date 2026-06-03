#pragma once

#include "NET_Main.h"
#include "SYS_Threading.h"
#include "CFeatureConfig.h"
#include "json.hpp"

#include "CNetServer.h"
#include "CNetGamePackets.h"
#include "ILogSink.h"
#include "CChatHistory.h"

using namespace std;

class CServerGamesManagerBase;
class CLobbyServiceServer;
class CGameServerRegistry;

// Generic base class for a lobby server.
// Handles networking plumbing, JSON send/broadcast, logging, init, config parsing,
// registry switching, and rate limiting.
// Game-specific subclasses override virtual hooks for packet handling, auth,
// connect/disconnect callbacks.
class CNetLobbyServer : public CNetServerCallback, public CFeatureConfig
{
public:
	CNetLobbyServer(int serverPort);
	virtual ~CNetLobbyServer();

	void Init(CNetServer *netServer);

	CFeatureConfig *config;
	CNetServer *netServer;
	CNetGamePackets *netPackets;

	// Pure virtual hooks — subclass must implement
	virtual void NetServerCallbackClientConnected(CNetClientData *clientData) = 0;
	virtual void NetServerCallbackClientDisconnected(CNetClientData *clientData) = 0;
	virtual void NetServerProcessPacket(CNetPacket *packet) = 0;
	virtual u8 NetServerAuthorize(CNetClientData *clientData, string userName, vector<u8> passwordHash) = 0;

	// Generic JSON packet sending
	void SendJson(CNetClientData *clientData, nlohmann::json sendJson);
	void BroadcastJson(nlohmann::json sendJson);
	void DisconnectWithError(CNetClientData *clientData, const char *format, ...);
	void SendError(CNetClientData *clientData, const char *format, ...);
	void SendStartGameFailed(CNetClientData *clientData, const char *format, ...);

	// Logging — uses ILogSink if set, otherwise LOGD
	ILogSink *logSink;

	CSlrMutex *mutexLog;
	char logBuf[MAX_STRING_LENGTH];
	CSlrDate *logDate;
	void AddLog(const char *fmt, ...);
	void AddLog(string str);

	// Config
	virtual void InitFromHjson(Hjson::Value hjsonRoot);
	virtual void StoreToHjson(Hjson::Value hjsonRoot);

	// Server port
	int serverPort;

	// Game server orchestration (pointer to base; subclass creates and sets it)
	CServerGamesManagerBase *gamesManager;

	// Inter-server communication
	CLobbyServiceServer *serviceServer;   // NULL in STANDALONE registry mode
	CGameServerRegistry *registry;
	string internalSecret;               // shared secret for internal services (lobby-service / registry)

	void SetInternalSecret(const string &secret) { internalSecret = secret; }
	const string &GetInternalSecret() const { return internalSecret; }

	// Switch to standalone registry mode (called from InitFromHjson or tests)
	void SwitchToStandaloneRegistry(const string &registryAddress, int registryPort);

	// Called after a new CGameServerRegistry is created (e.g., in SwitchToStandaloneRegistry).
	// Subclass should wire gamesManager <-> registry linkage here.
	virtual void OnRegistryCreated(CGameServerRegistry *newRegistry) {}

	// Security: rate limiting and packet size
	static const int MAX_PACKETS_PER_SECOND = 30;
	static const int MAX_PACKET_SIZE = 65536;  // 64KB

	// Lobby chat persistence
	CChatHistory *lobbyChatHistory = nullptr;
	void SetLobbyChatHistoryPath(const std::string &path);
	std::string GetLobbyChatHistoryPath() const;

	// Chat packet handler — call from subclass NetServerProcessPacket().
	// Returns true if the packet was a chat action and was handled.
	bool ProcessChatPacket(CNetClientData *clientData, nlohmann::json &j);

	// Send recent chat history to a single client (call after auth).
	void SendChatHistoryToClient(CNetClientData *clientData);

	static const int CHAT_HISTORY_PAGE_SIZE = 50;
};
