// CAPABILITY GUARD (MT_ENABLE_HTTPLIB). The file stays in all three build lists and
// compiles to nothing when the capability is off -- an Xcode PBXBuildFile
// cannot be conditioned on a build setting, so the condition lives here.
// MT_CAP_HTTP declared this removable and nothing enforced it: MT_ENABLE_HTTPLIB appeared in no source at all.
#if MT_ENABLE_HTTPLIB

#include "CAuthServiceProcess.h"
#include "CAuthServiceServer.h"
#include "CNetGameDataProviderLocalFiles.h"
#include "SYS_Main.h"
#include "SYS_Funct.h"
#include "SYS_FileSystem.h"
#include "DBG_Log.h"
#include <cstring>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <sstream>

static volatile bool sAuthShutdownRequested = false;

static void AuthServiceSignalHandler(int sig)
{
	LOGM("CAuthServiceProcess: received signal %d, shutting down", sig);
	sAuthShutdownRequested = true;
}

bool CAuthServiceProcess::ParseArgs(int argc, const char **argv, Config &config)
{
	bool isAuthServer = false;

	for (int i = 0; i < argc; i++)
	{
		if (strcmp(argv[i], "--auth-server") == 0)
			isAuthServer = true;
		else if (strcmp(argv[i], "--auth-port") == 0 && i + 1 < argc)
			config.port = atoi(argv[++i]);
		else if (strcmp(argv[i], "--app-path") == 0 && i + 1 < argc)
			config.appPath = argv[++i];
		else if (strcmp(argv[i], "--config") == 0 && i + 1 < argc)
			config.configPath = argv[++i];
		else if (strcmp(argv[i], "--rate-limit-max") == 0 && i + 1 < argc)
			config.rateLimitMaxAttempts = atoi(argv[++i]);
		else if (strcmp(argv[i], "--rate-limit-window") == 0 && i + 1 < argc)
			config.rateLimitWindowSeconds = atoi(argv[++i]);
		else if (strcmp(argv[i], "--listen") == 0 && i + 1 < argc)
			config.listenInterface = argv[++i];
		else if (strcmp(argv[i], "--ssl") == 0)
			config.useSSL = true;
		else if (strcmp(argv[i], "--ssl-cert") == 0 && i + 1 < argc)
			config.sslCertPath = argv[++i];
		else if (strcmp(argv[i], "--ssl-key") == 0 && i + 1 < argc)
			config.sslKeyPath = argv[++i];
		else if (strcmp(argv[i], "--allow-plaintext") == 0)
			config.allowPlaintext = true;
		else if (strcmp(argv[i], "--internal-secret") == 0 && i + 1 < argc)
			config.internalSecret = argv[++i];
		else if (strcmp(argv[i], "--internal-secret-file") == 0 && i + 1 < argc)
			config.internalSecretFile = argv[++i];
	}

	return isAuthServer;
}

// Reads a secret from a file and trims surrounding whitespace/newlines (an
// `openssl rand -hex 32 > secret.txt` file ends with a newline that must not
// become part of the secret). Returns "" when the file cannot be read.
static string ReadSecretFile(const string &path)
{
	std::ifstream f(path, std::ios::binary);
	if (!f.is_open())
		return "";
	std::stringstream ss;
	ss << f.rdbuf();
	string v = ss.str();
	size_t b = v.find_first_not_of(" \t\r\n");
	if (b == string::npos)
		return "";
	size_t e = v.find_last_not_of(" \t\r\n");
	return v.substr(b, e - b + 1);
}

void CAuthServiceProcess::Run(const Config &config)
{
	LOGM("CAuthServiceProcess::Run: port=%d appPath=%s", config.port, config.appPath.c_str());

	// Install signal handlers for graceful shutdown
	sAuthShutdownRequested = false;
	signal(SIGTERM, AuthServiceSignalHandler);
	signal(SIGINT, AuthServiceSignalHandler);

	// Set up resource path for data provider
	if (!config.appPath.empty())
	{
		gPathToResources = strdup(config.appPath.c_str());
	}

	// Create data provider
	string configPath = config.configPath;
	if (configPath.empty())
		configPath = string(gPathToResources) + "config.hjson";

	CNetGameDataProviderLocalFiles *dataProvider = new CNetGameDataProviderLocalFiles(configPath);

	// Create and configure auth server
	CAuthServiceServer *authServer = new CAuthServiceServer(dataProvider, config.port);
	authServer->rateLimitMaxAttempts = config.rateLimitMaxAttempts;
	authServer->rateLimitWindowSeconds = config.rateLimitWindowSeconds;
	authServer->listenInterface = config.listenInterface;
	authServer->useSSL = config.useSSL;
	authServer->sslCertPath = config.sslCertPath;
	authServer->sslKeyPath = config.sslKeyPath;
	authServer->allowPlaintext = config.allowPlaintext;
	// Resolve the internal secret: file, then environment, then the CLI flag.
	// The flag is the LAST resort because a command line is world-visible via
	// `ps` (final-review Minor 7); it stays supported for dev convenience.
	string resolvedSecret;
	const char *secretSource = "none";
	if (!config.internalSecretFile.empty())
	{
		resolvedSecret = ReadSecretFile(config.internalSecretFile);
		if (resolvedSecret.empty())
			LOGError("CAuthServiceProcess: --internal-secret-file %s is unreadable or empty", config.internalSecretFile.c_str());
		else
			secretSource = "--internal-secret-file";
	}
	if (resolvedSecret.empty())
	{
		const char *env = getenv("MT_INTERNAL_SECRET");
		if (env != NULL && env[0] != 0)
		{
			resolvedSecret = env;
			secretSource = "MT_INTERNAL_SECRET";
		}
	}
	if (resolvedSecret.empty() && !config.internalSecret.empty())
	{
		resolvedSecret = config.internalSecret;
		secretSource = "--internal-secret (visible via ps -- prefer --internal-secret-file or MT_INTERNAL_SECRET)";
	}
	LOGM("CAuthServiceProcess: internal secret source: %s", secretSource);
	authServer->internalSecret = resolvedSecret;

	// Start serving
	if (!authServer->Start())
	{
		LOGError("CAuthServiceProcess: auth service failed to start on %s:%d", config.listenInterface.c_str(), config.port);
		delete authServer;
		delete dataProvider;
		return;
	}

	LOGM("auth service: %s://%s:%d", authServer->useSSL ? "https" : "http",
		 config.listenInterface.c_str(), SYS_ApplyPortOffset(config.port));
	LOGM("CAuthServiceProcess: auth service started, waiting for requests");

	// Main loop: wait for shutdown signal
	while (!sAuthShutdownRequested)
	{
		SYS_Sleep(100);
	}

	LOGM("CAuthServiceProcess: shutting down");

	authServer->Shutdown();
	delete authServer;
	delete dataProvider;

	LOGM("CAuthServiceProcess: shutdown complete");
}

#endif // MT_ENABLE_HTTPLIB
