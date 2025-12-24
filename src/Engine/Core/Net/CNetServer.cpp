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
#include "CByteBuffer.h"

//#define DEBUG_PRINT_PACKETS

CNetServer::CNetServer(int serverPort)
{
	this->Init(NULL, NULL, serverPort);
}

CNetServer::CNetServer(CNetServerCallback *serverCallback, CNetPacketCallback *packetCallback, int serverPort)
{
	this->Init(serverCallback, packetCallback, serverPort);
}

void CNetServer::Init(CNetServerCallback *serverCallback, CNetPacketCallback *packetCallback, int serverPort)
{
	this->packetMutex = new CSlrMutex("CNetServer");
	
	this->status = NET_SERVER_STATUS_OFFLINE;
	
	this->serverPort = serverPort;

	this->AddServerCallback(serverCallback);
	this->AddPacketCallback(packetCallback);

	for (u32 i = 0; i < NET_MAX_CLIENTS; i++)
	{
		clients[i] = new CNetClientData(this, NULL, i);
	}

	this->currentFrame = 0;

	byteBufferIn = new CByteBuffer();
	byteBufferReliableOut = new CByteBuffer();
	byteBufferNotReliableOut = new CByteBuffer();
}

CNetServer::~CNetServer()
{
}

void CNetServer::StartServer()
{
	LOGCS("CNetServer::StartServer");
	this->status = NET_SERVER_STATUS_SETUP;
	SYS_StartThread(this);
}

void CNetServer::ThreadRun(void *data)
{
	LOGCS("CNetServer::ThreadRun: thread started");

	SYS_SetThreadName("Net >Server<");

	ENetAddress address;
	ENetHost * server;

	// Bind the server to the default localhost.
	// A specific host address can be specified by
	// enet_address_set_host (& address, "x.x.x.x");
	address.host = ENET_HOST_ANY;

	// Bind the server to port 5454
	address.port = serverPort;


	server = enet_host_create(&address,		// the address to bind the server host to
							   32,			// allow up to 32 clients and/or outgoing connections
							   2,			// allow up to 2 channels to be used, 0 and 1
							   0,			// assume any amount of incoming bandwidth
							   0			// assume any amount of outgoing bandwidth
			);

	if (server == NULL)
	{
		SYS_FatalExit("An error occurred while trying to create an ENet server host");
	}

	status = NET_SERVER_STATUS_ONLINE;

	LOGM("Server ready on port %d", serverPort);
	
	ENetEvent event;

	while(status != NET_SERVER_STATUS_SHUTDOWN)
	{
		//LOGD("... wait up to NET_SERVICE_EVENT_SLEEP_TIME milliseconds for an event ...");
		while (enet_host_service (server, &event, NET_SERVICE_EVENT_SLEEP_TIME) > 0)
		{
			switch (event.type)
			{
				case ENET_EVENT_TYPE_CONNECT:
				{
					char *strHostAddress = SYS_GetCharBuf();
					enet_address_get_host_ip(&(event.peer->host->address), strHostAddress, MAX_STRING_LENGTH);
					
					LOGCS("connection from %x %s:%u", event.peer->address.host, strHostAddress, event.peer->address.port);
					
					SYS_ReleaseCharBuf(strHostAddress);
					
					event.peer->data = this->ConnectPeer(event.peer);
					break;
				}

				case ENET_EVENT_TYPE_RECEIVE:
				{
					CNetClientData *netClientData = (CNetClientData *)(event.peer->data);
					if (netClientData == NULL)
					{
						LOGError("");
						break;
					}

					//LOGD("receive: len=%d %s", event.packet->dataLength, event.packet->data);
					byteBufferIn->SetData((u8*)event.packet->data, (u32)event.packet->dataLength);

					netClientData->totalNumReceived++;

#if defined(DEBUG_PRINT_PACKETS)
					if (event.channelID != 0)
					{
						LOGCS("FROM %d/%s (#%u) |%d|: %s",
							netClientData->peerId,
							netClientData->clientName,
							event.channelID,
							byteBufferIn->toHexString());
					}
					else
					{
						LOGCS("FROM %d/%s |%d|: %s",
							netClientData->peerId,
							netClientData->clientName,
							netClientData->totalNumReceived,
							byteBufferIn->toHexString());
					}
#endif

					// parse event.packet->data
					this->ParseDataBuffer(netClientData, byteBufferIn);

					// Clean up the packet now that we're done using it
					enet_packet_destroy (event.packet);
					byteBufferIn->data = NULL;

					if (byteBufferIn->error)
					{
#if defined(FINAL_RELEASE)
						LOGError("FROM %s: parse error, disconnect", netClientData->clientName.c_str());
						this->Disconnect(netClientData);
#else
						LOGError("FROM %s: parse error", netClientData->clientName);
#endif
					}
					continue;
				}
				case ENET_EVENT_TYPE_DISCONNECT:
				{
					CNetClientData *netClientData = (CNetClientData *)(event.peer->data);
					if (netClientData == NULL)
						break;

					LOGCS("FROM %s: disconnect", netClientData->clientName.c_str());
					this->Disconnected(netClientData);

					event.peer->data = NULL;
					break;
				}
				default:
				{
					CNetClientData *netClientData = (CNetClientData *)(event.peer->data);
					if (netClientData == NULL)
						break;

					LOGError("CNetServer enet_host_service: unknown event %d from %s", event.type, netClientData->clientName.c_str());
					this->Disconnect(netClientData);
					break;
				}
			}
		}

		// check players
		for (u32 i = 0; i < NET_MAX_CLIENTS; i++)
		{
			if (clients[i]->state == NET_CLIENT_STATE_DISCONNECT)
			{
				if (clients[i]->peer)
				{
					enet_peer_disconnect(clients[i]->peer, 0);
					this->Disconnected(clients[i]);
				}
				else
				{
					clients[i]->state = NET_CLIENT_STATE_EMPTY;
				}
			}
		}
		
		this->NetLogic();
	}

	this->status = NET_SERVER_STATUS_OFFLINE;

	enet_host_destroy(server);

	LOGCS("CNetServer::ThreadRun: thread finished");
}

