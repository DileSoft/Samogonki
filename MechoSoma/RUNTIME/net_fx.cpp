#include "StdAfx.h"

// net_transport.h holds a netSocket by value without including its definition,
// so the socket header comes first. See the same note in net_game.cpp.
#include "net_socket.h"
#include "net_fx.h"
#include "net_transport.h"
#include "net_spawn.h"

#include "mechosoma.h"
#include "BodyDispatcher.h"
#include "Meteorite.h"
#include "fxlabGeneral.h"

// fxlabBodyInterface.h takes a fxlabProtectionType as a parameter of the one
// template this file never instantiates, so a forward declaration is enough
// and saves pulling in the whole server-side header chain just for a name.
struct fxlabProtectionType;

#include "fxlabBodyInterface.h"

#include <cstring>

/*
	Class B entities. See net_fx.h for why this shape and for what is left out.
*/

struct netFxEntry
{
	unsigned short entity_id;
	int body_id;
	unsigned char type;
};

static netFxEntry net_fx_table[NET_MAX_ENTITIES];
static int net_fx_count = 0;
static unsigned short net_fx_next_id = 1;

static netSession* net_fx_sess = NULL;

static netSpawn net_fx_spawn_q[NET_MAX_SPAWN_PER_PACKET];
static int net_fx_spawn_q_count = 0;
static netDespawn net_fx_despawn_q[NET_MAX_DESPAWN_PER_PACKET];
static int net_fx_despawn_q_count = 0;

void net_fx_set_session(netSession* sess)
{
	net_fx_sess = sess;
}

void net_fx_reset(void)
{
	net_fx_count = 0;
	net_fx_next_id = 1;
	net_fx_spawn_q_count = 0;
	net_fx_despawn_q_count = 0;
}

/*
	Which bodies travel. The projectile classes the arcanes actually fire are
	checked one by one rather than by testing for Meteorite, because the race
	also puts things on the track that derive from it and are already present
	on every machine from the track load: replicating those would give every
	client a second copy of a static bonus box.

	These classes are siblings, not ancestors of one another, so the order of
	the tests means nothing; what matters is that each test is a real type.
*/
static int net_fx_type_of(const Body* b)
{
	if(!b) return 0;

	if(dynamic_cast<const fxlabCannonBomb*>(b)) return NET_ENT_CANNON_BOMB;
	if(dynamic_cast<const fxlabVolcanicBomb*>(b)) return NET_ENT_VOLCANIC_BOMB;
	if(dynamic_cast<const fxlabFireBolide*>(b)) return NET_ENT_FIRE_BOLIDE;
	if(dynamic_cast<const fxlabSnowBall*>(b)) return NET_ENT_SNOW_BALL;
	if(dynamic_cast<const fxlabSleepyMortar*>(b)) return NET_ENT_SLEEPY_MORTAR;

	// Bonus meteorites thrown off by an explosion rather than placed on the
	// track. They are scenery, and scenery that only exists on the machine
	// that fired the arcane reads as a bug.
	if(dynamic_cast<const fxlabMinorBonus*>(b)) return NET_ENT_MINOR_BONUS;
	if(dynamic_cast<const fxlabMajorBonus*>(b)) return NET_ENT_MAJOR_BONUS;

	return 0;
}

static int net_fx_register(unsigned short entity_id,int body_id,unsigned char type)
{
	netFxEntry* e;

	if(net_fx_count >= NET_MAX_ENTITIES) return 0;

	e = &net_fx_table[net_fx_count];
	e -> entity_id = entity_id;
	e -> body_id = body_id;
	e -> type = type;

	net_fx_count ++;

	return 1;
}

// Returns the index of the entry, or -1. The index rather than a pointer
// because callers remove entries while they are walking the result.
static int net_fx_find(unsigned short entity_id)
{
	for(int i = 0; i < net_fx_count; i ++)
		if(net_fx_table[i].entity_id == entity_id)
			return i;

	return -1;
}

static unsigned short net_fx_new_id(void)
{
	unsigned short id = net_fx_next_id;

	// Zero is not an id: an unset netSpawn reads as zero, and a body that
	// never got a slot must not be confusable with the first body of a match.
	net_fx_next_id ++;
	if(net_fx_next_id == 0) net_fx_next_id = 1;

	return id;
}

/* ------------------------------- host side ------------------------------- */

