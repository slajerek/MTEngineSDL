#pragma once

#include "json.hpp"
#include <string>
#include <map>
#include <vector>

using namespace std;
using namespace nlohmann;

// Runtime mode enum
enum class ERuntimeMode
{
	CLUSTER,       // Full distributed cluster mode (multi-VPS)
	LOCAL_DEV,     // Local development mode (in-process servers on localhost)
	HOTSEAT        // Hotseat mode (single machine, no infrastructure)
};

// Health check type enum
enum class EHealthCheckType
{
	PROCESS,    // Check if process is running
	TCP,        // Check if TCP port is listening
	HTTP        // Check HTTP endpoint for readiness
};

// Restart policy enum
enum class ERestartPolicy
{
	NEVER,
	ON_FAILURE,
	ALWAYS
};

// --- Config structs matching local-dev.hjson schema ---

// runtime: { mode: "local-dev" }
struct SRuntimeConfig
{
	ERuntimeMode mode = ERuntimeMode::LOCAL_DEV;
};

// localDev: { autoBootstrap, launchMode, clusterSimEnabled, storageProfile }
struct SLocalDevConfig
{
	bool autoBootstrap = true;
	string launchMode = "IN_PROCESS";
	bool clusterSimEnabled = false;
	string storageProfile = "local-fs";
};

// nodes[]: { id, address, roles[], maxGameProcesses, portRange[] }
struct SNodeConfig
{
	string id;
	string address;
	vector<string> roles;
	int maxGameProcesses = 25;
	int portRangeStart = 0;
	int portRangeEnd = 0;
};

// registry: { address, port }
struct SRegistryConfig
{
	string address = "127.0.0.1";
	int port = 14650;
};

// servicePorts: { registry, lobby, agentLocalHealth, gameRangeDefault[], auth, assets }
struct SServicePortsConfig
{
	int registry = 14650;
	int lobby = 14651;
	int agentLocalHealth = 14600;
	int gameRangeDefaultStart = 14701;
	int gameRangeDefaultEnd = 14725;
	int auth = 14880;
	int assets = 14900;
};

// storage: { kind, basePath, endpoint, bucket, prefix, accessKey, secretKey }
struct SStorageConfig
{
	string kind = "local-fs";
	string basePath = "./data/dev-game-state";
	string endpoint;
	string bucket;
	string prefix;
	string accessKey;
	string secretKey;
};

// compatibility: { protocolVersion, policy, defaultLocale, versionMismatchMessagesByLocale }
struct SCompatibilityConfig
{
	int protocolVersion = 1;
	string policy = "exact-match";
	string defaultLocale = "en";
	map<string, string> versionMismatchMessagesByLocale;
};

// Health check for a role policy
struct SHealthCheckConfig
{
	EHealthCheckType type = EHealthCheckType::PROCESS;
	int port = 0;
	string url;
	int intervalMs = 2000;
};

// Restart policy for a role policy
struct SRestartPolicyConfig
{
	ERestartPolicy mode = ERestartPolicy::NEVER;
	int maxRestartsPerWindow = 5;      // Max restarts before giving up
	int windowSeconds = 300;            // Time window for restart counting (5 min)
	int backoffBaseMs = 1000;           // Base backoff delay (1 sec)
	double backoffMultiplier = 2.0;     // Exponential backoff multiplier
	int backoffMaxMs = 30000;           // Max backoff delay (30 sec)
};

// rolePolicies: { roleName: { restart: {...}, health: {...} } }
struct SRolePolicyConfig
{
	SRestartPolicyConfig restart;
	SHealthCheckConfig health;
};

// Localized admission messages for a service endpoint
struct SAdmissionEndpointConfig
{
	bool maintenanceEnabled = false;
	string rejectMessageKey;
	map<string, string> rejectMessagesByLocale;
	string rejectMessageFallback;
	string disconnectMessageKey;
	map<string, string> disconnectMessagesByLocale;
	string disconnectMessageFallback;
};

