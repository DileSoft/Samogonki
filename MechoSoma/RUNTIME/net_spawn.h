#ifndef __NET_SPAWN_H__
#define __NET_SPAWN_H__

/*
	Class B entity replication: ballistic bodies, wire side.

	Split the same way net_snapshot.h is. Everything in this file is pure
	arithmetic on the wire structs, so the transport self test covers it
	before a single mesh is loaded, and net_fx.cpp is the only part that
	touches the engine.

	A projectile is replicated by its birth, not by its flight. The host
	sends where and how fast it started, every client builds the same body
	from those initial conditions, and the shared Body physics carries it
	from there. Over a two or three second flight the drift is centimetres
	and invisible, and it buys not sending anything at all for the rest of
	the body's life.

	model_id is the reason this is worth doing at all: fxlabCreateBolide
	rolls the mesh as model + fxlabServerRND.RND(noise), so the host's
	draw is the only one that knows which model a body got. Everything else
	about a spawn is exactly what the engine already had in hand.
*/

#include "net_protocol.h"

#include <cstring>

/* Positions are signed fixed point rather than the folded form used for
   racers: a projectile is born wherever the caster happens to be, and the
   world only wraps it on the following step. */
inline signed short net_q_spawn_pos(float p)
{
	float scaled = p * (float)NET_Q_SPAWN_POS;

	if(scaled > 32767.0f) scaled = 32767.0f;
	if(scaled < -32768.0f) scaled = -32768.0f;

	return (signed short)(scaled < 0 ? scaled - 0.5f : scaled + 0.5f);
}

inline float net_uq_spawn_pos(signed short p)
{
	return (float)p / (float)NET_Q_SPAWN_POS;
}

inline signed short net_q_signed_speed(float v)
{
	float scaled = v * (float)NET_Q_SPEED_XY;

	if(scaled > 32767.0f) scaled = 32767.0f;
	if(scaled < -32768.0f) scaled = -32768.0f;

	return (signed short)(scaled < 0 ? scaled - 0.5f : scaled + 0.5f);
}

inline float net_uq_signed_speed(signed short v)
{
	return (float)v / (float)NET_Q_SPEED_XY;
}

/*
	Encode a spawn batch. Returns bytes written, or 0 if it would not fit in
	one datagram. Refused rather than fragmented for the same reason as
	snapshots: a projectile that arrives late has already hit something.

	an owner_slot of 255 means nobody owns it, which is what the engine's own
	-1 owner would say on the wire.
*/
inline int net_spawn_pack(unsigned char* buf,int buf_size,unsigned int server_tick,
			  const netSpawn* ents,int count)
{
	if(!buf || !ents || count <= 0 || count > NET_MAX_SPAWN_PER_PACKET) return 0;

	int need = (int)sizeof(netSpawnHeader) + count * (int)sizeof(netSpawn);

	if(need > buf_size || need > NET_MAX_PAYLOAD) return 0;

	netSpawnHeader* hdr = (netSpawnHeader*)buf;

	hdr -> server_tick = server_tick;
	hdr -> count = (unsigned char)count;

	memcpy(buf + sizeof(netSpawnHeader),(const unsigned char*)ents,
	       count * sizeof(netSpawn));

	return need;
}

/*
	Decode a spawn batch. A claimed count past the payload length is refused
	rather than clamped down: a truncated spawn is half an entity, and half
	an entity is worse than none.
*/
inline int net_spawn_unpack(netSpawnHeader* hdr,netSpawn* out,int capacity,
			    const unsigned char* buf,int buf_size)
{
	if(!hdr || !out || !buf || buf_size < (int)sizeof(netSpawnHeader)) return 0;

	int count = ((netSpawnHeader*)buf) -> count;
	int need = (int)sizeof(netSpawnHeader) + count * (int)sizeof(netSpawn);

	if(count <= 0 || count > NET_MAX_SPAWN_PER_PACKET || need > buf_size) return 0;

	*hdr = *(netSpawnHeader*)buf;

	if(count > capacity) count = capacity;

	memcpy((unsigned char*)out,buf + sizeof(netSpawnHeader),count * sizeof(netSpawn));

	return count;
}

/*
	Encode a despawn batch. Count is the only thing a receiver needs to know
	how many ids follow, so there is no header beyond it.
*/
inline int net_despawn_pack(unsigned char* buf,int buf_size,
			    const netDespawn* ents,int count)
{
	if(!buf || !ents || count <= 0 || count > NET_MAX_DESPAWN_PER_PACKET) return 0;

	int need = (int)sizeof(netDespawnHeader) + count * (int)sizeof(netDespawn);

	if(need > buf_size || need > NET_MAX_PAYLOAD) return 0;

	netDespawnHeader* hdr = (netDespawnHeader*)buf;

	hdr -> count = (unsigned char)count;

	memcpy(buf + sizeof(netDespawnHeader),(const unsigned char*)ents,
	       count * sizeof(netDespawn));

	return need;
}

/*
	Decode a despawn batch. Returns an id count of at most capacity; an
	unknown id is silently dropped by the receiver rather than an error,
	because a body can die on the host before its despawn was ever read.
*/
inline int net_despawn_unpack(netDespawn* out,int capacity,
			      const unsigned char* buf,int buf_size)
{
	if(!out || !buf || buf_size < (int)sizeof(netDespawnHeader)) return 0;

	int count = ((netDespawnHeader*)buf) -> count;
	int need = (int)sizeof(netDespawnHeader) + count * (int)sizeof(netDespawn);

	if(count <= 0 || count > NET_MAX_DESPAWN_PER_PACKET || need > buf_size) return 0;

	if(count > capacity) count = capacity;

	memcpy((unsigned char*)out,buf + sizeof(netDespawnHeader),count * sizeof(netDespawn));

	return count;
}

#endif /* __NET_SPAWN_H__ */
