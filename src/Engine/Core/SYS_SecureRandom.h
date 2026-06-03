#pragma once

#include "SYS_Defs.h"

#include <cstddef>

#if defined(LINUX)
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#endif

// Best-effort secure random bytes from the OS CSPRNG.
// Returns true on success.
static inline bool SYS_SecureRandomBytes(void *outBytes, size_t len)
{
	if (len == 0)
		return true;
	if (outBytes == NULL)
		return false;

#if defined(MACOS) || defined(__APPLE__)
	// arc4random_buf is backed by the OS CSPRNG.
	arc4random_buf(outBytes, len);
	return true;

#elif defined(WIN32)
	// Use RtlGenRandom (SystemFunction036) via Advapi32.dll.
	// Load dynamically to avoid additional linker dependencies.
	static HMODULE sAdvapi = LoadLibraryA("Advapi32.dll");
	if (!sAdvapi)
		return false;

	typedef BOOLEAN (APIENTRY *RtlGenRandomFn)(PVOID, ULONG);
	static RtlGenRandomFn sRtlGenRandom = (RtlGenRandomFn)GetProcAddress(sAdvapi, "SystemFunction036");
	if (!sRtlGenRandom)
		return false;

	u8 *p = (u8 *)outBytes;
	size_t remaining = len;
	while (remaining > 0)
	{
		ULONG chunk = (remaining > 0xFFFFFFFFu) ? 0xFFFFFFFFu : (ULONG)remaining;
		if (!sRtlGenRandom(p, chunk))
			return false;
		p += chunk;
		remaining -= chunk;
	}
	return true;

#elif defined(LINUX)
	// /dev/urandom is an OS CSPRNG source.
	int fd = open("/dev/urandom", O_RDONLY);
	if (fd < 0)
		return false;

	u8 *p = (u8 *)outBytes;
	size_t remaining = len;
	while (remaining > 0)
	{
		ssize_t r = read(fd, p, remaining);
		if (r < 0)
		{
			if (errno == EINTR)
				continue;
			close(fd);
			return false;
		}
		if (r == 0)
		{
			close(fd);
			return false;
		}
		p += (size_t)r;
		remaining -= (size_t)r;
	}
	close(fd);
	return true;

#else
	// Unsupported platform.
	return false;
#endif
}
