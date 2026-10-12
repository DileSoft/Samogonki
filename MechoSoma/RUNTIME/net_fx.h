#ifndef __NET_FX_H__
#define __NET_FX_H__

/*
	Class B entity replication: ballistic bodies.

	A projectile is sent once, at birth. The host says which mesh it got, where
	it started and how fast it was going; every client builds the same body
	from those numbers and the shared Body physics carries it from there.
	Nothing is sent again until it dies.

	That is a trade and it is the right one here. Over a two or three second
	flight the drift between two machines running the same integrator is
	centimetres, which nobody can see, and the price of not making it is one
	datagram per projectile every tick for every projectile alive. What it
	does cost is that the mesh has to be named on the wire, because the host
	rolls it itself: fxlabCreateBolide picks model + fxlabServerRND.RND(noise)
	and no other machine has any way to reproduce that draw.

	The other half of the cost is despawns. A body that dies on the host has to
	be told to die everywhere, and it is told on the next tick rather than the
	same frame, so what a client sees is a projectile that vanishes up to one
	tick late. At twenty ticks a second that is not visible, and it is far
	cheaper than making the death itself reliable.

	Not covered, and deliberately: tracked actors (dragons, bonus men,
	butterflies) steer themselves toward players whose position the client
	only knows approximately, so their local simulation cannot work. They are
	class C and get their state replicated instead, which is a later part.
*/

#include "net_socket.h"

#include <cstdio>

class Body;
class netSession;

// Set by the game layer once the transport exists, cleared when it goes away.
void net_fx_set_session(netSession* sess);

// Once per frame, from the game layer. Host side this is what actually puts
// the queued spawns and despawns on the wire, and both sides use it to notice
// bodies that have died.
void net_fx_quant(void);

// Drops every tracked entity. Called when a match ends, because body ids
// start again from zero for the next race and an old table would then point
// at somebody else's projectile.
void net_fx_reset(void);

// Called from BodyDispatcher::attachBody. Host only: on a client this returns
// immediately, which is also what stops a body built from a spawn from being
// queued as a spawn of its own.
void net_fx_attach(Body* body);

/*
	Client side: should the world's own walking actors be built here at all?

	Called from BodyDispatcher::createBody with the factory name. During a net
	race the client builds none of them - the host owns every bonus man and
	creature and replicates it as class C - so this returns 1 for an actor name
	and drops the local one before it is made. Without it the client would put
	its own random set on the track next to the host's, twice the creatures in
	wrong places. Returns 0 for anything that is not an actor, and for the one
	createBody a class C spawn itself makes (see net_fx_create).
*/
int net_world_block_actor(const char* factory_name);

/*
	Host side: the next body attached is born with this client-side visual, an
	FXLAB_CLIENT_PROCESS_* type, or -1 for none. The engine builds some bodies
	and their visual together (fxlabCreateBolide); the body travels by mesh and
	the visual is rebuilt next to it on the client. Cleared on the next attach.
*/
void net_fx_set_pending_link(int type);

/*
	Host side: the next body attached is one the engine builds by factory name
	(Mdisp->createBody) rather than by mesh, and is class B all the same - the
	client rebuilds it from the name and simulates it. Call it right before the
	createBody that makes the body; cleared on the next attach. Pass the same
	name given to createBody.
*/
void net_fx_set_pending_body(const char* factory_name);

// A batch arrived. Raw payload including the batch header.
void net_fx_spawn_recv(const unsigned char* data,unsigned int len);
void net_fx_despawn_recv(const unsigned char* data,unsigned int len);
void net_fx_actor_recv(const unsigned char* data,unsigned int len);

/*
	How many entities are tracked right now, split by class.

	Not for the game to use - for the log. A live race is the only place the
	registry, the classification and the factory lookup have ever run, and
	"nothing appeared" is otherwise indistinguishable from "nothing was ever
	sent".
*/
void net_fx_stats(int* projectiles,int* actors);

/*
	-netdump: write one CSV line per tracked body, E kind, tagged with the host
	tick. Host side these are the authoritative poses, client side the locally
	simulated ones, and the entity_id in the key column is the same on both, so
	a host trace and a client trace line up. `f` is the trace file, or NULL.
*/
void net_fx_trace(FILE* f,unsigned int tick);

/*
	Host side: replicate the tracked actors' poses once, throttled internally
	to NET_ACTOR_REPL_HZ. Driven from the game layer and only while the match
	is running, so no actor is ever marked spawned before a client is there to
	receive it.
*/
void net_fx_update_actors(void);

// Host side: clear the "already spawned" flag on every actor so the next
// update pass sends them all again. For a player who joins mid-race.
void net_fx_resend_actors(void);

#endif /* __NET_FX_H__ */