static void net_fx_flush_spawn(void)
{
	unsigned char buf[NET_MAX_PAYLOAD];
	int bytes;

	if(!net_fx_sess || !net_fx_spawn_q_count){
		net_fx_spawn_q_count = 0;
		return;
		}

	bytes = net_spawn_pack(buf,sizeof(buf),net_fx_sess -> tick(),
				net_fx_spawn_q,net_fx_spawn_q_count);

	// Reliable on purpose. A dropped snapshot costs a frame of interpolation;
	// a dropped spawn is a hole in the world that never fills, because the
	// client has no other reason to build that body.
	if(bytes > 0)
		net_fx_sess -> broadcast(NET_MSG_SPAWN,buf,(unsigned int)bytes,1);

	net_fx_spawn_q_count = 0;
}

static void net_fx_queue_spawn(const netFxEntry* e,Body* b)
{
	netSpawn* s;

	if(!net_fx_sess) return;

	// A burst larger than one batch goes out as one batch rather than not at
	// all: send what fits, keep the rest for the next tick.
	if(net_fx_spawn_q_count >= NET_MAX_SPAWN_PER_PACKET)
		net_fx_flush_spawn();

	if(net_fx_spawn_q_count >= NET_MAX_SPAWN_PER_PACKET) return;

	s = &net_fx_spawn_q[net_fx_spawn_q_count ++];

	memset(s,0,sizeof(netSpawn));

	s -> entity_id = e -> entity_id;
	s -> type = e -> type;

	// The engine tracks ownership by body id and the wire by slot, and only
	// racers own anything. 255 is "nobody", which is what -1 says on the wire.
	s -> owner_slot = 255;

	const Vect3f& p = b -> R();
	const Vect3f& v = b -> Vglobal();

	s -> pos_x = net_q_spawn_pos(p.x);
	s -> pos_y = net_q_spawn_pos(p.y);
	s -> pos_z = net_q_spawn_pos(p.z);
	s -> vel_x = net_q_signed_speed(v.x);
	s -> vel_y = net_q_signed_speed(v.y);
	s -> vel_z = net_q_signed_speed(v.z);

	// Recovered from the mesh the body already loaded, which is the value the
	// body was constructed with. This is the one number that cannot be
	// derived locally, and the reason the field exists.
	s -> model_id = (unsigned short)(b -> geometry -> Type);

	// Reserved. The host owns lifetimes and every class has its own idea of
	// what one is, so a number here would be a promise the client could not
	// keep.
	s -> lifetime_ms = 0;

	s -> damage = (unsigned char)b -> Energy();
	s -> flags = 0;
}

static void net_fx_flush_despawn(void)
{
	unsigned char buf[NET_MAX_PAYLOAD];
	int bytes;

	if(!net_fx_sess || !net_fx_despawn_q_count){
		net_fx_despawn_q_count = 0;
		return;
		}

	bytes = net_despawn_pack(buf,sizeof(buf),net_fx_despawn_q,net_fx_despawn_q_count);

	if(bytes > 0)
		net_fx_sess -> broadcast(NET_MSG_DESPAWN,buf,(unsigned int)bytes,1);

	net_fx_despawn_q_count = 0;
}

static void net_fx_queue_despawn(unsigned short entity_id)
{
	netDespawn* d;

	if(!net_fx_sess) return;

	if(net_fx_despawn_q_count >= NET_MAX_DESPAWN_PER_PACKET)
		net_fx_flush_despawn();

	if(net_fx_despawn_q_count >= NET_MAX_DESPAWN_PER_PACKET) return;

	d = &net_fx_despawn_q[net_fx_despawn_q_count ++];

	d -> entity_id = entity_id;

	// Not derived. Telling a hit from an expiry would mean a hook in every
	// class's kill(), for a number nothing reads yet.
	d -> reason = NET_DESPAWN_EXPIRED;
}

void net_fx_attach(Body* body)
{
	int type;

	// Host only, and only in a net game. Both checks matter: the second keeps
	// the single player path untouched, and the first is what stops a body the
	// client itself built from a spawn from being queued as a spawn again.
	if(!mchNetGame || !mchNetHost) return;
	if(!body || !body -> geometry) return;
	if(net_fx_count >= NET_MAX_ENTITIES) return;

	type = net_fx_type_of(body);
	if(!type) return;

	// Nothing to rebuild from. A body with no mesh is not drawable anyway, so
	// skipping it costs nothing.
	if(!body -> geometry -> Type) return;

	if(!net_fx_register(net_fx_new_id(),body -> ID,(unsigned char)type)) return;

	net_fx_queue_spawn(&net_fx_table[net_fx_count - 1],body);
}

/* ------------------------------ client side ------------------------------ */

