#pragma once

#include "CRegistryClient.h"
#include "CClusterConfig.h"
#include "CProcessSpawner.h"
#include "SYS_Threading.h"
#include "json.hpp"
#include <string>
#include <vector>
#include <map>
#include <ctime>

using namespace std;
using namespace nlohmann;

// Engine-level Node Agent — manages local processes on behalf of the Registry.
// Host application configures roles and process arguments; CNodeAgent handles
// spawning, monitoring, health checking, restart policies, and system metrics.
class CNodeAgent : public CRegistryClientNodeAgentCallback
{
public:
	// Managed process record
	struct SManagedProcess
	{
		string roleName;
		pid_t pid;
		time_t startTime;
		int restartCount;
		time_t firstRestartInWindow;
		time_t lastCrashTime;
		string healthStatus;          // "unknown", "starting", "healthy", "unhealthy"
		time_t lastHealthCheck;
		int consecutiveHealthFailures;

		SManagedProcess()
			: pid(-1), startTime(0), restartCount(0), firstRestartInWindow(0),
			  lastCrashTime(0), lastHealthCheck(0), consecutiveHealthFailures(0),
			  healthStatus("unknown") {}
	};

	// Crashed role awaiting restart decision
	struct SCrashedRole
	{
		string roleName;
		time_t crashTime;
		int exitCode;
		int priorRestartCount;
		time_t priorFirstRestartInWindow;
	};

	CNodeAgent(const string &nodeId);
	virtual ~CNodeAgent();

	// Configuration (call before ConnectToRegistry)
	void SetExecutablePath(const string &path);
	void SetConfigPath(const string &path);
	void SetConfiguredRoles(const vector<string> &roles);
	void SetClusterConfig(const CClusterConfig *config);
	void AddRoleArgs(const string &roleName, const vector<string> &args);
	void SetSpawnEnvironment(const vector<string> &envVars);

	// Phase 6: PID file directory for process re-discovery on agent restart
	void SetPidFileDirectory(const string &dir);
	void ReAdoptProcesses();

	// Registry connection
	bool ConnectToRegistry(const string &address, int port, const string &internalSecret,
						   const string &nodeIp, int agentPort, int maxProcessCapacity);
	bool WaitForRegistration(int timeoutMs);

	// Main loop tick — handles process reconciliation, health checks, auto-restarts, metrics
	void Update();

	// Direct process management
	bool StartRole(const string &roleName, string &reason);
	bool StopRole(const string &roleName, bool graceful, string &reason);
	int GetManagedProcessCount();

	// Reconnect reconciliation snapshot
	json GetProcessSnapshot();

	// Shutdown all managed processes and disconnect
	void Shutdown();

	// CRegistryClientNodeAgentCallback
	virtual void RegistryManageServices(const string &operation, uint64_t commandId,
										const string &scope, const string &targetNodeId,
										const string &roleName, bool graceful, bool includeRegistry) override;

	// Phase 2: Game server process spawning
	virtual void RegistrySpawnGameServer(uint64_t requestId, const string &gameId, int port,
		const string &registryAddress, int registryPort, const json &extraArgs) override;

	// Get total managed process count (roles + game processes)
	int GetTotalProcessCount();

	CRegistryClient *registryClient;
	string nodeId;

	// Phase 6: PID file management (public for tests and external orchestration)
	void WritePidFile(const string &roleName, pid_t pid);
	void RemovePidFile(const string &roleName);

private:
	// Process lifecycle
	void ReconcileProcesses();
	void CheckHealthAll();
	void AutoRestartCrashedProcesses();

	// Role support
	bool IsRoleSupported(const string &roleName) const;
	vector<string> ResolveTargetRoles(const string &operation, const string &roleName, bool includeRegistry);
	vector<string> BuildProcessArgs(const string &roleName);

	// Health checks
	bool PerformHealthCheck(const SManagedProcess &proc, const SHealthCheckConfig &config);
	static bool CheckTcpHealth(int port, int timeoutMs);
	static bool CheckHttpHealth(int port, const string &path, int timeoutMs);

	// Restart policy
	bool ShouldAutoRestart(const string &roleName, const SCrashedRole &crashed);
	int CalculateBackoffMs(int restartCount, const SRestartPolicyConfig &policy);

	// Get role policy from cluster config (returns defaults if not configured)
	SRolePolicyConfig GetRolePolicy(const string &roleName) const;

	// System metrics
	json CollectSystemMetrics();

	// Phase 6: Crash reporting
	void SendCrashReportToRegistry(const string &roleName, int exitCode, const string &gameId = "");

	string executablePath;
	string configPath;
	string pidFileDir;
	const CClusterConfig *clusterConfig;
	vector<string> configuredRoles;
	map<string, vector<string>> roleArgs;      // roleName -> process args template
	vector<string> spawnEnvVars;               // Extra env vars for spawned processes
	map<string, SManagedProcess> managedProcesses;
	map<string, SManagedProcess> gameProcesses;    // gameId -> process (Phase 2)
	vector<SCrashedRole> crashedRoles;

	CSlrMutex *mutex;
	time_t startTime;
	time_t lastHealthCheckTime;
	time_t lastMetricsTime;
	json cachedMetrics;
	bool wasRegistered;                        // For reconnect reconciliation
};
