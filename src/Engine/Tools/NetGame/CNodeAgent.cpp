#include "CNodeAgent.h"
#include "DBG_Log.h"
#include "SYS_Funct.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <filesystem>

#ifndef _WIN32
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#ifdef __APPLE__
#include <mach/mach.h>
#endif
#endif

// How often to collect system metrics (seconds)
static const int METRICS_COLLECT_INTERVAL = 10;

// Minimum interval between health check passes (seconds)
static const int MIN_HEALTH_CHECK_INTERVAL = 1;

// How many consecutive health failures before marking unhealthy
static const int HEALTH_FAILURE_THRESHOLD = 3;

CNodeAgent::CNodeAgent(const string &nodeId)
{
	this->nodeId = nodeId;
	registryClient = NULL;
	clusterConfig = NULL;
	mutex = new CSlrMutex("CNodeAgentMutex");
	startTime = time(NULL);
	lastHealthCheckTime = 0;
	lastMetricsTime = 0;
	wasRegistered = false;
	executablePath = CProcessSpawner::GetExecutablePath();
}

CNodeAgent::~CNodeAgent()
{
	Shutdown();
	delete mutex;
}

void CNodeAgent::SetExecutablePath(const string &path)
{
	executablePath = path;
}

void CNodeAgent::SetConfigPath(const string &path)
{
	configPath = path;
}

void CNodeAgent::SetConfiguredRoles(const vector<string> &roles)
{
	configuredRoles = roles;
}

void CNodeAgent::SetClusterConfig(const CClusterConfig *config)
{
	clusterConfig = config;
}

void CNodeAgent::AddRoleArgs(const string &roleName, const vector<string> &args)
{
	roleArgs[roleName] = args;
}

void CNodeAgent::SetSpawnEnvironment(const vector<string> &envVars)
{
	spawnEnvVars = envVars;
}

void CNodeAgent::SetPidFileDirectory(const string &dir)
{
	pidFileDir = dir;

	// Ensure directory exists
	std::error_code ec;
	std::filesystem::create_directories(dir, ec);
}

void CNodeAgent::WritePidFile(const string &roleName, pid_t pid)
{
	if (pidFileDir.empty())
		return;

	string path = pidFileDir + "/" + roleName + ".pid";
	ofstream f(path);
	if (f.is_open())
	{
		f << pid;
		f.close();
		LOGD("CNodeAgent: wrote PID file %s (pid=%d)", path.c_str(), (int)pid);
	}
}

void CNodeAgent::RemovePidFile(const string &roleName)
{
	if (pidFileDir.empty())
		return;

	string path = pidFileDir + "/" + roleName + ".pid";
	std::error_code ec;
	std::filesystem::remove(path, ec);
}

void CNodeAgent::ReAdoptProcesses()
{
	if (pidFileDir.empty())
		return;

	LOGM("CNodeAgent[%s]: scanning PID files in %s for re-adoption", nodeId.c_str(), pidFileDir.c_str());

	std::error_code ec;
	for (auto &entry : std::filesystem::directory_iterator(pidFileDir, ec))
	{
		if (!entry.is_regular_file())
			continue;

		string filename = entry.path().filename().string();
		if (filename.size() < 5 || filename.substr(filename.size() - 4) != ".pid")
			continue;

		string roleName = filename.substr(0, filename.size() - 4);

		// Read PID from file
		ifstream f(entry.path());
		if (!f.is_open())
			continue;

		pid_t pid = -1;
		f >> pid;
		f.close();

		if (pid <= 0)
		{
			RemovePidFile(roleName);
			continue;
		}

		// Check if process is still running
		if (CProcessSpawner::IsRunning(pid))
		{
			LOGM("CNodeAgent[%s]: re-adopting running process role=%s pid=%d",
				 nodeId.c_str(), roleName.c_str(), (int)pid);

			mutex->Lock();
			SManagedProcess proc;
			proc.roleName = roleName;
			proc.pid = pid;
			proc.startTime = time(NULL);  // approximate
			proc.healthStatus = "unknown";

			// Distinguish game processes from role processes
			if (roleName.substr(0, 5) == "game:")
			{
				string gameId = roleName.substr(5);
				gameProcesses[gameId] = proc;
			}
			else
			{
				managedProcesses[roleName] = proc;
			}
			mutex->Unlock();
		}
		else
		{
			LOGM("CNodeAgent[%s]: stale PID file role=%s pid=%d (not running), removing",
				 nodeId.c_str(), roleName.c_str(), (int)pid);
			RemovePidFile(roleName);
		}
	}
}

