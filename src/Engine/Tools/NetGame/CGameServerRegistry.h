#pragma once

#include "CRegistryClient.h"
#include "json.hpp"
#include <string>
#include <map>
#include <ctime>
#include <functional>

using namespace std;
using namespace nlohmann;

class CLobbyServiceServer;
class CServerGamesManagerBase;
class CServerGame;

enum class RegistryMode { EMBEDDED, STANDALONE };

struct CGameServerInfo
{
	string gameId;
	string address;
	int port;
	int connectedPlayers;
	string status;
	bool isReachable;

	CGameServerInfo() : port(0), connectedPlayers(0), isReachable(false) {}
};

// Registry abstraction — works in two modes:
// EMBEDDED: wraps CLobbyServiceServer (local, in-process)
// STANDALONE: wraps CRegistryClient connected to external CRegistryServer
class CGameServerRegistry : public CRegistryClientLobbyCallback
{
public:
	// EMBEDDED mode: uses local CLobbyServiceServer
	CGameServerRegistry(CLobbyServiceServer *serviceServer, const string &internalSecret);

	// STANDALONE mode: connects to external CRegistryServer
	CGameServerRegistry(const string &registryAddress, int registryPort, const string &lobbyId, const string &internalSecret);

	virtual ~CGameServerRegistry();

	// Push connection tokens to a game server (or store as pending if not yet registered)
	void PushTokens(const string &gameId, const map<int, string> &tokens, const map<string, int> &nameToId, time_t tokenCreationTime);

	// Request game server to save its state
	void RequestSaveState(const string &gameId);

	// Request game server to shut down
	void RequestShutdown(const string &gameId, const string &reason);

	// Kick a player from a game server
	void KickPlayer(const string &gameId, const string &playerName, const string &reason);

	// Query game server info
	CGameServerInfo GetGameServerInfo(const string &gameId);
	bool IsGameServerReady(const string &gameId);

	// Phase 2: Request game allocation on a remote node (STANDALONE mode only)
	void RequestAllocateGameServer(const string &gameId, const string &mapName, int playerCount, bool resume);

	// Called by CLobbyServiceServer when player events come in (EMBEDDED mode only)
	// Override in subclass to update game tracking records
	virtual void OnPlayerEvent(const string &gameId, const string &playerName, const string &event);

	// Periodic update
	void Update();

	// Wait for registry client connection (STANDALONE mode)
	bool WaitForConnection(int timeoutMs);

	RegistryMode mode;
	string internalSecret;                // shared secret used for internal authorize
	CLobbyServiceServer *serviceServer;   // set in EMBEDDED mode
	CRegistryClient *registryClient;      // set in STANDALONE mode
	CServerGamesManagerBase *gamesManager;    // set externally for updating CServerGame records
	std::function<CServerGame*(const string&)> getGameCallback; // optional: lookup game by ID

	// Allocation result callback — called by the lobby-level flow when allocation completes
	// Game-specific subclasses can override to route tokens and gameReady to clients
	std::function<void(const string &gameId, bool accepted, const string &nodeId,
					   const string &address, int port, const string &reason)> onAllocationResult;

	// CRegistryClientLobbyCallback (STANDALONE mode — receives events from registry)
	virtual void RegistryPlayerEvent(const string &gameId, const string &playerName, const string &event) override;
	virtual void RegistryGameServerRegistered(const string &gameId, const string &serverId,
											  const string &listenAddress, int listenPort) override;
	virtual void RegistryGameServerDisconnected(const string &gameId) override;
	virtual void RegistryGameServerUnreachable(const string &gameId) override;
	virtual void RegistryShutdownComplete(const string &gameId) override;

	// Phase 2: Allocation result from registry
	virtual void RegistryAllocateGameServerResult(const string &gameId, bool accepted,
		const string &nodeId, const string &address, int port, const string &reason) override;
};
