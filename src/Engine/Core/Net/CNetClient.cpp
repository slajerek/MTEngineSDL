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
#include "CNetPacket.h"

CNetClient::CNetClient(const char *serverConnectAddress, int serverConnectPort, u64 serverId, std::string clientLoginName, std::vector<u8> passwordHash)
{
	this->Init(NULL, NULL, serverConnectAddress, serverConnectPort, serverId, clientLoginName, passwordHash);
}

CNetClient::CNetClient(CNetClientCallback *clientCallback, CNetPacketCallback *packetCallback, const char *serverConnectAddress, int serverConnectPort, u64 serverId, std::string clientLoginName, std::vector<u8> passwordHash)
{
	this->Init(clientCallback, packetCallback, serverConnectAddress, serverConnectPort, serverId, clientLoginName, passwordHash);
}

void CNetClient::Init(CNetClientCallback *clientCallback, CNetPacketCallback *packetCallback, const char *serverConnectAddress, int serverConnectPort, u64 serverId, std::string clientLoginName, std::vector<u8> passwordHash)
{
	this->packetMutex = new CSlrMutex("CNetClient");
	
	this->status = NET_CLIENT_STATUS_OFFLINE;

	this->AddClientCallback(clientCallback);
	this->AddPacketCallback(packetCallback);

	strcpy(this->serverAddress, serverConnectAddress);
	this->serverPort = serverConnectPort;

	this->serverId = serverId;
	this->clientLoginName = clientLoginName;
	this->passwordHash = passwordHash;

	this->peer = NULL;

	this->reconnectDelay = 1000;

	byteBufferIn = new CByteBuffer();
	byteBufferReliableOut = new CByteBuffer();
	byteBufferNotReliableOut = new CByteBuffer();
}

CNetClient::~CNetClient()
{
	delete byteBufferIn;
	delete byteBufferReliableOut;
	delete byteBufferNotReliableOut;
}

void CNetClient::SetClientLoginDetails(std::string clientLoginName, vector<u8> passwordHash)
{
	LOGD("CNetClient::SetClientLoginDetails");
	LockMutex();
	
	this->clientLoginName = clientLoginName;
	
	this->passwordHash = passwordHash;
	
	if (status == NET_CLIENT_STATUS_OFFLINE)
	{
		status = NET_CLIENT_STATUS_CONNECTING;
	}
	else 
	{
		status = NET_CLIENT_STATUS_RECONNECT;
	}
	
	UnlockMutex();
}

void CNetClient::Connect()
{
	this->status = NET_CLIENT_STATUS_CONNECTING;

	LOGD("CNetClient::Connect");
	if (isRunning == false)
	{
		SYS_StartThread(this);
	}
}