CNetClientData *CNetServer::ConnectPeer(ENetPeer *peer)
{
	for (u32 i = 0; i < NET_MAX_CLIENTS; i++)
	{
		if (clients[i]->state == NET_CLIENT_STATE_EMPTY)
		{
			clients[i]->state = NET_CLIENT_STATE_CONNECTED;
			clients[i]->peer = peer;
			//	enet_peer_throttle_configure(

			//LOGD("peer connected id=%d", i);
			return clients[i];
		}
	}

	LOGError("CNetServer::ConnectPeer: too many peers (max=%d)", NET_MAX_CLIENTS);
	enet_peer_disconnect(peer, 0);

	return NULL;
}

void CNetServer::Disconnect(CNetClientData *netClientData)
{
	netClientData->state = NET_CLIENT_STATE_DISCONNECT;
}

void CNetServer::Disconnected(CNetClientData *netClientData)
{
	LOGCS("%s DISCONNECTED", netClientData->clientName.c_str());

	for (std::list<CNetServerCallback *>::iterator it = this->serverCallbacks.begin();
		 it != this->serverCallbacks.end(); it++)
	{
		CNetServerCallback *callback = (*it);
		callback->NetServerCallbackClientDisconnected(netClientData);
	}

	netClientData->peer->data = NULL;
	netClientData->peer = NULL;
	netClientData->state = NET_CLIENT_STATE_EMPTY;
}


