#pragma once

#include "SYS_Defs.h"
#include "MT_NetGameAuthDomains.h"
#include "SYS_Crypto.h"
#include "SYS_SecureRandom.h"
#include "NET_AuthPw2.h"
#include "CNetClientData.h"
#include "CNetServer.h"

#include <array>
#include <string>
#include <vector>

// Shared-secret helper for internal NetGame services (lobby-service, registry).
// The secret is sent via the existing ENet authorize passwordHash field, using
// the same PW2 challenge-response protocol as user password auth (see
// NET_AuthPw2.h and the host lobby server's NetServerAuthorize) rather than a
// bare HMAC of the secret -- a bare HMAC is a static bearer value that a
// wire-level observer (or a leaked log line) can replay indefinitely. Here,
// the server never sends anything that lets a wire observer forge a response;
// it emits a fresh random nonce per attempt and the client proves knowledge of
// the secret by HMACing that nonce with a PBKDF2-derived key.

static inline std::vector<u8> NetGameInternalSecretToHash(const std::string &secret)
{
	if (secret.empty())
		return {};

	// Use HMAC-SHA256 with a domain separator key to prevent cross-protocol hash confusion.
	// App-registered (MT_SetNetGameAuthDomains); see MT_NetGameAuthDomains.h.
	const char *domainKey = MTNetGameInternalDomainRef();
	auto digest = SYS_HmacSha256(
		(const uint8_t *)domainKey, strlen(domainKey),
		(const uint8_t *)secret.data(), secret.size());
	return std::vector<u8>(digest.begin(), digest.end());
}

// A verifier is built once, at server construction, from the configured
// internal secret. It never leaves the process. `valid=false` (empty secret,
// or a random/derive failure) means "reject every internal connection" --
// callers must fail closed, never fall back to accepting anything.
struct NetGameInternalVerifier
{
	u8 version = 1;
	u32 iterations = 1000;
	std::vector<u8> salt;
	std::vector<u8> key;
	bool valid = false;
};

static inline NetGameInternalVerifier NetGameInternalMakeVerifier(const std::string &secret)
{
	NetGameInternalVerifier v;
	if (secret.empty())
		return v;                       // valid=false: no secret => reject everyone
	v.salt.resize(16);
	if (!SYS_SecureRandomBytes(v.salt.data(), v.salt.size()))
		return v;
	std::vector<u8> proof = NetGameInternalSecretToHash(secret);
	if (!NET_AuthPw2_DeriveKeyFromProof(v.version, proof, v.salt, v.iterations, v.key))
		return v;
	v.valid = true;
	return v;
}

// Server side, called from NetServerAuthorize. Handles both legs of the PW2
// handshake: the first packet (client sends only the "PW2\0" magic) gets a
// fresh-nonce challenge built and returns AUTHORIZE_CHALLENGE; the response
// packet (client's HMAC of that nonce under its derived key) is checked in
// constant time and returns CORRECT or WRONG_PASSWORD. Mirrors
// the host lobby server's PW2 branches exactly, minus
// the per-user profile lookup (there is exactly one verifier here).
static inline u8 NetGameInternalAuthorize(const NetGameInternalVerifier &v, CNetClientData *clientData, const std::vector<u8> &passwordHash)
{
	if (!v.valid)
		return NET_SERVER_CALLBACK_AUTHORIZE_WRONG_PASSWORD;

	// Challenge-response continuation.
	if (clientData->authorizePending)
	{
		u8 hashVer = 0;
		u32 iters = 0;
		std::vector<u8> salt;
		std::vector<u8> nonce;
		if (!NET_AuthPw2_ParseChallenge(clientData->authorizePendingChallenge, hashVer, iters, salt, nonce))
			return NET_SERVER_CALLBACK_AUTHORIZE_WRONG_PASSWORD;

		std::array<uint8_t, 32> receivedHmac;
		if (!NET_AuthPw2_ParseResponseHmac(passwordHash, receivedHmac))
			return NET_SERVER_CALLBACK_AUTHORIZE_WRONG_PASSWORD;

		std::array<uint8_t, 32> expectedHmac = NET_AuthPw2_ComputeHmacFromKey(v.key, nonce);
		std::vector<u8> expectedVec(expectedHmac.begin(), expectedHmac.end());
		std::vector<u8> receivedVec(receivedHmac.begin(), receivedHmac.end());
		if (!SYS_ConstantTimeEquals(expectedVec, receivedVec))
			return NET_SERVER_CALLBACK_AUTHORIZE_WRONG_PASSWORD;

		return NET_SERVER_CALLBACK_AUTHORIZE_CORRECT;
	}

	// First packet: the client sends only the 4-byte PW2 magic, never the proof itself.
	bool isPw2Magic = (passwordHash.size() == 4
					&& passwordHash[0] == 'P'
					&& passwordHash[1] == 'W'
					&& passwordHash[2] == '2'
					&& passwordHash[3] == 0);
	if (!isPw2Magic)
		return NET_SERVER_CALLBACK_AUTHORIZE_WRONG_PASSWORD;

	std::vector<u8> nonce(32);
	if (!SYS_SecureRandomBytes(nonce.data(), nonce.size()))
		return NET_SERVER_CALLBACK_AUTHORIZE_WRONG_PASSWORD;

	clientData->authorizePendingChallenge = NET_AuthPw2_BuildChallenge(v.version, v.iterations, v.salt, nonce);
	if (clientData->authorizePendingChallenge.empty())
		return NET_SERVER_CALLBACK_AUTHORIZE_WRONG_PASSWORD;

	return NET_SERVER_CALLBACK_AUTHORIZE_CHALLENGE;
}

// Client side: what to pass as the CNetClient passwordHash / authData. CNetClient
// recognizes this shape (NET_AuthPw2_IsClientAuthData) and answers a server
// challenge with it automatically (see CNetClient::ThreadRun).
static inline std::vector<u8> NetGameInternalClientAuthData(const std::string &secret)
{
	return NET_AuthPw2_BuildClientAuthData(NetGameInternalSecretToHash(secret));
}
