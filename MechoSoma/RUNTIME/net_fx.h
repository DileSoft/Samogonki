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

#endif /* __NET_FX_H__ */
