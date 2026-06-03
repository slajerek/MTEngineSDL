#include "CClusterConfig.h"
#include "DBG_Log.h"
#include "hjson.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <set>
#include <stdexcept>

CClusterConfig::CClusterConfig()
{
	ResetToDefaults();
}

CClusterConfig::~CClusterConfig()
{
}

void CClusterConfig::ResetToDefaults()
{
	runtime = SRuntimeConfig();
	localDev = SLocalDevConfig();
	nodes.clear();
	registry = SRegistryConfig();
	servicePorts = SServicePortsConfig();
	storage = SStorageConfig();
	compatibility = SCompatibilityConfig();
	rolePolicies.clear();
	admission = SAdmissionConfig();
	gameServer = SGameServerConfig();
	operations = SOperationsConfig();
}

bool CClusterConfig::LoadFromFile(const string &filePath)
{
	// Read file contents
	ifstream file(filePath);
	if (!file.is_open())
	{
		LOGError("CClusterConfig::LoadFromFile: cannot open '%s'", filePath.c_str());
		return false;
	}

	string content((istreambuf_iterator<char>(file)), istreambuf_iterator<char>());
	file.close();

	// Parse Hjson → Hjson::Value
	Hjson::Value hjsonRoot;
	try
	{
		stringstream ss(content);
		ss >> hjsonRoot;
	}
	catch (const exception &e)
	{
		LOGError("CClusterConfig::LoadFromFile: Hjson parse error: %s", e.what());
		return false;
	}

	// Convert Hjson::Value → JSON string → nlohmann::json
	try
	{
		string jsonStr = Hjson::MarshalJson(hjsonRoot);
		json j = json::parse(jsonStr);
		return LoadFromJson(j);
	}
	catch (const exception &e)
	{
		LOGError("CClusterConfig::LoadFromFile: JSON conversion error: %s", e.what());
		return false;
	}
}

bool CClusterConfig::LoadFromJson(const json &j)
{
	try
	{
		ResetToDefaults();

		if (j.contains("runtime"))       ParseRuntime(j["runtime"]);
		if (j.contains("localDev"))      ParseLocalDev(j["localDev"]);
		if (j.contains("nodes"))         ParseNodes(j["nodes"]);
		if (j.contains("registry"))      ParseRegistry(j["registry"]);
		if (j.contains("servicePorts"))  ParseServicePorts(j["servicePorts"]);
		if (j.contains("storage"))       ParseStorage(j["storage"]);
		if (j.contains("compatibility")) ParseCompatibility(j["compatibility"]);
		if (j.contains("rolePolicies"))  ParseRolePolicies(j["rolePolicies"]);
		if (j.contains("admission"))     ParseAdmission(j["admission"]);
		if (j.contains("gameServer"))    ParseGameServer(j["gameServer"]);
		if (j.contains("operations"))    ParseOperations(j["operations"]);

		ValidateOrThrow();
		return true;
	}
	catch (const exception &e)
	{
		LOGError("CClusterConfig::LoadFromJson: %s", e.what());
		return false;
	}
}

void CClusterConfig::ParseRuntime(const json &j)
{
	if (j.contains("mode"))
	{
		string mode = j["mode"].get<string>();
		if (mode == "cluster")
			runtime.mode = ERuntimeMode::CLUSTER;
		else if (mode == "local-dev")
			runtime.mode = ERuntimeMode::LOCAL_DEV;
		else if (mode == "hotseat")
			runtime.mode = ERuntimeMode::HOTSEAT;
		else
			throw runtime_error("runtime.mode: invalid value '" + mode + "'");
	}
}

void CClusterConfig::ParseLocalDev(const json &j)
{
	if (j.contains("autoBootstrap"))
		localDev.autoBootstrap = j["autoBootstrap"].get<bool>();
	if (j.contains("launchMode"))
		localDev.launchMode = j["launchMode"].get<string>();
	if (j.contains("clusterSimEnabled"))
		localDev.clusterSimEnabled = j["clusterSimEnabled"].get<bool>();
	if (j.contains("storageProfile"))
		localDev.storageProfile = j["storageProfile"].get<string>();
}

