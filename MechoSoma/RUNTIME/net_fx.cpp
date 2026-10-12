#include "StdAfx.h"

// net_transport.h holds a netSocket by value without including its definition,
// so the socket header comes first. See the same note in net_game.cpp.
#include "net_socket.h"
#include "net_fx.h"
#include "net_transport.h"
#include "net_spawn.h"
#include "net_actor.h"
#include "net_snapshot.h"

#include "mechosoma.h"
#include "BodyDispatcher.h"
#include "Meteorite.h"
#include "fxlabGeneral.h"

// The bolide classes, and the factory that builds them.
//
// Only the classes are wanted here, so the header is fxlabBolide.h:
// fxlabBodyInterface.h carries the factory templates, and those want the whole
// server-side chain laid down first for the one parameter type they take.
#include "fxlabBolide.h"

// Actor headers. These are the only two that need dynamic_cast to name a
// variant: all four BonusMan subclasses set type_ to BONUS_MAN, and
// WickedButterfly inherits Butterfly's, so body->type() cannot separate them.
//
// The order below is the order BodyFactory.cpp uses, because these headers
// are not self sufficient: each one assumes the previous ones have laid down
// Body, ControlledObject and PointControlledObject.
#include "Body.h"
#include "ControlledObject.h"
#include "PointControlledObject.h"
#include "CrawlingObject.h"
#include "FlyingObject.h"
#include "Butterfly.h"
#include "BonusMan.h"
#include "SmartMissile.h"
#include "TitaniumBall.h"

#include "race.h"

#include <cstring>
#include <cmath>

/*
	Class B entities. See net_fx.h for why this shape and for what is left out.
*/

struct netFxEntry
{
	unsigned short entity_id;
	int body_id;
	unsigned char type;
	unsigned char variant;

	// Class C only: created from a spawn by name rather than by mesh, so the
	// registry has to remember which one to ask the factory for.
	unsigned char is_actor;

	// Actor only: whether a spawn has already gone out, and when the last
	// state batch was. A state batch is throttled per actor, because a bonus
	// man that just came into view does not need to be sent at the same
	// moment as every other one that was already visible.
	unsigned char spawn_sent;
	unsigned int last_state_ms;
};

static netFxEntry net_fx_table[NET_MAX_ENTITIES];
static int net_fx_count = 0;
static unsigned short net_fx_next_id = 1;

static netSession* net_fx_sess = NULL;

static netSpawn net_fx_spawn_q[NET_MAX_SPAWN_PER_PACKET];
static int net_fx_spawn_q_count = 0;
static netDespawn net_fx_despawn_q[NET_MAX_DESPAWN_PER_PACKET];
static int net_fx_despawn_q_count = 0;
static netActorState net_fx_actor_q[NET_MAX_ACTORS_PER_PACKET];
static int net_fx_actor_q_count = 0;

// State that arrived before the spawn it belongs to. Declared here rather
// than next to the rest of the receive path because reset has to clear it.
static netActorState net_fx_actor_pending[NET_MAX_ACTORS_PER_PACKET];
static int net_fx_actor_pending_count = 0;
static unsigned int net_fx_actor_ms = 0;

// The visual the next attached body is born with, or -1. See the header.
static int net_fx_pending_link = -1;

// The type of the next factory-built body, or 0. See net_fx_set_pending_body.
static int net_fx_pending_type = 0;

// Set while net_fx_create is building a class C actor from a spawn, so that
// the client-side suppression in net_world_block_actor lets that one through.
// Every other createBody on a client in a net race is a world actor and is
// dropped.
static int net_fx_spawn_building = 0;

static void net_fx_flush_spawn(void);
static void net_fx_queue_spawn(const netFxEntry* e,Body* b,int client_link);
static void net_fx_pending_drain(void);
static int net_fx_body_type(const char* name);

void net_fx_set_session(netSession* sess)
{
	net_fx_sess = sess;
}

void net_fx_set_pending_link(int type)
{
	net_fx_pending_link = type;
}

