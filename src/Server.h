#ifndef CC_SERVERCONNECTION_H
#define CC_SERVERCONNECTION_H
#include "Core.h"
#include "Vectors.h"
CC_BEGIN_HEADER

/* 
Represents a connection to either a multiplayer or an internal singleplayer server
Copyright 2014-2025 ClassiCube | Licensed under BSD-3
*/

struct IGameComponent;
struct ScheduledTask2;
extern struct IGameComponent Server_Component;

/* Prepares a ping entry for sending to the server, then returns its ID */
int Ping_NextPingId(void);
/* Updates received time for ping entry with matching ID */
void Ping_Update(int id);
/* Calculates average ping time based on most recent ping entries */
int Ping_AveragePingMS(void);



/* Data for currently active connection to a server */
CC_VAR extern struct _ServerConnectionData {
	/* Begins connecting to the server */
	/* NOTE: Usually asynchronous, but not always */
	void (*BeginConnect)(void);
	/* Ticks state of the server. */
	cc_bool (*Tick)(struct ScheduledTask2* task);
	/* Sends a block update to the server */
	void (*SendBlock)(int x, int y, int z, BlockID old, BlockID now);
	/* Sends a chat message to the server */
	void (*SendChat)(const cc_string* text);
	/* NOTE: Deprecated and removed - change LocalPlayer's position instead */
	/*  Was a function pointer to send a position update to the multiplayer server */
	void (*__Unused)(void);
	/* Sends raw data to the server. */
	/* NOTE: Prefer SendBlock/SendChat instead, this does NOT work in singleplayer */
	void (*SendData)(const cc_uint8* data, cc_uint32 len);

	/* The current name of the server (Shows as first line when loading) */
	cc_string Name;
	/* The current MOTD of the server (Shows as second line when loading) */
	cc_string MOTD;
	/* The software name the client identifies itself as being to the server */
	/* By default this is GAME_APP_NAME */
	cc_string AppName;

	/* NOTE: Drprecated, was a pointer to a temp buffer  */
	cc_uint8* ___unused;
	/* Whether the player is connected to singleplayer/internal server */
	cc_bool IsSinglePlayer;
	/* Whether the player has been disconnected from the server */
	cc_bool Disconnected;

	/* Whether the server supports separate tab list from entities in world */
	cc_bool SupportsExtPlayerList;
	/* Whether the server supports packet with detailed info on mouse clicks */
	cc_bool SupportsPlayerClick;
	/* Whether the server supports combining multiple chat packets into one */
	cc_bool SupportsPartialMessages;
	/* Whether the server supports all of code page 437, not just ASCII */
	cc_bool SupportsFullCP437;

	/* Address of the server if multiplayer, empty string if singleplayer */
	cc_string Address;
	/* Port of the server if multiplayer, 0 if singleplayer */
	int Port;

	/* Whether the server supports NotifyAction CPE */
	cc_bool SupportsNotifyAction;
} Server;

void Server_LeaveLAN(void);
/* If user hasn't previously accepted url, displays a dialog asking to confirm downloading it */
/* Otherwise just calls TexturePack_Extract */
void Server_RetrieveTexturePack(const cc_string* url);
/* Switches from the current connection mode and begins connecting to a normal ClassiCube multiplayer server. */
void Server_ConnectTo(const cc_string* address, int port);
/* Connects to a normal ClassiCube/external server. */
void Server_ConnectToClassic(const cc_string* address, int port);
/* Connects to a CavFX LAN host using CavLAN. */
void Server_ConnectToCavLAN(const cc_string* address, int port);
/* Opens the current singleplayer world to direct LAN clients. */
cc_bool Server_StartLAN(int port);
/* Closes the current LAN host, if one is running. */
void Server_StopLAN(void);
/* Whether the current singleplayer world is listening for LAN clients. */
cc_bool Server_IsLANHosted(void);

#define SERVER_LAN_DISCOVERY_MAX 8
/* Clears old LAN discovery results and starts listening for broadcasts. */
void Server_LANDiscoveryRefresh(void);
/* Polls for LAN broadcasts; call this from the LAN browser screen. */
void Server_LANDiscoveryTick(void);
int Server_LANDiscoveryCount(void);
const char* Server_LANDiscoveryName(int index);
const char* Server_LANDiscoveryAddress(int index);
int Server_LANDiscoveryPort(int index);
/* Broadcasts a locally spawned dropped item to CavLAN peers, if connected. */
void Server_SendDroppedItem(BlockID block, Vec3 pos, Vec3 vel);
/* Broadcasts a locally picked up dropped item to CavLAN peers, if connected. */
void Server_SendPickedItem(BlockID block, Vec3 pos);
/* Broadcasts a host-authoritative mob snapshot/removal to CavLAN peers. */
void Server_SendMobSnapshot(EntityID id, cc_uint8 kind, Vec3 pos, Vec3 vel, float yaw, float pitch, int health);
void Server_SendMobRemove(EntityID id);

/* Path of map to automatically load in singleplayer */
extern cc_string SP_AutoloadMap;

CC_END_HEADER
#endif