// gameServer: { heartbeatTimeoutSeconds, heartbeatWarningSeconds, periodicSaveIntervalSeconds, portRangeStart, portRangeEnd }
struct SGameServerConfig
{
	int heartbeatTimeoutSeconds = 15;
	int heartbeatWarningSeconds = 8;
	int periodicSaveIntervalSeconds = 60;
	int portRangeStart = 14701;
	int portRangeEnd = 14799;
};

// operations: { adminCommandTimeoutSec }
struct SOperationsConfig
{
	int adminCommandTimeoutSec = 20;
	int adminAuthMaxAttempts = 5;
};

// admission: { defaultLocale, localeSelection, lobby: {...}, game: {...} }
struct SAdmissionConfig
{
	string defaultLocale = "en";
	string localeSelection = "server-side";
	SAdmissionEndpointConfig lobby;
	SAdmissionEndpointConfig game;
};

// Main cluster configuration class — parses Hjson config matching local-dev.hjson schema
class CClusterConfig
{
public:
	CClusterConfig();
	~CClusterConfig();

	// Load from Hjson file (uses Hjson parser, not json::parse)
	bool LoadFromFile(const string &filePath);

	// Load from already-parsed nlohmann::json
	bool LoadFromJson(const json &j);

	// --- Accessors ---
	ERuntimeMode GetRuntimeMode() const { return runtime.mode; }
	const SLocalDevConfig &GetLocalDev() const { return localDev; }
	const vector<SNodeConfig> &GetNodes() const { return nodes; }
	const SRegistryConfig &GetRegistry() const { return registry; }
	const SServicePortsConfig &GetServicePorts() const { return servicePorts; }
	const SStorageConfig &GetStorage() const { return storage; }
	const SCompatibilityConfig &GetCompatibility() const { return compatibility; }
	const map<string, SRolePolicyConfig> &GetRolePolicies() const { return rolePolicies; }
	const SAdmissionConfig &GetAdmission() const { return admission; }
	const SGameServerConfig &GetGameServer() const { return gameServer; }
	const SOperationsConfig &GetOperations() const { return operations; }

	// Node lookup by id
	const SNodeConfig *GetNode(const string &nodeId) const;

	// Get localized message from a locale map, with fallback
	static string GetLocalizedMessage(const map<string, string> &messagesByLocale,
									  const string &locale,
									  const string &fallback);

	// Serialize summary to JSON (for admin dashboard)
	json ToJson() const;

	// Debug output
	void PrintSummary() const;

	// Conversion helpers
	static ERuntimeMode StringToRuntimeMode(const string &s);
	static string RuntimeModeToString(ERuntimeMode mode);
	static EHealthCheckType StringToHealthCheckType(const string &s);
	static ERestartPolicy StringToRestartPolicy(const string &s);

private:
	void ResetToDefaults();
	void ValidateOrThrow() const;
	static bool IsValidPort(int port);

	SRuntimeConfig runtime;
	SLocalDevConfig localDev;
	vector<SNodeConfig> nodes;
	SRegistryConfig registry;
	SServicePortsConfig servicePorts;
	SStorageConfig storage;
	SCompatibilityConfig compatibility;
	map<string, SRolePolicyConfig> rolePolicies;
	SAdmissionConfig admission;
	SGameServerConfig gameServer;
	SOperationsConfig operations;

	// Internal parsing helpers
	void ParseRuntime(const json &j);
	void ParseLocalDev(const json &j);
	void ParseNodes(const json &j);
	void ParseRegistry(const json &j);
	void ParseServicePorts(const json &j);
	void ParseStorage(const json &j);
	void ParseCompatibility(const json &j);
	void ParseRolePolicies(const json &j);
	void ParseAdmission(const json &j);
	static void ParseAdmissionEndpoint(const json &j, SAdmissionEndpointConfig &out);
	void ParseGameServer(const json &j);
	void ParseOperations(const json &j);
	static map<string, string> ParseLocaleMap(const json &j);
};