void net_fx_set_pending_body(const char* factory_name)
{
	net_fx_pending_type = net_fx_body_type(factory_name);
}

void net_fx_reset(void)
{
	net_fx_count = 0;
	net_fx_next_id = 1;
	net_fx_spawn_q_count = 0;
	net_fx_despawn_q_count = 0;
	net_fx_actor_q_count = 0;
	net_fx_actor_pending_count = 0;
	net_fx_actor_ms = 0;
	net_fx_pending_link = -1;
	net_fx_pending_type = 0;
	net_fx_spawn_building = 0;
}

/*
	Which bodies travel.

	Class B is told apart by class identity, because every projectile class is
	a distinct C++ type and testing for Meteorite would be too broad: the race
	also puts things on the track that derive from it and are already present
	on every machine from the track load, and replicating those would give
	every client a second copy of a static bonus box.

	Class C is told apart by body->type(), the engine's own discriminator,
	with two exceptions that need a real type: all four BonusMan subclasses
	set type_ to BONUS_MAN, and WickedButterfly inherits Butterfly's. Both are
	resolved with dynamic_cast against BonusMan.h and Butterfly.h, which are
	the only two headers this file needed beyond Body for the purpose.

	The class B subclasses are siblings rather than ancestors of one another,
	so the order of those tests means nothing; what matters is that each is a
	real type.
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

	// The three balls a completed mechos throws. Ballistic, fixed mesh, no
	// random model - built with new rather than by factory name, so this is
	// where they are told apart.
	if(dynamic_cast<const TitaniumBall*>(b)) return NET_ENT_TITANIUM_BALL;

	return 0;
}

/*
	Which tracked actors travel, and what they are called.

	variant separates subclasses of one type. The engine's factory is keyed by
	name and BonusMan is four separate classes sharing one type, so this is
	also the table that says which name to hand the factory on the client:
	one table for both directions, which is why they cannot drift apart.
*/
struct netFxActorDef
{
	int type;
	int variant;
	const char* name;		// BodyDispatcher::createBody factory name
};

static const netFxActorDef net_fx_actor_defs[] =
{
	{ NET_ENT_BONUS_MAN, 1, "BonusMan1" },
	{ NET_ENT_BONUS_MAN, 2, "BonusMan2" },
	{ NET_ENT_BONUS_MAN, 3, "BonusMan3" },
	{ NET_ENT_BONUS_MAN, 4, "BonusMan4" },

	{ NET_ENT_BUTTERFLY, 0, "Butterfly" },
	{ NET_ENT_WICKED_BUTTERFLY, 0, "WickedButterfly" },
	{ NET_ENT_CATERPILLAR, 0, "Caterpillar" },
	{ NET_ENT_WORM, 0, "Worm" },
	{ NET_ENT_DRAGON, 0, "Dragon" },
	{ NET_ENT_FROG, 0, "Frog" },
	{ NET_ENT_SHARK, 0, "Shark" },
	{ NET_ENT_FISH, 0, "Fish" },

	{ 0, 0, NULL }
};

static const char* net_fx_actor_name(int type,int variant)
{
	for(int i = 0; net_fx_actor_defs[i].name; i ++)
		if(net_fx_actor_defs[i].type == type && net_fx_actor_defs[i].variant == variant)
			return net_fx_actor_defs[i].name;

	return NULL;
}

/*
	Client side: suppress the local build of a world actor.

	Only a client in a net race suppresses, and only actor names - everything
	else (projectiles, parts, scenery) is built as usual. net_fx_create clears
	the check for the single createBody it makes for a replicated actor.
*/
int net_world_block_actor(const char* factory_name)
{
	if(!mchNetGame || mchNetHost) return 0;
	if(net_fx_spawn_building) return 0;
	if(!factory_name) return 0;

	for(int i = 0; net_fx_actor_defs[i].name; i ++)
		if(!strcmp(net_fx_actor_defs[i].name,factory_name))
			return 1;

	return 0;
}

