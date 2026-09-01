#ifndef _MT_NETGAMEAUTHDOMAINS_H_
#define _MT_NETGAMEAUTHDOMAINS_H_

// HMAC domain separators for the NetGame auth protocols.
//
// These used to be one app's protocol constants hardcoded into engine source
// -- a public MIT repo carrying a commercial product's live protocol strings
// (unification plan, Phase 6 Tier 1). They are DOMAIN SEPARATORS, not
// secrets: their job is to prevent cross-protocol hash confusion, so the
// VALUE only matters for wire compatibility between one app's client and
// server. An app with an existing deployment registers its historical
// strings in MT_PreInit(); everyone else gets the neutral defaults.
//
// Header-only (C++17 inline state) on purpose: no new TU, so nothing to add
// to the three build systems.

inline const char *&MTNetGameAdminDomainRef()
{
	static const char *v = "MTEngine.AdminSecret.v1";
	return v;
}

inline const char *&MTNetGameInternalDomainRef()
{
	static const char *v = "MTEngine.InternalSecret.v1";
	return v;
}

// Call from MT_PreInit() BEFORE any registry/auth traffic. Pass nullptr to
// leave a value unchanged.
inline void MT_SetNetGameAuthDomains(const char *adminDomain, const char *internalDomain)
{
	if (adminDomain)    MTNetGameAdminDomainRef() = adminDomain;
	if (internalDomain) MTNetGameInternalDomainRef() = internalDomain;
}

#endif // _MT_NETGAMEAUTHDOMAINS_H_