void CNodeAgent::SendCrashReportToRegistry(const string &roleName, int exitCode, const string &gameId)
{
	if (registryClient && registryClient->isRegistered)
	{
		registryClient->SendCrashReport(roleName, exitCode, gameId);
	}
}

bool CNodeAgent::ConnectToRegistry(const string &address, int port, const string &internalSecret,
								   const string &nodeIp, int agentPort, int maxProcessCapacity)
{
	if (registryClient)
	{
		LOGWarning("CNodeAgent: ConnectToRegistry called but already connected");
		return false;
	}

	registryClient = new CRegistryClient(address, port,
		RegistryClientRole::NODE_AGENT, "node-agent-" + nodeId, internalSecret);
	registryClient->nodeAgentCallback = this;
	registryClient->RegisterAsNodeAgent(nodeId, nodeIp, agentPort, maxProcessCapacity);
	registryClient->Connect();
	return true;
}

bool CNodeAgent::WaitForRegistration(int timeoutMs)
{
	if (!registryClient)
		return false;

	int elapsed = 0;
	while (elapsed < timeoutMs && !registryClient->isConnected)
	{
		SYS_Sleep(50);
		elapsed += 50;
	}
	if (!registryClient->isConnected)
		return false;

	elapsed = 0;
	while (elapsed < timeoutMs && !registryClient->isRegistered)
	{
		registryClient->Update();
		SYS_Sleep(50);
		elapsed += 50;
	}
	return registryClient->isRegistered;
}

void CNodeAgent::Update()
{
	// Process reconciliation (detect crashes)
	mutex->Lock();
	ReconcileProcesses();
	mutex->Unlock();

	// Periodic health checks
	time_t now = time(NULL);
	if (difftime(now, lastHealthCheckTime) >= MIN_HEALTH_CHECK_INTERVAL)
	{
		mutex->Lock();
		CheckHealthAll();
		mutex->Unlock();
		lastHealthCheckTime = now;
	}

	// Reconcile game processes (remove exited ones)
	mutex->Lock();
	for (auto it = gameProcesses.begin(); it != gameProcesses.end(); )
	{
		if (CProcessSpawner::IsRunning(it->second.pid))
		{
			++it;
			continue;
		}
		int exitCode = 0;
		CProcessSpawner::TryWait(it->second.pid, &exitCode);
		LOGM("CNodeAgent[%s]: game process '%s' pid=%d exited (code=%d)",
			 nodeId.c_str(), it->first.c_str(), (int)it->second.pid, exitCode);

		// Phase 6: Send crash report with gameId
		SendCrashReportToRegistry("game", exitCode, it->first);

		// Phase 6: Clean up PID file
		RemovePidFile("game:" + it->first);

		it = gameProcesses.erase(it);
	}
	mutex->Unlock();

	// Auto-restart crashed processes per policy
	mutex->Lock();
	AutoRestartCrashedProcesses();
	mutex->Unlock();

	// Collect system metrics periodically
	if (difftime(now, lastMetricsTime) >= METRICS_COLLECT_INTERVAL)
	{
		cachedMetrics = CollectSystemMetrics();
		lastMetricsTime = now;
	}

	// Update registry client (sends heartbeats)
	if (registryClient)
	{
		int processCount = GetTotalProcessCount();
		registryClient->SetNodeAgentProcessCount(processCount);

		// Pass collected metrics to registry client (sent via heartbeat in Update())
		if (!cachedMetrics.empty())
			registryClient->SetNodeAgentMetrics(cachedMetrics);

		registryClient->Update();

		// Reconnect reconciliation: if we just re-registered, send current state
		bool isNowRegistered = registryClient->isRegistered;
		if (isNowRegistered && !wasRegistered)
		{
			LOGM("CNodeAgent[%s]: reconnected to registry, sending reconciliation heartbeat (processes=%d)",
				 nodeId.c_str(), processCount);
		}
		wasRegistered = isNowRegistered;
	}
}

// --- Process Lifecycle ---

