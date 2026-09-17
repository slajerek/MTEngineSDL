/*
 *
 *  MTEngine framework (c) 2009 Marcin Skoczylas
 *  Licensed under MIT
 *
 */

#ifndef _CNETPACKET_H_
#define _CNETPACKET_H_


// CAPABILITY GUARD (MT_ENABLE_ENET). The header stays INCLUDABLE either way and simply
// declares nothing when the capability is off, so a translation unit that
// includes it conditionally still compiles. Same shape as
// Video/CVideoSourceFFmpeg.h.
// MT_CAP_NET_TRANSPORT declared this removable and nothing enforced it: MT_ENABLE_ENET appeared in no source, so the code shipped whatever the manifest said.
#if MT_ENABLE_ENET

#include "SYS_Defs.h"
#include "DBG_Log.h"
#include "NET_Main.h"
#include "CByteBuffer.h"
#include "enet.h"
#include <list>

class CNetClientData;
class CByteBuffer;

#define NET_PACKET_TYPE_NOTHING			0
#define NET_PACKET_TYPE_AUTHORIZE		1
#define NET_PACKET_TYPE_AUTHORIZED		2

// Challenge-response extension for NET_PACKET_TYPE_AUTHORIZE.
// Sent/received while client is in NET_CLIENT_STATE_CONNECTED (pre-ONLINE).
#define NET_PACKET_TYPE_AUTHORIZE_CHALLENGE	3
#define NET_PACKET_TYPE_AUTHORIZE_RESPONSE	4

class CNetPacket
{
public:
	CNetPacket();
	virtual ~CNetPacket();

	u8 protocolType;
	u16 packetType;

	bool isReliable;
	u32 frameNum;

	CNetClientData *clientData;

	virtual void Serialize(CByteBuffer *byteBuffer);
	virtual void Deserialize(CByteBuffer *byteBuffer);
};

class CNetPacketCallback
{
public:
	virtual CNetPacket *NetDeserializePacket(u8 protocolType, u16 packetType, CByteBuffer *byteBuffer);
	virtual const char *NetGetPacketNameFromType(u8 protocolType, u16 packetType);
	virtual ~CNetPacketCallback();
};

// log connection server TO
void LOGCSTO(CNetClientData *netClientData, const char *fmt, ... );
#define LOGSTO(...) LOGCSTO(netClientData, __VA_ARGS__)
#define LOGST(...) LOGCSTO(netClientData, __VA_ARGS__)

// log connection server FROM
void LOGCSFROM(CNetClientData *netClientData, const char *fmt, ... );
#define LOGSFROM(...) LOGCSFROM(netClientData, __VA_ARGS__)
#define LOGSF(...) LOGCSFROM(netClientData, __VA_ARGS__)

// log connection server TO
void LOGCSTO(CNetClientData *netClientData, u8 protocolType, u16 packetType, const char *fmt, ... );

// log connection server FROM
void LOGCSFROM(CNetClientData *netClientData, u8 protocolType, u16 packetType, const char *fmt, ... );

// log connection server TO
void LOGCCTO(std::string clientName, const char *fmt, ... );
#define LOGCTO(...) LOGCCTO(clientLoginName, __VA_ARGS__)
#define LOGCT(...) LOGCCTO(netClient->clientLoginName, __VA_ARGS__)

// log connection server FROM
void LOGCCFROM(std::string clientName, const char *fmt, ... );
#define LOGCFROM(...) LOGCCFROM(clientLoginName, __VA_ARGS__)
#define LOGCF(...) LOGCCFROM(netClient->clientLoginName, __VA_ARGS__)

// log connection server TO
void LOGCCTO(std::string clientName, u8 protocolType, u16 packetType, const char *fmt, ... );

// log connection server FROM
void LOGCCFROM(std::string clientName, u8 protocolType, u16 packetType, const char *fmt, ... );

#endif // MT_ENABLE_ENET

#endif

//_CNETPACKET_H_
