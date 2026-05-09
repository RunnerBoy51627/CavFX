#include "Server.h"
/* CAVFX LAN discovery uses UDP broadcast on Windows. Non-Windows builds use safe stubs for now. */
#ifdef CC_BUILD_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#endif
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "String_.h"
#include "BlockPhysics.h"
#include "Game.h"
#include "Drawer2D.h"
#include "Chat.h"
#include "Block.h"
#include "Event.h"
#include "Http.h"
#include "Funcs.h"
#include "Entity.h"
#include "Graphics.h"
#include "Gui.h"
#include "Screens.h"
#include "Formats.h"
#include "Generator.h"
#include "World.h"
#include "Camera.h"
#include "TexturePack.h"
#include "Menus.h"
#include "Logger.h"
#include "Protocol.h"
#include "Inventory.h"
#include "Platform.h"
#include "Stream.h"
#include "Deflate.h"
#include "Input.h"
#include "Errors.h"
#include "Options.h"

/* Entity.c owns the mob array. When a LAN client finishes downloading the map,
 * ask Entity.c to resend all active mob snapshots so late joiners see them. */
extern void Mob_SendAllSnapshots(void);
#include "Utils.h"
#include "ExtMath.h"

static char nameBuffer[STRING_SIZE];
static char motdBuffer[STRING_SIZE];
static char appBuffer[STRING_SIZE];
static int ticks;
struct _ServerConnectionData Server;
struct CavLANSessionState CavLAN;

static void CavLAN_SetAddress(const cc_string* address, int port) {
	CavLAN.Address.length = 0;
	if (address) String_Copy(&CavLAN.Address, address);
	CavLAN.Port = port;
}

void CavLAN_Reset(void) {
	CavLAN.Active     = false;
	CavLAN.Hosting    = false;
	CavLAN.Connected  = false;
	CavLAN.IsLoopback = false;
	CavLAN.Role       = CAVLAN_ROLE_NONE;
	CavLAN.Port       = 0;
	CavLAN.Address.length = 0;
}

cc_bool CavLAN_IsActive(void) { return CavLAN.Active; }
cc_bool CavLAN_IsHost(void) { return CavLAN.Role == CAVLAN_ROLE_HOST && CavLAN.Hosting; }
cc_bool CavLAN_IsClient(void) { return CavLAN.Role == CAVLAN_ROLE_CLIENT && CavLAN.Connected; }

#define LAN_MAX_CLIENTS 4
#define LAN_BUFFER_SIZE 8192
#define LAN_MAX_FRAME_PAYLOAD 4096

enum CavLanPacket {
	CAVLAN_HELLO = 1, CAVLAN_MAP_BEGIN = 2, CAVLAN_MAP_CHUNK = 3, CAVLAN_MAP_END = 4,
	CAVLAN_BLOCK = 5, CAVLAN_CHAT = 6, CAVLAN_POSITION = 7, CAVLAN_PLAYER_INFO = 8,
	CAVLAN_GAME_STATE = 9, CAVLAN_DROPPED_ITEM = 10, CAVLAN_PICKED_ITEM = 11,
	CAVLAN_MOB_SNAPSHOT = 12, CAVLAN_MOB_REMOVE = 13
};

struct LanClient {
	cc_socket socket;
	cc_bool active, gotLogin;
	cc_uint8 read[LAN_BUFFER_SIZE];
	int readLen, id, protocol;
	cc_bool sendingMap;
	cc_uint32 mapOffset;
	cc_string name;
	char nameBuffer[STRING_SIZE];
	cc_string skin;
	char skinBuffer[STRING_SIZE];
	Vec3 pos;
	float yaw, pitch;
};

static cc_socket lan_listen = -1;
static cc_bool lan_hosting;
static struct LanClient lan_clients[LAN_MAX_CLIENTS];
static const cc_string cavlan_name = String_FromConst("CavFX LAN");
static BlockRaw* cavlan_blocks;
static int cavlan_width, cavlan_height, cavlan_length, cavlan_volume, cavlan_index;
static cc_bool cavlan_isClient;
static Vec3 cavlan_spawn;
static float cavlan_spawnYaw, cavlan_spawnPitch;
static cc_bool cavlan_hasSpawn;
static char cavlan_lastName[STRING_SIZE];
static char cavlan_lastSkin[STRING_SIZE];

#define CAVLAN_DISCOVERY_PORT 25566
#define CAVLAN_DISCOVERY_MAGIC "CAVFXLAN"

struct LanDiscoveryEntry {
	char name[64];
	char address[64];
	int port;
	cc_int64 lastSeen;
};
static struct LanDiscoveryEntry lan_discovered[SERVER_LAN_DISCOVERY_MAX];
static int lan_discovered_count;
static cc_int64 lan_last_announce;

#ifdef CC_BUILD_WIN
static SOCKET lan_discovery_socket = INVALID_SOCKET;
#else
static int lan_discovery_socket = -1;
#endif
static cc_bool lan_discovery_open;

static void Lan_Tick(void);
static void Server_LANDiscoveryAnnounce(void);
static void Lan_BroadcastBlock(int x, int y, int z, BlockID block);
static void Lan_BroadcastHostPosition(void);
static void Lan_BroadcastChat(const cc_string* text, struct LanClient* except);
extern void Mob_SetSyncedState(EntityID id, cc_uint8 kind, Vec3 pos, Vec3 vel, float yaw, float pitch, int health);
extern void Mob_RemoveSynced(EntityID id);
#ifndef CC_BUILD_WIN
static cc_result Socket_Accept(cc_socket s, cc_socket* client) { (void)s; (void)client; return ERR_NOT_SUPPORTED; }
#endif

/*########################################################################################################################*
*-----------------------------------------------------Common handlers-----------------------------------------------------*
*#########################################################################################################################*/
static void Server_ResetState(void) {
	Server.Disconnected            = false;
	Server.SupportsExtPlayerList   = false;
	Server.SupportsPlayerClick     = false;
	Server.SupportsPartialMessages = false;
	Server.SupportsFullCP437       = false;
	Server.SupportsNotifyAction    = false;
}

static void CavLan_FormatChat(cc_string* dst, const cc_string* name, const cc_string* text) {
	String_AppendConst(dst, "<");
	String_AppendString(dst, name);
	String_AppendConst(dst, "> ");
	String_AppendString(dst, text);
}

void Server_RetrieveTexturePack(const cc_string* url) {
	if (!Game_AllowServerTextures || TextureUrls_HasDenied(url)) return;

	if (!url->length || TextureUrls_HasAccepted(url)) {
		TexturePack_Extract(url);
	} else {
		TexPackOverlay_Show(url);
	}
}


/*########################################################################################################################*
*--------------------------------------------------------PingList---------------------------------------------------------*
*#########################################################################################################################*/
struct PingEntry { cc_int64 sent, recv; cc_uint16 id; };
static struct PingEntry ping_entries[10];
static int ping_head;

int Ping_NextPingId(void) {
	int head = ping_head;
	int next = ping_entries[head].id + 1;

	head = (head + 1) % Array_Elems(ping_entries);
	ping_entries[head].id   = next;
	ping_entries[head].sent = Stopwatch_Measure();
	ping_entries[head].recv = 0;
	
	ping_head = head;
	return next;
}

void Ping_Update(int id) {
	int i;
	for (i = 0; i < Array_Elems(ping_entries); i++) {
		if (ping_entries[i].id != id) continue;

		ping_entries[i].recv = Stopwatch_Measure();
		return;
	}
}

int Ping_AveragePingMS(void) {
	int i, measures = 0, totalMs;
	cc_int64 total = 0;

	for (i = 0; i < Array_Elems(ping_entries); i++) {
		struct PingEntry entry = ping_entries[i];
		if (!entry.sent || !entry.recv) continue;
	
		total += entry.recv - entry.sent;
		measures++;
	}
	if (!measures) return 0;

	totalMs = Stopwatch_ElapsedMS(0, total);
	/* (recv - send) is average time for packet to be sent to server and then sent back. */
	/* However for ping, only want average time to send data to server, so half the total. */
	totalMs /= 2;
	return totalMs / measures;
}

static void Ping_Reset(void) {
	Mem_Set(ping_entries, 0, sizeof(ping_entries));
	ping_head = 0;
}


/*########################################################################################################################*
*-------------------------------------------------Singleplayer connection-------------------------------------------------*
*#########################################################################################################################*/
static char autoloadBuffer[FILENAME_SIZE];
cc_string SP_AutoloadMap = String_FromArray(autoloadBuffer);

static void SPConnection_BeginConnect(void) {
	static const cc_string logName = String_FromConst("Singleplayer");
	const struct MapGenerator* gen;
	int seed, horSize, verSize;
	RNGState rnd;

	Chat_SetLogName(&logName);
	Game_UseCPEBlocks = Game_Version.HasCPE;
	if (Game_ShowMainMenu && !SP_AutoloadMap.length) {
		MainMenuScreen_Show();
		return;
	}

	/* For when user drops a map file onto ClassiCube.exe */
	if (SP_AutoloadMap.length) {
		Map_LoadFrom(&SP_AutoloadMap); return;
	}

#if defined CC_BUILD_NDS || defined CC_BUILD_PS1 || defined CC_BUILD_SATURN || defined CC_BUILD_MACCLASSIC || defined CC_BUILD_TINYMEM
	horSize = 16;
	verSize = 16;
#elif defined CC_BUILD_LOWMEM
	horSize = 64;
	verSize = 64;
#else
	horSize = Game_ClassicMode ? 256 : 128;
	verSize = 64;
#endif

#if defined CC_BUILD_N64 || defined CC_BUILD_NDS || defined CC_BUILD_PS1 || defined CC_BUILD_SATURN || defined CC_BUILD_TINYMEM
	gen = &FlatgrassGen;
#else
	gen = &NotchyGen;
#endif

	Random_SeedFromCurrentTime(&rnd);
	seed = Random_Next(&rnd, Int32_MaxValue);

	Gen_Start(gen, seed, horSize, verSize, horSize);
}

static char sp_lastCol;
static void SPConnection_AddPart(const cc_string* text) {
	cc_string tmp; char tmpBuffer[STRING_SIZE * 2];
	char col;
	int i;
	String_InitArray(tmp, tmpBuffer);

	/* Prepend color codes for subsequent lines of multi-line chat */
	if (!Drawer2D_IsWhiteColor(sp_lastCol)) {
		String_Append(&tmp, '&');
		String_Append(&tmp, sp_lastCol);
	}
	String_AppendString(&tmp, text);
	
	/* Replace all % with & */
	for (i = 0; i < tmp.length; i++) {
		if (tmp.buffer[i] == '%') tmp.buffer[i] = '&';
	}
	String_UNSAFE_TrimEnd(&tmp);

	col = Drawer2D_LastColor(&tmp, tmp.length);
	if (col) sp_lastCol = col;
	Chat_Add(&tmp);
}

