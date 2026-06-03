#pragma once

#include "SYS_Defs.h"
#include "sgSHA256.h"

#include <array>
#include <vector>
#include <cstring>
#include <algorithm>
#include <string>

#include "zlib.h"

static inline bool SYS_ConstantTimeEquals(const std::vector<u8> &a, const std::vector<u8> &b)
{
	if (a.size() != b.size())
		return false;

	u8 diff = 0;
	for (size_t i = 0; i < a.size(); i++)
		diff |= (u8)(a[i] ^ b[i]);
	return diff == 0;
}

static inline std::array<uint8_t, 32> SYS_Sha256(const uint8_t *data, size_t len)
{
	sgSHA256 sha;
	sha.update(data, len);
	return sha.digest();
}

static inline std::array<uint8_t, 32> SYS_HmacSha256(const uint8_t *key, size_t keyLen,
									  const uint8_t *msg, size_t msgLen)
{
	// HMAC-SHA256 per RFC 2104
	uint8_t keyBlock[64];
	memset(keyBlock, 0, sizeof(keyBlock));

	if (keyLen > sizeof(keyBlock))
	{
		auto keyHashed = SYS_Sha256(key, keyLen);
		memcpy(keyBlock, keyHashed.data(), keyHashed.size());
	}
	else if (keyLen > 0)
	{
		memcpy(keyBlock, key, keyLen);
	}

	uint8_t oKeyPad[64];
	uint8_t iKeyPad[64];
	for (size_t i = 0; i < 64; i++)
	{
		oKeyPad[i] = (uint8_t)(keyBlock[i] ^ 0x5c);
		iKeyPad[i] = (uint8_t)(keyBlock[i] ^ 0x36);
	}

	sgSHA256 inner;
	inner.update(iKeyPad, sizeof(iKeyPad));
	inner.update(msg, msgLen);
	auto innerDigest = inner.digest();

	sgSHA256 outer;
	outer.update(oKeyPad, sizeof(oKeyPad));
	outer.update(innerDigest.data(), innerDigest.size());
	return outer.digest();
}

static inline std::vector<u8> SYS_Pbkdf2HmacSha256(const std::vector<u8> &passwordKey,
										  const std::vector<u8> &salt,
										  u32 iterations,
										  size_t dkLen)
{
	std::vector<u8> dk;
	dk.resize(dkLen);
	if (dkLen == 0)
		return dk;
	if (iterations == 0)
		return {};

	const size_t hLen = 32; // SHA256 output size
	const size_t numBlocks = (dkLen + hLen - 1) / hLen;

	for (u32 blockIndex = 1; blockIndex <= numBlocks; blockIndex++)
	{
		std::vector<u8> saltBlock;
		saltBlock.reserve(salt.size() + 4);
		saltBlock.insert(saltBlock.end(), salt.begin(), salt.end());
		saltBlock.push_back((u8)((blockIndex >> 24) & 0xff));
		saltBlock.push_back((u8)((blockIndex >> 16) & 0xff));
		saltBlock.push_back((u8)((blockIndex >> 8) & 0xff));
		saltBlock.push_back((u8)(blockIndex & 0xff));

		std::array<uint8_t, 32> u = SYS_HmacSha256(passwordKey.data(), passwordKey.size(), saltBlock.data(), saltBlock.size());
		std::array<uint8_t, 32> t = u;

		for (u32 i = 1; i < iterations; i++)
		{
			u = SYS_HmacSha256(passwordKey.data(), passwordKey.size(), u.data(), u.size());
			for (size_t j = 0; j < hLen; j++)
				t[j] ^= u[j];
		}

		const size_t offset = (size_t)(blockIndex - 1) * hLen;
		const size_t toCopy = std::min(hLen, dkLen - offset);
		memcpy(dk.data() + offset, t.data(), toCopy);
	}

	return dk;
}

// ── CRC32 (wraps zlib) ──────────────────────────────────────────────

static inline uint32_t SYS_Crc32(const uint8_t *data, size_t len)
{
	uLong crc = crc32(0L, Z_NULL, 0);
	crc = crc32(crc, data, (uInt)len);
	return (uint32_t)crc;
}

// CRC32 of a file on disk. Returns 0 and sets ok=false on error.
static inline uint32_t SYS_Crc32File(const std::string &filePath, bool *ok = nullptr)
{
	FILE *f = fopen(filePath.c_str(), "rb");
	if (!f) { if (ok) *ok = false; return 0; }

	uLong crc = crc32(0L, Z_NULL, 0);
	uint8_t buf[8192];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
	{
		crc = crc32(crc, buf, (uInt)n);
	}
	fclose(f);
	if (ok) *ok = true;
	return (uint32_t)crc;
}

// CRC32 to hex string (8 chars, lowercase)
static inline std::string SYS_Crc32ToHex(uint32_t crc)
{
	char hex[9];
	snprintf(hex, sizeof(hex), "%08x", crc);
	return std::string(hex);
}