void CNetClient::ThreadRun(void *data)
{
	LOGD("CNetClient::ThreadRun: thread started");

	strcpy(threadName, "Net-Client");

	while (status != NET_CLIENT_STATUS_SHUTDOWN)
	{
		if (status == NET_CLIENT_STATUS_RECONNECT)
		{
			LOGD("NET_CLIENT_STATUS_RECONNECT sleep");
			SYS_Sleep(reconnectDelay);
			LOGD("NET_CLIENT_STATUS_RECONNECT sleep done");
		}

		if (status == NET_CLIENT_STATUS_OFFLINE)
		{
			SYS_Sleep(reconnectDelay);
			continue;
		}

		this->status = NET_CLIENT_STATUS_CONNECTING;

		ENetHost * client;
		client = enet_host_create (NULL,	// create a client host
								   1,		// only allow 1 outgoing connection
								   2,		// allow up 2 channels to be used, 0 and 1
								   0,		//57600 / 8 // 56K modem with 56 Kbps downstream bandwidth
								   0		//14400 / 8 // 56K modem with 14 Kbps upstream bandwidth
				);

		if (client == NULL)
		{
			LOGError("An error occurred while trying to create an ENet client host");
			this->status = NET_CLIENT_STATUS_RECONNECT;
			continue;
		}

		ENetAddress address;
		ENetEvent event;

		// connect
		enet_address_set_host (&address, serverAddress);
		address.port = serverPort;

		// Initiate the connection, allocating the two channels 0 and 1.
		peer = enet_host_connect (client, &address, 2, 0);
		if (peer == NULL)
		{
			LOGError("No available peers for initiating an ENet connection");
			this->status = NET_CLIENT_STATUS_RECONNECT;
			continue;
		}

		// Wait up to 5 seconds for the connection attempt to succeed.
		LOGCFROM("Connecting to %s:%d", serverAddress, serverPort);
		if (enet_host_service(client, & event, 5000) > 0 &&
			event.type == ENET_EVENT_TYPE_CONNECT)
		{
			LOGCFROM("Connection to %s:%d succeeded", serverAddress, serverPort);
			this->status = NET_CLIENT_STATUS_CONNECTED;
		}
		else
		{
			// Either the 5 seconds are up or a disconnect event was
			// received. Reset the peer in the event the 5 seconds
			// had run out without any significant event.
			enet_peer_reset (peer);
			LOGError("Connection to %s:%d failed", serverAddress, serverPort);
			this->status = NET_CLIENT_STATUS_RECONNECT;
			continue;
		}

		this->status = NET_CLIENT_STATUS_CONNECTED;

		//enet_peer_ping_interval(peer, 1);

		LockMutex();
		
		// login / authorize packet
		byteBufferReliableOut->Reset();
		byteBufferReliableOut->PutByte(NET_PACKET_TYPE_AUTHORIZE);
		byteBufferReliableOut->PutU32(NET_PROTOCOL_VERSION);
		byteBufferReliableOut->PutU64(serverId);
		byteBufferReliableOut->PutStdString(clientLoginName);
		byteBufferReliableOut->PutU16(passwordHash.size());
		byteBufferReliableOut->PutBytes(passwordHash.data(), passwordHash.size());

		this->SendReliableBufferAsync(byteBufferReliableOut);
		
		UnlockMutex();

		// check login
		while (status == NET_CLIENT_STATUS_CONNECTED)
		{
			if(enet_host_service (client, & event, NET_SERVICE_EVENT_SLEEP_TIME) > 0)
			{
				switch (event.type)
				{
				case ENET_EVENT_TYPE_RECEIVE:
				{
					byteBufferIn->SetData((u8*)event.packet->data, (u32)event.packet->dataLength);

					u32 packetType = byteBufferIn->GetByte();
					bool isAuthorized = false;
					if (packetType == NET_PACKET_TYPE_AUTHORIZED)
					{
						isAuthorized = byteBufferIn->GetBool();
					}

					// Clean up the packet now that we're done using it
					enet_packet_destroy (event.packet);
					byteBufferIn->data = NULL;

					if (byteBufferIn->error)
					{
						LOGError("FROM: parse error, disconnect");
						this->SetStatusDisconnectAndReconnect();
						break;
					}

					if (isAuthorized)
					{
						LOGCFROM("AUTHORIZED, go online");
						this->status = NET_CLIENT_STATUS_ONLINE;
						
						for (std::list<CNetClientCallback *>::iterator it = this->clientCallbacks.begin();
							it != this->clientCallbacks.end(); it++)
						{
							CNetClientCallback *callback = (*it);
							callback->NetClientCallbackConnected(this);
						}

						break;
					}
					else
					{
						LOGError("CONNECTED: not authorized");
						this->Disconnect();
						
						for (std::list<CNetClientCallback *>::iterator it = this->clientCallbacks.begin();
							it != this->clientCallbacks.end(); it++)
						{
							CNetClientCallback *callback = (*it);
							callback->NetClientCallbackNotAuthorized(this);
						}

						break;
					}

//					LOGD("[c] CONNECTED: A packet of length %u containing %s was received from %s on channel %u.",
//							event.packet -> dataLength,
//							event.packet -> data,
//							event.peer -> data,
//							event.channelID);

					break;
				}
				case ENET_EVENT_TYPE_DISCONNECT:
					LOGCC("CONNECTED: disconnected from %s:%d", serverAddress, serverPort);
					event.peer->data = NULL;
					this->SetStatusDisconnectAndReconnect();
					break;

				default:
					LOGError("CONNECTED: enet_host_service: unknown event %d", event.type);
					this->SetStatusDisconnectAndReconnect();
					break;
				}
			}
		}

		this->LockMutex();
		byteBufferIn->Reset();
		byteBufferReliableOut->Reset();
		byteBufferNotReliableOut->Reset();
		this->UnlockMutex();

		while(status == NET_CLIENT_STATUS_ONLINE)
		{
			//LOGD("status == NET_CLIENT_STATUS_ONLINE");
			
			if(enet_host_service (client, & event, NET_SERVICE_EVENT_SLEEP_TIME) > 0)
			{
				switch (event.type)
				{
					case ENET_EVENT_TYPE_RECEIVE:
					{
						//LOGD("ENET_EVENT_TYPE_RECEIVE");
						u32 dataLength = (u32)event.packet->dataLength;
						//LOGD(": dataLength=%d", dataLength);
						
						byteBufferIn->SetData((u8*)event.packet->data, dataLength);
						this->ParseDataBuffer(byteBufferIn);

						// Clean up the packet now that we're done using it
						enet_packet_destroy (event.packet);
						byteBufferIn->data = NULL;

						if (byteBufferIn->error)
						{
							LOGError("FROM: parse error, disconnect");
							this->SetStatusDisconnectAndReconnect();
						}
					}
					break;

					case ENET_EVENT_TYPE_DISCONNECT:
					{
						LOGCC("FROM: EVENT DISCONNECT");

						// Reset the peer's client information.
						event.peer->data = NULL;
						this->SetStatusDisconnectAndReconnect();
						break;
					}
					default:
						LOGError("CNetClient::ThreadRun enet_host_service: unknown event %d", event.type);
						break;
				}
			}
			
			this->NetLogic();
		}

		LOGD("enet_host_destroy");
		this->LockMutex();
		enet_host_destroy(client);
		this->peer = NULL;
		this->UnlockMutex();
	}

	LOGD("CNetClient::ThreadRun: thread finished");
}