/*
	The factory-built class B bodies: the arcanes' "smart" ones, which the
	engine makes by name (Mdisp->createBody) rather than by mesh. One table for
	both directions - the host looks the type up by the name it is about to
	build, the client looks the name up by the type on the wire - so the two
	cannot drift apart.
*/
struct netFxBodyDef
{
	int type;
	const char* name;		// BodyDispatcher::createBody factory name
};

static const netFxBodyDef net_fx_body_defs[] =
{
	{ NET_ENT_SMART_FIREBALL,	"SmartFireBall" },
	{ NET_ENT_SMART_LIGHTNINGBOLT,	"SmartLightningBolt" },
	{ NET_ENT_SUICIDE,		"Suicide" },
	{ NET_ENT_CLOWN,		"Clown" },
	{ NET_ENT_EVIL_EYE,		"EvilEye" },
	{ NET_ENT_TOMATO,		"Tomato" },
	{ NET_ENT_CRYSTAL,		"Crystal" },
	{ NET_ENT_BUBBLE,		"Bubble" },
	{ NET_ENT_LITTLE_SPIDER,	"LittleSpider" },

	{ 0, NULL }
};

static int net_fx_body_type(const char* name)
{
	for(int i = 0; net_fx_body_defs[i].name; i ++)
		if(!strcmp(net_fx_body_defs[i].name,name))
			return net_fx_body_defs[i].type;

	return 0;
}

static const char* net_fx_body_name(int type)
{
	for(int i = 0; net_fx_body_defs[i].name; i ++)
		if(net_fx_body_defs[i].type == type)
			return net_fx_body_defs[i].name;

	return NULL;
}

// Class C, named from the def table above, for the registry and the log.
static int net_fx_is_actor(int type)
{
	return type >= NET_ENT_BONUS_MAN && type < NET_ENT_TRAP_SPHERE;
}

// Fills in type and variant. Returns 0 for anything not replicated.
static int net_fx_class_of(const Body* b,int* variant)
{
	int type = net_fx_type_of(b);

	*variant = 0;

	if(type) return type;

	switch(b -> type()){
		case Body::BONUS_MAN:
			// One type, four classes, one mesh kind each. The kinds are how
			// the engine's own config names them, so they are read rather
			// than invented.
			if(dynamic_cast<const BonusMan1*>(b)) { *variant = 1; return NET_ENT_BONUS_MAN; }
			if(dynamic_cast<const BonusMan2*>(b)) { *variant = 2; return NET_ENT_BONUS_MAN; }
			if(dynamic_cast<const BonusMan3*>(b)) { *variant = 3; return NET_ENT_BONUS_MAN; }
			if(dynamic_cast<const BonusMan4*>(b)) { *variant = 4; return NET_ENT_BONUS_MAN; }
			return 0;

		case Body::BUTTERFLY:
			if(dynamic_cast<const WickedButterfly*>(b)) return NET_ENT_WICKED_BUTTERFLY;
			return NET_ENT_BUTTERFLY;

		case Body::CATERPILLAR:	return NET_ENT_CATERPILLAR;
		case Body::WORM:		return NET_ENT_WORM;
		case Body::DRAGON:		return NET_ENT_DRAGON;
		case Body::FROG:		return NET_ENT_FROG;
		case Body::SHARK:		return NET_ENT_SHARK;
		case Body::FISH:		return NET_ENT_FISH;

		// Everything else that passes through attachBody - racers, parts,
		// stones, gears, debris, static bonuses - is already identical on
		// every machine, or is a class of its own, and is not touched.
		default: return 0;
		}
}

