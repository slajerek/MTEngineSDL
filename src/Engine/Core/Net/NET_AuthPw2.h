#pragma once

#include "../SYS_Defs.h"
#include "../SYS_Crypto.h"

#include <vector>
#include <string>
#include <cstring>

// Password challenge-response auth helper (PW2).
//
// Client authData encoding (stored in CNetClient::passwordHash):
//   "PW2\0" + 32-byte passwordProof (SHA256(password))
// The authorize packet only sends the 4-byte magic, never the passwordProof.
//
// Challenge payload encoding:
//   "PW2C" + u8 passwordHashVersion + u32 iterationsLE + u8 saltLen + salt + u8 nonceLen + nonce
//
// Response payload encoding:
//   "PW2R" + 32-byte hmac

static inline bool NET_AuthPw2_IsClientAuthData(const std::vector<u8> &authData)
{
	static const u8 kMagic[4] = { 'P','W','2','\0' };
	return authData.size() >= (4 + 32) && memcmp(authData.data(), kMagic, 4) == 0;
}

static inline const u8 *NET_AuthPw2_GetPasswordProofPtr(const std::vector<u8> &authData)
{
	if (!NET_AuthPw2_IsClientAuthData(authData))
		return NULL;
	return authData.data() + 4;
}

static inline std::vector<u8> NET_AuthPw2_BuildClientAuthData(const std::vector<u8> &passwordProof)
{
	std::vector<u8> out;
	static const u8 kMagic[4] = { 'P','W','2','\0' };
	if (passwordProof.size() != 32)
		return out;
	out.reserve(4 + 32);
	out.insert(out.end(), kMagic, kMagic + 4);
	out.insert(out.end(), passwordProof.begin(), passwordProof.end());
	return out;
}

static inline std::vector<u8> NET_AuthPw2_BuildChallenge(u8 passwordHashVersion,
									 u32 iterations,
									 const std::vector<u8> &salt,
									 const std::vector<u8> &nonce)
{
	std::vector<u8> out;
	static const u8 kMagic[4] = { 'P','W','2','C' };
	if (nonce.empty() || nonce.size() > 255)
		return out;
	if (salt.size() > 255)
		return out;
	if (passwordHashVersion == 0)
	{
		// legacy: no salt/iterations needed
		iterations = 0;
	}

	out.reserve(4 + 1 + 4 + 1 + salt.size() + 1 + nonce.size());
	out.insert(out.end(), kMagic, kMagic + 4);
	out.push_back(passwordHashVersion);
	// iterations (little-endian)
	out.push_back((u8)(iterations & 0xFF));
	out.push_back((u8)((iterations >> 8) & 0xFF));
	out.push_back((u8)((iterations >> 16) & 0xFF));
	out.push_back((u8)((iterations >> 24) & 0xFF));
	out.push_back((u8)salt.size());
	out.insert(out.end(), salt.begin(), salt.end());
	out.push_back((u8)nonce.size());
	out.insert(out.end(), nonce.begin(), nonce.end());
	return out;
}

static inline bool NET_AuthPw2_ParseChallenge(const std::vector<u8> &challenge,
								  u8 &outPasswordHashVersion,
								  u32 &outIterations,
								  std::vector<u8> &outSalt,
								  std::vector<u8> &outNonce)
{
	static const u8 kMagic[4] = { 'P','W','2','C' };
	if (challenge.size() < (4 + 1 + 4 + 1 + 1))
		return false;
	if (memcmp(challenge.data(), kMagic, 4) != 0)
		return false;

	size_t idx = 4;
	outPasswordHashVersion = challenge[idx++];
	outIterations = (u32)challenge[idx]
		| ((u32)challenge[idx + 1] << 8)
		| ((u32)challenge[idx + 2] << 16)
		| ((u32)challenge[idx + 3] << 24);
	idx += 4;

	u8 saltLen = challenge[idx++];
	if (idx + saltLen + 1 > challenge.size())
		return false;
	outSalt.assign(challenge.begin() + idx, challenge.begin() + idx + saltLen);
	idx += saltLen;

	u8 nonceLen = challenge[idx++];
	if (idx + nonceLen != challenge.size())
		return false;
	outNonce.assign(challenge.begin() + idx, challenge.begin() + idx + nonceLen);
	return true;
}

static inline std::vector<u8> NET_AuthPw2_BuildResponse(const std::array<uint8_t, 32> &hmac)
{
	std::vector<u8> out;
	static const u8 kMagic[4] = { 'P','W','2','R' };
	out.reserve(4 + 32);
	out.insert(out.end(), kMagic, kMagic + 4);
	out.insert(out.end(), hmac.begin(), hmac.end());
	return out;
}

static inline bool NET_AuthPw2_ParseResponseHmac(const std::vector<u8> &response,
									std::array<uint8_t, 32> &outHmac)
{
	static const u8 kMagic[4] = { 'P','W','2','R' };
	if (response.size() != (4 + 32))
		return false;
	if (memcmp(response.data(), kMagic, 4) != 0)
		return false;
	memcpy(outHmac.data(), response.data() + 4, 32);
	return true;
}

static inline std::array<uint8_t, 32> NET_AuthPw2_ComputeHmacFromKey(const std::vector<u8> &key,
											const std::vector<u8> &nonce)
{
	return SYS_HmacSha256(key.data(), key.size(), nonce.data(), nonce.size());
}

static inline bool NET_AuthPw2_DeriveKeyFromProof(const u8 passwordHashVersion,
									 const std::vector<u8> &passwordProof,
									 const std::vector<u8> &salt,
									 const u32 iterations,
									 std::vector<u8> &outKey)
{
	if (passwordProof.size() != 32)
		return false;

	if (passwordHashVersion == 0)
	{
		outKey = passwordProof;
		return true;
	}
	else if (passwordHashVersion == 1)
	{
		if (salt.empty() || iterations == 0)
			return false;
		outKey = SYS_Pbkdf2HmacSha256(passwordProof, salt, iterations, 32);
		return outKey.size() == 32;
	}

	return false;
}