void CNetServer::NetLogic()
{
//	LOGD("CNetServer::NetLogic");
	while(true)
	{
		this->LockMutex();
		if (this->receivedPackets.empty())
		{
//			LOGD("receivedPackets is empty");
			this->UnlockMutex();
			break;
		}

		std::list<CNetPacket *>::iterator it = this->receivedPackets.begin();
		CNetPacket *packet = (CNetPacket *)*it;
		this->receivedPackets.pop_front();
		this->UnlockMutex();

		for (std::list<CNetServerCallback *>::iterator it = this->serverCallbacks.begin();
			it != this->serverCallbacks.end(); it++)
		{
			CNetServerCallback *callback = (*it);
			callback->NetServerProcessPacket(packet);
		}

		delete packet;
	}

//	LOGD("send out packets relOut=%d notRelOut=%d", this->byteBufferReliableOut->length, this->byteBufferNotReliableOut->length));

	this->LockMutex();

	for (u16 i = 0; i < NET_MAX_CLIENTS; i++)
	{
		if (this->clients[i]->state == NET_CLIENT_STATE_ONLINE)
		{
			// send out packets
			if (this->clients[i]->byteBufferReliableOut->length != 0)
			{
				//this->byteBufferReliableOut->InsertBytes(this->byteBuffer)

				SendReliableBufferAsync(this->clients[i], this->clients[i]->byteBufferReliableOut);
				this->clients[i]->byteBufferReliableOut->Reset();
			}

			if (this->clients[i]->byteBufferNotReliableOut->length != 0)
			{
				//this->byteBufferNotReliableOut->InsertBytes(this->byteBuffer)
				
				SendNotReliableBufferAsync(this->clients[i], this->clients[i]->byteBufferNotReliableOut);
				this->clients[i]->byteBufferNotReliableOut->Reset();
			}

		}
	}
	this->UnlockMutex();

	for (std::list<CNetServerCallback *>::iterator it = this->serverCallbacks.begin();
		 it != this->serverCallbacks.end(); it++)
	{
		CNetServerCallback *callback = (*it);
		callback->NetServerLogic(this);
	}
	
	
}

void CNetServer::IssuePacket(CNetClientData *clientData, CNetPacket *packet)
{
	LOGCSTO(clientData, packet->protocolType, packet->packetType, "IssuePacket");
	this->LockMutex();

	if (packet->isReliable)
	{
		clientData->byteBufferReliableOut->PutU8(packet->protocolType);
		clientData->byteBufferReliableOut->PutU16(packet->packetType);
		packet->Serialize(clientData->byteBufferReliableOut);
	}
	else
	{
		clientData->byteBufferNotReliableOut->PutU8(packet->protocolType);
		clientData->byteBufferNotReliableOut->PutU16(packet->packetType);
		packet->Serialize(clientData->byteBufferNotReliableOut);
	}
	this->UnlockMutex();
}


void CNetServer::BroadcastPacket(CNetPacket *packet)
{
	LOGD("CNetServer::BroadcastPacket: packetType=%d", packet->packetType);
	this->LockMutex();
	for (u16 i = 0; i < NET_MAX_CLIENTS; i++)
	{
		if (this->clients[i]->state == NET_CLIENT_STATE_ONLINE)
		{
			this->IssuePacket(this->clients[i], packet);
		}
	}
	this->UnlockMutex();
}