static int net_fx_register(unsigned short entity_id,int body_id,unsigned char type,
			   unsigned char variant,unsigned char is_actor)
{
	netFxEntry* e;

	if(net_fx_count >= NET_MAX_ENTITIES) return 0;

	e = &net_fx_table[net_fx_count];
	e -> entity_id = entity_id;
	e -> body_id = body_id;
	e -> type = type;
	e -> variant = variant;
	e -> spawn_sent = 0;
	e -> last_state_ms = 0;
	e -> is_actor = is_actor;

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

/*
	Distance to the nearest viewer, in the toroidal sense.

	The world wraps at NET_WORLD_SIZE, and the straight difference between two
	coordinates either side of the seam is nearly the size of the map. Folding
	the difference back is what makes an actor on the far edge count as being
	near a viewer on this one, which is the whole of interest management.
*/
static float net_fx_viewer_distance(const Vect3f& pos)
{
	float best = 1e30f;
	mchRacer* rp;

	if(!mch_raceD || !mch_raceD -> racerLst) return best;

	rp = mch_raceD -> racerLst -> first();

	while(rp){
		mchMechosRacer* mr = dynamic_cast<mchMechosRacer*>(rp);

		if(mr){
			const Vect3f& r = mr -> R();
			float dx = (float)fabs(r.x - pos.x);
			float dy = (float)fabs(r.y - pos.y);

			// Fold each axis into [0, half world]. Past the seam the short way
			// round is the one that matters.
			if(dx > (float)NET_WORLD_SIZE * 0.5f) dx = (float)NET_WORLD_SIZE - dx;
			if(dy > (float)NET_WORLD_SIZE * 0.5f) dy = (float)NET_WORLD_SIZE - dy;

			float d = dx*dx + dy*dy;

			if(d < best) best = d;
			}

		rp = rp -> next;
		}

	// Squared distance is kept through the comparison; the caller compares
	// against a squared radius for the same reason.
	return best;
}

static void net_fx_flush_actors(void)
{
	unsigned char buf[NET_MAX_PAYLOAD];
	int bytes;

	if(!net_fx_sess || !net_fx_actor_q_count){
		net_fx_actor_q_count = 0;
		return;
		}

	bytes = net_actor_pack(buf,sizeof(buf),net_fx_sess -> tick(),
				net_fx_actor_q,net_fx_actor_q_count);

	// Unreliable. A state batch that is a tick late is corrected by the next
	// one a hundred milliseconds later, whereas making them reliable would
	// delay every batch behind it in order to deliver a stale one on time.
	if(bytes > 0)
		net_fx_sess -> broadcast(NET_MSG_ACTOR_STATE,buf,(unsigned int)bytes,0);

	net_fx_actor_q_count = 0;
}

static void net_fx_queue_actor(const netFxEntry* e,const Body* b)
{
	netActorState* a;

	if(!net_fx_sess) return;

	if(net_fx_actor_q_count >= NET_MAX_ACTORS_PER_PACKET)
		net_fx_flush_actors();

	if(net_fx_actor_q_count >= NET_MAX_ACTORS_PER_PACKET) return;

	a = &net_fx_actor_q[net_fx_actor_q_count ++];

	memset(a,0,sizeof(netActorState));

	a -> entity_id = e -> entity_id;
	a -> type = e -> type;
	a -> pos_x = net_q_actor_pos(b -> R().x);
	a -> pos_y = net_q_actor_pos(b -> R().y);
	a -> pos_z = net_q_actor_pos(b -> R().z);
	a -> yaw = net_q_yaw(b -> psi());
	a -> vel_x = net_q_signed_speed(b -> Vglobal().x);
	a -> vel_y = net_q_signed_speed(b -> Vglobal().y);

	// Reserved. flags would carry the actor's internal state - paratrooper,
	// burning, beeped - and life_permille its remaining life, but both are
	// private to the class and neither is read by anything on a client.
	a -> flags = 0;
	a -> life_permille = 0;
}

/*
	Class C replication, once per tick.

	Three regimes, by distance to the nearest viewer: state inside the view
	radius, a bare spawn inside the spawn radius, and nothing beyond it. The
	spawn is sent once and then never again, so an actor that walks out of
	range and back is not respawned - it simply stops and starts moving.
*/
void net_fx_update_actors(void)
{
	unsigned int now = (unsigned int)clocki();

	if((int)(now - net_fx_actor_ms) < 1000 / NET_ACTOR_REPL_HZ) return;
	net_fx_actor_ms = now;

	for(int i = 0; i < net_fx_count; i ++){
		netFxEntry* e = &net_fx_table[i];
		Body* b;

		if(!e -> is_actor) continue;

		b = Mdisp ? Mdisp -> searchBody(e -> body_id) : NULL;

		// Liveness is handled by net_fx_quant's own pass; skipping here is
		// what keeps the two from desyncing on the same entry.
		if(!b || !b -> alive()) continue;

		float d2 = net_fx_viewer_distance(b -> R());

		if(d2 > (float)NET_ACTOR_SPAWN_RADIUS * (float)NET_ACTOR_SPAWN_RADIUS) continue;

		if(!e -> spawn_sent){
			net_fx_queue_spawn(e,b,-1);
			e -> spawn_sent = 1;
			}

		if(d2 > (float)NET_ACTOR_VIEW_RADIUS * (float)NET_ACTOR_VIEW_RADIUS) continue;

		if((int)(now - e -> last_state_ms) < 1000 / NET_ACTOR_REPL_HZ) continue;
		e -> last_state_ms = now;

		net_fx_queue_actor(e,b);
		}

	net_fx_flush_actors();
}

/*
	Host side: forget that the actors were already spawned, so the next
	net_fx_update_actors pass queues them again.

	Called when a player joins. Without it a client that connects after the
	host already sent the world's actors never receives them: each one is
	spawned once and then never again, and the first send went to nobody.
*/
void net_fx_resend_actors(void)
{
	for(int i = 0; i < net_fx_count; i ++)
		if(net_fx_table[i].is_actor)
			net_fx_table[i].spawn_sent = 0;
}

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

static void net_fx_queue_spawn(const netFxEntry* e,Body* b,int client_link)
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
	s -> variant = e -> variant;

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
	s -> model_id = (unsigned int)(b -> geometry -> Type);

	// Reserved. The host owns lifetimes and every class has its own idea of
	// what one is, so a number here would be a promise the client could not
	// keep.
	s -> lifetime_ms = 0;

	s -> damage = (unsigned char)b -> Energy();
	s -> flags = 0;

	// 0 means none; the type is offset by one because FXLAB_CLIENT_PROCESS_TWISTER
	// is 0 and would otherwise be indistinguishable from "no link".
	s -> client_link = (client_link < 0) ? 0 : (unsigned short)(client_link + 1);

	// The heading, for the bodies placed with one rather than left to physics.
	// The mesh-built ones ignore it.
	s -> yaw = net_q_yaw(b -> psi());
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
	int type,variant;

	// Taken and cleared first, so a link or type meant for one body can never
	// ride along with the next one if this body turns out not to be
	// replicated.
	int link = net_fx_pending_link;
	net_fx_pending_link = -1;
	int ptype = net_fx_pending_type;
	net_fx_pending_type = 0;

	// Host only, and only in a net game. Both checks matter: the second keeps
	// the single player path untouched, and the first is what stops a body the
	// client itself built from a spawn from being queued as a spawn again.
	if(!mchNetGame || !mchNetHost) return;
	if(!body || !body -> geometry) return;
	if(net_fx_count >= NET_MAX_ENTITIES) return;

	// Class C is told apart by body->type(), class B by class identity. Both
	// go through here, and one is a projectile the other is an actor, so the
	// split has to be made once rather than in two places.
	//
	// A body the arcane code announced with set_pending_body cannot be told
	// apart by identity - its class is not "smart" and its mesh says only what
	// it looks like - so the caller's word is taken over the table.
	variant = 0;
	if(ptype)
		type = ptype;
	else
		type = net_fx_class_of(body,&variant);
	if(!type) return;

	// Nothing to rebuild from. A body with no mesh is not drawable anyway, so
	// skipping it costs nothing.
	if(!body -> geometry -> Type) return;

	if(!net_fx_register(net_fx_new_id(),body -> ID,(unsigned char)type,(unsigned char)variant,
			    (unsigned char)net_fx_is_actor(type)))
		return;

	net_fx_queue_spawn(&net_fx_table[net_fx_count - 1],body,link);
}

