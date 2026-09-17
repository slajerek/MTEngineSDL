#pragma once

// Shared listen-interface policy for the NetGame HTTP servers (auth service,
// asset server, ...). Kept as a tiny header rather than duplicated per server
// so the loopback definition cannot drift between them.

#include <string>

// True for the loopback interface: "127.0.0.1", "localhost", "::1" (exact match).
static inline bool NetGameIsLoopbackInterface(const std::string &iface)
{
	return iface == "127.0.0.1" || iface == "localhost" || iface == "::1";
}
