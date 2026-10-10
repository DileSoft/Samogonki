#ifndef __NET_ACTOR_H__
#define __NET_ACTOR_H__

/*
	Class C entity replication: tracked actors, wire side.

	Same shape as net_spawn.h and for the same reason - pure arithmetic on
	the wire structs, covered by the transport self test before a single mesh
	is loaded, and net_fx.cpp is the only part that touches the engine.

	A tracked actor is replicated by state, not by birth. It steers itself
	toward players whose position the client only knows approximately, so
	letting the client's own copy run free would send it wandering off toward
	somewhere no racer ever was. The host owns the pose and pushes it, at
	NET_ACTOR_REPL_HZ, and only to viewers who are near enough for the drift
	to matter.

	Positions use the same signed fixed point as a spawn. The reasoning in
	net_spawn.h applies here too: an actor sits wherever the track put it,
	which is not guaranteed to be inside the folded range at the instant it
	is measured, and a wrapped coordinate would teleport it across the map.
*/

#include "net_spawn.h"

#include <cstring>

inline signed short net_q_actor_pos(float p) { return net_q_spawn_pos(p); }
inline float net_uq_actor_pos(signed short p) { return net_uq_spawn_pos(p); }

/*
	Encode a state batch. Returns bytes written, or 0 if it would not fit in
	one datagram. Refused rather than fragmented, like a snapshot: a state that
	arrives late is already wrong, and splitting it only delays the stale one.
*/
inline int net_actor_pack(unsigned char* buf,int buf_size,unsigned int server_tick,
			  const netActorState* actors,int count)
{
	if(!buf || !actors || count <= 0 || count > NET_MAX_ACTORS_PER_PACKET) return 0;

	int need = (int)sizeof(netActorHeader) + count * (int)sizeof(netActorState);

	if(need > buf_size || need > NET_MAX_PAYLOAD) return 0;

	netActorHeader* hdr = (netActorHeader*)buf;

	hdr -> server_tick = server_tick;
	hdr -> count = (unsigned char)count;

	memcpy(buf + sizeof(netActorHeader),(const unsigned char*)actors,
	       count * sizeof(netActorState));

	return need;
}

/*
	Decode. A count that runs past the payload is refused rather than clamped
	down to what fits: a half-written actor is worse than no actor, and the
	next batch is already on its way.
*/
inline int net_actor_unpack(netActorHeader* hdr,netActorState* out,int capacity,
			    const unsigned char* buf,int buf_size)
{
	if(!hdr || !out || !buf || buf_size < (int)sizeof(netActorHeader)) return 0;

	int count = ((netActorHeader*)buf) -> count;
	int need = (int)sizeof(netActorHeader) + count * (int)sizeof(netActorState);

	if(count <= 0 || count > NET_MAX_ACTORS_PER_PACKET || need > buf_size) return 0;

	*hdr = *(netActorHeader*)buf;

	if(count > capacity) count = capacity;

	memcpy((unsigned char*)out,buf + sizeof(netActorHeader),count * sizeof(netActorState));

	return count;
}

#endif /* __NET_ACTOR_H__ */
