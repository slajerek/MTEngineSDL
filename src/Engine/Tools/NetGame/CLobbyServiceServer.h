#pragma once


// CAPABILITY GUARD (MT_ENABLE_NET_GAME). The header stays INCLUDABLE and declares nothing
// when the capability is off, so a conditional consumer still compiles.
// MT_CAP_NET_GAME implies NET_TRANSPORT and HTTP, so this whole stack rides on one flag -- which did not exist until 2026-09-06 (enables was an empty list).
#if MT_ENABLE_NET_GAME

#include "NET_Main.h"
#include "SYS_Threading.h"
#include "CNetServer.h"
#include "CNetGamePackets.h"
#include "CNetGameInternalAuth.h"
#include "json.hpp"
#include <string>
#include <map>
#include <ctime>
#include <vector>

using namespace std;
using namespace nlohmann;

class CNetClientData;
class CGameServerRegistry;

// Record for each registered game server
struct CGameServerRecord
{
	string serverId;
	string gameId;
	string listenAddress;
	int listenPort;
	int connectedPlayers;
	string status;			// "running", "warning", "saving", "shutting_down"
	time_t lastHeartbeat;
	CNetClientData *clientData;	// the game server's ENet connection

	CGameServerRecord()
		: listenPort(0), connectedPlayers(0), lastHeartbeat(0), clientData(NULL) {}
};

// Lobby's internal service endpoint — game servers connect here to register and communicate
class CLobbyServiceServer : public CNetServerCallback
{
public:
	static const int SERVICE_PORT = 14600;
	int heartbeatTimeoutSeconds;
	int heartbeatWarningSeconds;

	CLobbyServiceServer(int port, const string &internalSecret);
	virtual ~CLobbyServiceServer();

	void Start();
	void Shutdown();

	// CNetServerCallback
	virtual void NetServerCallbackClientConnected(CNetClientData *clientData) override;
	virtual void NetServerCallbackClientDisconnected(CNetClientData *clientData) override;
	virtual void NetServerProcessPacket(CNetPacket *packet) override;
	virtual u8 NetServerAuthorize(CNetClientData *clientData, string userName, vector<u8> passwordHash) override;

	// Outbound messages to game servers
	void SendUpdateTokens(const string &gameId, const map<int, string> &tokens, const map<string, int> &nameToId, time_t tokenCreationTime);
	void SendSaveState(const string &gameId);
	void SendShutdown(const string &gameId, const string &reason);
	void SendKickPlayer(const string &gameId, const string &playerName, const string &reason);

	// Heartbeat monitoring
	void CheckHeartbeatTimeouts();

	// Lookup
	CGameServerRecord *GetRecord(const string &gameId);
	bool IsGameServerReady(const string &gameId);

	// Admin dashboard accessors
	int GetGameServerCount();
	vector<CGameServerRecord *> GetAllGameServerRecords();
	int GetPendingTokensCount();

	// Pending tokens: stored when lobby pushes tokens before game server registers
	void StorePendingTokens(const string &gameId, const map<int, string> &tokens, const map<string, int> &nameToId, time_t tokenCreationTime);

	// Registry back-reference (set by CGameServerRegistry)
	CGameServerRegistry *registry;

	CNetServer *netServer;
	CNetGamePackets *netPackets;
	CSlrMutex *mutex;

private:
	void HandleRegister(CNetClientData *clientData, json &j);
	void HandleHeartbeat(CNetClientData *clientData, json &j);
	void HandlePlayerEvent(CNetClientData *clientData, json &j);
	void HandleStateSaved(CNetClientData *clientData, json &j);
	void HandleShutdownComplete(CNetClientData *clientData, json &j);

	void SendJson(CNetClientData *clientData, json sendJson);

	// Registered game servers indexed by gameId
	map<string, CGameServerRecord *> gameServers;

	// Pending tokens for games that haven't registered yet
	struct PendingTokens {
		map<int, string> tokens;
		map<string, int> nameToId;
		time_t tokenCreationTime;
	};
	map<string, PendingTokens> pendingTokens;

	// PW2 verifier for internal service connections (challenge-response, not a
	// replayable HMAC -- see CNetGameInternalAuth.h). valid=false rejects everyone.
	NetGameInternalVerifier internalVerifier;
};

#endif // MT_ENABLE_NET_GAME
