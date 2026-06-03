#pragma once

#include "NET_Main.h"
#include "SYS_Threading.h"
#include "CNetServer.h"
#include "CNetGamePackets.h"
#include "CClusterConfig.h"
#include "json.hpp"
#include <string>
#include <map>
#include <ctime>
#include <vector>
#include <set>
#include <deque>

using namespace std;
using namespace nlohmann;

class CNetClientData;

// Record for a connected lobby
struct CRegistryLobbyRecord
{
	string lobbyId;
	CNetClientData *clientData;

	CRegistryLobbyRecord() : clientData(NULL) {}
};

// Record for a connected game server (mirrors CGameServerRecord from CLobbyServiceServer)
struct CRegistryGameServerRecord
{
	string serverId;
	string gameId;
	string listenAddress;
	int listenPort;
	int connectedPlayers;
	string status;          // "running", "warning", "saving", "shutting_down", "unreachable", "shutdown"
	time_t lastHeartbeat;
	CNetClientData *clientData;
	string nodeId;          // Node that hosts this game server (for port release on disconnect)

	CRegistryGameServerRecord()
		: listenPort(0), connectedPlayers(0), lastHeartbeat(0), clientData(NULL) {}
};

// Record for a Node Agent connection (orchestrates process spawning on a VPS)
struct CRegistryNodeAgentRecord
{
	string nodeId;                  // Unique node identifier (e.g., "vps-1", "localhost")
	string nodeIp;                  // IP address of the node
	int agentPort;                  // Node Agent port for local health checks
	string status;                  // "online", "degraded", "offline"
	bool maintenanceMode;           // If true, excluded from game allocation (draining)
	time_t lastHeartbeat;
	int connectedProcessCount;      // Running processes on this node
	int maxProcessCapacity;         // Max processes this node can run
	int runningGameCount;           // Active game server processes on this node (for drain tracking)
	CNetClientData *clientData;
	json nodeMetrics;               // CPU%, memory%, etc. from heartbeat

	CRegistryNodeAgentRecord()
		: agentPort(0), maintenanceMode(false), lastHeartbeat(0), connectedProcessCount(0),
		  maxProcessCapacity(0), runningGameCount(0), clientData(NULL) {}
};

// One crash report entry stored in Registry history
struct SCrashReport
{
	string nodeId;
	string roleName;
	int exitCode;
	string gameId;      // empty if not a game process crash
	time_t timestamp;
	uint64_t seq;       // monotonic sequence number for unique dedup

	SCrashReport() : exitCode(-1), timestamp(0), seq(0) {}
	SCrashReport(const string &nodeId_, const string &roleName_, int exitCode_,
				 const string &gameId_, time_t ts, uint64_t seq_)
		: nodeId(nodeId_), roleName(roleName_), exitCode(exitCode_), gameId(gameId_), timestamp(ts), seq(seq_) {}
};

// Standalone registry service — both lobby and game servers connect here
// The registry routes messages between them and tracks game server state
// Phase 0: Extended to support Node Agents and admin queries for distributed cluster management
class CRegistryServer : public CNetServerCallback
{
public:
	static const int DEFAULT_PORT = 14650;
	int heartbeatTimeoutSeconds;
	int heartbeatWarningSeconds;

	CRegistryServer(int port, const string &internalSecret);
	virtual ~CRegistryServer();

	void Start();
	void Shutdown();

	// CNetServerCallback
	virtual void NetServerCallbackClientConnected(CNetClientData *clientData) override;
	virtual void NetServerCallbackClientDisconnected(CNetClientData *clientData) override;
	virtual void NetServerProcessPacket(CNetPacket *packet) override;
	virtual u8 NetServerAuthorize(CNetClientData *clientData, string userName, vector<u8> passwordHash) override;

	// Heartbeat monitoring
	void CheckHeartbeatTimeouts();

	// Query
	bool GetGameServerRecordCopy(const string &gameId, CRegistryGameServerRecord &outRecord) const;
	bool IsGameServerReady(const string &gameId);