void CNodeAgent::ReconcileProcesses()
{
	for (auto it = managedProcesses.begin(); it != managedProcesses.end(); )
	{
		if (CProcessSpawner::IsRunning(it->second.pid))
		{
			++it;
			continue;
		}

		int exitCode = 0;
		CProcessSpawner::TryWait(it->second.pid, &exitCode);

		LOGWarning("CNodeAgent[%s]: role '%s' pid=%d exited (code=%d, restarts=%d)",
			nodeId.c_str(), it->first.c_str(), (int)it->second.pid, exitCode, it->second.restartCount);

		// Phase 6: Send crash report to Registry
		SendCrashReportToRegistry(it->first, exitCode);

		// Phase 6: Clean up PID file
		RemovePidFile(it->first);

		// Record as crashed for potential auto-restart
		SCrashedRole crashed;
		crashed.roleName = it->first;
		crashed.crashTime = time(NULL);
		crashed.exitCode = exitCode;
		crashed.priorRestartCount = it->second.restartCount;
		crashed.priorFirstRestartInWindow = it->second.firstRestartInWindow;
		crashedRoles.push_back(crashed);

		it = managedProcesses.erase(it);
	}
}

void CNodeAgent::CheckHealthAll()
{
	time_t now = time(NULL);

	for (auto &pair : managedProcesses)
	{
		SManagedProcess &proc = pair.second;
		SRolePolicyConfig policy = GetRolePolicy(pair.first);

		// Check if enough time has passed since last health check for this role
		double intervalSec = (double)policy.health.intervalMs / 1000.0;
		if (difftime(now, proc.lastHealthCheck) < intervalSec)
			continue;

		bool healthy = PerformHealthCheck(proc, policy.health);
		proc.lastHealthCheck = now;

		if (healthy)
		{
			if (proc.healthStatus != "healthy" && proc.healthStatus != "starting")
			{
				LOGM("CNodeAgent[%s]: role '%s' pid=%d is now healthy",
					 nodeId.c_str(), pair.first.c_str(), (int)proc.pid);
			}
			proc.healthStatus = "healthy";
			proc.consecutiveHealthFailures = 0;
		}
		else
		{
			proc.consecutiveHealthFailures++;
			if (proc.consecutiveHealthFailures >= HEALTH_FAILURE_THRESHOLD)
			{
				if (proc.healthStatus != "unhealthy")
				{
					LOGWarning("CNodeAgent[%s]: role '%s' pid=%d marked unhealthy (%d consecutive failures)",
						nodeId.c_str(), pair.first.c_str(), (int)proc.pid, proc.consecutiveHealthFailures);
				}
				proc.healthStatus = "unhealthy";
			}
		}
	}
}

void CNodeAgent::AutoRestartCrashedProcesses()
{
	time_t now = time(NULL);

	for (auto it = crashedRoles.begin(); it != crashedRoles.end(); )
	{
		if (!ShouldAutoRestart(it->roleName, *it))
		{
			LOGM("CNodeAgent[%s]: role '%s' will NOT be auto-restarted (policy or max restarts reached)",
				 nodeId.c_str(), it->roleName.c_str());
			it = crashedRoles.erase(it);
			continue;
		}

		// Calculate backoff
		SRolePolicyConfig policy = GetRolePolicy(it->roleName);
		int backoffMs = CalculateBackoffMs(it->priorRestartCount, policy.restart);
		double elapsedSincecrash = difftime(now, it->crashTime) * 1000.0;
		if (elapsedSincecrash < backoffMs)
		{
			++it; // Not yet time to restart
			continue;
		}

		// Attempt restart
		string reason;
		LOGM("CNodeAgent[%s]: auto-restarting role '%s' (attempt %d, backoff %dms)",
			 nodeId.c_str(), it->roleName.c_str(), it->priorRestartCount + 1, backoffMs);

		// Temporarily store restart state
		int priorRestartCount = it->priorRestartCount;
		time_t priorFirstRestart = it->priorFirstRestartInWindow;
		string roleName = it->roleName;
		it = crashedRoles.erase(it);

		if (StartRole(roleName, reason))
		{
			// Update restart tracking on the new process
			auto procIt = managedProcesses.find(roleName);
			if (procIt != managedProcesses.end())
			{
				procIt->second.restartCount = priorRestartCount + 1;
				if (priorFirstRestart == 0)
					procIt->second.firstRestartInWindow = time(NULL);
				else
					procIt->second.firstRestartInWindow = priorFirstRestart;
			}
		}
		else
		{
			LOGError("CNodeAgent[%s]: auto-restart of role '%s' failed: %s",
				nodeId.c_str(), roleName.c_str(), reason.c_str());
		}
	}
}