void CClusterConfig::ParseNodes(const json &j)
{
	if (!j.is_array())
		throw runtime_error("nodes: expected array");

	set<string> seenNodeIds;
	nodes.clear();
	for (const auto &nodeJson : j)
	{
		SNodeConfig node;
		node.id = nodeJson.value("id", "");
		node.address = nodeJson.value("address", "");
		if (node.id.empty())
			throw runtime_error("nodes[].id: cannot be empty");
		if (node.address.empty())
			throw runtime_error("nodes['" + node.id + "'].address: cannot be empty");
		if (!seenNodeIds.insert(node.id).second)
			throw runtime_error("nodes[].id: duplicate id '" + node.id + "'");

		if (nodeJson.contains("roles") && nodeJson["roles"].is_array())
		{
			for (const auto &r : nodeJson["roles"])
				node.roles.push_back(r.get<string>());
		}
		node.maxGameProcesses = nodeJson.value("maxGameProcesses", 25);
		if (node.maxGameProcesses <= 0)
			throw runtime_error("nodes['" + node.id + "'].maxGameProcesses: must be > 0");

		if (nodeJson.contains("portRange") && nodeJson["portRange"].is_array() && nodeJson["portRange"].size() == 2)
		{
			node.portRangeStart = nodeJson["portRange"][0].get<int>();
			node.portRangeEnd = nodeJson["portRange"][1].get<int>();
			if (!IsValidPort(node.portRangeStart) || !IsValidPort(node.portRangeEnd) || node.portRangeStart > node.portRangeEnd)
				throw runtime_error("nodes['" + node.id + "'].portRange: invalid range");

			int rangeSize = node.portRangeEnd - node.portRangeStart + 1;
			if (rangeSize < node.maxGameProcesses)
				throw runtime_error("nodes['" + node.id + "'].portRange: smaller than maxGameProcesses");
		}
		else
		{
			throw runtime_error("nodes['" + node.id + "'].portRange: expected [start, end]");
		}
		nodes.push_back(node);
	}
}

void CClusterConfig::ParseRegistry(const json &j)
{
	registry.address = j.value("address", "127.0.0.1");
	registry.port = j.value("port", 14650);
}

void CClusterConfig::ParseServicePorts(const json &j)
{
	servicePorts.registry = j.value("registry", 14650);
	servicePorts.lobby = j.value("lobby", 14651);
	servicePorts.agentLocalHealth = j.value("agentLocalHealth", 14600);
	servicePorts.auth = j.value("auth", 14880);
	servicePorts.assets = j.value("assets", 14900);
	if (j.contains("gameRangeDefault") && j["gameRangeDefault"].is_array() && j["gameRangeDefault"].size() == 2)
	{
		servicePorts.gameRangeDefaultStart = j["gameRangeDefault"][0].get<int>();
		servicePorts.gameRangeDefaultEnd = j["gameRangeDefault"][1].get<int>();
	}

	if (!IsValidPort(servicePorts.registry)
		|| !IsValidPort(servicePorts.lobby)
		|| !IsValidPort(servicePorts.agentLocalHealth)
		|| !IsValidPort(servicePorts.auth)
		|| !IsValidPort(servicePorts.assets)
		|| !IsValidPort(servicePorts.gameRangeDefaultStart)
		|| !IsValidPort(servicePorts.gameRangeDefaultEnd)
		|| servicePorts.gameRangeDefaultStart > servicePorts.gameRangeDefaultEnd)
	{
		throw runtime_error("servicePorts: invalid port values");
	}
}

