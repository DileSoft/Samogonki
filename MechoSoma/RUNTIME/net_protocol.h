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

// Version 2 added the arcane mana fields to netRacerState; version 3 added
// netSpawn::client_link, the visual a client builds next to a replicated body;
// version 4 added netSpawn::yaw, the heading the factory-built class B bodies
// are placed with; version 5 added netEvent::pos_z, so an arcane interrupt
// carries the place its effect sits; version 6 widened netSpawn::model_id from
// 16 to 32 bits, because the mesh id is M3D_TOTAL_TYPE and the kind lives in
// its high half - as 16 bits the kind was lost and the client could not find
// the mesh to build. Version 10 added the lobby: netHello and netRosterEntry
// carry the player's mechos (figure plus three spell part ids), and
// netRosterHeader carries the host's game name, so everybody sees who has
// joined and what they are driving before the race loads. Version 11 added
// netSpawn::protect_ms, the owner protection an owned class B body is born
// with, so the client's copy does not treat its own caster as a target. A peer
// of another version is refused in the handshake rather than read with the
// wrong layout.
const unsigned int NET_PROTO_VERSION	= 11;

// Reliable retransmission, milliseconds since clocki().
const int NET_RETRANSMIT_MS			= 300;
const int NET_RETRANSMIT_MAX			= 8;

// No packet at all for this long means the peer is gone.
const int NET_PEER_TIMEOUT_MS		= 30000;
const int NET_PING_PERIOD_MS		= 1000;

// One simulation and network tick. netWelcome::tick_rate is this expressed
// in hertz, so 1000 / NET_TICK_MS must stay equal to the tick_rate the host
// advertises or the agreed start tick means different times to each side.
//
// Forty a second, not twenty: snapshots at this rate cost a LAN almost
// nothing (a few tens of kbit) and halve both the staleness of a remote car
// and how far the local car can drift from the host's copy between
// corrections. On a slow link the interpolation buffer, not this, is what
// would have to give.
const int NET_TICK_MS			= 25;
const int NET_TICK_RATE_HZ		= 1000 / NET_TICK_MS;	// 40

// How long after START_GAME the race actually begins. Padding, not tuning:
// it has to cover one retransmit plus a slow frame on the slowest machine.
const int NET_START_DELAY_MS		= 1500;

// Roster is rebroadcast on change and on this heartbeat, so a peer that
// misses one still catches the next and late arrivals are not invisible.
const int NET_ROSTER_MS			= 500;

// Arcade rule, host side: once somebody else is seated, the race starts after
// this. Not a tuning knob - waiting for a full grid would strand a two-player
// LAN session in the lobby forever, and nobody else is coming until there is a
// matchmaker.
const int NET_LOBBY_START_MS		= 2000;

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

	// The mechos the player brings into the lobby: figure id (1..N, 0 unset)
	// and one arcane part id per spell slot. Everyone's copy of the roster
	// carries these back out, which is what lets the lobby show who is driving
	// what before the race loads.
	unsigned char figure;
	unsigned char spells[NET_LOBBY_SPELLS];
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

	// The host's game name, sent with every roster so a renaming in the lobby
	// reaches everybody without a message of its own.
	unsigned char game_name[NET_SERVER_NAME_LEN];
};

struct netRosterEntry
{
	unsigned char slot;
	unsigned char state;		// netRosterStates
	unsigned char racer_id;
	unsigned char kind;		// M3D_KIND of the mecha, 6 bits used
	unsigned char figure;		// mechos figure, 1..N, 0 unset
	unsigned char spells[NET_LOBBY_SPELLS];	// arcane part id per slot
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

// One racer, 54 bytes. Positions wrap around the world, the engine world is
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

	// Per part arcane state, indexed the same way as the part's own
	// part_by_index(). Power is Power/MaxPower scaled to 0..255 (see
	// net_q_mana); Status is mchArcaneData::Status, which is what the client
	// checks before it lets a keypress become a spell.
	unsigned char part_power[NET_ACTIVE_PARTS];
	unsigned char part_status[NET_ACTIVE_PARTS];