// --- Process Management ---

bool CNodeAgent::StartRole(const string &roleName, string &reason)
{
	if (!IsRoleSupported(roleName))
	{
		reason = "unsupported_role";
		return false;
	}

	auto it = managedProcesses.find(roleName);
	if (it != managedProcesses.end() && CProcessSpawner::IsRunning(it->second.pid))
	{
		reason = "already_running";
		return true;
	}
	if (it != managedProcesses.end())
		managedProcesses.erase(it);

	vector<string> args = BuildProcessArgs(roleName);
	if (args.empty())
	{
		reason = "no_args_configured";
		return false;
	}

	pid_t pid;
	if (!spawnEnvVars.empty())
		pid = CProcessSpawner::SpawnWithEnv(args, spawnEnvVars);
	else
		pid = CProcessSpawner::Spawn(args);

	if (pid <= 0)
	{
		reason = "spawn_failed";
		return false;
	}

	// Brief stability check — verify process didn't exit immediately
	bool stayedRunning = false;
	for (int i = 0; i < 10; i++)
	{
		if (CProcessSpawner::IsRunning(pid))
		{
			stayedRunning = true;
			break;
		}
		SYS_Sleep(50);
	}
	if (!stayedRunning)
	{
		int exitCode = 0;
		CProcessSpawner::TryWait(pid, &exitCode);
		reason = "process_exited_early";
		return false;
	}

	SManagedProcess managed;
	managed.roleName = roleName;
	managed.pid = pid;
	managed.startTime = time(NULL);
	managed.healthStatus = "starting";
	managedProcesses[roleName] = managed;

	// Phase 6: Write PID file for process re-discovery
	WritePidFile(roleName, pid);

	reason = "ok";
	LOGM("CNodeAgent[%s]: started role=%s pid=%d", nodeId.c_str(), roleName.c_str(), (int)pid);
	return true;
}

bool CNodeAgent::StopRole(const string &roleName, bool graceful, string &reason)
{
	auto it = managedProcesses.find(roleName);
	if (it == managedProcesses.end())
	{
		reason = "not_running";
		return true;
	}

	pid_t pid = it->second.pid;
	if (!CProcessSpawner::IsRunning(pid))
	{
		int exitCode = 0;
		CProcessSpawner::TryWait(pid, &exitCode);
		managedProcesses.erase(it);
		RemovePidFile(roleName);
		reason = "already_stopped";
		return true;
	}

	// Also remove from crashed list if present (prevent auto-restart of something we're stopping)
	for (auto cit = crashedRoles.begin(); cit != crashedRoles.end(); )
	{
		if (cit->roleName == roleName)
			cit = crashedRoles.erase(cit);
		else
			++cit;
	}

	if (graceful)
		CProcessSpawner::Kill(pid);  // SIGTERM
	else
		CProcessSpawner::ForceKill(pid);  // SIGKILL

	// Wait for graceful shutdown
	int waitLoops = graceful ? 40 : 8;  // 4s graceful, 0.8s forced
	for (int i = 0; i < waitLoops; i++)
	{
		int exitCode = 0;
		if (CProcessSpawner::TryWait(pid, &exitCode))
		{
			managedProcesses.erase(it);
			RemovePidFile(roleName);
			reason = "ok";
			LOGM("CNodeAgent[%s]: stopped role=%s pid=%d exitCode=%d",
				nodeId.c_str(), roleName.c_str(), (int)pid, exitCode);
			return true;
		}
		SYS_Sleep(100);
	}

	// Force kill if graceful failed
	if (graceful)
	{
		CProcessSpawner::ForceKill(pid);
		for (int i = 0; i < 20; i++)
		{
			int exitCode = 0;
			if (CProcessSpawner::TryWait(pid, &exitCode))
			{
				managedProcesses.erase(it);
				RemovePidFile(roleName);
				reason = "ok";
				return true;
			}
			SYS_Sleep(50);
		}
	}

	reason = "stop_timeout";
	return false;
}

