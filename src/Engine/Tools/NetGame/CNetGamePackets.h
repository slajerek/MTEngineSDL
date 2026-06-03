#pragma once

#include "CNetPacket.h"
#include "json.hpp"

#define NET_PROTOCOL_TYPE_NET_GAME			4
#define NET_PROTOCOL_NET_GAME_VERSION		0

// Backward compatibility aliases
#define NET_PROTOCOL_TYPE_LIGHT_HEROES		NET_PROTOCOL_TYPE_NET_GAME
#define NET_PROTOCOL_LIGHT_HEROES_VERSION	NET_PROTOCOL_NET_GAME_VERSION

#define NET_PACKET_TYPE_NOTHING			0
#define NET_PACKET_TYPE_ACK				1
#define NET_PACKET_TYPE_JSON		2

// JSON payloads larger than this (in bytes) are zlib-compressed before sending
#define NET_JSON_COMPRESSION_THRESHOLD	512

class CNetGamePackets : public CNetPacketCallback
{
public:
	virtual CNetPacket *NetDeserializePacket(u8 protocolType, u16 packetType, CByteBuffer *byteBuffer);
	virtual const char *NetGetPacketNameFromType(u8 protocolType, u16 packetType);
};

class CNetGamePacketJson : public CNetPacket
{
public:
	CNetGamePacketJson(CByteBuffer *byteBuffer);
	CNetGamePacketJson(nlohmann::json j);
	virtual ~CNetGamePacketJson();

	nlohmann::json jsonPayload;
	
	virtual void Serialize(CByteBuffer *byteBuffer);
	virtual void Deserialize(CByteBuffer *byteBuffer);
};