	unsigned char lap;
	unsigned char checkpoint;
	unsigned char place;
	unsigned int time_ms;
	unsigned short nstar;		// mchRacer::NStar + stPtr->NStar

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
	NET_EV_ARCANE_INTERRUPT,
	NET_EV_RACER_DESTROYED,
	NET_EV_RACER_RESPAWN,
	NET_EV_RACER_FINISHED,
	NET_EV_HORN,
	NET_EV_SLOT_TAKEN,
	NET_EV_CHAT,
	NET_EV_ARCANE_STOP,
	NET_EV_ARCANE_STOP_VISUAL,
	NET_EV_ARCANE_LATCH,		// target_id: the racer a spell latched onto
	NET_EV_BONUS,			// arcane_id: a pickup. Host to clients: the fanfare.
					// Client to host: the report the host replays against
					// its copy of that slot's racer (net_apply_client_bonus).

	NET_EV_MAX
};

struct netEvent
{
	unsigned char type;
	unsigned char slot;
	signed short pos_x;
	signed short pos_y;
	signed short pos_z;			// a place: an interrupt's effect sits here
	unsigned char part;			// part index, only meaningful for an arcane
	unsigned char arcane_id;
	unsigned char target_id;
	unsigned char reserved;
	unsigned int time_ms;
};

// NET_MSG_EVENT, batched. Not because a player casts often - one button press
// is one event - but because bursts happen: a racer crossing a line of seeds
// casts once per seed, and one datagram per cast is one datagram per pickup
// for the whole match.
struct netEventHeader
{
	unsigned int server_tick;
	unsigned char count;
};

const int NET_MAX_EVENT_PER_PACKET = 8;

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

	// Class B: the client recreates the same body from these initial
	// conditions and lets the shared Body physics carry it. For the ballistic
	// projectiles drift over a two or three second flight is centimetres and
	// invisible.
	//
	// The bodies from NET_ENT_SMART_FIREBALL down are the arcanes' "smart"
	// ones - the ones the engine builds by factory name rather than by mesh
	// (Mdisp->createBody). They steer themselves, so the client's copy can
	// wander off the host's, and that was a deliberate call: they are short
	// lived and cheap this way, whereas class C would want a state batch per
	// body per tick. See net_fx.cpp - the client aims a SmartMissile at the
	// nearest racer so it at least steers in the same direction.
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
	NET_ENT_SMART_FIREBALL,
	NET_ENT_SMART_LIGHTNINGBOLT,
	NET_ENT_SUICIDE,
	NET_ENT_CLOWN,
	NET_ENT_EVIL_EYE,
	NET_ENT_TOMATO,
	NET_ENT_LITTLE_SPIDER,

	// Tracked, class C: these steer themselves toward players whose
	// position the client only knows approximately, so local simulation
	// cannot work. The host replicates their state instead.
	//
	// BonusMan is one type with four subclasses, and they are separated by
	// netSpawn::variant rather than by four enum values: the split is a
	// detail of the engine's factory, not of the protocol. See the
	// netFxActorDef table in net_fx.cpp for the mapping.
	//
	// NET_ENT_WHALE is deliberately unused. There is no plain Whale body -
	// Whale.h only declares WhaleRacer, which is a racer and travels as a
	// snapshot, not as an actor.
	NET_ENT_BONUS_MAN,
	NET_ENT_BUTTERFLY,
	NET_ENT_WICKED_BUTTERFLY,
	NET_ENT_DRAGON,
	NET_ENT_SHARK,
	NET_ENT_WHALE,
	NET_ENT_FROG,
	NET_ENT_WORM,
	NET_ENT_CATERPILLAR,
	NET_ENT_FISH,

	// Areas, class C.
	NET_ENT_TRAP_SPHERE,
	NET_ENT_RED_TRACK,
	NET_ENT_SLIME_CIRCLE,
	NET_ENT_DAMAGE_SENSOR,

	// A force field: vortex, magnetic, attraction, bubble. Position, radius
	// and owner are the whole story - the force itself is built from a
	// constant the client already has, so the subtype rides in variant.
	NET_ENT_FORCE_FIELD,

	NET_ENT_MAX
};

