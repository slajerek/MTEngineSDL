#include "CNetGamePackets.h"
#include "zlib.h"

using namespace nlohmann;
using namespace std;

CNetPacket *CNetGamePackets::NetDeserializePacket(u8 protocolType, u16 packetType, CByteBuffer *byteBuffer)
{
	LOGD("CNetGamePackets::NetDeserializePacket: protocolType=%02x packetType=%04x", protocolType, packetType);
	
//	if (protocolType != NET_PROTOCOL_TYPE_NET_GAME)
//		return NULL;
	
//	if (packetType == NET_PACKET_TYPE_ACK)
//		return new CNetPacketAck(byteBuffer);
//	
	if (packetType == NET_PACKET_TYPE_JSON)
		return new CNetGamePacketJson(byteBuffer);
	
	LOGError("CNetGamePackets::NetDeserializePacket: unknown packetType=%4.4x", packetType);
	return NULL;
}

const char *CNetGamePackets::NetGetPacketNameFromType(u8 protocolType, u16 packetType)
{
//	if (protocolType != NET_PROTOCOL_TYPE_NET_GAME)
//		return NULL;
	
	if (packetType == NET_PACKET_TYPE_ACK)
	{
		return "ACK";
	}
	else if (packetType == NET_PACKET_TYPE_JSON)
	{
		return "JSON";
	}

	return "<NetGameUnknownPacket>";
}

CNetGamePacketJson::CNetGamePacketJson(CByteBuffer *byteBuffer)
{
	this->protocolType = NET_PROTOCOL_TYPE_NET_GAME;
	this->packetType = NET_PACKET_TYPE_JSON;
	this->Deserialize(byteBuffer);
}

CNetGamePacketJson::CNetGamePacketJson(nlohmann::json sendJson)
{
	this->protocolType = NET_PROTOCOL_TYPE_NET_GAME;
	this->packetType = NET_PACKET_TYPE_JSON;
	this->jsonPayload = sendJson;
}

CNetGamePacketJson::~CNetGamePacketJson()
{
}

void CNetGamePacketJson::Serialize(CByteBuffer *byteBuffer)
{
	{
		string dumpStr = jsonPayload.dump();
		if (dumpStr.size() > 200)
		{
			LOGD("CNetGamePacketJson::Serialize: json=%s... (%zu bytes total)", dumpStr.substr(0, 200).c_str(), dumpStr.size());
		}
		else
		{
			LOGD("CNetGamePacketJson::Serialize: json=%s", dumpStr.c_str());
		}
	}
	string sendJsonStr = jsonPayload.dump();
	u32 rawSize = (u32)sendJsonStr.size();

	if (rawSize > NET_JSON_COMPRESSION_THRESHOLD)
	{
		uLong compBound = compressBound(rawSize);
		u8 *compBuf = new u8[compBound];
		uLong compSize = compBound;
		int ret = compress2(compBuf, &compSize, (const Bytef *)sendJsonStr.data(), rawSize, 9);

		if (ret == Z_OK && compSize < rawSize)
		{
			// Compressed is smaller — send compressed
			byteBuffer->PutU8(1);
			byteBuffer->PutU32(rawSize);
			byteBuffer->PutU32((u32)compSize);
			byteBuffer->PutBytes(compBuf, (u32)compSize);
			LOGD("CNetGamePacketJson::Serialize: compressed %u -> %lu bytes", rawSize, compSize);
			delete[] compBuf;
			return;
		}
		delete[] compBuf;
	}

	// Send uncompressed (small payload or compression didn't help)
	byteBuffer->PutU8(0);
	byteBuffer->PutStdString(sendJsonStr);
}

void CNetGamePacketJson::Deserialize(CByteBuffer *byteBuffer)
{
	LOGD("CNetGamePacketJson::Deserialize");
	u8 flag = byteBuffer->GetU8();

	if (flag == 1)
	{
		// Compressed payload
		u32 decompressedSize = byteBuffer->GetU32();
		u32 compressedSize = byteBuffer->GetU32();

		u8 *compData = new u8[compressedSize];
		byteBuffer->GetBytes(compData, compressedSize);

		u8 *decompData = new u8[decompressedSize];
		uLongf destLen = decompressedSize;
		int ret = uncompress(decompData, &destLen, compData, compressedSize);
		delete[] compData;

		if (ret != Z_OK || destLen != decompressedSize)
		{
			LOGError("CNetGamePacketJson::Deserialize: zlib uncompress failed (ret=%d)", ret);
			delete[] decompData;
			jsonPayload = json::object();
			return;
		}

		string jsonStr(reinterpret_cast<const char *>(decompData), decompressedSize);
		delete[] decompData;

		LOGD("CNetGamePacketJson::Deserialize: decompressed %u -> %u bytes", compressedSize, decompressedSize);
		jsonPayload = json::parse(jsonStr, nullptr, false);
	}
	else
	{
		// Uncompressed payload
		string jsonStr = byteBuffer->GetStdString();
		jsonPayload = json::parse(jsonStr, nullptr, false);
	}

	if (jsonPayload.is_discarded())
	{
		LOGError("CNetGamePacketJson::Deserialize: failed to parse json");
		jsonPayload = json::object();
	}
}
