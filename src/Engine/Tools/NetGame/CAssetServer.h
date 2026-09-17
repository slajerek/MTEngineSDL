#pragma once


// CAPABILITY GUARD (MT_ENABLE_HTTPLIB). The header stays INCLUDABLE and declares nothing
// when the capability is off, so a conditional consumer still compiles.
// MT_CAP_HTTP declared this removable and nothing enforced it: MT_ENABLE_HTTPLIB appeared in no source at all.
#if MT_ENABLE_HTTPLIB

#include "SYS_Defs.h"
#include "SYS_Threading.h"

#include <string>
#include <vector>
#include <map>
#include <cstdint>
#include <ctime>
#include <thread>

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
	// Allow plaintext HTTP on a non-loopback interface (trusted network only).
	// Loopback plaintext is always allowed regardless of this flag.
	bool allowPlaintext = false;

	// Auth service connection (for token validation)
	std::string authServiceAddress;
	int authServicePort;
	// The OUTBOUND leg: how this server reaches the auth service to validate a
	// client's bearer token. Independent of useSSL above (that is the INBOUND
	// listener) -- the two hops are configured separately because they can be
	// two different machines with two different certificates. Must be true
	// whenever the auth service itself is started with --ssl: a plaintext POST
	// to a TLS listener never returns 200, so every asset request would 401.
	bool authServiceUseTls = false;
	// Optional CA bundle for the auth service's certificate (private CA or a
	// self-signed fixture). Empty means "use the system trust store".
	// Verification is ALWAYS on -- there is no disable switch here by design.
	std::string authServiceCaCertPath;

	bool isRunning;

	// Manifest access (thread-safe)
	SAssetManifest GetAssetsManifest();
	SAssetManifest GetProjectManifest(const std::string &projectId);

	// Build/rebuild manifests (called on Start, can be called to refresh)
	void RebuildManifests();

	// Returns false when the server refused to start (bad TLS cert/key, or
	// plaintext requested on a non-loopback interface without allowPlaintext).
	bool Start();
	void Shutdown();

private:
	void *httpServer;
	// The listen thread, kept (not detached) so Shutdown() can join it before
	// deleting the httplib::Server the thread is still inside -- every route
	// lambda captures `this`, so deleting under a live thread is a UAF.
	std::thread serverThread;
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

#endif // MT_ENABLE_HTTPLIB