void CNetServer::ParseDataBuffer(CNetClientData *netClientData, CByteBuffer *byteBuffer)
{
#if defined(DEBUG_PRINT_PACKETS)
	char *hexStr = byteBuffer->toHexString();
	LOGSF("ParseDataBuffer: %s index=%d", hexStr, byteBuffer->index);
	delete hexStr;
#endif
	
	if (netClientData->state == NET_CLIENT_STATE_CONNECTED)
	{
		LOGSF("netClientData->state==CONNECTED");
		u32 packetType = byteBuffer->GetByte();
		if (packetType == NET_PACKET_TYPE_AUTHORIZE)
		{
			this->ParseAuthorize(netClientData, byteBuffer);
		}
		else
		{
			char *hexStr = byteBuffer->toHexString();
			LOGError("CNetServer::ParseDataBuffer: unknown packet type=%d from %s data=%s index=%d",
				packetType, netClientData->clientName.c_str(), hexStr, byteBuffer->index);
			free(hexStr);

#if !defined(FINAL_RELEASE)
			//this->Disconnect(netPlayerData);
#endif

		}

		return;
	}
	else if (netClientData->state == NET_CLIENT_STATE_ONLINE)
	{
		LOGSF("netClientData->state==ONLINE");

		while(!byteBuffer->IsEof())
		{
			u8 protocolType = byteBuffer->GetU8();
			u16 packetType = byteBuffer->GetU16();
			LOGSF("packet %2.2x/%4.4x", protocolType, packetType);
			if (packetType == NET_PACKET_TYPE_NOTHING)
			{
				LOGError("CNetServer::ParseDataBuffer: NET_PACKET_TYPE_NOTHING received, stop parsing");
				byteBuffer->error = true;
				return;
			}
			else
			{
				bool parsed = false;

				for (std::list<CNetPacketCallback *>::iterator it = this->packetCallbacks.begin();
					it != this->packetCallbacks.end(); it++)
				{
					CNetPacketCallback *callback = (*it);

					CNetPacket *packet = callback->NetDeserializePacket(protocolType, packetType, byteBuffer);
					packet->protocolType = protocolType;
					packet->packetType = packetType;

					if (packet != NULL)
					{
						// parsed packets
						packet->clientData = netClientData;

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
					LOGError("CNetServer::ParseDataBuffer: unknown packet type=%d from %s data=%s index=%d",
						packetType, netClientData->clientName.c_str(), hexStr, byteBuffer->index);
					free(hexStr);

	#if !defined(FINAL_RELEASE)
					//this->Disconnect(netPlayerData);
	#endif


					return;
				}
			}
		}
	}
	else
	{
		LOGError("netClientData->state==%2.2x", netClientData->state);
	}


	LOGSF("ParseDataBuffer done");

}

bool CNetServer::ParseAuthorize(CNetClientData *netClientData, CByteBuffer *byteBuffer)
{
	LOGSF("CNetServer::ParseAuthorize");
	u32 v = byteBuffer->GetU32();
	if (!(v <= NET_PROTOCOL_VERSION))
	{
		LOGError("CNetServer::ParseAuthorize: unknown version %d", v);
		this->Disconnect(netClientData);
		return false;
	}

	u64 serverId = byteBuffer->GetU64();
	string userName = byteBuffer->GetStdString();
	u16 passwordHashLen = byteBuffer->getU16();
	u8 *passwordHashBytes = byteBuffer->getBytes(passwordHashLen);
	std::vector<uint8_t> passwordHashVector(passwordHashBytes, passwordHashBytes + passwordHashLen);
	
	LOGSF("ParseAuthorize: serverId=%llu userName='%s' passwordHashLen=%d", serverId, userName.c_str(), passwordHashLen);

	u8 authStatus = NET_SERVER_CALLBACK_AUTHORIZE_NOT_AVAILABLE;
	for (std::list<CNetServerCallback *>::iterator it = this->serverCallbacks.begin();
		it != this->serverCallbacks.end(); it++)
	{
		CNetServerCallback *callback = (*it);
		authStatus = callback->NetServerAuthorize(netClientData, userName, passwordHashVector);
		if (authStatus != NET_SERVER_CALLBACK_AUTHORIZE_NOT_AVAILABLE)
			break;
	}

	if (authStatus == NET_SERVER_CALLBACK_AUTHORIZE_WRONG_PASSWORD)
	{
		// send authorization confirmed
		byteBufferReliableOut->Reset();
		byteBufferReliableOut->PutByte(NET_PACKET_TYPE_AUTHORIZED);
		byteBufferReliableOut->PutBool(false);

		this->SendReliableBufferAsync(netClientData, byteBufferReliableOut);
		this->Disconnect(netClientData);

		LOGST("AUTHORIZATION FAILED");
		return false;
	}
	
	// authorized OK, check if we have already other user session and drop it
	for (int i = 0; i < NET_MAX_CLIENTS; i++)
	{
		CNetClientData *client = clients[i];
		if (client->clientName == userName)
		{
			client->clientName = "<PEER#" + std::to_string(client->peerId) + "> (" + client->clientName + ")";
			Disconnect(client);
		}
	}
	
	netClientData->SetClientName(userName);

	// send authorization confirmed
	byteBufferReliableOut->Reset();
	byteBufferReliableOut->PutByte(NET_PACKET_TYPE_AUTHORIZED);
	byteBufferReliableOut->PutBool(true);

	this->SendReliableBufferAsync(netClientData, byteBufferReliableOut);

	LOGST("AUTHORIZED successfully");
	
	netClientData->state = NET_CLIENT_STATE_ONLINE;

	for (std::list<CNetServerCallback *>::iterator it = this->serverCallbacks.begin();
		it != this->serverCallbacks.end(); it++)
	{
		CNetServerCallback *callback = (*it);
		callback->NetServerCallbackClientConnected(netClientData);
	}

	LOGSF("CNetServer::ParseAuthorize done");
	return true;
}

void CNetServer::AddServerCallback(CNetServerCallback *serverCallback)
{
	if (serverCallback != NULL)
	{
		LOGCS("CNetServer::AddServerCallback");
		this->serverCallbacks.push_back(serverCallback);
	}
}

void CNetServer::AddPacketCallback(CNetPacketCallback *packetCallback)
{
	if (packetCallback != NULL)
	{
		LOGCS("CNetServer::AddPacketCallback");
		this->packetCallbacks.push_back(packetCallback);
	}
}

const char *CNetServer::GetPacketNameFromType(u16 protocolType, u16 packetType)
{
	for (std::list<CNetPacketCallback *>::iterator it = this->packetCallbacks.begin();
		it != this->packetCallbacks.end(); it++)
	{
		CNetPacketCallback *callback = (*it);

		const char *packetName = callback->NetGetPacketNameFromType(protocolType, packetType);
		if (packetName != NULL)
			return packetName;
	}

	return NULL;
}


void CNetServer::SendReliableBufferAsync(CNetClientData *netClientData, CByteBuffer *byteBuffer)
{
	LOGST("SendReliableBufferAsync: len=%d", byteBuffer->length);
	ENetPacket *packet = enet_packet_create (byteBuffer->data, byteBuffer->length, ENET_PACKET_FLAG_RELIABLE);
	enet_peer_send (netClientData->peer, 0, packet);
	enet_host_flush(netClientData->peer->host);
}

void CNetServer::SendNotReliableBufferAsync(CNetClientData *netClientData, CByteBuffer *byteBuffer)
{
	LOGST("SendNotReliableBufferAsync: len=%d", byteBuffer->length);
	ENetPacket *packet = enet_packet_create (byteBuffer->data, byteBuffer->length, ENET_PACKET_FLAG_UNSEQUENCED);
	enet_peer_send (netClientData->peer, 0, packet);
	enet_host_flush(netClientData->peer->host);
}

const char *CNetServer::GetStatusName()
{
	switch(this->status)
	{
		case NET_SERVER_STATUS_OFFLINE:
			return ("OFFLINE");
		case NET_SERVER_STATUS_DISCONNECTED:
			return "DISCONNECTED";
		case NET_SERVER_STATUS_SETUP:
			return "SETUP";
		case NET_SERVER_STATUS_ONLINE:
			return "ONLINE";
		case NET_SERVER_STATUS_SHUTDOWN:
			return "SHUTDOWN";
		default:
			return "<UNKNOWN>";
	}
}


void CNetServer::LockMutex()
{
//	LOGD("CNetServer::LockMutex");
	packetMutex->Lock();
}

void CNetServer::UnlockMutex()
{
//	LOGD("CNetServer::UnlockMutex");
	packetMutex->Unlock();
}

void CNetServerCallback::NetServerCallbackClientConnected(CNetClientData *clientData)
{
	LOGError("CNetServerCalback::NetServerCallbackClientConnected");
}

void CNetServerCallback::NetServerCallbackClientDisconnected(CNetClientData *clientData)
{
	LOGError("CNetServerCalback::NetServerCallbackClientDisconnected");
}

void CNetServerCallback::NetServerProcessPacket(CNetPacket *packet)
{
	LOGError("CNetServerCalback::NetServerProcessPacket");
}

void CNetServerCallback::NetServerLogic(CNetServer *netServer)
{
}

u8 CNetServerCallback::NetServerAuthorize(CNetClientData *clientData, string userName, vector<u8> passwordHash)
{
	return NET_SERVER_CALLBACK_AUTHORIZE_NOT_AVAILABLE;
}

CNetServerCallback::~CNetServerCallback()
{
}