static void SPConnection_AddChatLocal(const cc_string* text) {
	cc_string left, part;

	sp_lastCol = '\0';
	left = *text;

	while (left.length > STRING_SIZE) {
		part = String_UNSAFE_Substring(&left, 0, STRING_SIZE);
		SPConnection_AddPart(&part);
		left = String_UNSAFE_SubstringAt(&left, STRING_SIZE);
	}
	SPConnection_AddPart(&left);
}

static void SPConnection_SendChat(const cc_string* text) {
	cc_string formatted; char formattedBuffer[STRING_SIZE * 2];
	if (!text->length) return;

	if (lan_hosting) {
		String_InitArray(formatted, formattedBuffer);
		CavLan_FormatChat(&formatted, &Game_Username, text);
		SPConnection_AddChatLocal(&formatted);
		Lan_BroadcastChat(&formatted, NULL);
		return;
	}

	SPConnection_AddChatLocal(text);
}

static void SPConnection_SendBlock(int x, int y, int z, BlockID old, BlockID now) {
	Physics_OnBlockChanged(x, y, z, old, now);
	Lan_BroadcastBlock(x, y, z, now);
}

static void SPConnection_SendData(const cc_uint8* data, cc_uint32 len) { }

static cc_bool SPConnection_Tick(struct ScheduledTask2* task) {
	if (Server.Disconnected) return true;
	Lan_Tick();
	Server_LANDiscoveryTick();
	/* 60 -> 20 ticks a second */
	if ((ticks++ % 3) != 0)  return true;
	
	Physics_Tick();
	Lan_BroadcastHostPosition();
	TexturePack_CheckPending();
	return true;
}

static void SPConnection_Init(void) {
	Server_ResetState();
	if (!lan_hosting) CavLAN_Reset();
	Physics_Init();

	Server.BeginConnect = SPConnection_BeginConnect;
	Server.Tick         = SPConnection_Tick;
	Server.SendBlock    = SPConnection_SendBlock;
	Server.SendChat     = SPConnection_SendChat;
	Server.SendData     = SPConnection_SendData;
	
	Server.SupportsFullCP437       = !Game_ClassicMode;
	Server.SupportsPartialMessages = true;
	Server.IsSinglePlayer          = true;
}



/*########################################################################################################################*
*-----------------------------------------------------LAN discovery------------------------------------------------------*
*#########################################################################################################################*/
static int Server_LANDiscoveryStrLen(const char* s) {
	int len = 0;
	if (!s) return 0;
	while (s[len]) len++;
	return len;
}

static void Server_LANDiscoveryCopy(char* dst, int dstSize, const char* src) {
	int len = Server_LANDiscoveryStrLen(src);
	if (len >= dstSize) len = dstSize - 1;
	if (len > 0) Mem_Copy(dst, src, len);
	dst[len] = '\0';
}

static cc_bool Server_LANDiscoverySameAddress(const char* a, const char* b) {
	return !strcmp(a, b);
}

static void Server_LANDiscoveryAddRaw(const char* name, const char* address, int port) {
	int i;
	cc_int64 now = Stopwatch_Measure();
	if (!name || !name[0] || !address || !address[0] || !port) return;

	for (i = 0; i < lan_discovered_count; i++) {
		if (!Server_LANDiscoverySameAddress(lan_discovered[i].address, address)) continue;
		lan_discovered[i].port     = port;
		lan_discovered[i].lastSeen = now;
		Server_LANDiscoveryCopy(lan_discovered[i].name, sizeof(lan_discovered[i].name), name);
		return;
	}

	if (lan_discovered_count >= SERVER_LAN_DISCOVERY_MAX) return;
	Server_LANDiscoveryCopy(lan_discovered[lan_discovered_count].name,    sizeof(lan_discovered[lan_discovered_count].name),    name);
	Server_LANDiscoveryCopy(lan_discovered[lan_discovered_count].address, sizeof(lan_discovered[lan_discovered_count].address), address);
	lan_discovered[lan_discovered_count].port     = port;
	lan_discovered[lan_discovered_count].lastSeen = now;
	lan_discovered_count++;
}

static void Server_LANDiscoveryParse(const char* msg, const char* address) {
	char name[64];
	int port, len;
	const char *p, *sep;

	/* Format: CAVFXLAN|1|World name|25565 */
	if (!msg || strncmp(msg, CAVLAN_DISCOVERY_MAGIC "|", sizeof(CAVLAN_DISCOVERY_MAGIC)) != 0) return;
	p = strchr(msg, '|'); if (!p) return; p++;
	p = strchr(p,   '|'); if (!p) return; p++;

	sep = strchr(p, '|');
	if (!sep) return;
	len = (int)(sep - p);
	if (len <= 0) return;
	if (len >= (int)sizeof(name)) len = sizeof(name) - 1;
	Mem_Copy(name, p, len); name[len] = '\0';

	port = atoi(sep + 1);
	if (port <= 0 || port > 65535) port = 25565;
	Server_LANDiscoveryAddRaw(name, address, port);
}

