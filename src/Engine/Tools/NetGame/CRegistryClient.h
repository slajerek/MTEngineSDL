#pragma once

#include "NET_Main.h"
#include "SYS_Threading.h"
#include "CNetClient.h"
#include "CNetGamePackets.h"
#include "json.hpp"
#include <string>
#include <map>
#include <ctime>
#include <functional>
#include <atomic>

using namespace std;
using namespace nlohmann;

// Thread-safe wrappers: atomic load/store, but implicit conversion allows
// existing code (lambda returns, printf args, conditionals) to work unchanged.
struct SAtomicBool {
	std::atomic<bool> _v;
	SAtomicBool() : _v(false) {}
	SAtomicBool(bool v) : _v(v) {}
	SAtomicBool(const SAtomicBool &o) : _v(o._v.load()) {}
	operator bool() const { return _v.load(); }
	SAtomicBool& operator=(bool v) { _v.store(v); return *this; }
	SAtomicBool& operator=(const SAtomicBool &o) { _v.store(o._v.load()); return *this; }
};

struct SAtomicInt {
	std::atomic<int> _v;
	SAtomicInt() : _v(0) {}
	SAtomicInt(int v) : _v(v) {}
	SAtomicInt(const SAtomicInt &o) : _v(o._v.load()) {}
	operator int() const { return _v.load(); }
	SAtomicInt& operator=(int v) { _v.store(v); return *this; }
	SAtomicInt& operator=(const SAtomicInt &o) { _v.store(o._v.load()); return *this; }
};

struct SAtomicU64 {
	std::atomic<uint64_t> _v;
	SAtomicU64() : _v(0) {}
	SAtomicU64(uint64_t v) : _v(v) {}
	SAtomicU64(const SAtomicU64 &o) : _v(o._v.load()) {}
	operator uint64_t() const { return _v.load(); }
	SAtomicU64& operator=(uint64_t v) { _v.store(v); return *this; }
	SAtomicU64& operator=(const SAtomicU64 &o) { _v.store(o._v.load()); return *this; }
};

class CNetGameServer;

enum class RegistryClientRole { LOBBY, GAME_SERVER, NODE_AGENT, ADMIN };

// Callback interface for registry events received by the lobby side
class CRegistryClientLobbyCallback
{
public:
	virtual ~CRegistryClientLobbyCallback() {}
	virtual void RegistryPlayerEvent(const string &gameId, const string &playerName, const string &event) {}
	virtual void RegistryGameServerRegistered(const string &gameId, const string &serverId,
											  const string &listenAddress, int listenPort) {}
	virtual void RegistryGameServerDisconnected(const string &gameId) {}
	virtual void RegistryGameServerUnreachable(const string &gameId) {}
	// Phase 6: Crash notification from Registry
	virtual void RegistryGameServerCrashed(const string &gameId, const string &nodeId, int exitCode) {}
	virtual void RegistryShutdownComplete(const string &gameId) {}
	virtual void RegistryStateSaved(const string &gameId) {}

	// Phase 2: Game allocation result (REMOTE_NODE mode)
	virtual void RegistryAllocateGameServerResult(const string &gameId, bool accepted,
		const string &nodeId, const string &address, int port, const string &reason) {}
};

// Callback interface for registry events received by the game server side
class CRegistryClientGameServerCallback
{
public:
	virtual ~CRegistryClientGameServerCallback() {}
	virtual void RegistryRegistered(const string &gameId) {}
	virtual void RegistryUpdateTokens(const map<int, string> &tokens, const map<string, int> &nameToId, time_t tokenCreationTime) {}
	virtual void RegistrySaveState() {}
	virtual void RegistryShutdown(const string &reason) {}
	virtual void RegistryKickPlayer(const string &playerName, const string &reason) {}
};

// Callback interface for registry events received by the node-agent side
class CRegistryClientNodeAgentCallback
{
public:
	virtual ~CRegistryClientNodeAgentCallback() {}
	virtual void RegistryManageServices(const string &operation, uint64_t commandId,
									 const string &scope, const string &targetNodeId,
									 const string &roleName, bool graceful, bool includeRegistry) {}

	// Phase 2: Registry requests spawning a game server process
	virtual void RegistrySpawnGameServer(uint64_t requestId, const string &gameId, int port,
		const string &registryAddress, int registryPort, const json &extraArgs) {}
};

// Client that connects to CRegistryServer. Used by both lobby and game servers.
class CRegistryClient : public CNetClientCallback
{
public:
	CRegistryClient(const string &registryAddress, int registryPort,
					RegistryClientRole role, const string &clientId,
					const string &internalSecret);
	virtual ~CRegistryClient();

	void Connect();
	void Disconnect();
	void Shutdown();

	// Periodic update (sends heartbeat for game servers)
	void Update();

	// --- Lobby-side operations ---
	void RegisterAsLobby();
	void PushTokens(const string &gameId, const map<int, string> &tokens, const map<string, int> &nameToId, time_t tokenCreationTime);
	void RequestSaveState(const string &gameId);
	void RequestShutdown(const string &gameId, const string &reason);
	void KickPlayer(const string &gameId, const string &playerName, const string &reason);
	void QueryGameServer(const string &gameId);

	// Phase 2: Request game server allocation on a remote node
	void RequestAllocateGameServer(const string &gameId, const string &mapName, int playerCount, bool resume);
	// Phase 2: Query status of an existing game session
	void QueryGameStatus(const string &gameId);