static void net_fx_create(const netSpawn* s)
{
	Body* p = NULL;
	Vect3f pos(net_uq_spawn_pos(s -> pos_x),
		   net_uq_spawn_pos(s -> pos_y),
		   net_uq_spawn_pos(s -> pos_z));
	Vect3f vel(net_uq_signed_speed(s -> vel_x),
		   net_uq_signed_speed(s -> vel_y),
		   net_uq_signed_speed(s -> vel_z));
	float damage = (float)s -> damage;
	int model = s -> model_id;

	// A spawn with no mesh cannot be rebuilt, and a spawn naming some future
	// body this build has never heard of must not stop the rest of the batch:
	// the next one may well be real.
	if(!model) return;

	switch(s -> type){
		case NET_ENT_CANNON_BOMB:	p = new fxlabCannonBomb(model,pos,vel,damage,-1); break;
		case NET_ENT_VOLCANIC_BOMB:	p = new fxlabVolcanicBomb(model,pos,vel,damage,-1); break;
		case NET_ENT_FIRE_BOLIDE:	p = new fxlabFireBolide(model,pos,vel,damage,-1); break;
		case NET_ENT_SNOW_BALL:	p = new fxlabSnowBall(model,pos,vel,damage,-1); break;
		case NET_ENT_SLEEPY_MORTAR:	p = new fxlabSleepyMortar(model,pos,vel,damage,-1); break;
		case NET_ENT_MINOR_BONUS:	p = new fxlabMinorBonus(model,pos,vel,damage,-1); break;
		case NET_ENT_MAJOR_BONUS:	p = new fxlabMajorBonus(model,pos,vel,damage,-1); break;
		default: return;
		}

	if(!p) return;

	// Register first: if the table is full the body is never tracked and would
	// then never be told to die, so it is not built at all. A missing
	// projectile is recoverable, an immortal one is not.
	if(!Mdisp || !net_fx_register(s -> entity_id,p -> ID,s -> type)){
		delete p;
		return;
		}

	Mdisp -> attachBody(p);
}

void net_fx_spawn_recv(const unsigned char* data,unsigned int len)
{
	netSpawnHeader hdr;
	netSpawn batch[NET_MAX_SPAWN_PER_PACKET];
	int count = net_spawn_unpack(&hdr,batch,NET_MAX_SPAWN_PER_PACKET,data,(int)len);

	for(int i = 0; i < count; i ++)
		net_fx_create(&batch[i]);
}

void net_fx_despawn_recv(const unsigned char* data,unsigned int len)
{
	netDespawn batch[NET_MAX_DESPAWN_PER_PACKET];
	int count = net_despawn_unpack(batch,NET_MAX_DESPAWN_PER_PACKET,data,(int)len);

	for(int i = 0; i < count; i ++){
		int at = net_fx_find(batch[i].entity_id);
		Body* b;

		// A despawn for something this machine never had. It happens whenever
		// a spawn was lost or arrived after the body it named was already
		// gone, and it is not an error.
		if(at < 0) continue;

		b = Mdisp -> searchBody(net_fx_table[at].body_id);

		// kill(), not deleteBody(). Letting the engine's own pre_quant drop
		// the body is the same path every other body in the game takes, and
		// it means the class's kill() runs and the projectile explodes where
		// it was when it died rather than simply blinking out.
		if(b && b -> alive())
			b -> kill();

		// The entry goes now rather than on the next liveness pass: the body
		// is dead as of this message, and leaving it would let a second
		// despawn for the same id match it.
		net_fx_table[at] = net_fx_table[net_fx_count - 1];
		net_fx_count --;
		}
}

/* --------------------------------- both ---------------------------------- */

void net_fx_quant(void)
{
	int i;

	if(!mchNetGame){
		net_fx_spawn_q_count = 0;
		net_fx_despawn_q_count = 0;
		return;
		}

	if(mchNetHost)
		net_fx_flush_spawn();

	/*
		Liveness. A body is finished when it is no longer in the world, or when
		the engine has killed it and the dispatcher has not yet got round to
		dropping it from its list. Either way it will not be drawn again, so
		the entry that tracked it is done.

		Looked up by body id rather than kept as a pointer: total_bodies holds
		bodies by handle, and a raw pointer into a list that gets rewritten
		under this loop would be a way to shoot the process.
	*/
	i = 0;
	while(i < net_fx_count){
		netFxEntry* e = &net_fx_table[i];
		Body* b = Mdisp ? Mdisp -> searchBody(e -> body_id) : NULL;

		if(b && b -> alive()){
			i ++;
			continue;
			}

		if(mchNetHost)
			net_fx_queue_despawn(e -> entity_id);

		// Swap in the last entry. The table is not ordered, and nothing about
		// the order entries come out in is meaningful.
		net_fx_table[i] = net_fx_table[net_fx_count - 1];
		net_fx_count --;
		}

	if(mchNetHost)
		net_fx_flush_despawn();
}
