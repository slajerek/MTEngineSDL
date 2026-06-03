#pragma once

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
		bool useSSL = false;
		std::string sslCertPath;
		std::string sslKeyPath;
	};

	static bool ParseArgs(int argc, const char **argv, Config &config);
	static void Run(const Config &config);
};
