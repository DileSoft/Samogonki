#ifndef __NET_PROTOCOL_H__
#define __NET_PROTOCOL_H__

/*
	Wire protocol for the arcade multiplayer mode.

	Design in one paragraph: the host is authoritative and is the only place
	where the simulation runs. Clients send nothing but their control
	intent, and draw every other vehicle from snapshots. Nothing in this file
	is a game object layout - the host translates between these packed
	structs and the engine types.

	Byte order is little endian everywhere, which covers every platform the
	game ships on.
*/

#include "net_protocol_types.h"

/*
	Everything below is a wire struct and is copied as bytes, not field by
	field. Without explicit packing the compiler inserts padding both to pad
	the struct out and into the middle of it, and memcpy then ships the
	padding along: the sizes stop matching what a reader of this file would
	expect, and the padding bytes themselves differ between two builds of the
	same code. The transport test asserts these sizes for exactly that
	reason.
*/
#pragma pack(push,1)

/* ------------------------------ FRAMING ---------------------------------- */

// Fixed per datagram header, payload follows immediately.
struct netHeader
{
	unsigned short magic;
	unsigned char type;
	unsigned char flags;
	unsigned short seq;
	unsigned short ack;
	unsigned short len;
};

// netHeader::flags
#define NET_FLAG_RELIABLE	0x01
#define NET_FLAG_FRAGMENT	0x02
#define NET_FLAG_ACK_ONLY	0x04

// One datagram never exceeds this, leaving room below a 1500 byte MTU for
// IP/UDP and for tunnelling overhead.
const int NET_MAX_DATAGRAM			= 1200;
const int NET_MAX_PAYLOAD			= NET_MAX_DATAGRAM - sizeof(netHeader);

// Fragment payload layout:
//	[0] original message type, [1] frag_id, [2] index, [3] count, [4..] data
const int NET_FRAG_PAYLOAD			= NET_MAX_PAYLOAD - 4;
const int NET_FRAG_MAX				= 8;
const int NET_FRAG_TIMEOUT_MS		= 2000;

const unsigned short NET_MAGIC		= 0x4D52;	// 'MR'

const unsigned int NET_PROTO_VERSION	= 1;

// Reliable retransmission, milliseconds since clocki().
const int NET_RETRANSMIT_MS			= 300;
const int NET_RETRANSMIT_MAX			= 8;

// No packet at all for this long means the peer is gone.
const int NET_PEER_TIMEOUT_MS		= 30000;
const int NET_PING_PERIOD_MS		= 1000;

// One simulation and network tick. netWelcome::tick_rate is this expressed
// in hertz, so 1000 / NET_TICK_MS must stay equal to the tick_rate the host
// advertises or the agreed start tick means different times to each side.
const int NET_TICK_MS			= 50;
const int NET_TICK_RATE_HZ		= 1000 / NET_TICK_MS;	// 20

// How long after START_GAME the race actually begins. Padding, not tuning:
// it has to cover one retransmit plus a slow frame on the slowest machine.
const int NET_START_DELAY_MS		= 1500;

// Roster is rebroadcast on change and on this heartbeat, so a peer that
// misses one still catches the next and late arrivals are not invisible.
const int NET_ROSTER_MS			= 500;

// Slots: 0 is the host and is never a peer.
const int NET_SLOT_HOST			= 0;

/* ---------------------------- MESSAGE TYPES ------------------------------ */

enum netMessageTypes
{
	NET_MSG_NONE = 0,

	NET_MSG_PING,
	NET_MSG_PONG,
	NET_MSG_HELLO,
	NET_MSG_WELCOME,
	NET_MSG_ROSTER,
	NET_MSG_READY,
	NET_MSG_START_GAME,
	NET_MSG_INPUT,
	NET_MSG_SNAPSHOT,
	NET_MSG_EVENT,
	NET_MSG_GAME_OVER,
	NET_MSG_LEAVE,
	NET_MSG_KICK,

	NET_MSG_SPAWN,
	NET_MSG_DESPAWN,
	NET_MSG_ACTOR_STATE,

	NET_MSG_FRAGMENT
};

const int NET_MSG_MAX				= NET_MSG_FRAGMENT;

/* ------------------------------ CLIENT -> HOST --------------------------- */

// NET_MSG_INPUT, batched: up to NET_INPUT_BATCH ticks per datagram.
struct netInput
{
	unsigned int tick;		// game tick this input belongs to
	signed char traction;	// -127..127, scaled to -1..1
	signed char rudder;		// -127..127, scaled to -1..1
	unsigned char buttons;	// netButtons
	unsigned char pad;
};

const int NET_INPUT_BATCH				= 4;

// netInput::buttons
#define NET_BTN_HORN			0x01
#define NET_BTN_ARCANE0			0x02
#define NET_BTN_ARCANE1			0x04
#define NET_BTN_ARCANE2			0x08
#define NET_BTN_ARCANE_REMOVE	0x10
#define NET_BTN_MAP				0x20
#define NET_BTN_CENTER_VIEW		0x40

