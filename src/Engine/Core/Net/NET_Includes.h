/*
 *
 *  MTEngine framework (c) 2009 Marcin Skoczylas
 *  Licensed under MIT
 *
 */

#ifndef _NET_INCLUDES_H_
#define _NET_INCLUDES_H_


// CAPABILITY GUARD (MT_ENABLE_ENET). The header stays INCLUDABLE either way and simply
// declares nothing when the capability is off, so a translation unit that
// includes it conditionally still compiles. Same shape as
// Video/CVideoSourceFFmpeg.h.
// MT_CAP_NET_TRANSPORT declared this removable and nothing enforced it: MT_ENABLE_ENET appeared in no source, so the code shipped whatever the manifest said.
#if MT_ENABLE_ENET

#include "CNetServer.h"
#include "CNetClientData.h"
#include "CNetClient.h"
#include "CNetPacket.h"

#endif // MT_ENABLE_ENET

#endif
