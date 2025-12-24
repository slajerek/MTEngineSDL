/*
 *
 *  MTEngine framework (c) 2009 Marcin Skoczylas
 *  Licensed under MIT
 *
 */

#include "enet.h"
#include "SYS_Main.h"
#include "SYS_Funct.h"
#include "NET_Main.h"
#include "NET_Includes.h"

CNetPacket::CNetPacket()
{
	this->protocolType = 0;
	this->packetType = NET_PACKET_TYPE_NOTHING;
	this->frameNum = 0;
	this->isReliable = true;
	this->clientData = NULL;
}

void CNetPacket::Serialize(CByteBuffer *byteBuffer)
{
	byteBuffer->PutU8(this->protocolType);
	byteBuffer->PutU16(this->packetType);
}

void CNetPacket::Deserialize(CByteBuffer *byteBuffer)
{
	// done by deserializer
	//this->gameType = byteBuffer->GetU16();
	//this->packetType = byteBuffer->GetU16();
}

CNetPacket::~CNetPacket()
{
}

CNetPacket *CNetPacketCallback::NetDeserializePacket(u8 protocolType, u16 packetType, CByteBuffer *byteBuffer)
{
	LOGError("CNetPacketCallback::NetDeserializePacket: protocolType=%02x packetType=%04x", protocolType, packetType);
	return NULL;
}


const char *CNetPacketCallback::NetGetPacketNameFromType(u8 protocolType, u16 packetType)
{
	if (protocolType == NET_SERVER_TYPE_SERVER_MAIN)
	{
		if (packetType == NET_PACKET_TYPE_NOTHING)
		{
			return "NOTHING";
		}
		else if (packetType == NET_PACKET_TYPE_AUTHORIZE)
		{
			return "AUTHORIZE";
		}
		else if (packetType == NET_PACKET_TYPE_AUTHORIZED)
		{
			return "AUTHORIZED";
		}
	}
	return NULL;
}

CNetPacketCallback::~CNetPacketCallback()
{
}

#define LOG_BUFFER_LENGTH	4096

// log connection server TO
void LOGCSTO(CNetClientData *netClientData, const char *fmt, ... )
{
#if !defined(FINAL_RELEASE)
    char buffer[LOG_BUFFER_LENGTH] = {0};

    va_list args;

    va_start(args, fmt);
    vsnprintf(buffer, LOG_BUFFER_LENGTH, fmt, args);
    va_end(args);

    LOGCS(">>>TO> %s: %s", netClientData->clientName, buffer);
#endif
}

// log connection server FROM
void LOGCSFROM(CNetClientData *netClientData, const char *fmt, ... )
{
#if !defined(FINAL_RELEASE)
    char buffer[LOG_BUFFER_LENGTH] = {0};

    va_list args;

    va_start(args, fmt);
    vsnprintf(buffer, LOG_BUFFER_LENGTH, fmt, args);
    va_end(args);

    LOGCS("<FROM< %s: %s", netClientData->clientName, buffer);
#endif
}

// log connection server TO
void LOGCSTO(CNetClientData *netClientData, u8 protocolType, u16 packetType, const char *fmt, ... )
{
#if !defined(FINAL_RELEASE)
    char buffer[LOG_BUFFER_LENGTH] = {0};

    va_list args;

    va_start(args, fmt);
    vsnprintf(buffer, LOG_BUFFER_LENGTH, fmt, args);
    va_end(args);

    LOGCS(">>>TO> %s %2.2x/%4.4x: %s", netClientData->clientName, protocolType, packetType, buffer);
#endif
}

// log connection server FROM
void LOGCSFROM(CNetClientData *netClientData, u8 protocolType, u16 packetType, const char *fmt, ... )
{
#if !defined(FINAL_RELEASE)
    char buffer[LOG_BUFFER_LENGTH] = {0};

    va_list args;

    va_start(args, fmt);
    vsnprintf(buffer, LOG_BUFFER_LENGTH, fmt, args);
    va_end(args);

    LOGCS("<FROM< %s %2.2x/%4.4x: %s", netClientData->clientName, protocolType, packetType, buffer);
#endif
}

// log connection client TO
void LOGCCTO(std::string clientName, const char *fmt, ... )
{
#if !defined(FINAL_RELEASE)
    char buffer[LOG_BUFFER_LENGTH] = {0};

    va_list args;

    va_start(args, fmt);
    vsnprintf(buffer, LOG_BUFFER_LENGTH, fmt, args);
    va_end(args);

    LOGCC("%s >TO>>> %s", clientName, buffer);
#endif
}

// log connection client FROM
void LOGCCFROM(std::string clientName, const char *fmt, ... )
{
#if !defined(FINAL_RELEASE)
    char buffer[LOG_BUFFER_LENGTH] = {0};

    va_list args;

    va_start(args, fmt);
    vsnprintf(buffer, LOG_BUFFER_LENGTH, fmt, args);
    va_end(args);

    LOGCC("%s <FROM< %s", clientName, buffer);
#endif
}

// log connection client TO
void LOGCCTO(std::string clientName, u8 protocolType, u16 packetType, const char *fmt, ... )
{
#if !defined(FINAL_RELEASE)
    char buffer[LOG_BUFFER_LENGTH] = {0};

    va_list args;

    va_start(args, fmt);
    vsnprintf(buffer, LOG_BUFFER_LENGTH, fmt, args);
    va_end(args);

    LOGCC("%s >TO>>> %2.2x/%4.4x: %s", clientName, protocolType, packetType, buffer);
#endif
}

// log connection client FROM
void LOGCCFROM(std::string clientName, u8 protocolType, u16 packetType, const char *fmt, ... )
{
#if !defined(FINAL_RELEASE)
    char buffer[LOG_BUFFER_LENGTH] = {0};

    va_list args;

    va_start(args, fmt);
    vsnprintf(buffer, LOG_BUFFER_LENGTH, fmt, args);
    va_end(args);

    LOGCC("%s <FROM< %2.2x/%4.4x: %s", clientName, protocolType, packetType, buffer);
#endif
}