// NET_MSG_SPAWN
struct netSpawn
{
	unsigned short entity_id;	// host assigned, unique per match
	unsigned char type;			// netEntityTypes
	unsigned char variant;		// which subclass of type, 0 when type has one
	unsigned char owner_slot;	// 255 for none
	signed short pos_x;
	signed short pos_y;
	signed short pos_z;
	signed short vel_x;
	signed short vel_y;
	signed short vel_z;
	// Full M3D_TOTAL_TYPE, kind and type both: the kind is the high half and a
	// mesh is looked up by the whole thing. Truncating it to 16 bits loses the
	// kind and the client's mesh library cannot resolve the model.
	unsigned int model_id;		// already resolved by the host
	unsigned short lifetime_ms;
	unsigned char damage;
	unsigned char flags;

	// FXLAB_CLIENT_PROCESS_* + 1, or 0 for none. Some bodies are born with a
	// client-side visual bound to them - a trail, a glow - created next to the
	// body by the code that made it. The body is replicated by its mesh, but
	// that visual has no way to guess its own type, so the host names it here.
	// The +1 keeps 0 meaning "no link": FXLAB_CLIENT_PROCESS_TWISTER is 0.
	unsigned short client_link;

	// Heading, net_q_yaw radians. The factory-built class B bodies are placed
	// with Mdisp->createBody(name,pose,angle), and the angle is part of what
	// the body looks and steers like. The mesh-built ones ignore this: their
	// orientation falls out of the shared physics.
	unsigned short yaw;

	// Remaining owner protection, milliseconds, 0 when the body has no owner.
	// An owned body damages itself on any mechos but its owner, and the
	// protection expires; without the remaining time the client's copy would
	// become unowned on its own schedule and destroy itself (LittleSpider) or
	// break its own crystal long before the host's does.
	unsigned short protect_ms;
};

// model_id must be the resolved id, never the noise range: fxlabCreateBolide
// rolls the model as model + fxlabServerRND.RND(noise) on the host side.

// NET_MSG_SPAWN, batched. One arcane cast makes several bodies at once - a
// splinter burst is seven - and one datagram per body would be one datagram
// per model for the whole match.
struct netSpawnHeader
{
	unsigned int server_tick;
	unsigned char count;
};

// Batched up to this many. A full race with every arcane firing is already
// past the point where fifteen projectiles in one tick is conspicuous, and a
// datagram that holds more would start being a reason to fragment.
const int NET_MAX_SPAWN_PER_PACKET	= 15;
const int NET_MAX_DESPAWN_PER_PACKET	= 15;

// Upper bound on live replicated entities on the host. Deliberately larger
// than one batch: a body that dies despawns on the next tick, but a body that
// never dies has to still be tracked, and refusing to track is better than
// running past the end of the array.
const int NET_MAX_ENTITIES		= 256;

// NET_MSG_DESPAWN, batched. Nothing carries a timestamp: a despawn is
// already late when it is sent, and its only content is which body went away.
struct netDespawnHeader
{
	unsigned char count;
};

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

// A tracked actor's pose is replicated only while somebody is near it, and
// not at all beyond the spawn radius:
//
//   within VIEW_RADIUS of any viewer   - spawn and state every ACTOR_REPL_HZ
//   within SPAWN_RADIUS                 - spawn only, no state
//   beyond that                         - nothing, as if it did not exist
//
// "Viewer" is any racer on the host, not just the local player: a remote
// racer being chased is as relevant to look at as one's own car, and every
// client is drawing all of them from the same snapshots anyway.
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

// Spawn positions are the one place a whole world coordinate is signed rather
// than folded: a projectile is born wherever the caster is, which can be
// outside the folded range for a moment before the world wraps it. Signed
// shorts at this scale cover +-8192, far past any track, and the residual
// error is a quarter of a unit - well under the size of the body itself.
const int NET_Q_SPAWN_POS	= 4;

#pragma pack(pop)

#endif /* __NET_PROTOCOL_H__ */