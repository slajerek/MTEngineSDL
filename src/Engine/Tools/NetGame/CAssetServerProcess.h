#pragma once


// CAPABILITY GUARD (MT_ENABLE_HTTPLIB). The header stays INCLUDABLE and declares nothing
// when the capability is off, so a conditional consumer still compiles.
// MT_CAP_HTTP declared this removable and nothing enforced it: MT_ENABLE_HTTPLIB appeared in no source at all.
#if MT_ENABLE_HTTPLIB

#include <string>

// Standalone asset server process (like CAuthServiceProcess)
class CAssetServerProcess
{
public:
	struct Config {
		int port = 14900;
		std::string appPath;
		std::string assetsFolder;
		std::string projectsFolder;
		std::string authServiceAddress = "localhost";
		int authServicePort = 14880;
		// --auth-tls / --auth-ca: the OUTBOUND hop to the auth service. Separate
		// from --ssl (the inbound listener) -- see CAssetServer::authServiceUseTls.
		bool authServiceUseTls = false;
		std::string authServiceCaCertPath;
		std::string listenInterface = "127.0.0.1";
		bool useSSL = false;
		std::string sslCertPath;
		std::string sslKeyPath;
		bool allowPlaintext = false;
	};

	static bool ParseArgs(int argc, const char **argv, Config &config);
	static void Run(const Config &config);
};

#endif // MT_ENABLE_HTTPLIB