	// --- Game server-side operations ---
	void RegisterAsGameServer(const string &serverId, const string &gameId,
							  int listenPort, const string &listenAddress = "localhost");
	void SendPlayerEvent(const string &playerName, const string &event);
	void SendStateSaved();
	void SendShutdownComplete();

	// --- Node-agent operations ---
	void RegisterAsNodeAgent(const string &nodeId, const string &nodeIp, int agentPort, int maxProcessCapacity);
	void SendNodeAgentHeartbeat(int processCount, const json &metrics = json::object());
	void SendNodeAgentCommandResult(uint64_t commandId, bool success, const string &reason);
	void SetNodeAgentProcessCount(int processCount) { nodeAgentProcessCount = processCount; }
	void SetNodeAgentMetrics(const json &metrics) { nodeAgentMetrics = metrics; }

	// Phase 2: Report game process spawn result to Registry
	void SendGameProcessSpawned(uint64_t requestId, const string &gameId, bool success, int pid, const string &reason);

	// Phase 6: Report a process crash to Registry
	void SendCrashReport(const string &roleName, int exitCode, const string &gameId = "");

	// --- Admin-side operations ---
	void AdminAuthenticate(const string &adminSecret);
	void AdminGetClusterState();
	void AdminStartManagedServices(const string &scope, const string &nodeId = "", const string &roleName = "");
	void AdminStopManagedServices(const string &scope, const string &nodeId = "", const string &roleName = "",
							 bool graceful = true, bool includeRegistry = false);
	void AdminMaintenanceNode(const string &nodeId, bool enable);
	void AdminGracefulShutdown(int drainTimeoutSec = 1800, bool forceDrain = false);

	// State — atomic wrappers for cross-thread safety (written from network thread, polled from main thread)
	SAtomicBool isConnected;
	SAtomicBool isRegistered;
	SAtomicBool shutdownRequested;   // set when game server receives lobby_shutdown
	SAtomicBool adminAuthReceived;
	SAtomicBool adminAuthorized;
	string adminAuthReason;
	SAtomicBool adminClusterStateReceived;
	json adminClusterState;
	SAtomicBool adminCommandResultReceived;
	SAtomicBool adminCommandAccepted;
	string adminCommandName;
	string adminCommandReason;
	SAtomicU64 adminCommandId;
	SAtomicBool adminCommandCompleted;
	SAtomicInt adminCommandTargetedNodes;
	SAtomicInt adminCommandDispatchedNodes;
	SAtomicInt adminCommandSuccessfulNodes;
	SAtomicInt adminCommandFailedNodes;
	SAtomicBool adminMaintenanceResultReceived;
	SAtomicBool adminMaintenanceAccepted;
	string adminMaintenanceMessage;
	SAtomicBool adminShutdownProgressReceived;
	string adminShutdownPhase;
	string adminShutdownMessage;
	SAtomicBool adminShutdownComplete;

	// Callbacks
	CRegistryClientLobbyCallback *lobbyCallback;
	CRegistryClientGameServerCallback *gameServerCallback;
	CRegistryClientNodeAgentCallback *nodeAgentCallback;

	// CNetClientCallback
	virtual void NetClientCallbackConnected(CNetClient *netClient) override;
	virtual void NetClientCallbackDisconnected(CNetClient *netClient) override;
	virtual void NetClientProcessPacket(CNetPacket *packet) override;

	CNetClient *netClient;
	CNetGamePackets *netPackets;

	RegistryClientRole role;
	string clientId;

	// Game server registration info (game server role only)
	string serverId;
	string gameId;
	int listenPort;
	string listenAddress;

	std::atomic<CNetGameServer*> gameServer;  // only set for GAME_SERVER role (atomic: set from main thread, read from network thread)

	// Node agent registration info (node-agent role only)
	string nodeId;
	string nodeIp;
	int agentPort;
	int maxProcessCapacity;
	int nodeAgentProcessCount;
	json nodeAgentMetrics;             // Metrics from CNodeAgent, sent with heartbeat

private:
	void SendJson(json sendJson);

	// Lobby-side message handlers
	void HandleRegistryLobbyRegistered(json &j);
	void HandleRegistryGameServerInfo(json &j);
	void HandleRegistryPlayerEvent(json &j);
	void HandleRegistryGameServerRegistered(json &j);
	void HandleRegistryGameServerDisconnected(json &j);
	void HandleRegistryGameServerUnreachable(json &j);
	void HandleRegistryShutdownComplete(json &j);
	void HandleRegistryStateSaved(json &j);
	void HandleRegistryAllocateGameServerResult(json &j);

	// Game server-side message handlers (same messages as CLobbyLink)
	void HandleLobbyRegistered(json &j);
	void HandleLobbyUpdateTokens(json &j);
	void HandleLobbySaveState(json &j);
	void HandleLobbyShutdown(json &j);
	void HandleLobbyKickPlayer(json &j);

	// Node-agent handlers
	void HandleRegistryNaRegistered(json &j);
	void HandleRegistryManageServices(json &j);
	void HandleRegistrySpawnGameServer(json &j);

	// Admin response handlers
	void HandleRegistryAdminAuthResult(json &j);
	void HandleRegistryClusterState(json &j);
	void HandleRegistryAdminCommandResult(json &j);
	void HandleRegistryAdminMaintenanceResult(json &j);
	void HandleRegistryShutdownProgress(json &j);

	// Heartbeat
	static const int HEARTBEAT_INTERVAL_SECONDS = 5;
	time_t lastHeartbeatTime;
	time_t startTime;
};