void CNetClient::NetLogic()
{
//	LOGD("CNetClient::NetLogic");

	// something in from clients?
	while(true)
	{
		this->LockMutex();
		if (this->receivedPackets.empty())
		{
			this->UnlockMutex();
			break;
		}

		std::list<CNetPacket *>::iterator it = this->receivedPackets.begin();
		CNetPacket *packet = (CNetPacket *)*it;
		this->receivedPackets.pop_front();
		this->UnlockMutex();
		
		for (std::list<CNetClientCallback *>::iterator it = this->clientCallbacks.begin();
			it != this->clientCallbacks.end(); it++)
		{
			CNetClientCallback *callback = (*it);
			callback->NetClientProcessPacket(packet);
		}

		delete packet;
	}

//	LOGD("send out packets relOut=%d notRelOut=%d", this->byteBufferReliableOut->length, this->byteBufferNotReliableOut->length);

	this->LockMutex();
	
	// send out packets
	if (this->byteBufferReliableOut->length != 0)
	{
		SendReliableBufferAsync(byteBufferReliableOut);
		byteBufferReliableOut->Reset();
	}

	if (this->byteBufferNotReliableOut->length != 0)
	{
		SendNotReliableBufferAsync(byteBufferNotReliableOut);
		byteBufferNotReliableOut->Reset();
	}
	
	this->UnlockMutex();
	
	// net logic
	for (std::list<CNetClientCallback *>::iterator it = this->clientCallbacks.begin();
		 it != this->clientCallbacks.end(); it++)
	{
		CNetClientCallback *callback = (*it);
		callback->NetClientLogic(this);
	}

}

void CNetClient::IssuePacket(u8 protocolType, bool isReliable, CNetPacket *packet)
{
	packet->protocolType = protocolType;
	this->IssuePacket(isReliable, packet);
}

void CNetClient::IssuePacket(bool isReliable, CNetPacket *packet)
{
	LOGD("CNetClient::IssuePacket: status=%s", GetStatusName());
	
	LOGCCTO(this->clientLoginName.c_str(), packet->protocolType, packet->packetType, "IssuePacket");

	this->LockMutex();

	if (this->status != NET_CLIENT_STATUS_ONLINE)
	{
		LOGError("CNetClient::IssuePacket: status is %s, skipping packet", GetStatusName());
		this->UnlockMutex();
		return;
	}
	
	if (isReliable)
	{
		byteBufferReliableOut->PutU8(packet->protocolType);
		byteBufferReliableOut->PutU16(packet->packetType);
		packet->Serialize(byteBufferReliableOut);
	}
	else
	{
		byteBufferNotReliableOut->PutU8(packet->protocolType);
		byteBufferNotReliableOut->PutU16(packet->packetType);
		packet->Serialize(byteBufferNotReliableOut);
	}
	LOGD("byteBufferReliableOut len=%d", byteBufferReliableOut->length);
	LOGD("byteBufferNotReliableOut len=%d", byteBufferNotReliableOut->length);
	this->UnlockMutex();
}