int CNodeAgent::GetManagedProcessCount()
{
	mutex->Lock();
	ReconcileProcesses();
	int count = (int)managedProcesses.size();
	mutex->Unlock();
	return count;
}

int CNodeAgent::GetTotalProcessCount()
{
	mutex->Lock();
	ReconcileProcesses();
	int count = (int)managedProcesses.size() + (int)gameProcesses.size();
	mutex->Unlock();
	return count;
}

json CNodeAgent::GetProcessSnapshot()
{
	mutex->Lock();
	json snapshot = json::array();
	for (const auto &pair : managedProcesses)
	{
		const SManagedProcess &proc = pair.second;
		json entry;
		entry["roleName"] = pair.first;
		entry["pid"] = (int)proc.pid;
		entry["healthStatus"] = proc.healthStatus;
		entry["restartCount"] = proc.restartCount;
		entry["uptimeSeconds"] = (int64_t)difftime(time(NULL), proc.startTime);
		snapshot.push_back(entry);
	}
	for (const auto &pair : gameProcesses)
	{
		const SManagedProcess &proc = pair.second;
		json entry;
		entry["roleName"] = "game:" + pair.first;
		entry["gameId"] = pair.first;
		entry["pid"] = (int)proc.pid;
		entry["healthStatus"] = proc.healthStatus;
		entry["restartCount"] = 0;
		entry["uptimeSeconds"] = (int64_t)difftime(time(NULL), proc.startTime);
		snapshot.push_back(entry);
	}
	mutex->Unlock();
	return snapshot;
}

void CNodeAgent::Shutdown()
{
	// Stop all managed processes
	mutex->Lock();
	vector<string> roleNames;
	for (const auto &pair : managedProcesses)
		roleNames.push_back(pair.first);
	mutex->Unlock();

	for (const string &roleName : roleNames)
	{
		string reason;
		mutex->Lock();
		StopRole(roleName, true, reason);
		mutex->Unlock();
	}

	// Stop all game processes
	mutex->Lock();
	for (auto &pair : gameProcesses)
	{
		if (pair.second.pid > 0 && CProcessSpawner::IsRunning(pair.second.pid))
		{
			LOGM("CNodeAgent[%s]: shutting down game process '%s' pid=%d",
				 nodeId.c_str(), pair.first.c_str(), (int)pair.second.pid);
			CProcessSpawner::Kill(pair.second.pid);
			CProcessSpawner::TryWait(pair.second.pid, nullptr);
		}
	}
	gameProcesses.clear();
	mutex->Unlock();

	// Disconnect from registry
	if (registryClient)
	{
		registryClient->Shutdown();
		delete registryClient;
		registryClient = NULL;
	}
}

// --- CRegistryClientNodeAgentCallback ---

void CNodeAgent::RegistryManageServices(const string &operation, uint64_t commandId,
										const string &scope, const string &targetNodeId,
										const string &roleName, bool graceful, bool includeRegistry)
{
	mutex->Lock();
	ReconcileProcesses();

	if (!targetNodeId.empty() && targetNodeId != nodeId)
	{
		LOGWarning("CNodeAgent[%s]: ignoring commandId=%llu targeted to %s",
			nodeId.c_str(), (unsigned long long)commandId, targetNodeId.c_str());
		if (registryClient)
			registryClient->SendNodeAgentCommandResult(commandId, false, "target_node_mismatch");
		mutex->Unlock();
		return;
	}

	LOGM("CNodeAgent[%s]: commandId=%llu operation=%s scope=%s role=%s graceful=%s includeRegistry=%s",
		 nodeId.c_str(), (unsigned long long)commandId,
		 operation.c_str(), scope.c_str(), roleName.c_str(),
		 graceful ? "true" : "false", includeRegistry ? "true" : "false");

	vector<string> targetRoles = ResolveTargetRoles(operation, roleName, includeRegistry);
	if (targetRoles.empty())
	{
		if (registryClient)
			registryClient->SendNodeAgentCommandResult(commandId, false, "no_target_roles");
		mutex->Unlock();
		return;
	}

	bool allOk = true;
	int failedCount = 0;
	int skippedCount = 0;
	string firstFailureReason = "ok";

	for (const string &targetRole : targetRoles)
	{
		if (targetRole == "registry")
		{
			skippedCount++;
			continue;
		}

		if (!IsRoleSupported(targetRole))
		{
			if (!roleName.empty())
			{
				allOk = false;
				failedCount++;
				if (firstFailureReason == "ok")
					firstFailureReason = "unsupported_role";
			}
			else
			{
				skippedCount++;
			}
			continue;
		}

		string roleReason;
		bool roleOk = false;
		if (operation == "start")
			roleOk = StartRole(targetRole, roleReason);
		else if (operation == "stop")
			roleOk = StopRole(targetRole, graceful, roleReason);
		else
		{
			roleReason = "invalid_operation";
			roleOk = false;
		}

		if (!roleOk)
		{
			allOk = false;
			failedCount++;
			if (firstFailureReason == "ok")
				firstFailureReason = roleReason;
		}
	}

	if (failedCount == 0 && skippedCount == (int)targetRoles.size())
	{
		allOk = false;
		firstFailureReason = "no_supported_roles";
	}

	string resultReason = allOk ? "ok" : firstFailureReason;
	if (registryClient)
		registryClient->SendNodeAgentCommandResult(commandId, allOk, resultReason);

	mutex->Unlock();
}