	// Phase 0: Admin interface for cluster monitoring
	// Set admin secret — pass the raw secret string, will be hashed with domain separator
	void SetAdminSecret(const string &adminSecret);

	// Load cluster config used by admin cluster-state snapshots.
	bool LoadClusterConfig(const string &configPath);

	// Get cluster state as JSON (for admin dashboard)
	json GetClusterState() const;

	// Get node info
	CRegistryNodeAgentRecord *GetNodeAgentRecord(const string &nodeId);
	const vector<CRegistryNodeAgentRecord *> &GetAllNodeAgents() const { return nodeAgents; }

	// Record a crash report (also called from HandleNodeAgentCrashReport).
	// Thread-safe: acquires mutex internally.
	void RecordCrashReport(const string &nodeId, const string &roleName,
						   int exitCode, const string &gameId);

	CNetServer *netServer;
	CNetGamePackets *netPackets;
	CSlrMutex *mutex;

	// External address of this registry (used in spawn commands so game servers can connect back).
	// Set by host app. Defaults to "localhost" for local-dev.
	string registryExternalAddress;

private:
	// Client role classification (determined by first message)
	enum class ClientRole { UNKNOWN, LOBBY, GAME_SERVER, NODE_AGENT, ADMIN };
	map<CNetClientData *, ClientRole> clientRoles;

	// Shared secret (hashed) required for internal service connections.
	vector<u8> internalSecretHash;

	// Admin secret (hashed) for admin panel authentication
	vector<u8> adminSecretHash;

	// Lobby messages
	void HandleLobbyRegister(CNetClientData *clientData, json &j);
	void HandleLobbyUpdateTokens(CNetClientData *clientData, json &j);
	void HandleLobbySaveState(CNetClientData *clientData, json &j);
	void HandleLobbyShutdown(CNetClientData *clientData, json &j);
	void HandleLobbyKickPlayer(CNetClientData *clientData, json &j);
	void HandleLobbyQueryGameServer(CNetClientData *clientData, json &j);

	// Game server messages
	void HandleGsRegister(CNetClientData *clientData, json &j);
	void HandleGsHeartbeat(CNetClientData *clientData, json &j);
	void HandleGsPlayerEvent(CNetClientData *clientData, json &j);
	void HandleGsStateSaved(CNetClientData *clientData, json &j);
	void HandleGsShutdownComplete(CNetClientData *clientData, json &j);

	// Node Agent messages (Phase 0)
	void HandleNodeAgentRegister(CNetClientData *clientData, json &j);
	void HandleNodeAgentHeartbeat(CNetClientData *clientData, json &j);
	void HandleNodeAgentCommandResult(CNetClientData *clientData, json &j);

	// Phase 6: Crash report from Node Agent
	void HandleNodeAgentCrashReport(CNetClientData *clientData, json &j);

	// Phase 2: Game allocation
	void HandleLobbyAllocateGameServer(CNetClientData *clientData, json &j);
	void HandleLobbyQueryGameStatus(CNetClientData *clientData, json &j);
	void HandleNodeAgentGameProcessSpawned(CNetClientData *clientData, json &j);

	// Game allocator
	CRegistryNodeAgentRecord *SelectBestNode(int requiredCapacity = 1);
	int AllocatePortOnNode(const string &nodeId);
	void ReleasePortOnNode(const string &nodeId, int port);

	// Admin messages (Phase 0)
	void HandleAdminAuth(CNetClientData *clientData, json &j);
	void HandleAdminGetClusterState(CNetClientData *clientData, json &j);
	void HandleAdminStartManagedServices(CNetClientData *clientData, json &j);
	void HandleAdminStopManagedServices(CNetClientData *clientData, json &j);
	void HandleAdminMaintenanceNode(CNetClientData *clientData, json &j);
	void HandleAdminGracefulShutdown(CNetClientData *clientData, json &j);

