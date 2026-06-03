#pragma once

#include "SYS_Defs.h"
#include "SYS_Crypto.h"

#include <string>
#include <vector>

// Shared-secret helper for internal NetGame services (lobby-service, registry).
// The secret is sent via the existing ENet authorize passwordHash field.

static inline std::vector<u8> NetGameInternalSecretToHash(const std::string &secret)
{
	if (secret.empty())
		return {};

	// Use HMAC-SHA256 with a domain separator key to prevent cross-protocol hash confusion.
	const char *domainKey = "LightHeroes.InternalSecret.v1";
	auto digest = SYS_HmacSha256(
		(const uint8_t *)domainKey, strlen(domainKey),
		(const uint8_t *)secret.data(), secret.size());
	return std::vector<u8>(digest.begin(), digest.end());
}

static inline bool NetGameInternalSecretMatches(const std::vector<u8> &expectedHash, const std::vector<u8> &providedHash)
{
	// No configured secret means "no access" (callers should treat this as reject).
	if (expectedHash.empty())
		return false;

	return SYS_ConstantTimeEquals(expectedHash, providedHash);
}
