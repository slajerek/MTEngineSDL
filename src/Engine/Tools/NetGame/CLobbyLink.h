#pragma once


// CAPABILITY GUARD (MT_ENABLE_NET_GAME). The header stays INCLUDABLE and declares nothing
// when the capability is off, so a conditional consumer still compiles.
// MT_CAP_NET_GAME implies NET_TRANSPORT and HTTP, so this whole stack rides on one flag -- which did not exist until 2026-09-06 (enables was an empty list).
#if MT_ENABLE_NET_GAME

#include "NET_Main.h"
#include "SYS_Threading.h"
#include "CNetClient.h"
#include "CNetGamePackets.h"
#include "json.hpp"
#include <string>
#include <map>
#include <ctime>

using namespace std;
using namespace nlohmann;

class CNetGameServer;

// Game server's client connection back to the lobby service port
class CLobbyLink : public CNetClientCallback
{
public:
	static const int HEARTBEAT_INTERVAL_SECONDS = 5;

	CLobbyLink(const string &lobbyAddress, int lobbyServicePort,
			   const string &serverId, const string &gameId,
			   int listenPort, CNetGameServer *gameServer,
			   const string &internalSecret);
	virtual ~CLobbyLink();

	void Connect();
	void Disconnect();
	// Stops the network thread (advisory 2s poll -- CNetClient has no robust
	// StopServer() equivalent) and returns whether it actually stopped. A
	// caller that gets false MUST NOT delete this object.
	bool Shutdown();

	// Called periodically from game server's network thread
	void Update();

	// Send player events to lobby
	void SendPlayerEvent(const string &playerName, const string &event);

	// Registration state
	bool isRegistered;
	bool isConnected;
	bool shutdownRequested;

	// CNetClientCallback
	virtual void NetClientCallbackConnected(CNetClient *netClient) override;
	virtual void NetClientCallbackDisconnected(CNetClient *netClient) override;
	virtual void NetClientProcessPacket(CNetPacket *packet) override;

	CNetClient *netClient;
	CNetGamePackets *netPackets;

	string serverId;
	string gameId;
	string listenAddress;
	int listenPort;

	CNetGameServer *gameServer;

private:
	void SendRegister();
	void SendHeartbeat();
	void SendJson(json sendJson);

	void HandleRegistered(json &j);
	void HandleUpdateTokens(json &j);
	void HandleSaveState(json &j);
	void HandleShutdown(json &j);
	void HandleKickPlayer(json &j);

	time_t lastHeartbeatTime;
	time_t startTime;
};

#endif // MT_ENABLE_NET_GAME
