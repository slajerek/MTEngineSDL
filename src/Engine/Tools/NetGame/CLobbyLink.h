#pragma once

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
	void Shutdown();

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