	// Graceful shutdown sequence (5 phases from flows.md)
	void AdvanceShutdownSequence();
	void ForceDrainAllGames();

	void SendJson(CNetClientData *clientData, json sendJson);
	bool AssignRoleLocked(CNetClientData *clientData, ClientRole role, const char *action);

	// Find the lobby that should receive events for a given game
	CNetClientData *FindLobbyForNotificationLocked();

	// Registered lobbies indexed by lobbyId
	map<string, CRegistryLobbyRecord *> lobbies;

	// Registered game servers indexed by gameId
	map<string, CRegistryGameServerRecord *> gameServers;

	// Registered node agents indexed by nodeId (Phase 0)
	map<string, CRegistryNodeAgentRecord *> nodeAgentsByNodeId;
	vector<CRegistryNodeAgentRecord *> nodeAgents;  // For iteration

	// Pending tokens for games that haven't registered yet
	struct PendingTokens {
		map<string, string> tokens;		// clientId (as string) → token
		map<string, int> nameToId;			// clientName → clientId
		time_t tokenCreationTime;
		string fromLobbyId;
	};
	map<string, PendingTokens> pendingTokens;

	// Optional cluster config (loaded by host app in standalone registry mode)
	bool hasClusterConfig;
	string clusterConfigPath;
	CClusterConfig clusterConfig;
	uint64_t adminCommandSeq;
	uint64_t crashSeqCounter;

	struct PendingAdminCommand
	{
		uint64_t commandId;
		string commandName;
		string scope;
		string targetNodeId;
		string roleName;
		bool graceful;
		bool includeRegistry;
		int targetedNodes;
		int dispatchedNodes;
		int successfulNodes;
		int failedNodes;
		time_t createdAt;
		set<string> awaitingNodeIds;
		json nodeResults;
		CNetClientData *adminClient;

		PendingAdminCommand()
			: commandId(0), graceful(true), includeRegistry(false), targetedNodes(0), dispatchedNodes(0),
			  successfulNodes(0), failedNodes(0), createdAt(0), nodeResults(json::array()), adminClient(NULL) {}
	};

	int adminCommandTimeoutSeconds;
	int adminAuthMaxAttempts;
	map<uint64_t, PendingAdminCommand> pendingAdminCommands;

	// Per-client failed admin auth attempt tracking (rate limiting)
	map<CNetClientData *, int> adminAuthFailures;

	// Phase 2: Game allocation tracking
	struct PendingGameAllocation
	{
		string gameId;
		string nodeId;
		uint64_t requestId;
		int allocatedPort;
		CNetClientData *lobbyClient;
		string mapName;
		int playerCount;
		bool resume;
		time_t createdAt;

		PendingGameAllocation()
			: requestId(0), allocatedPort(0), lobbyClient(NULL), playerCount(0), resume(false), createdAt(0) {}
	};

	map<string, PendingGameAllocation> pendingGameAllocations;  // gameId -> allocation
	uint64_t gameAllocationSeq;

	// Per-node port tracking (defaults from config, per-node overrides take priority)
	int gamePortRangeStart;
	int gamePortRangeEnd;
	map<string, set<int>> nodeUsedPorts;  // nodeId -> used ports

	static const int GAME_ALLOCATION_TIMEOUT_SECONDS = 30;

	// Crash history ring buffer (guarded by mutex)
	static const int MAX_CRASH_HISTORY = 100;
	static const int CRASH_HISTORY_IN_STATE = 20;  // how many to include in GetClusterState
	deque<SCrashReport> crashHistory;

	// Graceful shutdown state machine
	enum class ShutdownPhase { NONE, STOP_NEW_GAMES, DRAIN_ACTIVE, STOP_GAME_SERVERS, STOP_INFRASTRUCTURE, STOP_REGISTRY };
	ShutdownPhase shutdownPhase;
	time_t shutdownStartedAt;
	int shutdownDrainTimeoutSeconds;
	bool shutdownForceDrain;
	CNetClientData *shutdownAdminClient;
};