// NET_MSG_HELLO
struct netHello
{
	unsigned short proto_version;
	unsigned char name[NET_NAME_LEN];
	unsigned char client_build;
	unsigned char input_rate_hz;
};

// NET_MSG_READY
struct netReady
{
	unsigned char ready;
};

// NET_MSG_LEAVE
struct netLeave
{
	unsigned char reason;
};

/* ------------------------------ HOST -> CLIENT --------------------------- */

// NET_MSG_WELCOME
struct netWelcome
{
	unsigned char your_slot;
	unsigned char num_racers;
	unsigned char max_players;
	unsigned char reserved;
	unsigned short tick_rate;			// network ticks per second
	unsigned int tick_now;				// host tick at send time
	unsigned char world_id;
	unsigned char track_id;
	unsigned char laps;
	unsigned char client_rnd[4];			// seed for fxlabClientRND
	unsigned char server_name[NET_NAME_LEN];
};

// NET_MSG_ROSTER
struct netRosterHeader
{
	unsigned char count;
	unsigned char flags;
	unsigned char ping[NET_MAX_PLAYERS];
	unsigned int server_tick;
};

struct netRosterEntry
{
	unsigned char slot;
	unsigned char state;		// netRosterStates
	unsigned char racer_id;
	unsigned char kind;		// M3D_KIND of the mecha, 6 bits used
	unsigned char name[NET_NAME_LEN];
};

enum netRosterStates
{
	NET_ROSTER_FREE = 0,
	NET_ROSTER_CONNECTING,
	NET_ROSTER_READY,
	NET_ROSTER_RACING,
	NET_ROSTER_FINISHED,
	NET_ROSTER_BOT
};

// NET_MSG_START_GAME - every peer switches to racing on start_tick.
// start_tick == 0 together with NET_START_PREPARE means "the start is coming,
// acknowledge": the host commits to the tick only once every peer has the
// message in its inbox. See the note at the top of net_match.cpp.
#define NET_START_PREPARE		0x01

struct netStartGame
{
	unsigned int start_tick;
	unsigned int rng_seed;		// host RNG for its own simulation
	unsigned char world_id;
	unsigned char track_id;
	unsigned char laps;
	unsigned char bots_mask;
	unsigned char flags;
	unsigned char racer_kind[NET_MAX_PLAYERS];
};

// NET_MSG_KICK
enum netKickReason
{
	NET_KICK_ROOM_FULL = 1,
	NET_KICK_BAD_PASSWORD,
	NET_KICK_BAD_VERSION,
	NET_KICK_TIMEOUT,
	NET_KICK_HOST_LEFT,
	NET_KICK_ALREADY_RUNNING
};

struct netKick
{
	unsigned char reason;
	unsigned char text[NET_KICK_TEXT_LEN];
};

/* ------------------------------- SNAPSHOTS ------------------------------- */

// One racer, 50 bytes. Positions wrap around the world, the engine world is
// toroidal (GlobalWorldSize, 2048 by default) so a 16 bit value is exact
// enough and survives crossing the border for free.
struct netRacerState
{
	unsigned char slot;			// 0..NET_MAX_PLAYERS-1
	unsigned char racer_id;		// mchRacer::stPtr->ID
	unsigned char kind;			// M3D_KIND(geometry->Type)
	unsigned char mech_state;	// Mechos::State

	unsigned short pos_x;		// mod NET_WORLD_SIZE
	unsigned short pos_y;		// mod NET_WORLD_SIZE
	signed short pos_z;

	unsigned short yaw;			// 0..2*PI scaled by NET_Q_YAW
	signed char pitch;
	signed char roll;

	signed short vel_x;
	signed short vel_y;
	signed char vel_z;
	signed char ang_vel_z;

	unsigned short speed;
	unsigned char energy;		// EnergyValue() / EnergyValueMax()
	unsigned short flags;		// mchRacer::flags, MCH_NET_REMOTE included

	unsigned char features[3];	// Mechos::features_, 22 bits
	unsigned char part_kind[NET_ACTIVE_PARTS];
	unsigned char part_arcane[NET_ACTIVE_PARTS];
	unsigned char part_power[NET_ACTIVE_PARTS];

	unsigned char lap;
	unsigned char checkpoint;
	unsigned char place;
	unsigned int time_ms;

	unsigned char bonus_count;	// followed by that many netBonus
};

struct netBonus
{
	unsigned short id;
	unsigned char type;			// mchBonusArcaneData::Type | flags
};

// NET_MSG_SNAPSHOT
struct netSnapshotHeader
{
	unsigned int server_tick;
	unsigned char racer_count;
	unsigned char delta_mask;	// bit i: racer i changed since last snapshot
};

// NET_MSG_EVENT
enum netEventTypes
{
	NET_EV_ARCANE_CAST = 1,
	NET_EV_RACER_DESTROYED,
	NET_EV_RACER_RESPAWN,
	NET_EV_RACER_FINISHED,
	NET_EV_HORN,
	NET_EV_SLOT_TAKEN,
	NET_EV_CHAT
};

