#pragma once

#include "SYS_Defs.h"
#include "SYS_Threading.h"

#include <string>
#include <vector>
#include <map>
#include <cstdint>
#include <ctime>

#include "json.hpp"

class CSlrMutex;

// Manifest entry for a single file
struct SAssetManifestEntry
{
	std::string path;       // Relative path (e.g., "fonts/NotoSans.ttf")
	std::string crc32;      // Hex CRC32 (e.g., "a1b2c3d4")
	uint64_t size;           // File size in bytes
};

// Complete manifest for a folder
struct SAssetManifest
{
	int version = 1;
	std::string manifestHash;   // CRC32 of sorted "path:crc32" pairs
	std::vector<SAssetManifestEntry> files;
};

// HTTP asset server — serves files from configurable folders with CRC32 manifests.
// Requires auth token validation via external auth service HTTP call.
// Can run on a separate VPS from the auth service.
class CAssetServer
{
public:
	CAssetServer(int port = 14900);
	~CAssetServer();

	// Configuration
	int port;
	std::string listenInterface;
	std::string assetsFolder;       // Absolute path to assets folder
	std::string projectsFolder;     // Absolute path to projects folder
	bool useSSL;
	std::string sslCertPath;
	std::string sslKeyPath;

	// Auth service connection (for token validation)
	std::string authServiceAddress;
	int authServicePort;

	bool isRunning;

	// Manifest access (thread-safe)
	SAssetManifest GetAssetsManifest();
	SAssetManifest GetProjectManifest(const std::string &projectId);

	// Build/rebuild manifests (called on Start, can be called to refresh)
	void RebuildManifests();

	void Start();
	void Shutdown();

private:
	void *httpServer;
	// Token validation cache
	struct CachedTokenInfo {
		int profileId;
		std::string playerName;
		time_t validUntil;
	};

	bool ValidateToken(const std::string &token, int &outProfileId, std::string &outPlayerName);
	std::map<std::string, CachedTokenInfo> tokenCache;
	int tokenCacheTTLSeconds = 300;  // 5 minutes
	CSlrMutex *mutexTokenCache;

	// Manifests
	SAssetManifest assetsManifest;
	std::map<std::string, SAssetManifest> projectManifests;  // projectId -> manifest
	CSlrMutex *mutexManifests;

	SAssetManifest BuildManifestForFolder(const std::string &folderPath);
	std::string ComputeManifestHash(const SAssetManifest &manifest);

	// CRC32 cache: avoid re-reading unchanged files on rebuild
	struct SCrc32CacheEntry
	{
		int64_t mtime;       // last_write_time ticks (file_time_type::time_since_epoch().count())
		uint64_t size;
		std::string crc32;   // hex CRC32
	};
	std::map<std::string, SCrc32CacheEntry> crc32Cache;  // absolute path -> cached entry
	void LoadCrc32Cache(const std::string &cachePath);
	void SaveCrc32Cache(const std::string &cachePath);
	std::string GetCachedOrComputeCrc32(const std::string &absPath, uint64_t fileSize, int64_t mtime);

	// Helpers
	std::string ExtractBearerToken(const std::string &authHeader);
	nlohmann::json ManifestToJson(const SAssetManifest &manifest);

	// Find all project IDs by scanning projectsFolder
	std::vector<std::string> ScanProjectIds();
};