void CClusterConfig::ParseStorage(const json &j)
{
	storage.kind = j.value("kind", "local-fs");
	storage.basePath = j.value("basePath", "./data/dev-game-state");
	storage.endpoint = j.value("endpoint", "");
	storage.bucket = j.value("bucket", "");
	storage.prefix = j.value("prefix", "");
	storage.accessKey = j.value("accessKey", "");
	storage.secretKey = j.value("secretKey", "");
}

void CClusterConfig::ParseCompatibility(const json &j)
{
	compatibility.protocolVersion = j.value("protocolVersion", 1);
	compatibility.policy = j.value("policy", "exact-match");
	compatibility.defaultLocale = j.value("defaultLocale", "en");
	if (compatibility.protocolVersion <= 0)
		throw runtime_error("compatibility.protocolVersion: must be > 0");
	if (compatibility.policy.empty())
		throw runtime_error("compatibility.policy: cannot be empty");
	if (j.contains("versionMismatchMessagesByLocale"))
		compatibility.versionMismatchMessagesByLocale = ParseLocaleMap(j["versionMismatchMessagesByLocale"]);
}

void CClusterConfig::ParseRolePolicies(const json &j)
{
	if (!j.is_object())
		throw runtime_error("rolePolicies: expected object");
	rolePolicies.clear();
	for (auto &[roleName, policyJson] : j.items())
	{
		SRolePolicyConfig policy;
		if (policyJson.contains("restart"))
		{
			const json &r = policyJson["restart"];
			if (r.contains("mode"))
			{
				string mode = r["mode"].get<string>();
				if (mode == "never")
					policy.restart.mode = ERestartPolicy::NEVER;
				else if (mode == "on-failure")
					policy.restart.mode = ERestartPolicy::ON_FAILURE;
				else if (mode == "always")
					policy.restart.mode = ERestartPolicy::ALWAYS;
				else
					throw runtime_error("rolePolicies['" + roleName + "'].restart.mode: invalid value '" + mode + "'");
			}
			policy.restart.maxRestartsPerWindow = r.value("maxRestartsPerWindow", 5);
			policy.restart.windowSeconds = r.value("windowSeconds", 300);
			policy.restart.backoffBaseMs = r.value("backoffBaseMs", 1000);
			policy.restart.backoffMultiplier = r.value("backoffMultiplier", 2.0);
			policy.restart.backoffMaxMs = r.value("backoffMaxMs", 30000);
			if (policy.restart.maxRestartsPerWindow <= 0)
				throw runtime_error("rolePolicies['" + roleName + "'].restart.maxRestartsPerWindow: must be > 0");
			if (policy.restart.windowSeconds <= 0)
				throw runtime_error("rolePolicies['" + roleName + "'].restart.windowSeconds: must be > 0");
		}
		if (policyJson.contains("health"))
		{
			const json &h = policyJson["health"];
			if (h.contains("type"))
			{
				string type = h["type"].get<string>();
				if (type == "process")
					policy.health.type = EHealthCheckType::PROCESS;
				else if (type == "tcp")
					policy.health.type = EHealthCheckType::TCP;
				else if (type == "http")
					policy.health.type = EHealthCheckType::HTTP;
				else
					throw runtime_error("rolePolicies['" + roleName + "'].health.type: invalid value '" + type + "'");
			}
			policy.health.port = h.value("port", 0);
			policy.health.url = h.value("url", "");
			policy.health.intervalMs = h.value("intervalMs", 2000);
			if (policy.health.intervalMs <= 0)
				throw runtime_error("rolePolicies['" + roleName + "'].health.intervalMs: must be > 0");
		}
		rolePolicies[roleName] = policy;
	}
}

void CClusterConfig::ParseAdmission(const json &j)
{
	admission.defaultLocale = j.value("defaultLocale", "en");
	admission.localeSelection = j.value("localeSelection", "server-side");
	if (j.contains("lobby"))
		ParseAdmissionEndpoint(j["lobby"], admission.lobby);
	if (j.contains("game"))
		ParseAdmissionEndpoint(j["game"], admission.game);
}