/* ------------------------------ client side ------------------------------ */

// The visual the host built next to a class B body, if any. Bound to the body
// so it tracks the same physics and goes away with it.
static void net_fx_make_link(const netSpawn* s,Body* p,const Vect3f& pos,const Vect3f& vel)
{
	if(!s -> client_link) return;

	fxlabGeneralObjectType* ct = fxlabClientD->CreateObject((int)s -> client_link - 1);

	if(ct){
		ct -> SetStartTime(fxlabClientD->GetTime());
		ct -> SetPosition(pos);
		ct -> SetVelocity(vel);
		ct -> SetBody(p);
		ct -> Start();
		}
}

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
	int is_actor;
	const char* name;

	// Zero is not an id (see net_fx_new_id), so a spawn carrying it describes
	// nothing. A type this build has never heard of is dropped the same way:
	// its id then matches nothing here and the despawn is ignored as unknown.
	if(!s -> entity_id) return;

	is_actor = net_fx_is_actor(s -> type);
	name = is_actor ? net_fx_actor_name(s -> type,s -> variant) : NULL;

	if(is_actor ? !name : !model) return;
	if(!Mdisp) return;

	// Class B: built straight from the host's numbers and attached by hand,
	// because nothing else in the engine has heard of a network spawn.
	if(!is_actor){
		const char* bname = net_fx_body_name(s -> type);

		if(bname){
			// Factory-built class B: the arcane's "smart" bodies. The class
			// has no (model,pos,vel,...) constructor, so it is rebuilt the way
			// the engine builds it, name and heading and all. createBody
			// attaches it, so there is no attachBody below.
			float yaw = net_uq_yaw(s -> yaw);
			p = Mdisp -> createBody(bname,pos,(int)floorf(yaw * 180.0f / (float)M_PI + 0.5f));

			if(!p) return;

			net_fx_register(s -> entity_id,p -> ID,s -> type,s -> variant,0);

			// Homing. The host picks the target inside set_owner, keyed by the
			// owner's body id, which means nothing here. Point it at the
			// nearest racer from this machine's view - the two will not agree
			// exactly, but the missile turns the same way instead of flying
			// off straight.
			SmartMissile* sm = dynamic_cast<SmartMissile*>(p);
			if(sm) sm -> setTarget(Mdisp -> searchBody(p -> R(),Body::MECHOS));

			net_fx_make_link(s,p,pos,vel);
			return;
			}

		switch(s -> type){
			case NET_ENT_CANNON_BOMB:	p = new fxlabCannonBomb(model,pos,vel,damage,-1); break;
			case NET_ENT_VOLCANIC_BOMB:	p = new fxlabVolcanicBomb(model,pos,vel,damage,-1); break;
			case NET_ENT_FIRE_BOLIDE:	p = new fxlabFireBolide(model,pos,vel,damage,-1); break;
			case NET_ENT_SNOW_BALL:	p = new fxlabSnowBall(model,pos,vel,damage,-1); break;
			case NET_ENT_SLEEPY_MORTAR:	p = new fxlabSleepyMortar(model,pos,vel,damage,-1); break;
			case NET_ENT_MINOR_BONUS:	p = new fxlabMinorBonus(model,pos,vel,damage,-1); break;
			case NET_ENT_MAJOR_BONUS:	p = new fxlabMajorBonus(model,pos,vel,damage,-1); break;

			// The constructor turns its velocity argument into world units on
			// its own (V_*Gravity*0.1), so it is built at rest and handed the
			// host's velocity verbatim. Its spin is rolled from the local RNG
			// and will differ from the host's; that is cosmetic and the price
			// of not sending angular velocity.
			case NET_ENT_TITANIUM_BALL:
				p = new TitaniumBall(pos,Vect3f(0,0,0),damage,-1);
				if(p) p -> setGlobalVelocity(vel);
				break;

			default: return;
			}

		if(!p) return;

		net_fx_register(s -> entity_id,p -> ID,s -> type,s -> variant,0);
		Mdisp -> attachBody(p);

		net_fx_make_link(s,p,pos,vel);
		return;
		}

	/*
		Class C: built through the engine's own factory, by name. That is the
		whole point of the netFxActorDef table - the client never has to know
		what class a bonus man is, only what the factory calls it.

		createBody attaches the body itself, so there is no attach here and
		attaching again would put it in the world twice. The pose it is given
		is a placeholder: the first ACTOR_STATE batch arrives within one tick
		and replaces it, which is far less jarring than appearing at the
		origin.
	*/
	// This is the one createBody on a client that must go through: the actor
	// is the host's, arriving as a spawn, not a local world one to be dropped.
	int saved_building = net_fx_spawn_building;
	net_fx_spawn_building = 1;
	p = Mdisp -> createBody(name,pos,G2R(0));
	net_fx_spawn_building = saved_building;

	if(!p) return;

	// Registered after the body exists, because the id it has to remember is
	// the body's. If the table is full the entry is dropped and the body is
	// left untracked: it will then be despawned by the engine's own lifetime
	// rather than by a message, which is the one failure mode that still
	// cleans up after itself.
	net_fx_register(s -> entity_id,p -> ID,s -> type,s -> variant,1);

	// A state batch may have arrived while this spawn was in flight.
	net_fx_pending_drain();
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
		// it means the class's kill() runs: a projectile explodes where it
		// died, and a bonus man is picked up rather than blinking out.
		if(b && b -> alive())
			b -> kill();

		// The entry goes now rather than on the next liveness pass: the body
		// is dead as of this message, and leaving it would let a second
		// despawn for the same id match it.
		net_fx_table[at] = net_fx_table[net_fx_count - 1];
		net_fx_count --;
		}
}

