/*
 *
 *  MTEngine framework (c) 2009 Marcin Skoczylas
 *  Licensed under MIT
 *
 */

// enet ipv6 https://github.com/Ericson2314/enet
#include "enet.h"

void NET_Test();

void NET_Initialize()
{
	enet_initialize();
	//NET_Test();
}

void NET_Test()
{
	/*
	LOGM("NET_Test");
	if (enet_initialize () != 0)
    {
        SYS_FatalExit("An error occurred while initializing ENet");
    }
	
	CNetServer *server = new CNetServer(NULL);
	server->Host();

	SYS_Sleep(1000);
	
	CNetClient *client = new CNetClient();
	client->Connect();

	while(server->isRunning)
	{
		SYS_Sleep(100);
	}
	
	enet_deinitialize();
	LOGM("NET_Test finished");*/
}