#ifdef CC_BUILD_WIN
static void Server_LANDiscoveryOpen(void) {
	u_long nonblocking = 1;
	BOOL yes = TRUE;
	struct sockaddr_in addr;
	if (lan_discovery_open) return;

	lan_discovery_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (lan_discovery_socket == INVALID_SOCKET) return;

	setsockopt(lan_discovery_socket, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
	setsockopt(lan_discovery_socket, SOL_SOCKET, SO_BROADCAST, (const char*)&yes, sizeof(yes));
	ioctlsocket(lan_discovery_socket, FIONBIO, &nonblocking);

	Mem_Set(&addr, 0, sizeof(addr));
	addr.sin_family      = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port        = htons(CAVLAN_DISCOVERY_PORT);

	if (bind(lan_discovery_socket, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
		closesocket(lan_discovery_socket);
		lan_discovery_socket = INVALID_SOCKET;
		return;
	}
	lan_discovery_open = true;
}

static void Server_LANDiscoveryAnnounce(void) {
	char msg[160];
	struct sockaddr_in addr;
	int len;
	cc_int64 now;
	if (!lan_hosting) return;

	Server_LANDiscoveryOpen();
	if (!lan_discovery_open) return;

	now = Stopwatch_Measure();
	if (lan_last_announce && Stopwatch_ElapsedMS(lan_last_announce, now) < 1500) return;
	lan_last_announce = now;

	len = sprintf(msg, CAVLAN_DISCOVERY_MAGIC "|1|%s|25565", "CavFX LAN World");
	Mem_Set(&addr, 0, sizeof(addr));
	addr.sin_family      = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);
	addr.sin_port        = htons(CAVLAN_DISCOVERY_PORT);
	sendto(lan_discovery_socket, msg, len, 0, (struct sockaddr*)&addr, sizeof(addr));
}

static void Server_LANDiscoveryReceive(void) {
	char msg[256];
	char addrRaw[64];
	struct sockaddr_in from;
	int fromLen, len;
	const char* fromAddr;
	if (!lan_discovery_open) return;

	for (;;) {
		fromLen = sizeof(from);
		len = recvfrom(lan_discovery_socket, msg, sizeof(msg) - 1, 0, (struct sockaddr*)&from, &fromLen);
		if (len <= 0) return;
		msg[len] = '\0';
		fromAddr = inet_ntoa(from.sin_addr);
		Server_LANDiscoveryCopy(addrRaw, sizeof(addrRaw), fromAddr);
		Server_LANDiscoveryParse(msg, addrRaw);
	}
}
#else
static void Server_LANDiscoveryOpen(void) { }
static void Server_LANDiscoveryAnnounce(void) { }
static void Server_LANDiscoveryReceive(void) { }
#endif

void Server_LANDiscoveryRefresh(void) {
	lan_discovered_count = 0;
	Server_LANDiscoveryOpen();
	Server_LANDiscoveryTick();
}

void Server_LANDiscoveryTick(void) {
	Server_LANDiscoveryOpen();
	Server_LANDiscoveryAnnounce();
	Server_LANDiscoveryReceive();
}

int Server_LANDiscoveryCount(void) { return lan_discovered_count; }
const char* Server_LANDiscoveryName(int index) {
	return (index >= 0 && index < lan_discovered_count) ? lan_discovered[index].name : "";
}
const char* Server_LANDiscoveryAddress(int index) {
	return (index >= 0 && index < lan_discovered_count) ? lan_discovered[index].address : "";
}
int Server_LANDiscoveryPort(int index) {
	return (index >= 0 && index < lan_discovered_count) ? lan_discovered[index].port : 25565;
}

/*########################################################################################################################*
*--------------------------------------------------------LAN host---------------------------------------------------------*
*#########################################################################################################################*/
static cc_bool CavLan_SendRaw(cc_socket socket, const cc_uint8* data, cc_uint32 len) {
	cc_uint32 wrote;
	cc_result res;
	int tries = 0;
	while (len) {
		res = Socket_Write(socket, data, len, &wrote);
		if (res && tries < 100 && (res == ReturnCode_SocketInProgess || res == ReturnCode_SocketWouldBlock)) {
			Thread_Sleep(1); tries++; continue;
		}
		if (res || !wrote) return false;
		data += wrote; len -= wrote;
	}
	return true;
}

static cc_bool CavLan_SendFrame(cc_socket socket, cc_uint8 type, const cc_uint8* payload, cc_uint32 length) {
	cc_uint8 header[5];
	header[0] = type;
	Mem_WriteU32_BE(header + 1, length);
	if (!CavLan_SendRaw(socket, header, sizeof(header))) return false;
	return !length || CavLan_SendRaw(socket, payload, length);
}

static cc_bool CavLan_SendFrameClient(struct LanClient* c, cc_uint8 type, const cc_uint8* payload, cc_uint32 length) {
	return CavLan_SendFrame(c->socket, type, payload, length);
}

static void CavLan_WriteS32(cc_uint8* dst, int value) { Mem_WriteU32_BE(dst, (cc_uint32)value); }
static int  CavLan_ReadS32(const cc_uint8* src) { return (int)Mem_ReadU32_BE(src); }
static void CavLan_WriteS16(cc_uint8* dst, int value) { Mem_WriteU16_BE(dst, (cc_uint16)value); }
static int  CavLan_ReadS16(const cc_uint8* src) { return (cc_int16)Mem_ReadU16_BE(src); }

static void CavLan_GetLocalSkin(cc_string* skin, char skinBuffer[STRING_SIZE]) {
	struct Entity* e = &Entities.CurPlayer->Base;
	cc_string raw;
	skin->buffer = skinBuffer;
	skin->length = 0;
	skin->capacity = STRING_SIZE;

	if (e->SkinRaw[0]) {
		raw = String_FromRawArray(e->SkinRaw);
		String_Copy(skin, &raw);
	}
	if (!skin->length) String_Copy(skin, &Game_Username);
}

static int CavLan_WriteStringPair(cc_uint8* pkt, const cc_string* name, const cc_string* skin) {
	int nameLen = min(name->length, STRING_SIZE);
	int skinLen = min(skin->length, STRING_SIZE);
	pkt[0] = (cc_uint8)nameLen;
	pkt[1] = (cc_uint8)skinLen;
	Mem_Copy(pkt + 2, name->buffer, nameLen);
	Mem_Copy(pkt + 2 + nameLen, skin->buffer, skinLen);
	return 2 + nameLen + skinLen;
}

static cc_bool CavLan_ReadStringPair(cc_uint8* data, cc_uint32 length, cc_string* name, cc_string* skin) {
	int nameLen, skinLen;
	if (length < 2) return false;

	nameLen = data[0];
	skinLen = data[1];
	if (nameLen > STRING_SIZE || skinLen > STRING_SIZE) return false;
	if (2 + nameLen + skinLen > (int)length) return false;

	String_AppendAll(name, data + 2, nameLen);
	String_AppendAll(skin, data + 2 + nameLen, skinLen);
	String_UNSAFE_TrimEnd(name);
	String_UNSAFE_TrimEnd(skin);
	return true;
}

static void CavLan_ReadLegacyName(cc_uint8* data, cc_uint32 length, cc_string* name, cc_string* skin) {
	int nameLen = min((int)length, STRING_SIZE);
	String_AppendAll(name, data, nameLen);
	String_UNSAFE_TrimEnd(name);
	String_Copy(skin, name);
}

static void CavLan_FallbackPlayerInfo(EntityID id, cc_string* name, cc_string* skin) {
	if (!name->length) {
		String_AppendConst(name, "Player ");
		String_AppendInt(name, id);
	}
	if (!skin->length) String_Copy(skin, name);
}

static cc_bool CavLan_LocalPlayerInfoChanged(const cc_string* name, const cc_string* skin) {
	cc_string lastName = String_FromRawArray(cavlan_lastName);
	cc_string lastSkin = String_FromRawArray(cavlan_lastSkin);
	if (String_Equals(name, &lastName) && String_Equals(skin, &lastSkin)) return false;

	String_CopyToRawArray(cavlan_lastName, name);
	String_CopyToRawArray(cavlan_lastSkin, skin);
	return true;
}

static void CavLan_SendGameStateTo(struct LanClient* c) {
	struct HacksComp* hacks = &Entities.CurPlayer->Hacks;
	cc_bool cheats = !Game_PureClassic && hacks->Enabled;
	cc_uint8 pkt[12];
	pkt[0]  = Game_SurvivalMode;
	pkt[1]  = Game_ClassicMode;
	pkt[2]  = Game_ClassicHacks;
	pkt[3]  = cheats;
	pkt[4]  = cheats && hacks->CanAnyHacks;
	pkt[5]  = cheats && hacks->CanFly;
	pkt[6]  = cheats && hacks->CanNoclip;
	pkt[7]  = cheats && hacks->CanSpeed;
	pkt[8]  = cheats && hacks->CanRespawn;
	pkt[9]  = cheats && hacks->CanUseThirdPerson;
	pkt[10] = cheats && hacks->CanPushbackBlocks;
	pkt[11] = cheats && hacks->CanSeeAllNames;
	CavLan_SendFrameClient(c, CAVLAN_GAME_STATE, pkt, sizeof(pkt));
}

static void CavLan_SendPlayerInfoTo(struct LanClient* c, EntityID id, const cc_string* name, const cc_string* skin) {
	cc_uint8 pkt[1 + 2 + STRING_SIZE * 2];
	int length;
	pkt[0] = id;
	length = CavLan_WriteStringPair(pkt + 1, name, skin);
	CavLan_SendFrameClient(c, CAVLAN_PLAYER_INFO, pkt, 1 + length);
}

static void CavLan_BroadcastPlayerInfo(EntityID id, const cc_string* name, const cc_string* skin, struct LanClient* except) {
	int i;
	for (i = 0; i < LAN_MAX_CLIENTS; i++) {
		if (&lan_clients[i] == except) continue;
		if (lan_clients[i].active && lan_clients[i].gotLogin) CavLan_SendPlayerInfoTo(&lan_clients[i], id, name, skin);
	}
}

static void Lan_CloseClient(struct LanClient* c) {
	if (!c->active) return;

	if (c->gotLogin) {
		cc_string msg;
		char msgBuffer[STRING_SIZE];

		String_InitArray(msg, msgBuffer);

		String_AppendConst(&msg, "&e");
		String_AppendString(&msg, &c->name);
		String_AppendConst(&msg, " left the world");

		SPConnection_AddChatLocal(&msg);
		Lan_BroadcastChat(&msg, c);
	}

	Socket_Close(c->socket);
	if (c->gotLogin && c->id >= 0 && c->id < MAX_NET_PLAYERS) Entities_Remove(c->id);
	c->active   = false;
	c->gotLogin = false;
	c->sendingMap = false;
	c->mapOffset  = 0;
	c->readLen  = 0;
}

static void Lan_SendBlock(struct LanClient* c, int x, int y, int z, BlockID block) {
	cc_uint8 pkt[7];
	Mem_WriteU16_BE(pkt + 0, x);
	Mem_WriteU16_BE(pkt + 2, y);
	Mem_WriteU16_BE(pkt + 4, z);
	pkt[6] = (cc_uint8)block;
	CavLan_SendFrameClient(c, CAVLAN_BLOCK, pkt, sizeof(pkt));
}

static void CavLan_SendPositionTo(struct LanClient* c, EntityID id, Vec3 pos, float yaw, float pitch) {
	cc_uint8 pkt[9];
	pkt[0] = id;
	Mem_WriteU16_BE(pkt + 1, (int)(pos.x * 32));
	Mem_WriteU16_BE(pkt + 3, (int)(pos.y * 32) + 51);
	Mem_WriteU16_BE(pkt + 5, (int)(pos.z * 32));
	pkt[7] = Math_Deg2Packed(yaw);
	pkt[8] = Math_Deg2Packed(pitch);
	CavLan_SendFrameClient(c, CAVLAN_POSITION, pkt, sizeof(pkt));
}

static void CavLan_WriteDroppedItem(cc_uint8* pkt, BlockID block, Vec3 pos, Vec3 vel) {
	Mem_WriteU16_BE(pkt + 0, block);
	CavLan_WriteS32(pkt + 2,  (int)(pos.x * 4096.0f));
	CavLan_WriteS32(pkt + 6,  (int)(pos.y * 4096.0f));
	CavLan_WriteS32(pkt + 10, (int)(pos.z * 4096.0f));
	CavLan_WriteS16(pkt + 14, (int)(vel.x * 4096.0f));
	CavLan_WriteS16(pkt + 16, (int)(vel.y * 4096.0f));
	CavLan_WriteS16(pkt + 18, (int)(vel.z * 4096.0f));
}

static void CavLan_ReadDroppedItem(cc_uint8* pkt, BlockID* block, Vec3* pos, Vec3* vel) {
	*block = Mem_ReadU16_BE(pkt + 0);
	Vec3_Set(*pos, CavLan_ReadS32(pkt + 2) / 4096.0f, CavLan_ReadS32(pkt + 6) / 4096.0f, CavLan_ReadS32(pkt + 10) / 4096.0f);
	Vec3_Set(*vel, CavLan_ReadS16(pkt + 14) / 4096.0f, CavLan_ReadS16(pkt + 16) / 4096.0f, CavLan_ReadS16(pkt + 18) / 4096.0f);
}

static void CavLan_WritePickedItem(cc_uint8* pkt, BlockID block, Vec3 pos) {
	Mem_WriteU16_BE(pkt + 0, block);
	CavLan_WriteS32(pkt + 2,  (int)(pos.x * 4096.0f));
	CavLan_WriteS32(pkt + 6,  (int)(pos.y * 4096.0f));
	CavLan_WriteS32(pkt + 10, (int)(pos.z * 4096.0f));
}

static void CavLan_ReadPickedItem(cc_uint8* pkt, BlockID* block, Vec3* pos) {
	*block = Mem_ReadU16_BE(pkt + 0);
	Vec3_Set(*pos, CavLan_ReadS32(pkt + 2) / 4096.0f, CavLan_ReadS32(pkt + 6) / 4096.0f, CavLan_ReadS32(pkt + 10) / 4096.0f);
}

static void CavLan_WriteMobSnapshot(cc_uint8* pkt, EntityID id, cc_uint8 kind, Vec3 pos, Vec3 vel, float yaw, float pitch, int health) {
	/* Mob entity IDs can be above 255, so keep them 16-bit over LAN. */
	Mem_WriteU16_BE(pkt + 0, (cc_uint16)id);
	pkt[2] = kind;
	CavLan_WriteS32(pkt + 3,  (int)(pos.x * 4096.0f));
	CavLan_WriteS32(pkt + 7,  (int)(pos.y * 4096.0f));
	CavLan_WriteS32(pkt + 11, (int)(pos.z * 4096.0f));
	CavLan_WriteS16(pkt + 15, (int)(vel.x * 4096.0f));
	CavLan_WriteS16(pkt + 17, (int)(vel.y * 4096.0f));
	CavLan_WriteS16(pkt + 19, (int)(vel.z * 4096.0f));
	pkt[21] = Math_Deg2Packed(yaw);
	pkt[22] = Math_Deg2Packed(pitch);
	CavLan_WriteS16(pkt + 23, health);
}

static void CavLan_ReadMobSnapshot(cc_uint8* pkt, EntityID* id, cc_uint8* kind, Vec3* pos, Vec3* vel, float* yaw, float* pitch, int* health) {
	*id   = (EntityID)Mem_ReadU16_BE(pkt + 0);
	*kind = pkt[2];
	Vec3_Set(*pos, CavLan_ReadS32(pkt + 3) / 4096.0f, CavLan_ReadS32(pkt + 7) / 4096.0f, CavLan_ReadS32(pkt + 11) / 4096.0f);
	Vec3_Set(*vel, CavLan_ReadS16(pkt + 15) / 4096.0f, CavLan_ReadS16(pkt + 17) / 4096.0f, CavLan_ReadS16(pkt + 19) / 4096.0f);
	*yaw    = pkt[21] * (360.0f / 256.0f);
	*pitch  = pkt[22] * (360.0f / 256.0f);
	*health = CavLan_ReadS16(pkt + 23);
}

static void CavLan_SendMobSnapshotTo(struct LanClient* c, EntityID id, cc_uint8 kind, Vec3 pos, Vec3 vel, float yaw, float pitch, int health) {
	cc_uint8 pkt[25];
	CavLan_WriteMobSnapshot(pkt, id, kind, pos, vel, yaw, pitch, health);
	CavLan_SendFrameClient(c, CAVLAN_MOB_SNAPSHOT, pkt, sizeof(pkt));
}

static void Lan_BroadcastMobSnapshot(EntityID id, cc_uint8 kind, Vec3 pos, Vec3 vel, float yaw, float pitch, int health, struct LanClient* except) {
	int i;
	if (!lan_hosting) return;
	for (i = 0; i < LAN_MAX_CLIENTS; i++) {
		if (&lan_clients[i] == except) continue;
		if (lan_clients[i].active && lan_clients[i].gotLogin) CavLan_SendMobSnapshotTo(&lan_clients[i], id, kind, pos, vel, yaw, pitch, health);
	}
}

static void CavLan_SendMobRemoveTo(struct LanClient* c, EntityID id) {
	cc_uint8 pkt[2];
	Mem_WriteU16_BE(pkt + 0, (cc_uint16)id);
	CavLan_SendFrameClient(c, CAVLAN_MOB_REMOVE, pkt, sizeof(pkt));
}

static void Lan_BroadcastMobRemove(EntityID id, struct LanClient* except) {
	int i;
	if (!lan_hosting) return;
	for (i = 0; i < LAN_MAX_CLIENTS; i++) {
		if (&lan_clients[i] == except) continue;
		if (lan_clients[i].active && lan_clients[i].gotLogin) CavLan_SendMobRemoveTo(&lan_clients[i], id);
	}
}

void Server_SendMobSnapshot(EntityID id, cc_uint8 kind, Vec3 pos, Vec3 vel, float yaw, float pitch, int health) {
	Lan_BroadcastMobSnapshot(id, kind, pos, vel, yaw, pitch, health, NULL);
}

void Server_SendMobRemove(EntityID id) {
	Lan_BroadcastMobRemove(id, NULL);
}

static void CavLan_SendDroppedItemTo(struct LanClient* c, BlockID block, Vec3 pos, Vec3 vel) {
	cc_uint8 pkt[20];
	CavLan_WriteDroppedItem(pkt, block, pos, vel);
	CavLan_SendFrameClient(c, CAVLAN_DROPPED_ITEM, pkt, sizeof(pkt));
}

static void CavLan_SendPickedItemTo(struct LanClient* c, BlockID block, Vec3 pos) {
	cc_uint8 pkt[14];
	CavLan_WritePickedItem(pkt, block, pos);
	CavLan_SendFrameClient(c, CAVLAN_PICKED_ITEM, pkt, sizeof(pkt));
}

static void Lan_BroadcastDroppedItem(BlockID block, Vec3 pos, Vec3 vel, struct LanClient* except) {
	int i;
	if (!lan_hosting) return;
	for (i = 0; i < LAN_MAX_CLIENTS; i++) {
		if (&lan_clients[i] == except) continue;
		if (lan_clients[i].active && lan_clients[i].gotLogin) CavLan_SendDroppedItemTo(&lan_clients[i], block, pos, vel);
	}
}

static void Lan_BroadcastPickedItem(BlockID block, Vec3 pos, struct LanClient* except) {
	int i;
	if (!lan_hosting) return;
	for (i = 0; i < LAN_MAX_CLIENTS; i++) {
		if (&lan_clients[i] == except) continue;
		if (lan_clients[i].active && lan_clients[i].gotLogin) CavLan_SendPickedItemTo(&lan_clients[i], block, pos);
	}
}

static void CavLan_SetNetPlayerInfo(EntityID id, const cc_string* name, const cc_string* skin) {
	struct Entity* e;
	static const cc_string group = String_FromConst("Players");
	if (id >= MAX_NET_PLAYERS) return;

	e = Entities.List[id];
	if (!e) return;
	Entity_SetSkin(e, skin);
	Entity_SetName(e, name);
	TabList_Set(id, name, name, &group, 0);
	TabList_EntityLinked_Set(id);
}

static void CavLan_EnsureNetPlayer(EntityID id, const cc_string* name, const cc_string* skin, Vec3 pos, float yaw, float pitch) {
	struct Entity* e;
	struct LocationUpdate update;
	static const cc_string group = String_FromConst("Players");
	if (id >= MAX_NET_PLAYERS) return;
	if (!Entities.List[id]) {
		e = &NetPlayers_List[id].Base;
		NetPlayer_Init((struct NetPlayer*)e);
		Entities.List[id] = e;
		Entity_SetSkin(e, skin);
		Entity_SetName(e, name);
		Event_RaiseInt(&EntityEvents.Added, id);
		TabList_Set(id, name, name, &group, 0);
		TabList_EntityLinked_Set(id);
	} else {
		CavLan_SetNetPlayerInfo(id, name, skin);
	}

	update.flags = LU_HAS_POS | LU_HAS_YAW | LU_HAS_PITCH | LU_POS_ABSOLUTE_INSTANT;
	update.pos   = pos;
	update.yaw   = yaw;
	update.pitch = pitch;
	Entities.List[id]->VTABLE->SetLocation(Entities.List[id], &update);
}

static void CavLan_UpdateNetPlayer(EntityID id, Vec3 pos, float yaw, float pitch) {
	struct LocationUpdate update;
	if (id >= MAX_NET_PLAYERS) return;
	if (!Entities.List[id]) return;
	update.flags = LU_HAS_POS | LU_HAS_YAW | LU_HAS_PITCH | LU_POS_ABSOLUTE_SMOOTH | LU_ORI_INTERPOLATE;
	update.pos   = pos;
	update.yaw   = yaw;
	update.pitch = pitch;
	Entities.List[id]->VTABLE->SetLocation(Entities.List[id], &update);
}

static void Lan_BroadcastBlock(int x, int y, int z, BlockID block) {
	int i;
	if (!lan_hosting) return;
	for (i = 0; i < LAN_MAX_CLIENTS; i++) {
		if (lan_clients[i].active && lan_clients[i].gotLogin) Lan_SendBlock(&lan_clients[i], x, y, z, block);
	}
}

static void Lan_SendChatPacket(struct LanClient* c, const cc_string* text) {
	CavLan_SendFrameClient(c, CAVLAN_CHAT, (cc_uint8*)text->buffer, text->length);
}

static void Lan_BroadcastChat(const cc_string* text, struct LanClient* except) {
	int i;
	for (i = 0; i < LAN_MAX_CLIENTS; i++) {
		if (&lan_clients[i] == except) continue;
		if (lan_clients[i].active && lan_clients[i].gotLogin) Lan_SendChatPacket(&lan_clients[i], text);
	}
}

static cc_bool Lan_SendMapBegin(struct LanClient* c) {
	struct LocalPlayer* p = Entities.CurPlayer;
	cc_uint8 pkt[18];

	Mem_WriteU16_BE(pkt + 0, World.Width);
	Mem_WriteU16_BE(pkt + 2, World.Height);
	Mem_WriteU16_BE(pkt + 4, World.Length);
	Mem_WriteU32_BE(pkt + 6, World.Volume);
	Mem_WriteU16_BE(pkt + 10, (int)(p->Spawn.x * 32));
	Mem_WriteU16_BE(pkt + 12, (int)(p->Spawn.y * 32));
	Mem_WriteU16_BE(pkt + 14, (int)(p->Spawn.z * 32));
	pkt[16] = Math_Deg2Packed(p->SpawnYaw);
	pkt[17] = Math_Deg2Packed(p->SpawnPitch);
	c->mapOffset  = 0;
	c->sendingMap = true;
	return CavLan_SendFrameClient(c, CAVLAN_MAP_BEGIN, pkt, sizeof(pkt));
}

static cc_bool Lan_SendMapTick(struct LanClient* c) {
	cc_uint32 count;
	cc_bool writable;
	cc_result res;
	int chunks = 0;
	if (!c->sendingMap) return true;

	while (c->mapOffset < (cc_uint32)World.Volume && chunks < 8) {
		res = Socket_Poll(c->socket, 0, SOCKET_POLL_WRITE, &writable);
		if (res) return false;
		if (!writable) return true;

		count = min(1024, (cc_uint32)World.Volume - c->mapOffset);
		if (!CavLan_SendFrameClient(c, CAVLAN_MAP_CHUNK, World.Blocks + c->mapOffset, count)) return false;
		c->mapOffset += count;
		chunks++;
	}
	if (c->mapOffset < (cc_uint32)World.Volume) return true;

	c->sendingMap = false;
	return CavLan_SendFrameClient(c, CAVLAN_MAP_END, NULL, 0);
}

static int CavLan_FrameSize(const cc_uint8* data, int readLen) {
	cc_uint32 length;
	if (readLen < 5) return 0;
	length = Mem_ReadU32_BE(data + 1);
	if (length > LAN_MAX_FRAME_PAYLOAD) return -1;
	return 5 + length;
}

static void Lan_HandleLogin(struct LanClient* c, cc_uint8* data) {
	struct Entity* host = &Entities.CurPlayer->Base;
	cc_uint32 length = Mem_ReadU32_BE(data + 1);

	c->name.length = 0;
	c->skin.length = 0;
	if (!CavLan_ReadStringPair(data + 5, length, &c->name, &c->skin)) {
		CavLan_ReadLegacyName(data + 5, length, &c->name, &c->skin);
	}
	CavLan_FallbackPlayerInfo((EntityID)c->id, &c->name, &c->skin);

	c->pos   = host->Position;
	c->yaw   = host->Yaw;
	c->pitch = host->Pitch;
	c->gotLogin = true;
	{
		cc_string msg;
		char msgBuffer[STRING_SIZE];

		String_InitArray(msg, msgBuffer);

		String_AppendConst(&msg, "&e");
		String_AppendString(&msg, &c->name);
		String_AppendConst(&msg, " joined the world");

		SPConnection_AddChatLocal(&msg);
		Lan_BroadcastChat(&msg, NULL);
	}
	CavLan_EnsureNetPlayer((EntityID)c->id, &c->name, &c->skin, c->pos, c->yaw, c->pitch);

	CavLan_SendGameStateTo(c);
	if (!Lan_SendMapBegin(c)) {
		Lan_CloseClient(c);
		return;
	}
}

static void Lan_HandleSetBlock(struct LanClient* c, cc_uint8* data) {
	cc_uint32 length = Mem_ReadU32_BE(data + 1);
	int x, y, z;
	BlockID old, block;
	(void)c;
	if (length < 7) return;
	x = Mem_ReadU16_BE(data + 5);
	y = Mem_ReadU16_BE(data + 7);
	z = Mem_ReadU16_BE(data + 9);
	block = data[11];
	if (!World_Contains(x, y, z)) return;
	old = World_GetBlock(x, y, z);
	Game_UpdateBlock(x, y, z, block);
	Physics_OnBlockChanged(x, y, z, old, block);
	Lan_BroadcastBlock(x, y, z, block);
}

static void Lan_HandlePosition(struct LanClient* c, cc_uint8* data) {
	cc_uint32 length = Mem_ReadU32_BE(data + 1);
	int i, x, y, z;
	if (length < 8) return;
	x = Mem_ReadU16_BE(data + 5);
	y = Mem_ReadU16_BE(data + 7);
	z = Mem_ReadU16_BE(data + 9);
	Vec3_Set(c->pos, x / 32.0f, (y - 51) / 32.0f, z / 32.0f);
	c->yaw   = data[11] * (360.0f / 256.0f);
	c->pitch = data[12] * (360.0f / 256.0f);
	CavLan_UpdateNetPlayer((EntityID)c->id, c->pos, c->yaw, c->pitch);

	for (i = 0; i < LAN_MAX_CLIENTS; i++) {
		if (&lan_clients[i] == c) continue;
		if (lan_clients[i].active && lan_clients[i].gotLogin) {
			CavLan_SendPositionTo(&lan_clients[i], (EntityID)c->id, c->pos, c->yaw, c->pitch);
		}
	}
}

static void Lan_HandleChat(struct LanClient* c, cc_uint8* data) {
	cc_uint32 length = Mem_ReadU32_BE(data + 1);
	cc_string msg = String_Init((char*)data + 5, min(length, STRING_SIZE), STRING_SIZE);
	cc_string formatted; char formattedBuffer[STRING_SIZE * 2];
	String_UNSAFE_TrimEnd(&msg);

	String_InitArray(formatted, formattedBuffer);
	CavLan_FormatChat(&formatted, &c->name, &msg);
	SPConnection_AddChatLocal(&formatted);
	Lan_BroadcastChat(&formatted, NULL);
}

static void Lan_HandlePlayerInfo(struct LanClient* c, cc_uint8* data) {
	cc_uint32 length = Mem_ReadU32_BE(data + 1);
	cc_string name, skin;
	char nameBuffer[STRING_SIZE], skinBuffer[STRING_SIZE];
	if (length < 1) return;

	String_InitArray(name, nameBuffer);
	String_InitArray(skin, skinBuffer);
	if (!CavLan_ReadStringPair(data + 6, length - 1, &name, &skin)) {
		CavLan_ReadLegacyName(data + 6, length - 1, &name, &skin);
	}
	CavLan_FallbackPlayerInfo((EntityID)c->id, &name, &skin);

	c->name.length = 0;
	c->skin.length = 0;
	String_Copy(&c->name, &name);
	String_Copy(&c->skin, &skin);
	CavLan_SetNetPlayerInfo((EntityID)c->id, &c->name, &c->skin);
	CavLan_BroadcastPlayerInfo((EntityID)c->id, &c->name, &c->skin, c);

	/* Late join fix: resend the host current mobs after the new client has the map.
	 * Without this, host can see skeletons/zombies while the joined client sees none. */
	Mob_SendAllSnapshots();
}

static void Lan_HandleDroppedItem(struct LanClient* c, cc_uint8* data) {
	cc_uint32 length = Mem_ReadU32_BE(data + 1);
	BlockID block;
	Vec3 pos, vel;
	if (length < 20) return;
	CavLan_ReadDroppedItem(data + 5, &block, &pos, &vel);
	DroppedItem_SpawnAtVelocity(block, pos, vel);
	Lan_BroadcastDroppedItem(block, pos, vel, c);
}

static void Lan_HandlePickedItem(struct LanClient* c, cc_uint8* data) {
	cc_uint32 length = Mem_ReadU32_BE(data + 1);
	BlockID block;
	Vec3 pos;
	if (length < 14) return;
	CavLan_ReadPickedItem(data + 5, &block, &pos);
	DroppedItem_RemoveNearest(block, pos);
	Lan_BroadcastPickedItem(block, pos, c);
}

static void Lan_HandlePacket(struct LanClient* c, cc_uint8* data) {
	if (data[0] == CAVLAN_HELLO)            { Lan_HandleLogin(c, data); return; }
	if (!c->gotLogin) return;
	if (data[0] == CAVLAN_BLOCK)            { Lan_HandleSetBlock(c, data); return; }
	if (data[0] == CAVLAN_POSITION)         { Lan_HandlePosition(c, data); return; }
	if (data[0] == CAVLAN_CHAT)             { Lan_HandleChat(c, data); return; }
	if (data[0] == CAVLAN_PLAYER_INFO)      { Lan_HandlePlayerInfo(c, data); return; }
	if (data[0] == CAVLAN_DROPPED_ITEM)     { Lan_HandleDroppedItem(c, data); return; }
	if (data[0] == CAVLAN_PICKED_ITEM)      { Lan_HandlePickedItem(c, data); return; }
}

static void Lan_SendPostMapInfo(struct LanClient* c) {
	struct Entity* host = &Entities.CurPlayer->Base;
	cc_string hostSkin; char hostSkinBuffer[STRING_SIZE];
	int i;

	CavLan_GetLocalSkin(&hostSkin, hostSkinBuffer);
	CavLan_SendPlayerInfoTo(c, 0, &Game_Username, &hostSkin);
	CavLan_SendPositionTo(c, 0, host->Position, host->Yaw, host->Pitch);
	for (i = 0; i < LAN_MAX_CLIENTS; i++) {
		if (&lan_clients[i] == c) continue;
		if (!lan_clients[i].active || !lan_clients[i].gotLogin) continue;
		CavLan_SendPlayerInfoTo(c, (EntityID)lan_clients[i].id, &lan_clients[i].name, &lan_clients[i].skin);
		CavLan_SendPositionTo(c, (EntityID)lan_clients[i].id, lan_clients[i].pos, lan_clients[i].yaw, lan_clients[i].pitch);
	}
	CavLan_BroadcastPlayerInfo((EntityID)c->id, &c->name, &c->skin, c);
}

static void Lan_TickClient(struct LanClient* c) {
	cc_uint32 read;
	cc_result res;
	int size, remaining;

	if (!c->active) return;
	if (c->sendingMap) {
		if (!Lan_SendMapTick(c)) { Lan_CloseClient(c); return; }
		if (!c->sendingMap) Lan_SendPostMapInfo(c);
	}

	res = Socket_Read(c->socket, c->read + c->readLen, sizeof(c->read) - c->readLen, &read);
	if (res == ReturnCode_SocketInProgess || res == ReturnCode_SocketWouldBlock) return;
	if (res || !read) { Lan_CloseClient(c); return; }
	c->readLen += read;

	while (c->readLen) {
		size = CavLan_FrameSize(c->read, c->readLen);
		if (size < 0) { Lan_CloseClient(c); return; }
		if (!size) return;
		if (c->readLen < size) return;
		Lan_HandlePacket(c, c->read);
		if (!c->active) return;
		remaining = c->readLen - size;
		if (remaining) Mem_Move(c->read, c->read + size, remaining);
		c->readLen = remaining;
	}
}

static void Lan_AcceptPending(void) {
	cc_socket client;
	cc_result res;
	int i;
	cc_bool readable;

	if (!lan_hosting) return;
	while (true) {
		res = Socket_Poll(lan_listen, 0, SOCKET_POLL_READ, &readable);
		if (res || !readable) return;
		res = Socket_Accept(lan_listen, &client);
		if (res == ReturnCode_SocketInProgess || res == ReturnCode_SocketWouldBlock) return;
		if (res) return;

		for (i = 0; i < LAN_MAX_CLIENTS; i++) {
			if (!lan_clients[i].active) break;
		}
		if (i == LAN_MAX_CLIENTS) { Socket_Close(client); continue; }

		Socket_SetNonBlocking(client, true);
		lan_clients[i].socket   = client;
		lan_clients[i].active   = true;
		lan_clients[i].gotLogin = false;
		lan_clients[i].sendingMap = false;
		lan_clients[i].mapOffset  = 0;
		lan_clients[i].readLen  = 0;
		lan_clients[i].id       = i + 1;
		lan_clients[i].protocol = PROTOCOL_0030;
		String_InitArray(lan_clients[i].name, lan_clients[i].nameBuffer);
		String_InitArray(lan_clients[i].skin, lan_clients[i].skinBuffer);
	}
}

static void Lan_BroadcastHostPosition(void) {
	struct Entity* host = &Entities.CurPlayer->Base;
	int i;
	if (!lan_hosting) return;

	for (i = 0; i < LAN_MAX_CLIENTS; i++) {
		if (lan_clients[i].active && lan_clients[i].gotLogin) {
			CavLan_SendPositionTo(&lan_clients[i], 0, host->Position, host->Yaw, host->Pitch);
		}
	}
}

static void Lan_Tick(void) {
	cc_string skin; char skinBuffer[STRING_SIZE];
	int i;
	Lan_AcceptPending();
	for (i = 0; i < LAN_MAX_CLIENTS; i++) Lan_TickClient(&lan_clients[i]);

	CavLan_GetLocalSkin(&skin, skinBuffer);
	if (CavLan_LocalPlayerInfoChanged(&Game_Username, &skin)) {
		CavLan_BroadcastPlayerInfo(0, &Game_Username, &skin, NULL);
	}
}

cc_bool Server_StartLAN(int port) {
#ifdef CC_BUILD_WIN
	cc_result res;
	if (lan_hosting) return true;
	if (!World.Loaded) return false;

	res = Socket_CreateListener(&lan_listen, port);
	if (res) {
		Logger_SysWarn(res, "opening LAN host");
		return false;
	}
	Socket_SetNonBlocking(lan_listen, true);
	lan_hosting = true;
	CavLAN.Active     = true;
	CavLAN.Hosting    = true;
	CavLAN.Connected  = true;
	CavLAN.IsLoopback = false;
	CavLAN.Role       = CAVLAN_ROLE_HOST;
	CavLAN_SetAddress(NULL, port);
	cavlan_lastName[0] = '\0';
	cavlan_lastSkin[0] = '\0';
	return true;
#else
	(void)port;
	return false;
#endif
}

void Server_StopLAN(void) {
	int i;
	for (i = 0; i < LAN_MAX_CLIENTS; i++) Lan_CloseClient(&lan_clients[i]);
	if (lan_hosting) Socket_Close(lan_listen);
	lan_hosting = false;
	lan_listen  = -1;
	if (CavLAN.Role == CAVLAN_ROLE_HOST) CavLAN_Reset();
}

cc_bool Server_IsLANHosted(void) { return CavLAN_IsHost(); }

static cc_bool Server_IsLoopbackAddress(const cc_string* address) {
	return String_CaselessEqualsConst(address, "localhost")
		|| String_CaselessEqualsConst(address, "127.0.0.1")
		|| String_CaselessEqualsConst(address, "::1");
}


/*########################################################################################################################*
*--------------------------------------------------Multiplayer connection-------------------------------------------------*
*#########################################################################################################################*/
static cc_socket net_socket = -1;
static cc_result net_writeFailure;
static void OnClose(void);

#ifdef CC_BUILD_NETWORKING
static cc_uint8  net_readBuffer[4096 * 5];
static cc_uint8* net_readCurrent;
static cc_uint8 lastOpcode;
static float timeSinceLast;

static cc_bool net_connecting;
#define NET_TIMEOUT_SECS 15

void Server_SendDroppedItem(BlockID block, Vec3 pos, Vec3 vel) {
	cc_uint8 pkt[20];
	if (lan_hosting) {
		Lan_BroadcastDroppedItem(block, pos, vel, NULL);
	} else if (cavlan_isClient && !Server.Disconnected && !net_connecting) {
		CavLan_WriteDroppedItem(pkt, block, pos, vel);
		CavLan_SendFrame(net_socket, CAVLAN_DROPPED_ITEM, pkt, sizeof(pkt));
	}
}

void Server_SendPickedItem(BlockID block, Vec3 pos) {
	cc_uint8 pkt[14];
	if (lan_hosting) {
		Lan_BroadcastPickedItem(block, pos, NULL);
	} else if (cavlan_isClient && !Server.Disconnected && !net_connecting) {
		CavLan_WritePickedItem(pkt, block, pos);
		CavLan_SendFrame(net_socket, CAVLAN_PICKED_ITEM, pkt, sizeof(pkt));
	}
}

static void MPConnection_FinishConnect(void) {
	net_connecting = false;
	timeSinceLast  = 0.0f;

	Event_RaiseVoid(&NetEvents.Connected);
	Event_RaiseFloat(&WorldEvents.Loading, 0.0f);

	net_readCurrent = net_readBuffer;
}

static void MPConnection_Fail(const cc_string* reason) {
	cc_string msg; char msgBuffer[STRING_SIZE * 2];
	String_InitArray(msg, msgBuffer);
	net_connecting = false;

	String_Format2(&msg, "Failed to connect to %s:%i", &Server.Address, &Server.Port);
	Game_Disconnect(&msg, reason);
	OnClose();
}

static void MPConnection_FailConnect(cc_result result) {
	static const cc_string reason = String_FromConst("You failed to connect to the server. It's probably down!");
	cc_string msg; char msgBuffer[STRING_SIZE * 2];
	String_InitArray(msg, msgBuffer);

	if (result) {
		String_Format3(&msg, "Error connecting to %s:%i: %e" _NL, &Server.Address, &Server.Port, &result);
		Logger_Log(&msg);
	}
	MPConnection_Fail(&reason);
}

static void MPConnection_TickConnect(void) {
	cc_bool writable;
	cc_result res = Socket_Poll(net_socket, 0, SOCKET_POLL_WRITE, &writable);

	if (res) {
		MPConnection_FailConnect(res);
	} else if (writable) {
		struct LoginPacket pkt;
		cc_uint32 wrote = 0;
		cc_uint8* buf = (cc_uint8*)&pkt;

		Classic_BuildLogin(&pkt);
		res = Socket_Write(net_socket, buf, sizeof(pkt), &wrote);

		if (res) {
			MPConnection_FailConnect(res);
		} else {
			MPConnection_FinishConnect();

			if (wrote < sizeof(pkt)) { Server.SendData(buf + wrote, sizeof(pkt) - wrote); }
		}
	} else if (timeSinceLast > NET_TIMEOUT_SECS) {
		MPConnection_FailConnect(0);
	} else {
		float left = NET_TIMEOUT_SECS - timeSinceLast;
		Event_RaiseFloat(&WorldEvents.Loading, left / NET_TIMEOUT_SECS);
	}
}

static void MPConnection_BeginConnect(void) {
	static const cc_string invalid_reason = String_FromConst("Invalid IP address");
	cc_string title; char titleBuffer[STRING_SIZE];
	cc_sockaddr addrs[SOCKET_MAX_ADDRS];
	int numValidAddrs;
	cc_result res;
	String_InitArray(title, titleBuffer);

	/* Default block permissions (in case server supports SetBlockPermissions but doesn't send) */
	Blocks.CanPlace[BLOCK_AIR] = false;
	Blocks.CanPlace[BLOCK_LAVA] = false;        Blocks.CanDelete[BLOCK_LAVA] = false;
	Blocks.CanPlace[BLOCK_WATER] = false;       Blocks.CanDelete[BLOCK_WATER] = false;
	Blocks.CanPlace[BLOCK_STILL_LAVA] = false;  Blocks.CanDelete[BLOCK_STILL_LAVA] = false;
	Blocks.CanPlace[BLOCK_STILL_WATER] = false; Blocks.CanDelete[BLOCK_STILL_WATER] = false;
	Blocks.CanPlace[BLOCK_BEDROCK] = false;     Blocks.CanDelete[BLOCK_BEDROCK] = false;
	
	res = Socket_ParseAddress(&Server.Address, Server.Port, addrs, &numValidAddrs);
	if (res == ERR_INVALID_ARGUMENT) {
		MPConnection_Fail(&invalid_reason); return;
	} else if (res) {
		MPConnection_FailConnect(res); return;
	}

	res = Socket_Create(&net_socket, &addrs[0]);
	if (res) { MPConnection_FailConnect(res); return; }

	Socket_SetNonBlocking(net_socket, true);
	res = Socket_Connect(net_socket, &addrs[0]);

	if (res && res != ReturnCode_SocketInProgess && res != ReturnCode_SocketWouldBlock) {
		MPConnection_FailConnect(res);
	} else {
		Server.Disconnected = false;
		net_connecting      = true;
		timeSinceLast       = 0.0f;

		String_Format2(&title, "Connecting to %s:%i..", &Server.Address, &Server.Port);
		LoadingScreen_Show(&title, &String_Empty);
	}
}

static void MPConnection_SendBlock(int x, int y, int z, BlockID old, BlockID now) {
	if (now == BLOCK_AIR) {
		now = Inventory_SelectedBlock;
		Classic_SendSetBlock(x, y, z, false, now);
	} else {
		Classic_SendSetBlock(x, y, z, true, now);
	}
}

static void MPConnection_SendChat(const cc_string* text) {
	#define CHAT_MAX_PKTS 2
	struct ChatPacket pkts[CHAT_MAX_PKTS];
	cc_bool partial;
	cc_string left;
	int pktCount, partLen;

	if (!text->length || net_connecting) return;
	left = *text;
	pktCount = 0;

	/* Try to fit multilined chat messages into one TCP packet */
	while (left.length) {
		partial = left.length > STRING_SIZE;
		Classic_BuildChat(&left, partial, &pkts[pktCount++]);

		partLen = min(left.length, STRING_SIZE);
		left    = String_UNSAFE_SubstringAt(&left, partLen);

		if (pktCount == CHAT_MAX_PKTS) continue;
		Server.SendData((cc_uint8*)pkts, pktCount * sizeof(pkts[0]));
		pktCount = 0;
	}

	if (!pktCount) return;
	Server.SendData((cc_uint8*)pkts, pktCount * sizeof(pkts[0]));
}

static void MPConnection_Disconnect(void) {
	static const cc_string title  = String_FromConst("Disconnected!");
	static const cc_string reason = String_FromConst("You've lost connection to the server");
	Game_Disconnect(&title, &reason);
}

void Server_LeaveLAN(void) {
	MPConnection_Disconnect();
	CavLAN_Reset();
	cavlan_isClient = false;
}

static void DisconnectReadFailed(cc_result res) {
	cc_string msg; char msgBuffer[STRING_SIZE * 2];
	String_InitArray(msg, msgBuffer);
	String_Format3(&msg, "Error reading from %s:%i: %e" _NL, &Server.Address, &Server.Port, &res);

	Logger_Log(&msg);
	MPConnection_Disconnect();
}

static void DisconnectInvalidOpcode(cc_uint8 opcode) {
	static const cc_string title = String_FromConst("Disconnected");
	cc_string tmp; char tmpBuffer[STRING_SIZE];
	String_InitArray(tmp, tmpBuffer);

	String_Format2(&tmp, "Server sent invalid packet %b! (prev %b)", &opcode, &lastOpcode);
	Game_Disconnect(&title, &tmp); return;
}

static cc_bool MPConnection_Tick(struct ScheduledTask2* task) {
	Net_Handler handler;
	cc_uint8* readEnd;
	cc_uint8* readCur;
	cc_uint32 read;
	int i, remaining;
	cc_result res;

	timeSinceLast += task->interval;
	if (Server.Disconnected) return true;
	if (net_connecting) { MPConnection_TickConnect(); return true; }

	/* NOTE: using a read call that is a multiple of 4096 (appears to?) improve read performance */	
	res = Socket_Read(net_socket, net_readCurrent, 4096 * 4, &read);
	
	if (res) {
		/* 'no data available for non-blocking read' is an expected error */
		if (res == ReturnCode_SocketInProgess)  res = 0;
		if (res == ReturnCode_SocketWouldBlock) res = 0;

		if (res) { DisconnectReadFailed(res); return true; }
	} else if (read == 0) {
		/* recv only returns 0 read when socket is closed.. probably? */
		/* Over 30 seconds since last packet, connection probably dropped */
		/* TODO: Should this be checked unconditonally instead of just when read = 0 ? */
		if (timeSinceLast >= 30.0f) { MPConnection_Disconnect(); return true; }
	} else {
		readCur       = net_readBuffer;
		readEnd       = net_readCurrent + read;
		timeSinceLast = 0.0f;

		while (readCur < readEnd) {
			cc_uint8 opcode = readCur[0];

			/* Workaround for older D3 servers which wrote one byte too many for HackControl packets */
			if (cpe_needD3Fix && lastOpcode == OPCODE_HACK_CONTROL && (opcode == 0x00 || opcode == 0xFF)) {
				Platform_LogConst("Skipping invalid HackControl byte from D3 server");
				readCur++;
				LocalPlayer_ResetJumpVelocity(Entities.CurPlayer);
				continue;
			}

			if (readCur + Protocol.Sizes[opcode] > readEnd) break;
			handler = Protocol.Handlers[opcode];
			if (!handler) { DisconnectInvalidOpcode(opcode); return true; }

			lastOpcode = opcode;
			handler(readCur + 1); /* skip opcode */
			readCur += Protocol.Sizes[opcode];
		}

		/* Protocol packets might be split up across TCP packets */
		/* If so, copy last few unprocessed bytes back to beginning of buffer */
		/* These bytes are then later combined with subsequently read TCP packet data */
		remaining = (int)(readEnd - readCur);
		for (i = 0; i < remaining; i++) 
		{
			net_readBuffer[i] = readCur[i];
		}
		net_readCurrent = net_readBuffer + remaining;
	}

	if (net_writeFailure) {
		Platform_Log1("Error from send: %e", &net_writeFailure);
		MPConnection_Disconnect(); return true;
	}

	/* Network is ticked 60 times a second. We only send position updates 20 times a second */
	if ((ticks++ % 3) != 0) return true;

	TexturePack_CheckPending();
	Protocol_Tick();
	return true;
}

static void MPConnection_SendData(const cc_uint8* data, cc_uint32 len) {
	cc_uint32 wrote;
	cc_result res;
	int tries = 0;
	if (Server.Disconnected) return;

	while (len) {
		res = Socket_Write(net_socket, data, len, &wrote);
		/* If sending would block (send buffer full), retry for a bit up to 10 seconds */
		/* TODO: Avoid doing this and manually buffer data when this happens */
		if (res && tries < 1000 && (res == ReturnCode_SocketInProgess || res == ReturnCode_SocketWouldBlock)) {
			Thread_Sleep(10);
			tries++;
			continue;
		}

		/* NOTE: Not immediately disconnecting here, as otherwise we sometimes miss out on kick messages */
		if (res)    { net_writeFailure = res;                  return; }
		if (!wrote) { net_writeFailure = ERR_INVALID_ARGUMENT; return; }

		data += wrote; len -= wrote;
	}
}

static void MPConnection_Init(void) {
	Server_ResetState();
	Server.IsSinglePlayer = false;

	Server.BeginConnect = MPConnection_BeginConnect;
	Server.Tick         = MPConnection_Tick;
	Server.SendBlock    = MPConnection_SendBlock;
	Server.SendChat     = MPConnection_SendChat;
	Server.SendData     = MPConnection_SendData;
	net_readCurrent     = net_readBuffer;
	cavlan_isClient     = false;
	CavLAN_Reset();
}

static void CavLanClient_FailConnect(cc_result result) {
	static const cc_string reason = String_FromConst("Could not connect to the CavFX LAN host.");
	if (result) Logger_SysWarn(result, "connecting to CavFX LAN host");
	Game_Disconnect(&cavlan_name, &reason);
	OnClose();
}

static void CavLanClient_FinishConnect(void) {
	cc_uint8 hello[2 + STRING_SIZE * 2];
	cc_string skin; char skinBuffer[STRING_SIZE];
	int length;

	CavLAN.Active    = true;
	CavLAN.Connected = true;
	CavLAN.Hosting   = false;
	CavLAN.Role      = CAVLAN_ROLE_CLIENT;

	net_connecting = false;
	timeSinceLast  = 0.0f;
	net_readCurrent = net_readBuffer;
	CavLan_GetLocalSkin(&skin, skinBuffer);
	length = CavLan_WriteStringPair(hello, &Game_Username, &skin);
	CavLan_LocalPlayerInfoChanged(&Game_Username, &skin);
	CavLan_SendFrame(net_socket, CAVLAN_HELLO, hello, length);
	Event_RaiseVoid(&NetEvents.Connected);
	Event_RaiseFloat(&WorldEvents.Loading, 0.0f);
}

static void CavLanClient_TickConnect(void) {
	cc_bool writable;
	cc_result res = Socket_Poll(net_socket, 0, SOCKET_POLL_WRITE, &writable);

	if (res) {
		CavLanClient_FailConnect(res);
	} else if (writable) {
		CavLanClient_FinishConnect();
	} else if (timeSinceLast > NET_TIMEOUT_SECS) {
		CavLanClient_FailConnect(0);
	} else {
		float left = NET_TIMEOUT_SECS - timeSinceLast;
		Event_RaiseFloat(&WorldEvents.Loading, left / NET_TIMEOUT_SECS);
	}
}

static void CavLanClient_BeginConnect(void) {
	static const cc_string invalid_reason = String_FromConst("Invalid IP address");
	cc_string title; char titleBuffer[STRING_SIZE];
	cc_sockaddr addrs[SOCKET_MAX_ADDRS];
	int numValidAddrs;
	cc_result res;
	String_InitArray(title, titleBuffer);
	cavlan_lastName[0] = '\0';
	cavlan_lastSkin[0] = '\0';

	res = Socket_ParseAddress(&Server.Address, Server.Port, addrs, &numValidAddrs);
	if (res == ERR_INVALID_ARGUMENT) {
		Game_Disconnect(&cavlan_name, &invalid_reason); return;
	} else if (res) {
		CavLanClient_FailConnect(res); return;
	}

	res = Socket_Create(&net_socket, &addrs[0]);
	if (res) { CavLanClient_FailConnect(res); return; }

	Socket_SetNonBlocking(net_socket, true);
	res = Socket_Connect(net_socket, &addrs[0]);

	if (res && res != ReturnCode_SocketInProgess && res != ReturnCode_SocketWouldBlock) {
		CavLanClient_FailConnect(res);
	} else {
		Server.Disconnected = false;
		net_connecting      = true;
		timeSinceLast       = 0.0f;

		String_Format2(&title, "Connecting to %s:%i..", &Server.Address, &Server.Port);
		LoadingScreen_Show(&title, &String_Empty);
	}
}

static void CavLanClient_HandleMapBegin(cc_uint8* data, cc_uint32 length) {
	if (length < 10) return;
	World_NewMap();
	cavlan_width  = Mem_ReadU16_BE(data + 0);
	cavlan_height = Mem_ReadU16_BE(data + 2);
	cavlan_length = Mem_ReadU16_BE(data + 4);
	cavlan_volume = Mem_ReadU32_BE(data + 6);
	cavlan_index  = 0;
	cavlan_hasSpawn = false;
	if (length >= 18) {
		Vec3_Set(cavlan_spawn, Mem_ReadU16_BE(data + 10) / 32.0f, Mem_ReadU16_BE(data + 12) / 32.0f, Mem_ReadU16_BE(data + 14) / 32.0f);
		cavlan_spawnYaw   = Math_Packed2Deg(data[16]);
		cavlan_spawnPitch = Math_Packed2Deg(data[17]);
		cavlan_hasSpawn   = true;
	}
	Mem_Free(cavlan_blocks);
	cavlan_blocks = (BlockRaw*)Mem_TryAlloc(cavlan_volume, 1);
	Event_RaiseFloat(&WorldEvents.Loading, 0.0f);
}

static void CavLanClient_HandleMapChunk(cc_uint8* data, cc_uint32 length) {
	if (!cavlan_blocks) return;
	if (cavlan_index + (int)length > cavlan_volume) length = cavlan_volume - cavlan_index;
	Mem_Copy(cavlan_blocks + cavlan_index, data, length);
	cavlan_index += length;
	if (cavlan_volume) Event_RaiseFloat(&WorldEvents.Loading, cavlan_index / (float)cavlan_volume);
}

static void CavLanClient_HandleMapEnd(void) {
	struct Entity* host;
	struct LocationUpdate spawn;
	Vec3 pos;
	static const cc_string hostName = String_FromConst("Host");
	static const cc_string hostSkin = String_FromConst("Host");
	if (!cavlan_blocks || cavlan_index != cavlan_volume) return;
	World_SetNewMap(cavlan_blocks, cavlan_width, cavlan_height, cavlan_length);
	cavlan_blocks = NULL;

	if (cavlan_hasSpawn) {
		spawn.flags = LU_HAS_POS | LU_HAS_YAW | LU_HAS_PITCH;
		spawn.pos   = cavlan_spawn;
		spawn.yaw   = cavlan_spawnYaw;
		spawn.pitch = cavlan_spawnPitch;
	} else {
		LocalPlayer_CalcDefaultSpawn(Entities.CurPlayer, &spawn);
	}
	LocalPlayers_MoveToSpawn(&spawn);

	host = &Entities.CurPlayer->Base;
	pos  = host->Position;
	CavLan_EnsureNetPlayer(0, &hostName, &hostSkin, pos, host->Yaw, host->Pitch);
}

static void CavLanClient_HandleBlock(cc_uint8* data, cc_uint32 length) {
	int x, y, z;
	if (length < 7) return;
	x = Mem_ReadU16_BE(data + 0);
	y = Mem_ReadU16_BE(data + 2);
	z = Mem_ReadU16_BE(data + 4);
	if (World_Contains(x, y, z)) Game_UpdateBlock(x, y, z, data[6]);
}

static void CavLanClient_HandleChat(cc_uint8* data, cc_uint32 length) {
	cc_string msg = String_Init((char*)data, min(length, STRING_SIZE), STRING_SIZE);
	Chat_Add(&msg);
}

static void CavLanClient_HandleDroppedItem(cc_uint8* data, cc_uint32 length) {
	BlockID block;
	Vec3 pos, vel;
	if (length < 20) return;
	CavLan_ReadDroppedItem(data, &block, &pos, &vel);
	DroppedItem_SpawnAtVelocity(block, pos, vel);
}

static void CavLanClient_HandlePickedItem(cc_uint8* data, cc_uint32 length) {
	BlockID block;
	Vec3 pos;
	if (length < 14) return;
	CavLan_ReadPickedItem(data, &block, &pos);
	DroppedItem_RemoveNearest(block, pos);
}

static void CavLanClient_HandleGameState(cc_uint8* data, cc_uint32 length) {
	struct HacksComp* hacks = &Entities.CurPlayer->Hacks;
	if (length < 12) return;

	Game_SurvivalMode = data[0] != 0;
	Game_ClassicMode = data[1] != 0;
	Game_ClassicHacks = data[2] != 0;

	Options_SetBool(OPT_SURVIVAL_MODE, Game_SurvivalMode);

	{
		int i;

		Inventory_ResetMapping();
		Inventory.Offset = 0;

		for (i = 0; i < Array_Elems(Inventory.Table); i++) {
			Inventory.Table[i] = BLOCK_AIR;
			Inventory.Counts[i] = 0;
		}

		for (i = 0; i < INVENTORY_CRAFTING_GRID; i++) {
			Inventory.Craft[i] = BLOCK_AIR;
			Inventory.CraftCounts[i] = 0;
		}

		Inventory_UpdateCrafting();

		if (!Game_SurvivalMode) {
			for (i = 0; i < INVENTORY_BLOCKS_PER_HOTBAR; i++) {
				Inventory_Set(i, Game_Version.Hotbar[i]);
			}
		}

		Inventory.SelectedIndex = 0;
		Event_RaiseVoid(&UserEvents.HeldBlockChanged);
	}

	Game_AllowCustomBlocks   = !Game_ClassicMode && Options_GetBool(OPT_CUSTOM_BLOCKS,      true);
	Game_SimpleArmsAnim      = !Game_ClassicMode && Options_GetBool(OPT_SIMPLE_ARMS_ANIM,   false);
	Game_BreakableLiquids    = !Game_ClassicMode && Options_GetBool(OPT_MODIFIABLE_LIQUIDS, false);
	Game_AllowServerTextures = !Game_ClassicMode && Options_GetBool(OPT_SERVER_TEXTURES,    true);
	GameVersion_Load();

	hacks->Enabled           = data[3]  != 0;
	hacks->CanAnyHacks       = data[4]  != 0;
	hacks->CanFly            = data[5]  != 0;
	hacks->CanNoclip         = data[6]  != 0;
	hacks->CanSpeed          = data[7]  != 0;
	hacks->CanRespawn        = data[8]  != 0;
	hacks->CanUseThirdPerson = data[9]  != 0;
	hacks->CanPushbackBlocks = data[10] != 0;
	hacks->CanSeeAllNames    = data[11] != 0;
	HacksComp_Update(hacks);

	Server.SupportsFullCP437 = !Game_ClassicMode;
}


static void CavLanClient_HandleMobSnapshot(cc_uint8* data, cc_uint32 length) {
	EntityID id;
	cc_uint8 kind;
	Vec3 pos, vel;
	float yaw, pitch;
	int health;
	if (length < 25) return;
	CavLan_ReadMobSnapshot(data, &id, &kind, &pos, &vel, &yaw, &pitch, &health);
	if (health <= 0) { Mob_RemoveSynced(id); return; }
	Mob_SetSyncedState(id, kind, pos, vel, yaw, pitch, health);
}

static void CavLanClient_HandleMobRemove(cc_uint8* data, cc_uint32 length) {
	if (length < 2) return;
	Mob_RemoveSynced((EntityID)Mem_ReadU16_BE(data + 0));
}

static void CavLanClient_HandlePlayerInfo(cc_uint8* data, cc_uint32 length) {
	struct Entity* local = &Entities.CurPlayer->Base;
	cc_string name, skin;
	char nameBuffer[STRING_SIZE], skinBuffer[STRING_SIZE];
	EntityID id;

	if (length < 1) return;
	id = data[0];
	if (id >= MAX_NET_PLAYERS) return;

	String_InitArray(name, nameBuffer);
	String_InitArray(skin, skinBuffer);
	if (!CavLan_ReadStringPair(data + 1, length - 1, &name, &skin)) {
		CavLan_ReadLegacyName(data + 1, length - 1, &name, &skin);
	}
	CavLan_FallbackPlayerInfo(id, &name, &skin);

	if (Entities.List[id]) {
		CavLan_SetNetPlayerInfo(id, &name, &skin);
		return;
	}
	CavLan_EnsureNetPlayer(id, &name, &skin, local->Position, local->Yaw, local->Pitch);
}

static void CavLanClient_HandleFrame(cc_uint8 type, cc_uint8* data, cc_uint32 length) {
	if (type == CAVLAN_MAP_BEGIN) { CavLanClient_HandleMapBegin(data, length); return; }
	if (type == CAVLAN_MAP_CHUNK) { CavLanClient_HandleMapChunk(data, length); return; }
	if (type == CAVLAN_MAP_END)   { CavLanClient_HandleMapEnd(); return; }
	if (type == CAVLAN_BLOCK)     { CavLanClient_HandleBlock(data, length); return; }
	if (type == CAVLAN_CHAT)      { CavLanClient_HandleChat(data, length); return; }
	if (type == CAVLAN_DROPPED_ITEM) { CavLanClient_HandleDroppedItem(data, length); return; }
	if (type == CAVLAN_PICKED_ITEM) { CavLanClient_HandlePickedItem(data, length); return; }
	if (type == CAVLAN_MOB_SNAPSHOT) { CavLanClient_HandleMobSnapshot(data, length); return; }
	if (type == CAVLAN_MOB_REMOVE) { CavLanClient_HandleMobRemove(data, length); return; }
	if (type == CAVLAN_GAME_STATE) { CavLanClient_HandleGameState(data, length); return; }
	if (type == CAVLAN_PLAYER_INFO) { CavLanClient_HandlePlayerInfo(data, length); return; }
	if (type == CAVLAN_POSITION) {
		EntityID id;
		Vec3 pos;
		float yaw, pitch;
		if (length < 9) return;
		id = data[0];
		if (id >= MAX_NET_PLAYERS) return;
		Vec3_Set(pos, Mem_ReadU16_BE(data + 1) / 32.0f, (Mem_ReadU16_BE(data + 3) - 51) / 32.0f, Mem_ReadU16_BE(data + 5) / 32.0f);
		yaw   = data[7] * (360.0f / 256.0f);
		pitch = data[8] * (360.0f / 256.0f);
		if (!Entities.List[id]) {
			static const cc_string name = String_FromConst("Player");
			CavLan_EnsureNetPlayer(id, &name, &name, pos, yaw, pitch);
		} else {
			CavLan_UpdateNetPlayer(id, pos, yaw, pitch);
		}
	}
}

static void CavLanClient_SendPlayerInfo(void) {
	cc_uint8 pkt[1 + 2 + STRING_SIZE * 2];
	cc_string skin; char skinBuffer[STRING_SIZE];
	int length;

	CavLan_GetLocalSkin(&skin, skinBuffer);
	if (!CavLan_LocalPlayerInfoChanged(&Game_Username, &skin)) return;

	pkt[0] = 0;
	length = CavLan_WriteStringPair(pkt + 1, &Game_Username, &skin);
	CavLan_SendFrame(net_socket, CAVLAN_PLAYER_INFO, pkt, 1 + length);
}

static cc_bool CavLanClient_Tick(struct ScheduledTask2* task) {
	cc_uint8* readEnd;
	cc_uint8* readCur;
	cc_uint32 read, length;
	int remaining, size, i;
	cc_result res;

	timeSinceLast += task->interval;
	if (Server.Disconnected) return true;
	if (net_connecting) { CavLanClient_TickConnect(); return true; }

	res = Socket_Read(net_socket, net_readCurrent, 4096 * 4, &read);
	if (res) {
		if (res == ReturnCode_SocketInProgess)  res = 0;
		if (res == ReturnCode_SocketWouldBlock) res = 0;
		if (res) { MPConnection_Disconnect(); return true; }
	} else if (read == 0) {
		if (timeSinceLast >= 30.0f) { MPConnection_Disconnect(); return true; }
	} else {
		readCur       = net_readBuffer;
		readEnd       = net_readCurrent + read;
		timeSinceLast = 0.0f;

		while (readCur < readEnd) {
			size = CavLan_FrameSize(readCur, (int)(readEnd - readCur));
			if (size < 0) { MPConnection_Disconnect(); return true; }
			if (!size || readCur + size > readEnd) break;
			length = Mem_ReadU32_BE(readCur + 1);
			CavLanClient_HandleFrame(readCur[0], readCur + 5, length);
			readCur += size;
		}

		remaining = (int)(readEnd - readCur);
		for (i = 0; i < remaining; i++) net_readBuffer[i] = readCur[i];
		net_readCurrent = net_readBuffer + remaining;
	}

	if ((ticks++ % 3) == 0 && World.Loaded) {
		struct Entity* e = &Entities.CurPlayer->Base;
		cc_uint8 pkt[8];
		CavLanClient_SendPlayerInfo();
		Mem_WriteU16_BE(pkt + 0, (int)(e->Position.x * 32));
		Mem_WriteU16_BE(pkt + 2, (int)(e->Position.y * 32) + 51);
		Mem_WriteU16_BE(pkt + 4, (int)(e->Position.z * 32));
		pkt[6] = Math_Deg2Packed(e->Yaw);
		pkt[7] = Math_Deg2Packed(e->Pitch);
		CavLan_SendFrame(net_socket, CAVLAN_POSITION, pkt, sizeof(pkt));
	}
	return true;
}

static void CavLanClient_SendBlock(int x, int y, int z, BlockID old, BlockID now) {
	cc_uint8 pkt[7];
	(void)old;
	Mem_WriteU16_BE(pkt + 0, x);
	Mem_WriteU16_BE(pkt + 2, y);
	Mem_WriteU16_BE(pkt + 4, z);
	pkt[6] = (cc_uint8)now;
	CavLan_SendFrame(net_socket, CAVLAN_BLOCK, pkt, sizeof(pkt));
}

static void CavLanClient_SendChat(const cc_string* text) {
	CavLan_SendFrame(net_socket, CAVLAN_CHAT, (cc_uint8*)text->buffer, text->length);
}

static void CavLanClient_Init(void) {
	Server_ResetState();
	Server.IsSinglePlayer = false;
	Server.BeginConnect = CavLanClient_BeginConnect;
	Server.Tick         = CavLanClient_Tick;
	Server.SendBlock    = CavLanClient_SendBlock;
	Server.SendChat     = CavLanClient_SendChat;
	Server.SendData     = MPConnection_SendData;
	net_readCurrent     = net_readBuffer;
	cavlan_isClient     = true;
	CavLAN.Active       = true;
	CavLAN.Role         = CAVLAN_ROLE_CLIENT;
}
#else
static void MPConnection_Init(void) { SPConnection_Init(); }
static void CavLanClient_Init(void) { SPConnection_Init(); }
void Server_SendDroppedItem(BlockID block, Vec3 pos, Vec3 vel) { (void)block; (void)pos; (void)vel; }
void Server_SendPickedItem(BlockID block, Vec3 pos) { (void)block; (void)pos; }
#endif


/*########################################################################################################################*
*---------------------------------------------------Component interface---------------------------------------------------*
*#########################################################################################################################*/
static void OnNewMap(void) {
	int i;
	if (Server.IsSinglePlayer && !cavlan_isClient) return;

	/* wipe all existing entities */
	for (i = 0; i < MAX_NET_PLAYERS; i++) 
	{
		Entities_Remove(i);
	}
}

static void OnInit(void) {
	String_InitArray(Server.Name,    nameBuffer);
	String_InitArray(Server.MOTD,    motdBuffer);
	String_InitArray(Server.AppName, appBuffer);
	String_InitArray(CavLAN.Address, CavLAN.AddressBuffer);
	CavLAN_Reset();

	if (!Server.Address.length) {
		SPConnection_Init();
	} else {
		MPConnection_Init();
	}

	Game_Tasks.network.interval = GAME_NET_TICKS;
	Game_Tasks.network.callback = Server.Tick;
	ScheduledTask2_Add(&Game_Tasks.network);

	String_AppendConst(&Server.AppName, GAME_APP_NAME);
	String_AppendConst(&Server.AppName, Platform_AppNameSuffix);

#ifdef CC_BUILD_WEB
	if (!Input_TouchMode) return;
	Server.AppName.length = 0;
	String_AppendConst(&Server.AppName, GAME_APP_ALT);
#endif
}

static void Server_ConnectCommon(const cc_string* address, int port) {
	OnClose();

	Server.Address.length = 0;
	String_Copy(&Server.Address, address);
	Server.Port = port;
}

void Server_ConnectToClassic(const cc_string* address, int port) {
	Server_ConnectCommon(address, port);
	CavLAN_Reset();

	MPConnection_Init();
	Protocol_Reset();
	Game_Tasks.network.callback = Server.Tick;
	Server.BeginConnect();
}

void Server_ConnectToCavLAN(const cc_string* address, int port) {
	if (lan_hosting && Server_IsLoopbackAddress(address)) {
		static const cc_string title  = String_FromConst("LAN host is already open");
		static const cc_string reason = String_FromConst("Open another CavFX window to join 127.0.0.1. This window is the host.");
		Game_Disconnect(&title, &reason);
		return;
	}

	Server_ConnectCommon(address, port);
	CavLAN.Active     = true;
	CavLAN.Hosting    = false;
	CavLAN.Connected  = false;
	CavLAN.IsLoopback = Server_IsLoopbackAddress(address);
	CavLAN.Role       = CAVLAN_ROLE_CLIENT;
	CavLAN_SetAddress(address, port);

	CavLanClient_Init();
	Game_Tasks.network.callback = Server.Tick;
	Server.BeginConnect();
}

void Server_ConnectTo(const cc_string* address, int port) {
	Server_ConnectToClassic(address, port);
}

static void OnReset(void) {
	if (Server.IsSinglePlayer) {
		Server_StopLAN();
		return;
	}
	net_writeFailure = 0;
	OnClose();
}

static void OnFree(void) {
	Server.Address.length = 0;
	OnClose();
}

static void OnClose(void) {
	cc_bool wasLanClient = CavLAN.Role == CAVLAN_ROLE_CLIENT || cavlan_isClient;
	Server.Address.length = 0;
	Server.Port = 0;

	if (Server.IsSinglePlayer) {
		Server_StopLAN();
		Physics_Free();
	}
	else {
		Ping_Reset();
		if (!Server.Disconnected) {
			Mem_Free(cavlan_blocks); cavlan_blocks = NULL;
			Socket_Close(net_socket);
			Server.Disconnected = true;
		}
	}

	cavlan_isClient = false;
	if (wasLanClient || CavLAN.Role == CAVLAN_ROLE_HOST) CavLAN_Reset();
}

struct IGameComponent Server_Component = {
	OnInit,  /* Init  */
	OnFree,  /* Free  */
	OnReset, /* Reset */
	OnNewMap /* OnNewMap */
};