/*
	Push a state onto the body an entry tracks.

	Pose and velocity, and nothing else: the actor keeps doing whatever it is
	doing, only its place in the world is corrected. That is the same contract
	the racers run under, and it is enough because the interval is short enough
	that no scripted behaviour is visible in the gap between two batches.
*/
static void net_fx_apply_actor_state(const netActorState* a,const netFxEntry* e)
{
	Body* b = Mdisp -> searchBody(e -> body_id);

	if(!b || !b -> alive()) return;

	// Vertical velocity is not carried: nothing would use it and a bonus man's
	// arc is short enough that keeping whatever it already has between batches
	// is closer than a number that crossed the wire.
	b -> setPositionExact(Vect3f(net_uq_actor_pos(a -> pos_x),
				    net_uq_actor_pos(a -> pos_y),
				    net_uq_actor_pos(a -> pos_z)),
			     net_uq_yaw(a -> yaw));

	b -> setGlobalVelocity(Vect3f(net_uq_signed_speed(a -> vel_x),
				      net_uq_signed_speed(a -> vel_y),
				      b -> Vglobal().z));
}

/*
	State that arrived before the spawn it belongs to.

	The host queues a spawn and a state in the same tick and sends them as two
	datagrams. UDP does not promise the first one arrives first, and a state
	batch for an entity the client has not built yet is otherwise silently
	dropped - which would leave that actor frozen at its placeholder pose until
	the next batch, or forever if it is outside the view radius.

	Remembered here and drained when the spawn lands. Bounded by dropping the
	oldest entry when it is full: a spawn this late is probably lost, and the
	states behind it are being superseded a hundred milliseconds apart anyway.
*/
static const int NET_FX_PENDING_ROOM = NET_MAX_ACTORS_PER_PACKET;