void CClusterConfig::ParseAdmissionEndpoint(const json &j, SAdmissionEndpointConfig &out)
{
	out.maintenanceEnabled = j.value("maintenanceEnabled", false);
	out.rejectMessageKey = j.value("rejectMessageKey", "");
	out.rejectMessageFallback = j.value("rejectMessageFallback", "");
	out.disconnectMessageKey = j.value("disconnectMessageKey", "");
	out.disconnectMessageFallback = j.value("disconnectMessageFallback", "");
	if (j.contains("rejectMessagesByLocale"))
		out.rejectMessagesByLocale = ParseLocaleMap(j["rejectMessagesByLocale"]);
	if (j.contains("disconnectMessagesByLocale"))
		out.disconnectMessagesByLocale = ParseLocaleMap(j["disconnectMessagesByLocale"]);
}

void CClusterConfig::ParseGameServer(const json &j)
{
	gameServer.heartbeatTimeoutSeconds = j.value("heartbeatTimeoutSeconds", 15);
	gameServer.heartbeatWarningSeconds = j.value("heartbeatWarningSeconds", 8);
	gameServer.periodicSaveIntervalSeconds = j.value("periodicSaveIntervalSeconds", 60);
	gameServer.portRangeStart = j.value("portRangeStart", 14701);
	gameServer.portRangeEnd = j.value("portRangeEnd", 14799);
	if (gameServer.heartbeatTimeoutSeconds <= 0)
		throw runtime_error("gameServer.heartbeatTimeoutSeconds: must be > 0");
	if (gameServer.heartbeatWarningSeconds <= 0)
		throw runtime_error("gameServer.heartbeatWarningSeconds: must be > 0");
	if (gameServer.periodicSaveIntervalSeconds < 0)
		throw runtime_error("gameServer.periodicSaveIntervalSeconds: must be >= 0");
	if (!IsValidPort(gameServer.portRangeStart) || !IsValidPort(gameServer.portRangeEnd)
		|| gameServer.portRangeStart > gameServer.portRangeEnd)
		throw runtime_error("gameServer.portRange: invalid range");
}

void CClusterConfig::ParseOperations(const json &j)
{
	operations.adminCommandTimeoutSec = j.value("adminCommandTimeoutSec", 20);
	operations.adminAuthMaxAttempts = j.value("adminAuthMaxAttempts", 5);
	if (operations.adminCommandTimeoutSec <= 0)
		throw runtime_error("operations.adminCommandTimeoutSec: must be > 0");
	if (operations.adminAuthMaxAttempts <= 0)
		throw runtime_error("operations.adminAuthMaxAttempts: must be > 0");
}

map<string, string> CClusterConfig::ParseLocaleMap(const json &j)
{
	map<string, string> result;
	if (!j.is_object())
		throw runtime_error("locale map: expected object");

	for (auto &[key, val] : j.items())
	{
		if (key.empty())
			throw runtime_error("locale map: locale key cannot be empty");
		result[key] = val.get<string>();
	}
	return result;
}

const SNodeConfig *CClusterConfig::GetNode(const string &nodeId) const
{
	for (const auto &node : nodes)
	{
		if (node.id == nodeId)
			return &node;
	}
	return nullptr;
}

string CClusterConfig::GetLocalizedMessage(const map<string, string> &messagesByLocale,
										   const string &locale,
										   const string &fallback)
{
	auto it = messagesByLocale.find(locale);
	if (it != messagesByLocale.end())
		return it->second;
	// Try English fallback
	it = messagesByLocale.find("en");
	if (it != messagesByLocale.end())
		return it->second;
	return fallback;
}

