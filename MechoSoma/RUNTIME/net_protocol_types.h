#ifndef __NET_PROTOCOL_TYPES_H__
#define __NET_PROTOCOL_TYPES_H__

/*
	Plain types and limits shared by the protocol header and the transport.
	Deliberately free of any project or OS dependency.
*/

typedef signed char int8;
typedef unsigned char uint8;
typedef signed short int16;
typedef unsigned short uint16;
typedef signed int int32;
typedef unsigned int uint32;

const int NET_NAME_LEN		= 16;		// player nick, NUL terminated
const int NET_SERVER_NAME_LEN = 24;
const int NET_KICK_TEXT_LEN	= 32;

// Engine limits, see mechosoma.h HS_PLAYERS_MAX and M3d_id.h NUM_ACTIVE_PARTS.
const int NET_MAX_PLAYERS	= 5;
const int NET_ACTIVE_PARTS	= 4;

// One mechos is a figure plus one arcane per remaining part. The lobby carries
// that shape: NET_ACTIVE_PARTS - 1 spell part ids.
const int NET_LOBBY_SPELLS	= NET_ACTIVE_PARTS - 1;

// Maximum number of peers a host tracks.
const int NET_MAX_PEERS		= 8;

#pragma pack(push,1)

struct netAddr
{
	unsigned int ip;			// network byte order
	unsigned short port;		// host byte order

	netAddr(void){ ip = 0; port = 0; }
};

#pragma pack(pop)

#endif /* __NET_PROTOCOL_TYPES_H__ */