static void net_fx_pending_remember(const netActorState* a)
{
	if(net_fx_actor_pending_count >= NET_FX_PENDING_ROOM){
		for(int i = 1; i < net_fx_actor_pending_count; i ++)
			net_fx_actor_pending[i - 1] = net_fx_actor_pending[i];

		net_fx_actor_pending_count = NET_FX_PENDING_ROOM - 1;
		}

	net_fx_actor_pending[net_fx_actor_pending_count ++] = *a;
}

static void net_fx_pending_drain(void)
{
	int left = 0;

	for(int i = 0; i < net_fx_actor_pending_count; i ++){
		const netActorState* a = &net_fx_actor_pending[i];
		int at = net_fx_find(a -> entity_id);

		if(at < 0 || !net_fx_table[at].is_actor){
			// Still waiting on the spawn, or never getting one. Either way it
			// stays and is tried again on the next batch; only a full list
			// drops one, and that happens in pending_remember.
			net_fx_actor_pending[left ++] = *a;
			continue;
			}

		net_fx_apply_actor_state(a,&net_fx_table[at]);
		}

	net_fx_actor_pending_count = left;
}

void net_fx_actor_recv(const unsigned char* data,unsigned int len)
{
	netActorHeader hdr;
	netActorState batch[NET_MAX_ACTORS_PER_PACKET];
	int count = net_actor_unpack(&hdr,batch,NET_MAX_ACTORS_PER_PACKET,data,(int)len);

	// The header carries the host's tick, which is what an interpolation
	// buffer would key off. There is no buffer: poses are applied as they
	// arrive, so every tracked actor here moves at NET_ACTOR_REPL_HZ rather
	// than smoothly.
	(void)hdr;

	for(int i = 0; i < count; i ++){
		int at = net_fx_find(batch[i].entity_id);

		// State for something this machine never had. Normally the spawn is
		// still in flight and the drain below catches up with it.
		if(at < 0 || !net_fx_table[at].is_actor){
			net_fx_pending_remember(&batch[i]);
			continue;
			}

		net_fx_apply_actor_state(&batch[i],&net_fx_table[at]);
		}

	net_fx_pending_drain();
}