json CClusterConfig::ToJson() const
{
	json j;
	j["runtime"]["mode"] = RuntimeModeToString(runtime.mode);

	j["registry"]["address"] = registry.address;
	j["registry"]["port"] = registry.port;

	j["servicePorts"]["registry"] = servicePorts.registry;
	j["servicePorts"]["lobby"] = servicePorts.lobby;
	j["servicePorts"]["auth"] = servicePorts.auth;
	j["servicePorts"]["assets"] = servicePorts.assets;

	j["storage"]["kind"] = storage.kind;

	j["compatibility"]["protocolVersion"] = compatibility.protocolVersion;
	j["compatibility"]["policy"] = compatibility.policy;

	json nodesJson = json::array();
	for (const auto &node : nodes)
	{
		json nj;
		nj["id"] = node.id;
		nj["address"] = node.address;
		nj["roles"] = node.roles;
		nj["maxGameProcesses"] = node.maxGameProcesses;
		nodesJson.push_back(nj);
	}
	j["nodes"] = nodesJson;

	j["admission"]["lobby"]["maintenanceEnabled"] = admission.lobby.maintenanceEnabled;
	j["admission"]["game"]["maintenanceEnabled"] = admission.game.maintenanceEnabled;

	j["gameServer"]["heartbeatTimeoutSeconds"] = gameServer.heartbeatTimeoutSeconds;
	j["gameServer"]["heartbeatWarningSeconds"] = gameServer.heartbeatWarningSeconds;
	j["gameServer"]["periodicSaveIntervalSeconds"] = gameServer.periodicSaveIntervalSeconds;
	j["gameServer"]["portRangeStart"] = gameServer.portRangeStart;
	j["gameServer"]["portRangeEnd"] = gameServer.portRangeEnd;

	j["operations"]["adminCommandTimeoutSec"] = operations.adminCommandTimeoutSec;
	j["operations"]["adminAuthMaxAttempts"] = operations.adminAuthMaxAttempts;

	return j;
}

void CClusterConfig::PrintSummary() const
{
	LOGM("=== Cluster Configuration ===");
	LOGM("Runtime Mode: %s", RuntimeModeToString(runtime.mode).c_str());
	LOGM("Registry: %s:%d", registry.address.c_str(), registry.port);
	LOGM("Nodes: %d", (int)nodes.size());
	LOGM("Storage: %s", storage.kind.c_str());
	LOGM("Protocol Version: %d (%s)", compatibility.protocolVersion, compatibility.policy.c_str());
	LOGM("Maintenance - Lobby: %s, Game: %s",
		 admission.lobby.maintenanceEnabled ? "ON" : "off",
		 admission.game.maintenanceEnabled ? "ON" : "off");
}

ERuntimeMode CClusterConfig::StringToRuntimeMode(const string &s)
{
	if (s == "cluster") return ERuntimeMode::CLUSTER;
	if (s == "hotseat") return ERuntimeMode::HOTSEAT;
	return ERuntimeMode::LOCAL_DEV;
}

string CClusterConfig::RuntimeModeToString(ERuntimeMode mode)
{
	switch (mode)
	{
	case ERuntimeMode::CLUSTER:  return "cluster";
	case ERuntimeMode::HOTSEAT:  return "hotseat";
	default:                     return "local-dev";
	}
}

EHealthCheckType CClusterConfig::StringToHealthCheckType(const string &s)
{
	if (s == "tcp")  return EHealthCheckType::TCP;
	if (s == "http") return EHealthCheckType::HTTP;
	return EHealthCheckType::PROCESS;
}

ERestartPolicy CClusterConfig::StringToRestartPolicy(const string &s)
{
	if (s == "on-failure") return ERestartPolicy::ON_FAILURE;
	if (s == "always")     return ERestartPolicy::ALWAYS;
	return ERestartPolicy::NEVER;
}

bool CClusterConfig::IsValidPort(int port)
{
	return port >= 1 && port <= 65535;
}

void CClusterConfig::ValidateOrThrow() const
{
	if (!IsValidPort(registry.port))
		throw runtime_error("registry.port: invalid value");

	if (compatibility.protocolVersion <= 0)
		throw runtime_error("compatibility.protocolVersion: must be > 0");

	if (admission.defaultLocale.empty())
		throw runtime_error("admission.defaultLocale: cannot be empty");
}