void CNetClient::ParseDataBuffer(CByteBuffer *byteBuffer)
{
//	char *hexStr = byteBuffer->toHexString();
//	LOGCFROM("ParseDataBuffer: %s", hexStr);
//	delete hexStr;

	u16 numPackets = 0;
	while(!byteBuffer->IsEof())
	{
		LOGCFROM("parse packet buffer");

		u8 protocolType = byteBuffer->GetU8();
		u16 packetType = byteBuffer->GetU16();

		LOGCFROM("packet header: %2.2x/%4.4x", protocolType, packetType);

		if (packetType == NET_PACKET_TYPE_NOTHING)
		{
			LOGError("CNetServer::ParseDataBuffer: NET_PACKET_TYPE_NOTHING received");
		}
		else
		{
			bool parsed = false;

			LOGD("parsing...");
			for (std::list<CNetPacketCallback *>::iterator it = this->packetCallbacks.begin();
				it != this->packetCallbacks.end(); it++)
			{
				CNetPacketCallback *callback = (*it);

				LOGD("parsing callback");
				CNetPacket *packet = callback->NetDeserializePacket(protocolType, packetType, byteBuffer);

				if (packet != NULL)
				{
					// parsed packets
					//packet->playerData = NULL;
					packet->protocolType = protocolType;
					packet->packetType = packetType;

					this->LockMutex();
					receivedPackets.push_back(packet);
					this->UnlockMutex();

					parsed = true;
					break;
				}
			}

			if (parsed == false)
			{
				char *hexStr = byteBuffer->toHexString();
				LOGError("CNetClient::ParseDataBuffer: unknown packet type=%4.4x data=%s index=%d",
					packetType, hexStr, byteBuffer->index);
				free(hexStr);
				this->SetStatusDisconnectAndReconnect();
				return;
			}
		}

		numPackets++;
	}

	//LOGCFROM("ParseDataBuffer: done, parsed %d packets", numPackets);
}


void CNetClient::SendReliableBufferAsync(CByteBuffer *byteBuffer)
{
#if defined(DEBUG_PRINT_PACKETS)
	char *hexStr = byteBuffer->toHexString();
	LOGCTO("SendReliableBufferAsync: %s", hexStr);
	delete hexStr;
#endif

	ENetPacket *packet = enet_packet_create (byteBuffer->data, byteBuffer->length, ENET_PACKET_FLAG_RELIABLE);
	enet_peer_send (peer, 0, packet);
}

void CNetClient::SendNotReliableBufferAsync(CByteBuffer *byteBuffer)
{
#if defined(DEBUG_PRINT_PACKETS)
	char *hexStr = byteBuffer->toHexString();
	LOGCTO("SendReliableBufferAsync: %s", hexStr);
	delete hexStr;
#endif

	ENetPacket *packet = enet_packet_create (byteBuffer->data, byteBuffer->length, ENET_PACKET_FLAG_UNSEQUENCED);
	enet_peer_send (peer, 0, packet);
}


void CNetClient::AddClientCallback(CNetClientCallback *clientCallback)
{
	if (clientCallback != NULL)
	{
		LOGCC("CNetClient::AddClientCallback");
		this->clientCallbacks.push_back(clientCallback);
	}
}

void CNetClient::AddPacketCallback(CNetPacketCallback *packetCallback)
{
	if (packetCallback != NULL)
	{
		LOGCC("CNetClient::AddPacketCallback");
		this->packetCallbacks.push_back(packetCallback);
	}
}

bool CNetClient::IsOnline()
{
	return (this->status == NET_CLIENT_STATUS_ONLINE);
}

void CNetClient::SetStatusDisconnectAndReconnect()
{
	LOGCFROM("CNetClient::SetStatusDisconnectAndReconnect");
	this->status = NET_CLIENT_STATUS_RECONNECT;
}

void CNetClient::Disconnect()
{
	LOGCFROM("CNetClient::SetStatusDisconnectAndReconnect");
	this->status = NET_CLIENT_STATUS_OFFLINE;
}

const char *CNetClient::GetStatusName()
{
	switch (this->status)
	{
		case NET_CLIENT_STATUS_SHUTDOWN:
			return "SHUTDOWN";
		case NET_CLIENT_STATUS_OFFLINE:
			return "OFFLINE";
		case NET_CLIENT_STATUS_RECONNECT:
			return "RECONNECT";
		case NET_CLIENT_STATUS_CONNECTING:
			return "CONNECTING";
		case NET_CLIENT_STATUS_CONNECTED:
			return "CONNECTED";
		case NET_CLIENT_STATUS_ONLINE:
			return "ONLINE";
		default:
			return "UNKNOWN";
	}
}

void CNetClient::LockMutex()
{
	//LOGD("CNetClient::LockMutex");
	packetMutex->Lock();
}

void CNetClient::UnlockMutex()
{
	//LOGD("CNetClient::UnlockMutex");
	packetMutex->Unlock();
}

CNetClientCallback::~CNetClientCallback()
{
}

void CNetClientCallback::NetClientCallbackConnected(CNetClient *netClient)
{
	LOGD("CNetClientCallback::NetClientCallbackConnected");
}

void CNetClientCallback::NetClientCallbackNotAuthorized(CNetClient *netClient)
{
	LOGWarning("CNetClientCallback::NetClientCallbackNotAuthorized");
}

void CNetClientCallback::NetClientProcessPacket(CNetPacket *packet)
{
	LOGError("CNetClientCallback::NetClientProcessPacket");
}

void CNetClientCallback::NetClientLogic(CNetClient *netClient)
{
}