/* --------------------------------- both ---------------------------------- */

void net_fx_stats(int* projectiles,int* actors)
{
	int p = 0,a = 0;

	for(int i = 0; i < net_fx_count; i ++)
		if(net_fx_table[i].is_actor) a ++;
		else p ++;

	if(projectiles) *projectiles = p;
	if(actors) *actors = a;
}

/*
	-netdump. See net_fx.h. Same schema as the racer lines in net_game.cpp, with
	the racer-only trailing fields left at zero: the consumer keys E rows by
	(tick, entity_id) and reads the pose columns.
*/
void net_fx_trace(FILE* f,unsigned int tick)
{
	if(!f) return;

	for(int i = 0; i < net_fx_count; i ++){
		netFxEntry* e = &net_fx_table[i];
		Body* b = Mdisp ? Mdisp -> searchBody(e -> body_id) : NULL;

		if(!b) continue;

		const Vect3f& p = b -> R();
		const Vect3f& v = b -> Vglobal();

		// model is the mesh the host resolved (the one number the client
		// cannot derive); alive guards the count comparison against a body
		// the engine has killed but not yet dropped from its list.
		fprintf(f,
			"%u,E,%u,%u,%u,%.4f,%.4f,%.4f,%.6f,%.4f,%.4f,%.4f,%.2f,"
			"0,0,0,0,0,0,0,0,0,0,0,0,0,0,%u,%d\n",
			tick,(unsigned)e -> entity_id,(unsigned)e -> type,
			(unsigned)e -> variant,
			p.x,p.y,p.z,b -> psi(),v.x,v.y,v.z,v.norm(),
			(unsigned)(b -> geometry ? b -> geometry -> Type : 0),
			b -> alive() ? 1 : 0);
	}
}

void net_fx_quant(void)
{
	int i;

	if(!mchNetGame){
		net_fx_spawn_q_count = 0;
		net_fx_despawn_q_count = 0;
		return;
		}

	if(mchNetHost){
		// The spawn batch is flushed here; the actor pass itself is driven
		// from the game layer, and only once the match is running - see
		// mchNetQuant. Spawning actors before any client has connected would
		// mark them sent into the void.
		net_fx_flush_spawn();
		}

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