void CNodeAgent::RegistrySpawnGameServer(uint64_t requestId, const string &gameId, int port,
	const string &registryAddress, int registryPort, const json &extraArgs)
{
	LOGM("CNodeAgent[%s]: RegistrySpawnGameServer requestId=%llu gameId=%s port=%d",
		 nodeId.c_str(), (unsigned long long)requestId, gameId.c_str(), port);

	mutex->Lock();

	// Check if game process already exists for this gameId
	auto existing = gameProcesses.find(gameId);
	if (existing != gameProcesses.end())
	{
		if (CProcessSpawner::IsRunning(existing->second.pid))
		{
			LOGM("CNodeAgent[%s]: game process already running for gameId=%s pid=%d",
				 nodeId.c_str(), gameId.c_str(), (int)existing->second.pid);
			mutex->Unlock();
			if (registryClient)
				registryClient->SendGameProcessSpawned(requestId, gameId, true, (int)existing->second.pid, "already_running");
			return;
		}
		// Dead process, clean up
		CProcessSpawner::TryWait(existing->second.pid, nullptr);
		gameProcesses.erase(existing);
	}

	// Build game server process args
	string resolvedExecPath = executablePath;
	if (resolvedExecPath.empty())
		resolvedExecPath = CProcessSpawner::GetExecutablePath();

	vector<string> args = {
		resolvedExecPath,
		"--game-server",
		"--game-id", gameId,
		"--port", to_string(port),
		"--registry-address", registryAddress,
		"--registry-port", to_string(registryPort),
		"--headless"
	};

	if (!configPath.empty())
	{
		args.push_back("--app-path");
		args.push_back(configPath);
	}

	// Spawn the process
	pid_t pid = CProcessSpawner::SpawnWithEnv(args, spawnEnvVars);
	if (pid <= 0)
	{
		LOGError("CNodeAgent[%s]: failed to spawn game server for gameId=%s", nodeId.c_str(), gameId.c_str());
		mutex->Unlock();
		if (registryClient)
			registryClient->SendGameProcessSpawned(requestId, gameId, false, 0, "spawn_failed");
		return;
	}

	// Track the game process
	SManagedProcess proc;
	proc.roleName = "game:" + gameId;
	proc.pid = pid;
	proc.startTime = time(NULL);
	proc.healthStatus = "starting";
	gameProcesses[gameId] = proc;

	// Phase 6: Write PID file for game process
	WritePidFile("game:" + gameId, pid);

	LOGM("CNodeAgent[%s]: spawned game server for gameId=%s pid=%d port=%d",
		 nodeId.c_str(), gameId.c_str(), pid, port);

	mutex->Unlock();

	if (registryClient)
		registryClient->SendGameProcessSpawned(requestId, gameId, true, pid, "ok");
}

// --- Role Support ---

bool CNodeAgent::IsRoleSupported(const string &roleName) const
{
	return roleArgs.find(roleName) != roleArgs.end();
}