struct netEvent
{
	unsigned char type;
	unsigned char slot;
	signed short pos_x;
	signed short pos_y;
	unsigned char part;
	unsigned char arcane_id;
	unsigned char target_id;
	unsigned char reserved;
	unsigned int time_ms;
};

// NET_MSG_GAME_OVER
struct netGameOver
{
	unsigned char reason;
	unsigned char order[NET_MAX_PLAYERS];
	unsigned short time[NET_MAX_PLAYERS];
};

/* ------------------------- WORLD ACTORS (SPAWN) -------------------------- */

enum netEntityTypes
{
	NET_ENT_NONE = 0,

	// Ballistic, class B: the client recreates the same body from these
	// initial conditions and lets the shared Body physics carry it. Drift
	// over a two or three second flight is centimetres and invisible.
	NET_ENT_CANNON_BOMB,
	NET_ENT_VOLCANIC_BOMB,
	NET_ENT_FIRE_BOLIDE,
	NET_ENT_SNOW_BALL,
	NET_ENT_SLEEPY_MORTAR,
	NET_ENT_MINOR_BONUS,
	NET_ENT_MAJOR_BONUS,
	NET_ENT_DEBRIS,
	NET_ENT_TITANIUM_BALL,
	NET_ENT_CRYSTAL,
	NET_ENT_BUBBLE,

	// Tracked, class C: these steer themselves toward players whose
	// position the client only knows approximately, so local simulation
	// cannot work. The host replicates their state instead.
	NET_ENT_BONUS_MAN,
	NET_ENT_BUTTERFLY,
	NET_ENT_WICKED_BUTTERFLY,
	NET_ENT_DRAGON,
	NET_ENT_SHARK,
	NET_ENT_WHALE,
	NET_ENT_FROG,
	NET_ENT_WORM,

	// Areas, class C.
	NET_ENT_TRAP_SPHERE,
	NET_ENT_RED_TRACK,
	NET_ENT_SLIME_CIRCLE,
	NET_ENT_DAMAGE_SENSOR,

	NET_ENT_MAX
};

// NET_MSG_SPAWN
struct netSpawn
{
	unsigned short entity_id;	// host assigned, unique per match
	unsigned char type;			// netEntityTypes
	unsigned char owner_slot;	// 255 for none
	signed short pos_x;
	signed short pos_y;
	signed short pos_z;
	signed short vel_x;
	signed short vel_y;
	signed short vel_z;
	unsigned short model_id;		// already resolved by the host
	unsigned short lifetime_ms;
	unsigned char damage;
	unsigned char flags;
};

// model_id must be the resolved id, never the noise range: fxlabCreateBolide
// rolls the model as model + fxlabServerRND.RND(noise) on the host side.

// NET_MSG_DESPAWN
enum netDespawnReason
{
	NET_DESPAWN_EXPIRED = 0,
	NET_DESPAWN_HIT,
	NET_DESPAWN_COLLECTED,
	NET_DESPAWN_KILLED_BY_HOST
};

struct netDespawn
{
	unsigned short entity_id;
	unsigned char reason;
};

// NET_MSG_ACTOR_STATE, tracked entities only
struct netActorHeader
{
	unsigned int server_tick;
	unsigned char count;
};

// netActorState reuses the racer block layout, slot carries the entity_id.
struct netActorState
{
	unsigned short entity_id;
	unsigned char type;
	signed short pos_x;
	signed short pos_y;
	signed short pos_z;
	unsigned short yaw;
	signed short vel_x;
	signed short vel_y;
	unsigned char flags;
	unsigned char life_permille;
};

/* ---------------------- INTEREST MANAGEMENT (CLASS C) -------------------- */

// Actors far from every viewer are not replicated at all. They appear as a
// spawn without state and start moving once a viewer comes close.
const int NET_ACTOR_REPL_HZ			= 10;
const int NET_ACTOR_VIEW_RADIUS		= 1200;		// world units
const int NET_ACTOR_SPAWN_RADIUS	= 2400;		// spawn this far out, tracked coarsely
const int NET_MAX_ACTORS_PER_PACKET	= 16;

/* --------------------------- QUANTIZATION -------------------------------- */

const int NET_WORLD_SIZE	= 2048;		// GlobalWorldSize default

const int NET_Q_WORLD		= 32;		// pos_x, pos_y: 2048 * 32 = 65536
const int NET_Q_HEIGHT		= 16;		// pos_z: +/-2048 -> +/-32768
const int NET_Q_YAW		= 10430;	// 65536 / (2*PI)
const int NET_Q_ANGLE		= 64;		// pitch, roll: +/-PI/2 -> +/-101
const int NET_Q_SPEED_XY	= 64;		// vel_x, vel_y: +/-512 -> +/-32768
const int NET_Q_SPEED_Z	= 32;		// vel_z: +/-64 -> +/-2048
const int NET_Q_SPEED		= 64;		// on screen speed: 0..1024 -> 65536

#pragma pack(pop)

#endif /* __NET_PROTOCOL_H__ */