vector<string> CNodeAgent::ResolveTargetRoles(const string &operation, const string &roleName, bool includeRegistry)
{
	vector<string> targetRoles;
	if (!roleName.empty())
	{
		targetRoles.push_back(roleName);
		return targetRoles;
	}

	// For stop: include currently running roles
	if (operation == "stop")
	{
		for (const auto &pair : managedProcesses)
			targetRoles.push_back(pair.first);
	}

	// Add configured roles (skip meta-roles)
	for (const string &configuredRole : configuredRoles)
	{
		if (configuredRole == "registry" || configuredRole == "nodeagent" || configuredRole == "game")
			continue;
		if (std::find(targetRoles.begin(), targetRoles.end(), configuredRole) == targetRoles.end())
			targetRoles.push_back(configuredRole);
	}

	if (includeRegistry && operation == "stop")
	{
		if (std::find(targetRoles.begin(), targetRoles.end(), "registry") == targetRoles.end())
			targetRoles.push_back("registry");
	}

	return targetRoles;
}

vector<string> CNodeAgent::BuildProcessArgs(const string &roleName)
{
	auto it = roleArgs.find(roleName);
	if (it != roleArgs.end())
		return it->second;

	// No configured args for this role
	return {};
}

// --- Health Checks ---

bool CNodeAgent::PerformHealthCheck(const SManagedProcess &proc, const SHealthCheckConfig &config)
{
	switch (config.type)
	{
		case EHealthCheckType::PROCESS:
			return CProcessSpawner::IsRunning(proc.pid);

		case EHealthCheckType::TCP:
			if (config.port <= 0)
				return CProcessSpawner::IsRunning(proc.pid);
			return CheckTcpHealth(config.port, 2000);

		case EHealthCheckType::HTTP:
		{
			if (config.port <= 0 || config.url.empty())
				return CProcessSpawner::IsRunning(proc.pid);
			return CheckHttpHealth(config.port, config.url, 3000);
		}
	}
	return CProcessSpawner::IsRunning(proc.pid);
}

bool CNodeAgent::CheckTcpHealth(int port, int timeoutMs)
{
#ifdef _WIN32
	// Windows: not implemented yet, fall back to true
	return true;
#else
	int sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock < 0) return false;

	// Set non-blocking
	int flags = fcntl(sock, F_GETFL, 0);
	fcntl(sock, F_SETFL, flags | O_NONBLOCK);

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	int ret = connect(sock, (struct sockaddr *)&addr, sizeof(addr));
	if (ret == 0)
	{
		close(sock);
		return true;
	}
	if (errno != EINPROGRESS)
	{
		close(sock);
		return false;
	}

	struct pollfd pfd;
	pfd.fd = sock;
	pfd.events = POLLOUT;
	ret = poll(&pfd, 1, timeoutMs);

	bool connected = false;
	if (ret > 0 && (pfd.revents & POLLOUT))
	{
		int err = 0;
		socklen_t len = sizeof(err);
		getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &len);
		connected = (err == 0);
	}

	close(sock);
	return connected;
#endif
}

bool CNodeAgent::CheckHttpHealth(int port, const string &path, int timeoutMs)
{
#ifdef _WIN32
	return true;
#else
	int sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock < 0) return false;

	// Set timeout
	struct timeval tv;
	tv.tv_sec = timeoutMs / 1000;
	tv.tv_usec = (timeoutMs % 1000) * 1000;
	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0)
	{
		close(sock);
		return false;
	}

	// Determine the path portion from the URL
	// If path starts with http://, extract just the path component
	string httpPath = path;
	if (httpPath.find("http://") == 0 || httpPath.find("https://") == 0)
	{
		size_t pathStart = httpPath.find('/', httpPath.find("://") + 3);
		if (pathStart != string::npos)
			httpPath = httpPath.substr(pathStart);
		else
			httpPath = "/";
	}

	string request = "GET " + httpPath + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
	if (send(sock, request.c_str(), request.size(), 0) < 0)
	{
		close(sock);
		return false;
	}

	char buf[512];
	ssize_t n = recv(sock, buf, sizeof(buf) - 1, 0);
	close(sock);

	if (n <= 0) return false;
	buf[n] = '\0';

	// Check for HTTP 200 response
	string response(buf, n);
	return (response.find("HTTP/") == 0 && response.find(" 200 ") != string::npos);
#endif
}

// --- Restart Policy ---

bool CNodeAgent::ShouldAutoRestart(const string &roleName, const SCrashedRole &crashed)
{
	SRolePolicyConfig policy = GetRolePolicy(roleName);

	switch (policy.restart.mode)
	{
		case ERestartPolicy::NEVER:
			return false;

		case ERestartPolicy::ON_FAILURE:
			if (crashed.exitCode == 0)
				return false; // Clean exit, don't restart
			break;

		case ERestartPolicy::ALWAYS:
			break; // Always restart
	}

	// Check restart window limits
	time_t now = time(NULL);
	time_t windowStart = crashed.priorFirstRestartInWindow;
	if (windowStart > 0 && difftime(now, windowStart) > policy.restart.windowSeconds)
	{
		// Window has expired, reset counter — this restart starts a new window
		return true;
	}

	if (crashed.priorRestartCount >= policy.restart.maxRestartsPerWindow)
	{
		LOGWarning("CNodeAgent[%s]: role '%s' reached max restarts (%d) in window",
			nodeId.c_str(), roleName.c_str(), policy.restart.maxRestartsPerWindow);
		return false;
	}

	return true;
}

int CNodeAgent::CalculateBackoffMs(int restartCount, const SRestartPolicyConfig &policy)
{
	if (restartCount <= 0)
		return policy.backoffBaseMs;

	// Cap exponent to prevent overflow/NaN from pow() with large restart counts
	int cappedCount = min(restartCount, 20);
	double backoff = (double)policy.backoffBaseMs * pow(policy.backoffMultiplier, cappedCount);
	if (!isfinite(backoff) || backoff > (double)policy.backoffMaxMs)
		return policy.backoffMaxMs;
	int result = (int)backoff;
	if (result < policy.backoffBaseMs)
		result = policy.backoffBaseMs;
	return result;
}

SRolePolicyConfig CNodeAgent::GetRolePolicy(const string &roleName) const
{
	if (clusterConfig)
	{
		const auto &policies = clusterConfig->GetRolePolicies();
		auto it = policies.find(roleName);
		if (it != policies.end())
			return it->second;
	}
	// Return defaults
	return SRolePolicyConfig();
}

// --- System Metrics ---

json CNodeAgent::CollectSystemMetrics()
{
	json metrics;
	metrics["uptimeSeconds"] = (int64_t)difftime(time(NULL), startTime);

#if defined(__APPLE__) && !defined(_WIN32)
	// Memory usage via Mach APIs
	mach_port_t host = mach_host_self();
	vm_size_t pageSize;
	host_page_size(host, &pageSize);

	vm_statistics64_data_t vmStats;
	mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
	if (host_statistics64(host, HOST_VM_INFO64, (host_info64_t)&vmStats, &count) == KERN_SUCCESS)
	{
		uint64_t used = ((uint64_t)vmStats.active_count + vmStats.wire_count) * pageSize;
		uint64_t total = used + ((uint64_t)vmStats.free_count + vmStats.inactive_count) * pageSize;
		if (total > 0)
		{
			metrics["memoryUsedPercent"] = (int)((double)used / total * 100.0);
			metrics["memoryUsedMB"] = (int)(used / (1024 * 1024));
		}
	}

	double loadAvg[3];
	if (getloadavg(loadAvg, 3) > 0)
	{
		metrics["loadAvg1m"] = loadAvg[0];
		metrics["loadAvg5m"] = loadAvg[1];
	}

#elif defined(__linux__)
	// Memory from /proc/meminfo
	FILE *f = fopen("/proc/meminfo", "r");
	if (f)
	{
		long memTotal = 0, memAvailable = 0;
		char line[256];
		while (fgets(line, sizeof(line), f))
		{
			if (sscanf(line, "MemTotal: %ld kB", &memTotal) == 1) {}
			if (sscanf(line, "MemAvailable: %ld kB", &memAvailable) == 1) {}
		}
		fclose(f);
		if (memTotal > 0)
		{
			long memUsed = memTotal - memAvailable;
			metrics["memoryUsedPercent"] = (int)((double)memUsed / memTotal * 100.0);
			metrics["memoryUsedMB"] = (int)(memUsed / 1024);
		}
	}

	double loadAvg[3];
	if (getloadavg(loadAvg, 3) > 0)
	{
		metrics["loadAvg1m"] = loadAvg[0];
		metrics["loadAvg5m"] = loadAvg[1];
	}
#endif

	// Add per-process snapshot
	metrics["processes"] = GetProcessSnapshot();
	return metrics;
}
