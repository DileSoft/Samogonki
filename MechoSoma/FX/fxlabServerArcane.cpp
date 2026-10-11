#include "StdAfx.h"

#include "aci_parser.h"
#include "TERRA.H"

#include "BodyDispatcher.h"
#include "Mechos.h"
#include "CustomFieldSources.h"
#include "KEYS.H"
#include "ctl_point.h"
#include "race.h"
#include "mechosoma.h"
#include "arcane_menu.h"
#include "sound.h"

#include "fxlabID.h"
#include "fxlabGeneral.h"
#include "fxlabResource.h"
#include "fxlabInterface.h"
#include "fxlabApplication.h"
#include "XTList.h"
#include "fxlabServerGeneral.h"
#include "fxlabServerStuff.h"
#include "fxlabServerArcane.h"
#include "Mesh3ds.h"

#include "M3d_id.h"
#include "M3d_effects.h"
#include "fxlabBodyInterface.h"

#include "TrackIterator.h"
#include "AnimalArcansPrm.h"


const float FXLAB_REALOBJECT_OFFSET_Z = 20.0f;

MatXf getPlacementPose(const Vect3f& center);
void fxlabGetArcanePosition(Mechos* owner,const Vect3f& offset,Vect3f& pos);

extern int mchCurrentWorld;

/*
	These two arcanes read them, and so does fxlabClientArcaneStart(), which
	sits above their old place, so they move up here. Values unchanged.
*/
const float FXLAB_FIRE_GARDEN_ROSE_LEN = 200.0f;
const float FXLAB_FIRE_GARDEN_POSITION = 0;
const float FXLAB_FURY_FUNGUS_RADIUS = 35.0f;

/*
	Client-side visual for an arcane, without the game part.

	Mirrors only the fxlabClientD pieces of the matching Start() below - the
	server objects (fields, damage sensors, features) are deliberately absent,
	because the host owns those and a client rebuilding them would apply forces
	and damage a second time.

	Positions and keys are copied from the matching Start() in this file so the
	client look matches the host's. If a Start() changes, change it here too.

	Not here, on purpose:
	  - anything the arcane produces as a Body (fireballs, mines, spikes,
	    spawned creatures, thrown bonuses). Those travel as class B already,
	    and their client-link FX need the body pointer, which does not exist
	    at cast time on this machine.
	  - per-interrupt pieces (a storm's later strikes, a seed's drops). They
	    happen after the cast and are not part of the cast event.

	FuryFungus uses a deterministic ring instead of fxlabClientRND, because
	that stream is shared with every other cosmetic effect.
*/

// The shape every arcane uses for a one shot sound.
static void fxlabClientArcaneSoundImpulse(const Vect3f& p,int sound)
{
	fxlabGeneralObjectType* t = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_SOUND_SPACE_IMPULSE);

	t->SetStartTime(fxlabClientD->GetTime());
	t->SetSoundID(sound);
	t->SetPosition(p);
	t->Start();
}

static void fxlabClientArcaneSoundLong(const Vect3f& p,int sound)
{
	fxlabGeneralObjectType* t = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_SOUND_SPACE_LONG);

	t->SetStartTime(fxlabClientD->GetTime());
	t->SetSoundID(sound);
	t->SetPosition(p);
	t->Start();
}

/*
	Make one of the arcane's own client objects and tag it with the arcane that
	owns it, so a stop can find it again.

	Only the objects built directly here are tagged. A child that one of them
	makes in its own Start() - a claw's spline, a link's trail - is left alone:
	it belongs to its owner, and killing it out from under the owner would
	leave the owner holding a freed pointer the next time it draws.
*/
static fxlabGeneralObjectType* fxlabClientArcaneNew(int type,int owner_id,int arcane_id)
{
	fxlabGeneralObjectType* t = fxlabClientD->CreateObject(type);

	t -> NetArcaneOwner = owner_id;
	t -> NetArcaneID = arcane_id;

	return t;
}

void fxlabClientArcaneStart(int arcane_id,int owner_id)
{
	mchRacer* n;
	mchMechosRacer* owner = NULL;
	fxlabGeneralObjectType* t;
	Vect3f Position;

	if(!mch_raceD || !mch_raceD->racerLst) return;

	n = mch_raceD->racerLst->first();
	while(n){
		if(n->ID == owner_id && n->Type == MCH_RACER_TYPE_MECHOS){
			owner = static_cast<mchMechosRacer*>(n);
			break;
		}
		n = n->next;
	}

	if(!owner) return;

	/*
		Feature arcanes. Their whole visual is the car itself - wings, a
		helicopter rotor, a dragon wind - set on the Mechos by the host's
		Start(). The client runs the same call so its copy of the car matches;
		the stop event resets it. Nothing else about these arcanes is visible.
	*/
	switch(arcane_id){
		case ARCANE050: owner -> set_feature(Mechos::DRAGON_POWER); break;
		case ARCANE051: owner -> set_feature(Mechos::HELICOPTER); break;
		case ARCANE052: owner -> set_feature(Mechos::JABASOMA); break;
		case ARCANE059: owner -> set_feature(Mechos::BUTTERFLY); break;
		case ARCANE060: owner -> set_feature(Mechos::DIRIGIBLE); break;
		case ARCANE038: owner -> set_feature(Mechos::IRON_HIP); break;
		//RevoltSpace and FireTrack are features too: their whole visual is the
		//car, and fxlabRevoltSpace::Start / fxlabArcaneRedTrack::Start are the
		//calls that set it.
		case ARCANE014: owner -> set_feature(Mechos::MAGNETIC_CUSHION); break;
		case ARCANE011: owner -> set_feature(Mechos::RED_TRACK_FEATURE); break;
		default: break;
		}

	switch(arcane_id){
		//Petard: the throw. The burst is a later interrupt.
		case ARCANE002:
			Position = owner->part_coords(M3D_FRONT);
			{
				Vect3f v = owner->Yglobal() * 3.0f;
				v += owner->Zglobal();
				v.Normalize(20.0f);

				t = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_PETARD_MODEL);
				t->SetStartTime(fxlabClientD->GetTime());
				t->SetKeyID(FXLAB_ID_KEY_PETARD_MODEL);
				t->SetPosition(Position);
				t->SetVelocity(v);
				t->Start();
			}
			fxlabClientArcaneSoundImpulse(Position,EFF_PETARD_START);
			break;

		//FireGarden
		case ARCANE003:
			{
				int i;
				Vect3f v,dv;

				Position = owner->R();
				Position.z = fxlabGetLevel(XCYCL(round(Position.x)),YCYCL(round(Position.y)),Position.z + owner->radius());
				fxlabGetArcanePosition(owner,Vect3f(0,owner->radius()*1.5f,0),Position);

				v = owner->Yglobal() * FXLAB_FIRE_GARDEN_POSITION;
				v += owner->Xglobal() * FXLAB_FIRE_GARDEN_ROSE_LEN * 0.5f;
				dv = owner->Xglobal() * FXLAB_FIRE_GARDEN_ROSE_LEN / (float)(-FXLAB_FIRE_GARDEN_ROSE_NUM);
				v += Position;

				for(i = 0;i < FXLAB_FIRE_GARDEN_ROSE_NUM;i++){
					CYCLE(v);
					v += dv;
					v.z = fxlabGetLevel(v.x,v.y,v.z + owner->radius());

					t = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_OBJECT_SET);
					t->SetStartTime(fxlabClientD->GetTime());
					t->SetKeyID(FXLAB_ID_KEY_ROSE_FIRE_SET);
					t->SetPosition(v);
					t->Start();
				}

				fxlabClientArcaneSoundImpulse(Position,EFF_LURID_BILLOW);
				fxlabClientArcaneSoundLong(Position,EFF_LURID_BILLOW_ADD);
			}
			break;

		//RageSlime
		case ARCANE004:
			Position = owner->R();
			Position.z = fxlabGetLevel(XCYCL(round(Position.x)),YCYCL(round(Position.y)),owner->R().z + owner->radius());
			fxlabGetArcanePosition(owner,Vect3f(0,owner->radius()*1.5f,0),Position);

			t = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_SLIME_MODEL);
			t->SetStartTime(fxlabClientD->GetTime());
			t->SetKeyID(FXLAB_ID_KEY_RAGE_SLIME_MODEL);
			t->SetPosition(Position);
			t->SetMatrix(getPlacementPose(Position));
			t->Start();

			fxlabClientArcaneSoundLong(Position,EFF_SLIME_FURY);
			fxlabClientArcaneSoundImpulse(Position,EFF_SLIME_FURY_ADD);
			break;

		//FuryFungus
		case ARCANE005:
			{
				int i;
				Vect3f v;

				Position = owner->R();
				Position.z += owner->radius();
				fxlabGetArcanePosition(owner,Vect3f(0,owner->radius()*1.5f,0),Position);

				for(i = 0;i < FXLAB_FURY_FUNGUS_NUM;i++){
					float a,r;

					if(i < 7){
						a = 6.2831853f * (float)i / 7.0f;
						r = FXLAB_FURY_FUNGUS_RADIUS;
					}else{
						// Same ring, but without fxlabClientRND: that stream is
						// shared with every other cosmetic effect, and drawing
						// from it here would knock them all out of step.
						a = 6.2831853f * (float)((i * 7) % 13) / 13.0f;
						r = FXLAB_FURY_FUNGUS_RADIUS * (float)((i % 5) + 1) / 5.0f;
					}

					v = Position;
					v += Vect3f(r * cosf(a),r * sinf(a),0);
					CYCLE(v);
					v.z = fxlabGetLevel(v.x,v.y,v.z + owner->radius()) + 10.0f;

					t = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_LASH_MODEL);
					t->SetStartTime(fxlabClientD->GetTime());
					t->SetKeyID(FXLAB_ID_KEY_FURY_FUNGUS_LASH);
					t->SetPosition(v);
					t->Start();
				}

				v = Position;
				CYCLE(v);
				v.z = fxlabGetLevel(v.x,v.y,v.z + owner->radius());

				t = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_SPARK_GENERATOR);
				t->SetStartTime(fxlabClientD->GetTime());
				t->SetKeyID(FXLAB_ID_KEY_FURY_FUNGUS_FOG);
				t->SetPosition(v);
				t->Start();

				fxlabClientArcaneSoundLong(v,EFF_WILD_FUNGUS);
			}
			break;

		//Tomato
		case ARCANE012:
			Position = owner->R();
			fxlabGetArcanePosition(owner,Vect3f(0,owner->radius()*1.5f,0),Position);
			fxlabClientArcaneSoundImpulse(Position,EFF_TOMATO);
			break;

		//RevoltSpace
		case ARCANE014:
			Position = owner->R();
			t = fxlabClientArcaneNew(FXLAB_CLIENT_PROCESS_REVOLT_SPACE_LINK,owner_id,arcane_id);
			t->SetStartTime(fxlabClientD->GetTime());
			t->SetKeyID(FXLAB_ID_KEY_REVOLT_SPACE_LINK);
			t->SetPosition(Position);
			t->SetBody(owner);
			t->Start();
			break;

		//SpiderSpawn: the spiders are bodies and travel as class B.
		case ARCANE017:
			Position = owner->R();
			fxlabClientArcaneSoundImpulse(Position,EFF_SPIDERS_ATTACK);
			break;

		//CaterpillarSpawn
		case ARCANE021:
			Position = owner->R();
			fxlabClientArcaneSoundImpulse(Position,EFF_CATERPILLAR);
			break;

		//FireStone
		case ARCANE024:
			Position = owner->R();
			{
				Vect3f v = owner->Yglobal() * 2.0f;
				v += owner->Zglobal();
				v.Normalize(40.0f);

				t = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_BULLET_FIRE_MODEL);
				t->SetStartTime(fxlabClientD->GetTime());
				t->SetKeyID(FXLAB_ID_KEY_FIRE_BULLET_MODEL);
				t->SetPosition(Position);
				t->SetVelocity(v);
				t->Start();
			}
			fxlabClientArcaneSoundImpulse(Position,EFF_STAR_SHOWER);
			break;

		//ElectricStorm: the opening only. Each later strike is an interrupt.
		case ARCANE025:
			Position = owner->R();
			Position.z = fxlabGetLevel(XCYCL(round(Position.x)),YCYCL(round(Position.y)),Position.z + owner->radius());

			t = fxlabClientArcaneNew(FXLAB_CLIENT_PROCESS_CONTROL_OMNI,owner_id,arcane_id);
			t->SetStartTime(fxlabClientD->GetTime());
			t->SetKeyID(FXLAB_ID_KEY_ELECTRIC_STORM_MARKER_OMNI);
			t->SetPosition(Position);
			t->Start();

			Position.z += 150.0f;

			t = fxlabClientArcaneNew(FXLAB_CLIENT_PROCESS_TWISTER,owner_id,arcane_id);
			t->SetStartTime(fxlabClientD->GetTime());
			t->SetKeyID(FXLAB_ID_KEY_STORM_CLOUDS);
			t->SetPosition(Position);
			t->Start();

			fxlabClientArcaneSoundImpulse(Position,EFF_THUNDERSTORM_ADD);
			break;

		//BowMachine
		case ARCANE032:
			Position = owner->part_coords(M3D_FRONT);
			t = fxlabClientArcaneNew(FXLAB_CLIENT_PROCESS_BOW_MACHINE_FRONT_LINK,owner_id,arcane_id);
			t->SetStartTime(fxlabClientD->GetTime());
			t->SetKeyID(FXLAB_ID_KEY_BOW_MACHINE);
			t->SetPosition(Position);
			t->SetBody(owner);
			t->Start();
			break;

		//ShaftMachine
		case ARCANE033:
			Position = owner->part_coords(M3D_FRONT);
			t = fxlabClientArcaneNew(FXLAB_CLIENT_PROCESS_SHAFT_MECHINE_FRONT_LINK,owner_id,arcane_id);
			t->SetStartTime(fxlabClientD->GetTime());
			t->SetKeyID(FXLAB_ID_KEY_SHAFT_MACHINE_LINK);
			t->SetPosition(Position);
			t->SetBody(owner);
			t->Start();
			break;

		//KineticHit
		case ARCANE034:
			Position = owner->R();

			t = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_CONTROL_MODEL);
			t->SetStartTime(fxlabClientD->GetTime());
			t->SetKeyID(FXLAB_ID_KEY_KINETIC_HIT_WAVE);
			t->SetPosition(Position);
			t->Start();

			t = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_TERRAIN_DEFORMATOR);
			t->SetStartTime(fxlabClientD->GetTime());
			t->SetKeyID(FXLAB_ID_KEY_KINETIC_HIT_DEFORMATION);
			t->SetPosition(Position);
			t->Start();

			fxlabClientArcaneSoundImpulse(Position,EFF_KINETIC_HIT);
			break;

		//PlasmaClaw
		case ARCANE036:
			Position = owner->part_coords(M3D_FRONT);
			t = fxlabClientArcaneNew(FXLAB_CLIENT_PROCESS_WILD_CLAW_FRONT_LINK,owner_id,arcane_id);
			t->SetStartTime(fxlabClientD->GetTime());
			t->SetKeyID(FXLAB_ID_KEY_WILD_CLAW);
			t->SetPosition(Position);
			t->SetBody(owner);
			t->Start();
			fxlabClientArcaneSoundImpulse(Position,EFF_TENTACLE);
			break;

		//JumpBall: the balls are bodies and travel as class B.
		case ARCANE037:
			Position = owner->R();
			fxlabClientArcaneSoundImpulse(Position,EFF_TITANIUM_BALLS);
			break;

		/*
			VoodooMaster: the heads are server-only damage objects, and each one
			owns a client link that draws the orbiting head (see
			fxlabServerVoodooHead::Start). A client never runs a server process,
			so those links never get made and the whole arcane is invisible.
			Rebuilt here, one per head, spaced the way the host spaces them.

			The latch onto a victim does not travel: a mirrored head keeps
			orbiting instead of freezing where it hit. Cosmetic only.
		*/
		case ARCANE039:
			{
				int i;
				float a = 0,da = 2.0f * M_PI / FXLAB_VOOODOO_MASTER_OBJECT_NUM;

				Position = owner->part_coords(M3D_FRONT);

				for(i = 0;i < FXLAB_VOOODOO_MASTER_OBJECT_NUM;i++){
					t = fxlabClientArcaneNew(FXLAB_CLIENT_PROCESS_VOODOO_MASTER_LINK,
								 owner_id,arcane_id);
					t->SetStartTime(fxlabClientD->GetTime());
					t->SetPosition(Position);
					t->SetAngle(a);
					t->SetDeltaAngle((M_PI / 20.0f) / fxlabServerTimeRate);
					t->SetRadius(50.0f);
					t->SetMechosPoint(owner);
					t->Start();

					a += da;
				}
			}
			break;

		//FireBall: the ball is a body and travels as class B.
		case ARCANE040:
			Position = owner->R();
			fxlabClientArcaneSoundImpulse(Position,EFF_FIREBALLS);
			break;

		//Clown
		case ARCANE041:
			Position = owner->R();
			fxlabClientArcaneSoundImpulse(Position,EFF_CLOWN);
			break;

		//Suicide: the warrior is a body and travels as class B; this is its spark.
		case ARCANE042:
			Position = owner->R();
			t = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_SPARK_GENERATOR);
			t->SetStartTime(fxlabClientD->GetTime());
			t->SetKeyID(FXLAB_ID_KEY_CLOWN);
			t->SetPosition(Position);
			t->Start();
			break;

		//BallWind
		case ARCANE045:
			Position = owner->R();

			t = fxlabClientArcaneNew(FXLAB_CLIENT_PROCESS_LEFT_BALL_WIND_LINK,owner_id,arcane_id);
			t->SetStartTime(fxlabClientD->GetTime());
			t->SetKeyID(FXLAB_ID_KEY_BALL_WIND_LEFT_LINK);
			t->SetPosition(Position);
			t->SetBody(owner);
			t->Start();

			t = fxlabClientArcaneNew(FXLAB_CLIENT_PROCESS_RIGHT_BALL_WIND_LINK,owner_id,arcane_id);
			t->SetStartTime(fxlabClientD->GetTime());
			t->SetKeyID(FXLAB_ID_KEY_BALL_WIND_RIGHT_LINK);
			t->SetPosition(Position);
			t->SetBody(owner);
			t->Start();

			fxlabClientArcaneSoundImpulse(Position,EFF_BULLET_FLIGHT);
			fxlabClientArcaneSoundLong(Position,EFF_BULLET_FLIGHT_ADD);
			break;

		//ShiftTrap
		case ARCANE048:
			Position = owner->R();
			Position.z = fxlabGetLevel(XCYCL(round(Position.x)),YCYCL(round(Position.y)),owner->R().z + owner->radius());
			fxlabGetArcanePosition(owner,Vect3f(0,owner->radius()*1.5f,0),Position);

			t = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_GROUND_MODEL);
			t->SetStartTime(fxlabClientD->GetTime());
			t->SetKeyID(FXLAB_ID_KEY_SHIFT_TRAP_MODEL);
			t->SetPosition(Position);

			{
				MatXf pose = getPlacementPose(Position);
				t->SetMatrix(pose);
				Position = pose.trans();
			}

			t->Start();

			fxlabClientArcaneSoundLong(Position,EFF_TELEPORT);
			break;

		//DragonWind
		case ARCANE050:
			Position = owner->R();
			fxlabClientArcaneSoundImpulse(Position,EFF_DRAGON_POWER_ADD);
			break;

		//FrogWind
		case ARCANE052:
			Position = owner->R();
			fxlabClientArcaneSoundImpulse(Position,EFF_FROG);
			break;

		//ButterflyWind
		case ARCANE059:
			Position = owner->R();
			fxlabClientArcaneSoundImpulse(Position,EFF_WING_BUTTERFLY);
			break;

		//DirigibleWind
		case ARCANE060:
			Position = owner->R();
			fxlabClientArcaneSoundImpulse(Position,EFF_DIRIGIBLE);
			break;

		//SnowFall
		case ARCANE064:
			Position = owner->R();
			{
				Vect3f v = owner->Yglobal() * 1.8f;
				v += owner->Zglobal();
				v.Normalize(40.0f);

				t = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_BULLET_SNOW_MODEL);
				t->SetStartTime(fxlabClientD->GetTime());
				t->SetKeyID(FXLAB_ID_KEY_SNOW_BULLET_MODEL);
				t->SetPosition(Position);
				t->SetVelocity(v);
				t->Start();
			}
			fxlabClientArcaneSoundImpulse(Position,EFF_SNOW_FALL);
			break;

		//LightningBolt: the bolt is a body and travels as class B.
		case ARCANE066:
			Position = owner->R();
			fxlabClientArcaneSoundImpulse(Position,EFF_LIGHT_BOLT);
			break;

		default:
			break;
	}
}

/*
	Client-side visual for one later interrupt of an arcane.

	The host's InterruptAction() does two things in one body: it plays the
	client visual and it runs the game effect - fields, damage sensors, bodies.
	Only the first belongs on a client, so this mirrors just the fxlabClientD
	pieces of each InterruptAction() in this file. If one of those changes,
	change it here too.

	The place comes from the host on the event: a storm strike waits at the
	cast point, a seed drops at the owner, and neither can be recovered from
	the owner alone.

	Interrupts whose visible result is a Body are not listed here: those bodies
	travel as class B (and their link rides netSpawn::client_link), so only the
	client-only pieces are left to rebuild.
*/
void fxlabClientArcaneInterrupt(int arcane_id,int owner_id,const Vect3f& Position)
{
	mchRacer* n;
	mchMechosRacer* owner = NULL;
	fxlabGeneralObjectType* t;

	// A seed reads the owner's radius; the rest only the place. Same lookup as
	// fxlabClientArcaneStart, and it may come back NULL.
	if(mch_raceD && mch_raceD->racerLst){
		n = mch_raceD->racerLst->first();
		while(n){
			if(n->ID == owner_id && n->Type == MCH_RACER_TYPE_MECHOS){
				owner = static_cast<mchMechosRacer*>(n);
				break;
			}
			n = n->next;
		}
	}

	switch(arcane_id){
		//CrystalSpike: the spike is a body and travels as class B; this is its crack.
		case ARCANE009:
			fxlabClientArcaneSoundImpulse(Position,EFF_CRYSTALLIZATION);
			break;

		//Bubbles: the bubble is a body and travels as class B.
		case ARCANE019:
			fxlabClientArcaneSoundImpulse(Position,EFF_SOAP_BUBBLE);
			break;

		//ElectricStorm: each later strike. Mirrors fxlabArcaneElecricStorm::InterruptAction.
		case ARCANE025:
			{
				Vect3f v = Position;
				v.z -= 150.0f;

				t = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_LIGHTNING_LINE);
				t->SetStartTime(fxlabClientD->GetTime());
				t->SetKeyID(FXLAB_ID_KEY_ELECTRIC_STORM_BOW);
				t->SetPosition(v);
				t->SetVelocity(Vect3f(0,0,1.0f));
				t->Start();

				t = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_CONTROL_OMNI);
				t->SetStartTime(fxlabClientD->GetTime());
				t->SetKeyID(FXLAB_ID_KEY_ELECTRIC_STORM_LIGHT);
				t->SetPosition(v);
				t->Start();

				fxlabClientArcaneSoundImpulse(Position,EFF_THUNDERSTORM);

				t = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_FEAR_STREAM);
				t->SetKeyID(FXLAB_ID_KEY_STORM_SPARK);
				t->SetStartTime(fxlabClientD->GetTime());
				t->SetPosition(v);
				t->SetVelocity(Vect3f(0,0,0));
				t->Start();
			}
			break;

		//ElmLight: a seed drops. Mirrors fxlabArcaneLightningSeed::InterruptAction.
		case ARCANE029:
			if(!owner) return;

			t = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_LIGHTNING_SEED);
			t->SetStartTime(fxlabClientD->GetTime());
			t->SetKeyID(FXLAB_ID_KEY_LIGHTNING_SEEDS);
			t->SetPosition(Position + Vect3f(0,0,owner->radius() * 0.33f));
			t->Start();
			break;

		//Mines: the mortar is a body and travels as class B.
		case ARCANE061:
			fxlabClientArcaneSoundImpulse(Position,EFF_MINES);
			break;

		default:
			break;
	}
}

/*
	Client side: the arcane is over, so take its visual down.

	The mirror of fxlabClientArcaneStart. Every object that function built was
	tagged with (owner, arcane); this kills them all. Short effects are already
	gone and SetAlive(0) on a dead object costs nothing, so only the lasting
	ones - the links bound to the car, a storm's clouds - are what this really
	does. It also puts back any feature the cast changed on the car.
*/
void fxlabClientArcaneStop(int arcane_id,int owner_id)
{
	mchRacer* n;
	mchMechosRacer* owner = NULL;
	std::list<fxlabGeneralObjectType*>::iterator it;

	if(!fxlabClientD) return;

	it = fxlabClientD -> ObjectList.begin();
	while(it != fxlabClientD -> ObjectList.end()){
		fxlabGeneralObjectType* t = *it;

		if(t -> NetArcaneOwner == owner_id && t -> NetArcaneID == arcane_id)
			t -> SetAlive(0);

		++ it;
		}

	it = fxlabClientD -> CreateList.begin();
	while(it != fxlabClientD -> CreateList.end()){
		fxlabGeneralObjectType* t = *it;

		if(t -> NetArcaneOwner == owner_id && t -> NetArcaneID == arcane_id)
			t -> SetAlive(0);

		++ it;
		}

	if(!mch_raceD || !mch_raceD -> racerLst) return;

	n = mch_raceD -> racerLst -> first();
	while(n){
		if(n -> ID == owner_id && n -> Type == MCH_RACER_TYPE_MECHOS){
			owner = static_cast<mchMechosRacer*>(n);
			break;
			}
		n = n -> next;
		}

	if(!owner) return;

	switch(arcane_id){
		case ARCANE050: owner -> reset_feature(Mechos::DRAGON_POWER); break;
		case ARCANE051: owner -> reset_feature(Mechos::HELICOPTER); break;
		case ARCANE052: owner -> reset_feature(Mechos::JABASOMA); break;
		case ARCANE059: owner -> reset_feature(Mechos::BUTTERFLY); break;
		case ARCANE060: owner -> reset_feature(Mechos::DIRIGIBLE); break;
		case ARCANE038: owner -> reset_feature(Mechos::IRON_HIP); break;
		case ARCANE014: owner -> reset_feature(Mechos::MAGNETIC_CUSHION); break;
		case ARCANE011: owner -> reset_feature(Mechos::RED_TRACK_FEATURE); break;
		default: break;
		}
}

void fxlabGeneralArcaneType::Open()
{
	fxlabServerSpaceType::Open();

	LinkPoint = NULL;
	Owner = NULL;
	InterruptNum = -1;
	ActionPoint = -1;
	LifeTime = 1.0f;
};

void fxlabGeneralArcaneType::Start()
{
	int i;
	float time;
	fxlabServerSpaceType::Start();

	if(!LinkPoint)
		ErrH.Abort("UnDefined Link in fxlabArcane");
	if(!Owner)
		ErrH.Abort("UnDefined Owner in fxlabArcane");
	if(InterruptNum == -1)
		ErrH.Abort("UnDefined InterruptNum in fxlabArcane");

//	if(mchCurrentWorld == FXLAB_ARCANE_DEMO_WORLD)
//		LinkPoint->Power = LinkPoint->MaxPower;

	ActionTimer.start(0);
	ProcessPoint = &(LinkPoint->fxlabFace);

	RacerID = LinkPoint->ownerID;
	ArcaneID = LinkPoint->ID;
	MechosID = Owner->Mechos::ID;

	PartNumber = -1;
	for(i = 0;i < NUM_PARTS;i++){
		 if(LinkPoint == &(Owner->part_by_index(i)->arcaneData))
			 PartNumber = i;
	};

	if(PartNumber == -1)
		ErrH.Abort("Bad Calc Part's Number in fxlabArcane");

	GetOwnerID() = RacerID;
	GetArcaneID() = PartNumber;
	GetArcaneOwner() = this;
	GetObjectID() = ID;

	ActionStatus = 0;
	LinkPoint->Status = 0;
	
//	time = LinkPoint->DeltaPower / fxlabServerQuantPeriod;
	time = LifeTime / fxlabServerQuantPeriod;
	PowerTime = LinkPoint->Power * time / LinkPoint->MaxPower;
	DeltaPower = LinkPoint->MaxPower / time;

	if(InterruptNum > 1)
		InterruptBorder = time / (float)(InterruptNum - 1);
	else
		InterruptBorder = time + 1.0f;

	InterruptTime = InterruptBorder;

/*	fff < "\n\nPower = " <= LinkPoint->Power;
	fff < "\nMaxPower = " <= LinkPoint->MaxPower;
	fff < "\nDeltaPower = " <= LinkPoint->DeltaPower;
	fff < "\nTime = " <= time;
	fff < "\nPowerTime = " <= PowerTime;
	fff < "\nDecDeltaPower = " <= DeltaPower;*/

	fxlabArcaneStart(static_cast<ArcaneStatisticsInfo*>(this));

	CurrentProtectionTimer.start(ProtectionTime);
};

void fxlabGeneralArcaneType::Close()
{
//	if(mchCurrentWorld == FXLAB_ARCANE_DEMO_WORLD)
//		LinkPoint->Power = LinkPoint->MaxPower;

	// Both endings meet here - the arcane running out of power and the host
	// being told to stop it - so this is the one place to tell the clients
	// their mirror visual is done. No-op off the host.
	mchNetArcaneVisualStop(RacerID,ArcaneID);

	fxlabArcaneStop(static_cast<ArcaneStatisticsInfo*>(this));
	fxlabServerSpaceType::Close();
};

void fxlabGeneralArcaneType::Quant()
{
	fxlabServerSpaceType::Quant();
	InterruptUse();
	Condition();
	PowerUse();
	if(ActionStatus && !ActionTimer()){
		ActionStatus = 0;
		ActionPoint--;
		if(ActionPoint <= 0)
			ActionZero();
	};
};

void fxlabGeneralArcaneType::InterruptUse()
{
	InterruptTime += 1.0f;
	while(InterruptTime >= InterruptBorder){
		InterruptAction();

		// Host side, no-op elsewhere. Told after the action so Position holds
		// whatever place the action just used; a client rebuilds only the
		// look, never the server objects the action also made.
		mchNetArcaneInterrupt(RacerID,ArcaneID,Position.x,Position.y,Position.z);

		InterruptTime -= InterruptBorder;
	};
};

void fxlabGeneralArcaneType::PowerUse()
{
	PowerTime -= 1.0f;
	LinkPoint->Power -= DeltaPower;

	if(LinkPoint->Power <= 0)
		LinkPowerZero();

	if(PowerTime <= 0)
		PowerZero();
};

void fxlabGeneralArcaneType::ArcaneAction()
{
	if(ActionPoint > 0){
		ActionStatus = 1;
		ActionTimer.start(ActionDelay);
	};
};

void fxlabGeneralArcaneType::ArcaneFatality()
{
	if(ActionPoint > 0){
		ActionPoint--;
		if(ActionPoint <= 0)
			ActionZero();
	};
};

void fxlabGeneralArcaneType::SetArcaneAlive(int arcane_id,int owner_id)
{
	if(ArcaneID == arcane_id && RacerID == owner_id)
		SetAlive(0);
};

void fxlabGeneralArcaneType::LinkPowerZero()
{
	LinkPoint->Power = 0;
};

//------------------------------------------------------

void fxlabInternalArcane::Start()
{
	fxlabGeneralArcaneType::Start();
	Position = Owner->R();
};

void fxlabInternalArcane::Quant()
{
	fxlabGeneralArcaneType::Quant();
	Position = Owner->R();
};

void fxlabInternalArcane::Close()
{
	LinkPoint->Status = 1;
	fxlabGeneralArcaneType::Close();
};

void fxlabInternalArcane::Condition()
{
	fxlabGeneralArcaneType::Condition();
	if(!(Owner->completed()))
		SetAlive(0);
};

void fxlabInternalArcane::LinkPowerZero()
{
	fxlabGeneralArcaneType::LinkPowerZero();
	SetAlive(0); 
};

void fxlabInternalArcane::ActionZero()
{ 	
	fxlabGeneralArcaneType::ActionZero();
	SetAlive(0); 
};

//------------------------------------------------------

void fxlabArcaneDragonWind::Start()
{
	fxlabGeneralObjectType* t;

	fxlabInternalArcane::Start();
	Owner->set_feature(Mechos::DRAGON_POWER);

	t = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_SOUND_SPACE_IMPULSE);
	t->SetStartTime(fxlabClientD->GetTime());
	t->SetSoundID(EFF_DRAGON_POWER_ADD);
	t->SetPosition(Position);
	t->Start();
};

void fxlabArcaneDragonWind::Close()
{
	Owner->reset_feature(Mechos::DRAGON_POWER);
	fxlabInternalArcane::Close();
};

void fxlabArcaneDirigibleWind::Start()
{
	fxlabGeneralObjectType* t;

	fxlabInternalArcane::Start();
	Owner->set_feature(Mechos::DIRIGIBLE);

	t = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_SOUND_SPACE_IMPULSE);
	t->SetStartTime(fxlabClientD->GetTime());
	t->SetSoundID(EFF_DIRIGIBLE);
	t->SetPosition(Position);
	t->Start();
};

void fxlabArcaneDirigibleWind::Close()
{
	Owner->reset_feature(Mechos::DIRIGIBLE);
	fxlabInternalArcane::Close();
};


void fxlabArcaneButterflyWind::Start()
{
	fxlabGeneralObjectType* t;
	fxlabInternalArcane::Start();
	Owner->set_feature(Mechos::BUTTERFLY);
	
	t = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_SOUND_SPACE_IMPULSE);
	t->SetStartTime(fxlabClientD->GetTime());
	t->SetSoundID(EFF_WING_BUTTERFLY);
	t->SetPosition(Position);
	t->Start();
};

void fxlabArcaneButterflyWind::Close()
{
	Owner->reset_feature(Mechos::BUTTERFLY);
	fxlabInternalArcane::Close();
};


void fxlabArcaneFrogWind::Start()
{
	fxlabGeneralObjectType* t;

	fxlabInternalArcane::Start();
	Owner->set_feature(Mechos::JABASOMA);

	t = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_SOUND_SPACE_IMPULSE);
	t->SetStartTime(fxlabClientD->GetTime());
	t->SetSoundID(EFF_FROG);
	t->SetPosition(Position);
	t->Start();
};

void fxlabArcaneFrogWind::Close()
{
	Owner->reset_feature(Mechos::JABASOMA);
	fxlabInternalArcane::Close();
};


void fxlabArcaneHelicopterWind::Start()
{
	fxlabInternalArcane::Start();
	Owner->set_feature(Mechos::HELICOPTER);
};

void fxlabArcaneHelicopterWind::Close()
{
	Owner->reset_feature(Mechos::HELICOPTER);
	fxlabInternalArcane::Close();
};

//-----------------------

void fxlabRevoltSpace::Start()
{
	fxlabGeneralObjectType* t;

	fxlabInternalArcane::Start();
	Owner->set_feature(Mechos::MAGNETIC_CUSHION);

	t = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_REVOLT_SPACE_LINK);
	t->SetStartTime(fxlabClientD->GetTime());
	t->SetKeyID(FXLAB_ID_KEY_REVOLT_SPACE_LINK);
	t->SetPosition(Position);
	t->SetBody(Owner);
	t->SetProcessInterface(&ClientPoint);
	t->Start();
};

void fxlabRevoltSpace::Close()
{
	Owner->reset_feature(Mechos::MAGNETIC_CUSHION);
	if(ClientPoint.Process)
		ClientPoint.Process->SetAlive(0);
	fxlabInternalArcane::Close();
};

//-----------------------------------

void fxlabArcanePlasmaClaw::Start()
{
	fxlabGeneralObjectType* ct;
	fxlabGeneralObjectType* st;

	fxlabInternalArcane::Start();

	if(Owner->completed()){
		Position = Owner->part_coords(M3D_FRONT);

		ct = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_WILD_CLAW_FRONT_LINK);
		ct->SetStartTime(fxlabClientD->GetTime());
		ct->SetKeyID(FXLAB_ID_KEY_WILD_CLAW);
		ct->SetPosition(Position);
		ct->SetBody(Owner);
		ct->Start();

		st = fxlabServerD->CreateObject(FXLAB_SERVER_PROCESS_PLASMA_CLAW);
		st->SetStartTime(fxlabServerD->GetTime());
		st->SetKeyID(FXLAB_ID_KEY_PLASMA_CLAW_CONTROL);
		st->SetPosition(Position);
		st->SetRemoteObject(ct);
		st->SetMechosPoint(Owner);
		st->SetProtection(ProtectionID,ProtectionTime);
		st->SetStatistic(GetOwnerID(),GetArcaneID(),GetArcaneOwner(),GetObjectID());
		st->SetProcessInterface(&ServerPoint);
		st->Start();

		ct = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_SOUND_SPACE_IMPULSE);
		ct->SetStartTime(fxlabClientD->GetTime());
		ct->SetSoundID(EFF_TENTACLE);
		ct->SetPosition(Position);
		ct->Start();
	};
};

void fxlabArcanePlasmaClaw::Quant()
{
	fxlabGeneralArcaneType::Quant();

	if(Owner->completed()){
		Position = Owner->part_coords(M3D_FRONT);
		if(ServerPoint.Process)
			ServerPoint.Process->SetPosition(Position);
	};
};

void fxlabArcanePlasmaClaw::Close()
{
	if(ServerPoint.Process)
		ServerPoint.Process->SetAlive(0);

	fxlabInternalArcane::Close();
};

//-----------------------------------

void fxlabArcaneBowMachine::Start()
{
	fxlabGeneralObjectType* ct;
	fxlabGeneralObjectType* st;

	fxlabInternalArcane::Start();

	if(Owner->completed()){
		Position = Owner->part_coords(M3D_FRONT);

		ct = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_BOW_MACHINE_FRONT_LINK);
		ct->SetStartTime(fxlabClientD->GetTime());
		ct->SetKeyID(FXLAB_ID_KEY_BOW_MACHINE);
		ct->SetPosition(Position);
		ct->SetBody(Owner);
		ct->Start();

		st = fxlabServerD->CreateObject(/*FXLAB_SERVER_PROCESS_BOW_MACHINE_DAMAGE*/FXLAB_SERVER_PROCESS_SUCK_DAMAGE);
		st->SetStartTime(fxlabServerD->GetTime());
		st->SetKeyID(FXLAB_ID_KEY_BOW_MACHINE_DAMAGE);
		st->SetPosition(Position);
		st->SetRemoteObject(ct);
		st->SetProtection(ProtectionID,ProtectionTime);
		st->SetStatistic(GetOwnerID(),GetArcaneID(),GetArcaneOwner(),GetObjectID());
		st->SetProcessInterface(&ServerPoint);
		st->Start();
	};
};

void fxlabArcaneBowMachine::Quant()
{
	fxlabGeneralArcaneType::Quant();

	if(ServerPoint.Process){
		if(Owner->completed()){
			Position = Owner->part_coords(M3D_FRONT);
			ServerPoint.Process->SetPosition(Position);
		};
	}else SetAlive(0);
};

void fxlabArcaneBowMachine::Close()
{
	if(ServerPoint.Process)
		ServerPoint.Process->SetAlive(0);

	fxlabInternalArcane::Close();
};

//---------------------------------------------

void fxlabArcaneShaftMachine::Start()
{
	fxlabGeneralObjectType* t;
	Vect3f v;

	fxlabInternalArcane::Start();

	if(Owner->completed()){
		Position = Owner->part_coords(M3D_FRONT);
//		v = getDist(Position,Vect3f(Owner->R()));
//		v.normalize();

		v = Vect3f(0,1.0f,0.15f) * Owner->Alg();
		v.Normalize();

		t = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_SHAFT_MECHINE_FRONT_LINK);
		t->SetStartTime(fxlabClientD->GetTime());
		t->SetKeyID(FXLAB_ID_KEY_SHAFT_MACHINE_LINK);
		t->SetPosition(Position);
		t->SetBody(Owner);
		t->SetProcessInterface(&ClientPoint);
		t->Start();

		t = fxlabServerD->CreateObject(FXLAB_SERVER_PROCESS_DAMAGE_LINE);
		t->SetStartTime(fxlabServerD->GetTime());
		t->SetKeyID(FXLAB_ID_KEY_SHAFT_MACHINE_DAMAGE);
		t->SetPosition(Position);
		t->SetVelocity(v);
		t->SetProtection(ProtectionID,ProtectionTime);
		t->SetStatistic(GetOwnerID(),GetArcaneID(),GetArcaneOwner(),GetObjectID());
		t->SetProcessInterface(&ServerPoint);
		t->Start();
	};
};

void fxlabArcaneShaftMachine::Quant()
{
	Vect3f v;
	fxlabGeneralArcaneType::Quant();

	if(Owner->completed()){
		Position = Owner->part_coords(M3D_FRONT);
		v = Vect3f(0,1.0f,0.15f) * Owner->Alg();
		v.Normalize();
		ServerPoint.Process->SetPosition(Position);
		ServerPoint.Process->SetVelocity(v);
	};
};

void fxlabArcaneShaftMachine::Close()
{
	if(ClientPoint.Process)
		ClientPoint.Process->SetAlive(0);
	if(ServerPoint.Process)
		ServerPoint.Process->SetAlive(0);

	fxlabInternalArcane::Close();
};

//------------------------------------------

/*void fxlabArcaneVoodooMaster::Start()
{
	fxlabGeneralObjectType* t;

	fxlabInternalArcane::Start();

	if(Owner->completed()){
		Position = Owner->part_coords(M3D_FRONT);

		t = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_FRONT_LINK);
		t->SetStartTime(fxlabClientD->GetTime());
		t->SetKeyID(FXLAB_ID_KEY_VOODOO_MASTER_LINK);
		t->SetPosition(Position);
		t->SetBody(Owner);
		t->SetProcessInterface(&ClientPoint);
		t->Start();

		t = fxlabServerD->CreateObject(FXLAB_SERVER_PROCESS_DAMAGE_CIRCLE);
		t->SetStartTime(fxlabServerD->GetTime());
		t->SetKeyID(FXLAB_ID_KEY_VOODOO_MASTER_DAMAGE);
		t->SetPosition(Position);
		t->SetProtection(ProtectionID,ProtectionTime);
		t->SetStatistic(GetOwnerID(),GetArcaneID(),GetArcaneOwner(),GetObjectID());
		t->SetProcessInterface(&ServerPoint);
		t->Start();
	};
};

void fxlabArcaneVoodooMaster::Quant()
{
	fxlabGeneralArcaneType::Quant();

	if(Owner->completed()){
		Position = Owner->part_coords(M3D_FRONT);
		ServerPoint.Process->SetPosition(Position);
	};
};

void fxlabArcaneVoodooMaster::Close()
{
	if(ClientPoint.Process)
		ClientPoint.Process->SetAlive(0);
	if(ServerPoint.Process)
		ServerPoint.Process->SetAlive(0);

	fxlabInternalArcane::Close();
};*/


void fxlabArcaneVoodooMaster::Start()
{
	int i;
	float a,da;
	fxlabGeneralObjectType* t;

	fxlabInternalArcane::Start();

	if(Owner->completed()){
		Position = Owner->part_coords(M3D_FRONT);
		a = M_PI * fxlabServerRND.UnitRND();
		da = 2.0f * M_PI / FXLAB_VOOODOO_MASTER_OBJECT_NUM;
		for(i = 0;i < FXLAB_VOOODOO_MASTER_OBJECT_NUM;i++){
			t = fxlabServerD->CreateObject(FXLAB_SERVER_PROCESS_SERVER_VOODOO_HEAD);
			t->SetStartTime(fxlabServerD->GetTime());
			t->SetKeyID(FXLAB_ID_KEY_VOODOO_MASTER_DAMAGE);
			t->SetPosition(Position);
			t->SetAngle(a);
			t->SetMechosPoint(Owner);
			t->SetProtection(ProtectionID,ProtectionTime);
			t->SetStatistic(GetOwnerID(),GetArcaneID(),GetArcaneOwner(),GetObjectID());
			t->SetProcessInterface(&(ServerPoint[i]));
			t->Start();
			a += da;
		};
	};
};

void fxlabArcaneVoodooMaster::Quant()
{
	int i,cnt;
	fxlabGeneralArcaneType::Quant();

	if(Owner->completed()){
		Position = Owner->part_coords(M3D_FRONT);
		cnt = 0;
		for(i = 0;i < FXLAB_VOOODOO_MASTER_OBJECT_NUM;i++){
			if(ServerPoint[i].Process){
				ServerPoint[i].Process->SetPosition(Position);
				cnt++;
			};
		};
		if(!cnt)
			SetAlive(0);
	};
};

void fxlabArcaneVoodooMaster::Close()
{
	int i;
	for(i = 0;i < FXLAB_VOOODOO_MASTER_OBJECT_NUM;i++){
		if(ServerPoint[i].Process)
			ServerPoint[i].Process->SetAlive(0);
	};
	fxlabInternalArcane::Close();
};

//-----------------------------------------

void fxlabArcaneIronSide::Start()
{
	fxlabInternalArcane::Start();
	Owner->set_feature(Mechos::IRON_HIP);
};

void fxlabArcaneIronSide::Close()
{
	Owner->reset_feature(Mechos::IRON_HIP);
	fxlabInternalArcane::Close();
};

//--------------------------------------------

void fxlabArcaneBallWind::Start()
{
	fxlabGeneralObjectType* t;
	fxlabInternalArcane::Start();

	t = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_LEFT_BALL_WIND_LINK);
	t->SetStartTime(fxlabClientD->GetTime());
	t->SetKeyID(FXLAB_ID_KEY_BALL_WIND_LEFT_LINK);
	t->SetPosition(Position);
	t->SetBody(Owner);
	t->SetProcessInterface(&LeftPoint);
	t->Start();

	t = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_RIGHT_BALL_WIND_LINK);
	t->SetStartTime(fxlabClientD->GetTime());
	t->SetKeyID(FXLAB_ID_KEY_BALL_WIND_RIGHT_LINK);
	t->SetPosition(Position);
	t->SetBody(Owner);
	t->SetProcessInterface(&RightPoint);
	t->Start();

	Owner->cannon_ball_flight();

	t = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_SOUND_SPACE_IMPULSE);
	t->SetStartTime(fxlabClientD->GetTime());
	t->SetSoundID(EFF_BULLET_FLIGHT);
	t->SetPosition(Position);
	t->Start();

	t = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_SOUND_SPACE_LONG);
	t->SetStartTime(fxlabClientD->GetTime());
	t->SetSoundID(EFF_BULLET_FLIGHT_ADD);
	t->SetPosition(Position);
	t->SetProcessInterface(&SoundPoint);
	t->Start();
};

void fxlabArcaneBallWind::Quant()
{
	fxlabInternalArcane::Quant();

	if(SoundPoint.Process)
		SoundPoint.Process->SetPosition(Position);
};

void fxlabArcaneBallWind::Close()
{
	if(LeftPoint.Process)
		LeftPoint.Process->SetAlive(0);
	if(RightPoint.Process)
		RightPoint.Process->SetAlive(0);
	if(SoundPoint.Process)
		SoundPoint.Process->SetAlive(0);

	fxlabInternalArcane::Close();
};

//----------------------

void fxlabImpulseArcane::Quant()
{
	fxlabGeneralArcaneType::Quant();
	SetAlive(0);
};

void fxlabImpulseArcane::Close()
{
	LinkPoint->Status = 1;
	fxlabGeneralArcaneType::Close();
};

//-----------------

void fxlabArcaneFormicaryWind::Start()
{
	fxlabImpulseArcane::Start();

	Vect3f p0(0), p1(0),v;
	if(mchCurrentWorld == FXLAB_ARCANE_DEMO_WORLD){
		v = Vect3f(0,Owner->radius() * 10,0) * Mat3f(Owner->psi(),Z_AXIS);
		p0.x = XCYCL(round(v.x + Owner->R().x));
		p0.y = YCYCL(round(v.y + Owner->R().y));

		p1 = v;
		p1 *= 2.0f;
		p1 += Owner->R();
		CYCLE(p1);
	}else{
		mchCheckpoint* cp = mch_raceD -> getCP(Owner->stPtr->Checkpoint);
		if(cp){
			p0.x = XCYCL(cp->PosX0 + getDistX(cp->PosX1,cp->PosX0) / 2);
			p0.y = YCYCL(cp->PosY0 + getDistY(cp->PosY1,cp->PosY0) / 2);
		}else
			p0 = Owner -> R();

		TrackIterator ti;
		ti.set(Owner->stPtr->Checkpoint);
		p1 = ti += 3;
	};

	v = getDist(p0,p1);
	v.Normalize(Owner->radius() * 1.5f);
	p0 += v;
	CYCLE(p0);

	Owner->formicTransport(p0, p1);
	if(Owner->flags & MCH_AI){
		Owner->RemoveSeeds();
		Owner->AddSeed(p0);
	};
};

//-----------------------

void fxlabArcaneSuicideWarrior::Start()
{
	Body *p;
	OwnerProtection* op;
	fxlabGeneralObjectType* t;
	Vect3f v;
	float d;

	fxlabImpulseArcane::Start();

	Position = Owner->R();
	v = Owner->Yglobal();
	d = Owner->length() * 1.5f;
	Position.x = XCYCL(round(Position.x + v.x * d));
	Position.y = YCYCL(round(Position.y + v.y * d));
	Position.z = Position.z + v.z * d + FXLAB_REALOBJECT_OFFSET_Z;

	ArcaneStatisticsService::setCurrentArcane(*this);
	net_fx_set_pending_body("Suicide");
	net_fx_set_pending_link(FXLAB_CLIENT_PROCESS_SUICIDE_LINK);
	p = Mdisp->createBody("Suicide",Position,Owner->psi() * 180.0f / M_PI);
	
	op = dynamic_cast<OwnerProtection*>(p);
	if(op) op->set_owner(ProtectionID,ProtectionTime);

	t = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_SUICIDE_LINK);
	t->SetStartTime(fxlabClientD->GetTime());
//!!!!!
//	t->SetKeyID(FXLAB_ID_KEY_SUICIDE_LINK);
	t->SetPosition(Position);
	t->SetBody(p);
	t->Start();

	t = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_SPARK_GENERATOR);
	t->SetKeyID(FXLAB_ID_KEY_CLOWN);
	t->SetStartTime(fxlabClientD->GetTime());
	t->SetPosition(Position);
	t->Start();
};


void fxlabArcaneClownWarrior::Start()
{
	Vect3f v;
	float d;
	Body *p;
	OwnerProtection* op;
	fxlabGeneralObjectType* t;

	fxlabImpulseArcane::Start();

	Position = Owner->R();
	v = Owner->Yglobal();
	d = Owner->length() * 1.5f;
	Position.x = XCYCL(round(Position.x + v.x * d));
	Position.y = YCYCL(round(Position.y + v.y * d));
	Position.z = Position.z + v.z * d + FXLAB_REALOBJECT_OFFSET_Z;

	ArcaneStatisticsService::setCurrentArcane(*this);
	net_fx_set_pending_body("Clown");
	p = Mdisp->createBody("Clown",Position,Owner->psi() * 180.0f / M_PI);
	
	op = dynamic_cast<OwnerProtection*>(p);
	if(op) op->set_owner(ProtectionID,ProtectionTime);

	t = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_SOUND_SPACE_IMPULSE);
	t->SetStartTime(fxlabClientD->GetTime());
	t->SetSoundID(EFF_CLOWN);
	t->SetPosition(Position);
	t->Start();
};


void fxlabArcaneEyeWarrior::Start()
{
	Vect3f v;
	float d;
	Body *p;
	OwnerProtection* op;

	fxlabImpulseArcane::Start();

	Position = Owner->R();
	v = Owner->Yglobal();
	d = Owner->length() * 0.5f;
	Position.x = XCYCL(round(Position.x - v.x * d));
	Position.y = YCYCL(round(Position.y - v.y * d));
	Position.z = Position.z - v.z * d + FXLAB_REALOBJECT_OFFSET_Z;

	ArcaneStatisticsService::setCurrentArcane(*this);
	net_fx_set_pending_body("EvilEye");
	p = Mdisp->createBody("EvilEye",Position,Owner->psi() * 180.0f / M_PI + 180.0f);
	
	op = dynamic_cast<OwnerProtection*>(p);
	if(op) op->set_owner(ProtectionID,ProtectionTime);
};

void fxlabArcaneTomatoWarrior::Start()
{
	Body *p;
	OwnerProtection* op;
	fxlabGeneralObjectType* t;

	fxlabImpulseArcane::Start();

	Position = Owner->R();
	fxlabGetArcanePosition(Owner,Vect3f(0,Owner->radius()*1.5f,0),Position);

	ArcaneStatisticsService::setCurrentArcane(*this);
	net_fx_set_pending_body("Tomato");
	p = Mdisp->createBody("Tomato",Position,Owner->psi() * 180.0f / M_PI + 180.0f);
	
	op = dynamic_cast<OwnerProtection*>(p);
	if(op) op->set_owner(ProtectionID,ProtectionTime);

	t = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_SOUND_SPACE_IMPULSE);
	t->SetStartTime(fxlabClientD->GetTime());
	t->SetSoundID(EFF_TOMATO);
	t->SetPosition(Position);
	t->Start();
};

//--------------------------

void fxlabArcaneKineticHit::Start()
{
	fxlabGeneralObjectType* t;

	fxlabImpulseArcane::Start();
	Position = Owner->R();

	t = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_CONTROL_MODEL);
	t->SetStartTime(fxlabClientD->GetTime());
	t->SetKeyID(FXLAB_ID_KEY_KINETIC_HIT_WAVE);
	t->SetPosition(Position);
	t->Start();

	t = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_TERRAIN_DEFORMATOR);
	t->SetStartTime(fxlabClientD->GetTime());
	t->SetKeyID(FXLAB_ID_KEY_KINETIC_HIT_DEFORMATION);
	t->SetPosition(Position);
	t->Start();

	t = fxlabServerD->CreateObject(FXLAB_SERVER_PROCESS_DAMAGE_SENSOR);
	t->SetStartTime(fxlabServerD->GetTime());
	t->SetKeyID(FXLAB_ID_KEY_KINETIC_HIT_DAMAGE);
	t->SetPosition(Position);
	t->SetProtection(ProtectionID,ProtectionTime);
	t->SetStatistic(GetOwnerID(),GetArcaneID(),GetArcaneOwner(),GetObjectID());
	t->Start();

	t = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_SOUND_SPACE_IMPULSE);
	t->SetStartTime(fxlabClientD->GetTime());
	t->SetSoundID(EFF_KINETIC_HIT);
	t->SetPosition(Position);
	t->Start();
};

//--------------------------------

void fxlabArcaneFireBall::InterruptAction()
{
	Body *n;
	OwnerProtection* op;
	fxlabGeneralObjectType* t;
	Vect3f v;
	float d;

	Position = Owner->R();
	v = Owner->Yglobal();
	d = Owner->length() * 0.5f;
	Position.x = XCYCL(round(Position.x + v.x * d));
	Position.y = YCYCL(round(Position.y + v.y * d));
	Position.z = Position.z + v.z * d + 12.0f;

	ArcaneStatisticsService::setCurrentArcane(*this);
	net_fx_set_pending_body("SmartFireBall");
	net_fx_set_pending_link(FXLAB_CLIENT_PROCESS_FIRE_BALL_LINK);
	n = Mdisp->createBody("SmartFireBall",Position,Owner->psi() * 180.0f / M_PI);
	
	op = dynamic_cast<OwnerProtection*>(n);
	if(op) 
		op->set_owner(ProtectionID,ProtectionTime);

	t = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_FIRE_BALL_LINK);
	t->SetStartTime(fxlabClientD->GetTime());
//!!!!!
//	t->SetKeyID(FXLAB_ID_KEY_FIREBALL_LINK);
	t->SetPosition(Position);
	t->SetBody(n);
	t->Start();

/*	t = fxlabServerD->CreateObject(FXLAB_SERVER_PROCESS_BODY_LINK);
	t->SetStartTime(fxlabServerD->GetTime());
	t->SetKeyID(FXLAB_ID_KEY_SERVER_FIREBALL_LINK);
	t->SetPosition(Position);
	t->SetBody(n);
	t->SetProtection(ProtectionID,ProtectionTime);
	t->SetStatistic(GetOwnerID(),GetArcaneID(),GetArcaneOwner(),GetObjectID());
	t->Start();*/

	t = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_SOUND_SPACE_IMPULSE);
	t->SetStartTime(fxlabClientD->GetTime());
	t->SetSoundID(EFF_FIREBALLS);
	t->SetPosition(Position);
	t->Start();
};

//--------------------

void fxlabArcaneLightningBolt::InterruptAction()
{
	Body *n;
	OwnerProtection* op;
	fxlabGeneralObjectType* t;
	Vect3f v;
	float d;

	Position = Owner->R();
	v = Owner->Yglobal();
	d = Owner->length() * 0.5f;
	Position.x = XCYCL(round(Position.x + v.x * d));
	Position.y = YCYCL(round(Position.y + v.y * d));
	Position.z = Position.z + v.z * d + 12.0f;

	ArcaneStatisticsService::setCurrentArcane(*this);
	net_fx_set_pending_body("SmartLightningBolt");
	net_fx_set_pending_link(FXLAB_CLIENT_PROCESS_LIGHTNING_BOLT_LINK);
	n = Mdisp->createBody("SmartLightningBolt",Position,Owner->psi() * 180.0f / M_PI);
	
	op = dynamic_cast<OwnerProtection*>(n);
	if(op)
		op->set_owner(ProtectionID,ProtectionTime);

	t = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_LIGHTNING_BOLT_LINK);
	t->SetStartTime(fxlabClientD->GetTime());
//!!!!!
//	t->SetKeyID(FXLAB_ID_KEY_LIGHTNING_BOLT_LINK);
	t->SetPosition(Position);
	t->SetBody(n);
	t->Start();

/*	t = fxlabServerD->CreateObject(FXLAB_SERVER_PROCESS_BODY_LINK);
	t->SetStartTime(fxlabServerD->GetTime());
	t->SetKeyID(FXLAB_ID_KEY_SERVER_LIGHTNING_BOLT_LINK);
	t->SetPosition(Position);
	t->SetBody(n);
	t->SetProtection(ProtectionID,ProtectionTime);
	t->SetStatistic(GetOwnerID(),GetArcaneID(),GetArcaneOwner(),GetObjectID());
	t->Start();*/

	t = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_SOUND_SPACE_IMPULSE);
	t->SetStartTime(fxlabClientD->GetTime());
	t->SetSoundID(EFF_LIGHT_BOLT);
	t->SetPosition(Position);
	t->Start();
};

//--------------------

/*void fxlabArcaneJumpBall::InterruptAction()
{
	fxlabGeneralObjectType* t;
	if(Owner->completed()){
		ArcaneStatisticsService::setCurrentArcane(*this);
		Owner->throwTitaniumBall(titanium_ball_damage);

		t = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_SOUND_SPACE_IMPULSE);
		t->SetStartTime(fxlabClientD->GetTime());
		t->SetSoundID(EFF_TITANIUM_BALLS);
		t->SetPosition(Position);
		t->Start();
	};
};*/

void fxlabArcaneJumpBall::Start()
{
	fxlabGeneralObjectType* t;

	fxlabImpulseArcane::Start();

	if(Owner->completed()){
		Position = Owner->R();

		ArcaneStatisticsService::setCurrentArcane(*this);
		Owner->throwTitaniumBall(titanium_ball_damage);

		t = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_SOUND_SPACE_IMPULSE);
		t->SetStartTime(fxlabClientD->GetTime());
		t->SetSoundID(EFF_TITANIUM_BALLS);
		t->SetPosition(Position);
		t->Start();
	};
};

//----------------------

void fxlabArcaneSpiderSpawn::InterruptAction()
{
	Body* b;
	OwnerProtection* op;
	fxlabGeneralObjectType* t;

	if(Owner->completed()){		
		ArcaneStatisticsService::setCurrentArcane(*this);
		net_fx_set_pending_body("LittleSpider");
		b = Mdisp->createBody("LittleSpider",Owner->part_coords(M3D_LB_WHEEL) + Vect3f(0,0,FXLAB_REALOBJECT_OFFSET_Z),random(360));
		op = dynamic_cast<OwnerProtection*>(b);
		if(op) op->set_owner(ProtectionID,ProtectionTime);

		ArcaneStatisticsService::setCurrentArcane(*this);
		net_fx_set_pending_body("LittleSpider");
		b = Mdisp->createBody("LittleSpider",Owner->part_coords(M3D_RB_WHEEL) + Vect3f(0,0,FXLAB_REALOBJECT_OFFSET_Z),random(360));
		op = dynamic_cast<OwnerProtection*>(b);
		if(op) op->set_owner(ProtectionID,ProtectionTime);

		t = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_SOUND_SPACE_IMPULSE);
		t->SetStartTime(fxlabClientD->GetTime());
		t->SetSoundID(EFF_SPIDERS_ATTACK);
		t->SetPosition(Position);
		t->Start();
	};
};

//---------------------

void fxlabArcaneCaterpillarSpawn::InterruptAction()
{
	Body* b;
	OwnerProtection* op;
	fxlabGeneralObjectType* t;

	if(Owner->completed()){		
		ArcaneStatisticsService::setCurrentArcane(*this);
		b = Mdisp->createBody("Caterpillar",Owner->part_coords(M3D_LB_WHEEL) + Vect3f(0,0,FXLAB_REALOBJECT_OFFSET_Z),random(360));
		op = dynamic_cast<OwnerProtection*>(b);
		if(op) op->set_owner(ProtectionID,ProtectionTime);

		ArcaneStatisticsService::setCurrentArcane(*this);
		b = Mdisp->createBody("Caterpillar",Owner->part_coords(M3D_RB_WHEEL) + Vect3f(0,0,FXLAB_REALOBJECT_OFFSET_Z),random(360));
		op = dynamic_cast<OwnerProtection*>(b);
		if(op) op->set_owner(ProtectionID,ProtectionTime);

		t = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_SOUND_SPACE_IMPULSE);
		t->SetStartTime(fxlabClientD->GetTime());
		t->SetSoundID(EFF_CATERPILLAR);
		t->SetPosition(Position);
		t->Start();
	};
};

//-------------------------------------

void fxlabArcaneMassShifter::Start()
{
	fxlabGeneralObjectType* t;

	fxlabImpulseArcane::Start();
	Position = Owner->R();

	t = fxlabServerD->CreateObject(FXLAB_SERVER_PROCESS_MASS_SHIFTER);
	t->SetStartTime(fxlabServerD->GetTime());
	t->SetPosition(Position);
	t->SetMechosPoint(Owner);
	t->Start();
};

//------------------------------

void fxlabExternalPowerArcane::Start()
{
	fxlabGeneralArcaneType::Start();
	LinkPoint->Power = 0;
	LinkPoint->Status = 1;
};

void fxlabExternalPowerArcane::PowerUse()
{
	PowerTime -= 1.0f;
	if(PowerTime <= 0)
		PowerZero();
};

void fxlabExternalPowerArcane::LinkPowerZero()
{
};

void fxlabExternalPowerArcane::ActionZero()
{
	SetAlive(0);
};

void fxlabExternalPowerArcane::PowerZero()
{
	SetAlive(0);
};

//------------------------------------------

void fxlabArcaneFireStone::Start()
{
	fxlabGeneralObjectType* t;

	fxlabExternalPowerArcane::Start();

	Position = Owner->R();
	Velocity = Owner->Yglobal() * 2.0f;
	Velocity += Owner->Zglobal();
	Velocity.Normalize(40.0f);
//	Velocity += Owner->Yglobal() * Owner->actualSpeed() / 20.0f;

	t = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_BULLET_FIRE_MODEL);
	t->SetStartTime(fxlabClientD->GetTime());
	t->SetKeyID(FXLAB_ID_KEY_FIRE_BULLET_MODEL);
	t->SetPosition(Position);
	t->SetVelocity(Velocity);
	t->SetProcessInterface(&BulletPoint);
	t->Start();

	t = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_SOUND_SPACE_IMPULSE);
	t->SetStartTime(fxlabClientD->GetTime());
	t->SetSoundID(EFF_STAR_SHOWER);
	t->SetPosition(Position);
	t->Start();

	BulletTimer.start(400);
};

void fxlabArcaneFireStone::Close()
{
	if(CloudPoint.Process)
		CloudPoint.Process->SetAlive(0);

	if(BulletPoint.Process)
		BulletPoint.Process->SetAlive(0);

	fxlabExternalPowerArcane::Close();
};

/*void fxlabArcaneFireStone::InterruptAction()
{
//!!!!
	if(!BulletTimer()){
		fxlabCreateSplinter<fxlabFireBolide>(5,5,Position,7,fxlabServerRND.UnitRND()*M_PI,M_PI*0.5f,0,50.0f * fxlabServerRND.UnitRND(),0,
			FXLAB_CLIENT_PROCESS_BOLIDE_LINK,
			static_cast<ArcaneStatisticsInfo*>(this),static_cast<fxlabProtectionType*>(this),NULL);
	};
};*/

const float FXLAB_FIRE_STONE_RADIUS = 70.0f;

void fxlabArcaneFireStone::Quant()
{
	float a,r;
	fxlabGeneralObjectType* t;

	fxlabExternalPowerArcane::Quant();

	if(!BulletTimer()){
		if(BulletPoint.Process){
			BulletPoint.Process->SetAlive(0);

			Position += Velocity * (CurrentTime * fxlabGlobalTimeRate);
			Position = CYCLE(Position);

			t = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_TWISTER);
			t->SetStartTime(fxlabClientD->GetTime());
			t->SetKeyID(FXLAB_ID_KEY_FIRE_CLOUDS);
			t->SetPosition(Position);
			t->SetProcessInterface(&CloudPoint);
			t->Start();
		};

		a = fxlabServerRND.UnitRND() * M_PI * 2.0f;
		r = fxlabServerRND.UnitRND() * FXLAB_FIRE_STONE_RADIUS;
		//!!!!!!
		if(!fxlabServerRND.RND(2)){
			fxlabCreateBolide<fxlabFireBolide>(FXLAB_MODEL_INTERFACE_ID_BOLIDE_START,FXLAB_MODEL_INTERFACE_ID_BOLIDE_NOISE,Vect3f(XCYCL(round(Position.x + cosf(a) * r)),YCYCL(round(Position.y + sinf(a) * r)),Position.z),Vect3f(0,0,0),25.0f,
				FXLAB_CLIENT_PROCESS_BOLIDE_LINK,
				static_cast<ArcaneStatisticsInfo*>(this),static_cast<fxlabProtectionType*>(this),NULL);
		};
	};
};

//------------------------------------------

const float FXLAB_SNOW_FALL_RADIUS = 70.0f;

void fxlabArcaneSnowFall::Start()
{
	fxlabGeneralObjectType* t;

	fxlabExternalPowerArcane::Start();

	Position = Owner->R();
	Velocity = Owner->Yglobal() * 1.8f;
	Velocity += Owner->Zglobal();
	Velocity.Normalize(40.0f);
//	Velocity += Owner->Yglobal() * Owner->actualSpeed() / 20.0f;

	t = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_BULLET_SNOW_MODEL);
	t->SetStartTime(fxlabClientD->GetTime());
	t->SetKeyID(FXLAB_ID_KEY_SNOW_BULLET_MODEL);
	t->SetPosition(Position);
	t->SetVelocity(Velocity);
	t->SetProcessInterface(&BulletPoint);
	t->Start();

	t = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_SOUND_SPACE_IMPULSE);
	t->SetStartTime(fxlabClientD->GetTime());
	t->SetSoundID(EFF_SNOW_FALL);
	t->SetPosition(Position);
	t->Start();

	BulletTimer.start(400);
};

void fxlabArcaneSnowFall::Close()
{
	if(CloudPoint.Process)
		CloudPoint.Process->SetAlive(0);

	if(SoundPoint.Process)
		SoundPoint.Process->SetAlive(0);

	if(BulletPoint.Process)
		BulletPoint.Process->SetAlive(0);

	fxlabExternalPowerArcane::Close();
};

void fxlabArcaneSnowFall::Quant()
{
	float a,r;
	fxlabGeneralObjectType* t;

	fxlabExternalPowerArcane::Quant();

	if(!BulletTimer()){
		if(BulletPoint.Process){
			BulletPoint.Process->SetAlive(0);

			Position += Velocity * (CurrentTime * fxlabGlobalTimeRate);
			Position = CYCLE(Position);

			t = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_TWISTER);
			t->SetStartTime(fxlabClientD->GetTime());
			t->SetKeyID(FXLAB_ID_KEY_SNOW_CLOUDS);
			t->SetPosition(Position);
			t->SetProcessInterface(&CloudPoint);
			t->Start();

			t = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_SOUND_SPACE_LONG);
			t->SetStartTime(fxlabClientD->GetTime());
			t->SetSoundID(EFF_SNOW_FALL_ADD);
			t->SetPosition(Position);
			t->SetProcessInterface(&SoundPoint);
			t->Start();
		};

//		if(!fxlabServerRND.RND(2)){
			a = fxlabServerRND.UnitRND() * M_PI * 2.0f;
			r = fxlabServerRND.UnitRND() * FXLAB_SNOW_FALL_RADIUS;
		//!!!!!!
			fxlabCreateBolide<fxlabSnowBall>(FXLAB_MODEL_INTERFACE_ID_SNOW_START,FXLAB_MODEL_INTERFACE_ID_SNOW_NOISE,Vect3f(XCYCL(round(Position.x + cosf(a) * r)),YCYCL(round(Position.y + sinf(a) * r)),Position.z),Vect3f(0,0,0),25.0f,
				FXLAB_CLIENT_PROCESS_SNOW_FALL_LINK,
				static_cast<ArcaneStatisticsInfo*>(this),static_cast<fxlabProtectionType*>(this),NULL);
//		};
	};
};

//--------------------------

void fxlabArcaneElecricStorm::Start()
{
	fxlabGeneralObjectType* t;

	fxlabExternalPowerArcane::Start();
	Position = Owner->R();

	Position.z = fxlabGetLevel(XCYCL(round(Position.x)),YCYCL(round(Position.y)),Position.z + Owner->radius());

	t = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_CONTROL_OMNI);
	t->SetStartTime(fxlabClientD->GetTime());
	t->SetKeyID(FXLAB_ID_KEY_ELECTRIC_STORM_MARKER_OMNI);
	t->SetPosition(Position);
	t->SetProcessInterface(&MarkerPoint);
	t->Start();

	Position.z += 150.0f;

	t = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_TWISTER);
	t->SetStartTime(fxlabClientD->GetTime());
	t->SetKeyID(FXLAB_ID_KEY_STORM_CLOUDS);
	t->SetPosition(Position);
	t->SetProcessInterface(&CloudPoint);
	t->Start();

	t = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_SOUND_SPACE_IMPULSE);
	t->SetStartTime(fxlabClientD->GetTime());
	t->SetSoundID(EFF_THUNDERSTORM_ADD);
	t->SetPosition(Position);
	t->Start();	
};

void fxlabArcaneElecricStorm::Close()
{
	if(CloudPoint.Process)
		CloudPoint.Process->SetAlive(0);
	if(MarkerPoint.Process)
		MarkerPoint.Process->SetAlive(0);

	fxlabExternalPowerArcane::Close();
};

void fxlabArcaneElecricStorm::InterruptAction()
{
	fxlabGeneralObjectType* t;
	Vect3f v;

	v = Position;
	v.z -= 150.0f;

	t = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_LIGHTNING_LINE);
	t->SetStartTime(fxlabClientD->GetTime());
	t->SetKeyID(FXLAB_ID_KEY_ELECTRIC_STORM_BOW);
	t->SetPosition(v);
	t->SetVelocity(Vect3f(0,0,1.0f));
	t->Start();

	t = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_CONTROL_OMNI);
	t->SetStartTime(fxlabClientD->GetTime());
	t->SetKeyID(FXLAB_ID_KEY_ELECTRIC_STORM_LIGHT);
	t->SetPosition(v);
	t->Start();

	t = fxlabServerD->CreateObject(FXLAB_SERVER_PROCESS_DAMAGE_SENSOR);
	t->SetStartTime(fxlabServerD->GetTime());
	t->SetKeyID(FXLAB_ID_KEY_ELECTRIC_STORM_DAMAGE);
	t->SetPosition(v);
	t->SetProtection(ProtectionID,ProtectionTime);
	t->SetStatistic(GetOwnerID(),GetArcaneID(),GetArcaneOwner(),GetObjectID());
	t->Start();

	t = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_SOUND_SPACE_IMPULSE);
	t->SetStartTime(fxlabClientD->GetTime());
	t->SetSoundID(EFF_THUNDERSTORM);
	t->SetPosition(Position);
	t->Start();

	t = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_FEAR_STREAM);
	t->SetKeyID(FXLAB_ID_KEY_STORM_SPARK);
	t->SetStartTime(fxlabClientD->GetTime());
	t->SetPosition(Vect3f(Position.x,Position.y,Position.z - 150.0f));
	t->SetVelocity(Vect3f(0,0,0));
	t->Start();

};


//------------------------------------------

void fxlabArcaneFireGarden::Start()
{
	int i;
	Vect3f v,dv;
	fxlabGeneralObjectType* t;

	fxlabExternalPowerArcane::Start();

	Position = Owner->R();
	Position.z = fxlabGetLevel(XCYCL(round(Position.x)),YCYCL(round(Position.y)),Position.z + Owner->radius());
	fxlabGetArcanePosition(Owner,Vect3f(0,Owner->radius()*1.5f,0),Position);

	v = Owner->Yglobal() * FXLAB_FIRE_GARDEN_POSITION;
	v += Owner->Xglobal() * FXLAB_FIRE_GARDEN_ROSE_LEN * 0.5f;
	dv = Owner->Xglobal() * FXLAB_FIRE_GARDEN_ROSE_LEN / (float)(-FXLAB_FIRE_GARDEN_ROSE_NUM);
	v += Position;

	for(i = 0;i < FXLAB_FIRE_GARDEN_ROSE_NUM;i++){
		CYCLE(v);
		v += dv;
		v.z = fxlabGetLevel(v.x,v.y,v.z + Owner->radius());
	
		t = fxlabServerD->CreateObject(FXLAB_SERVER_PROCESS_DAMAGE_SENSOR);
		t->SetStartTime(fxlabServerD->GetTime());
		t->SetKeyID(FXLAB_ID_KEY_FIRE_GARDEN_DAMAGE);
		t->SetPosition(v);
		t->SetProtection(ProtectionID,ProtectionTime);
		t->SetStatistic(GetOwnerID(),GetArcaneID(),GetArcaneOwner(),GetObjectID());
		t->SetProcessInterface(&ServerPoint[i]);
		t->Start();

		t = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_OBJECT_SET);
		t->SetStartTime(fxlabClientD->GetTime());
		t->SetKeyID(FXLAB_ID_KEY_ROSE_FIRE_SET);
		t->SetPosition(v);
		t->SetProcessInterface(&ClientPoint[i]);
		t->Start();
	};

	t = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_SOUND_SPACE_IMPULSE);
	t->SetStartTime(fxlabClientD->GetTime());
	t->SetSoundID(EFF_LURID_BILLOW);
	t->SetPosition(Position);
	t->Start();

	t = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_SOUND_SPACE_LONG);
	t->SetStartTime(fxlabClientD->GetTime());
	t->SetSoundID(EFF_LURID_BILLOW_ADD);
	t->SetPosition(Position);
	t->SetProcessInterface(&SoundPoint);
	t->Start();
};

void fxlabArcaneFireGarden::Close()
{
	int i;
	for(i = 0;i < FXLAB_FIRE_GARDEN_ROSE_NUM;i++){	
		if(ClientPoint[i].Process)
			ClientPoint[i].Process->SetAlive(0);
		if(ServerPoint[i].Process)
			ServerPoint[i].Process->SetAlive(0);
	};

	if(SoundPoint.Process)
		SoundPoint.Process->SetAlive(0);

	fxlabExternalPowerArcane::Close();
};

//-----------------------------------------

	
void fxlabArcaneRageSlime::Start()
{
	fxlabGeneralObjectType* t;

	fxlabExternalPowerArcane::Start();

	Position = Owner->R();
	Position.z = fxlabGetLevel(XCYCL(round(Position.x)),YCYCL(round(Position.y)),Owner->R().z + Owner->radius());
	fxlabGetArcanePosition(Owner,Vect3f(0,Owner->radius()*1.5f,0),Position);

	t = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_SLIME_MODEL);
	t->SetStartTime(fxlabClientD->GetTime());
	t->SetKeyID(FXLAB_ID_KEY_RAGE_SLIME_MODEL);
	t->SetPosition(Position);
	t->SetMatrix(getPlacementPose(Position));
	t->SetProcessInterface(&ClientPoint);
	t->Start();

	t = fxlabServerD->CreateObject(FXLAB_SERVER_PROCESS_RAGE_SLIME_CIRCLE);
	t->SetStartTime(fxlabServerD->GetTime());
	t->SetKeyID(FXLAB_ID_KEY_RAGE_SLIME_CIRCLE);
	t->SetPosition(Position);
	t->SetProcessInterface(&ServerPoint);
	t->SetProtection(ProtectionID,ProtectionTime);
	t->SetStatistic(GetOwnerID(),GetArcaneID(),GetArcaneOwner(),GetObjectID());
	t->Start();

	t = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_SOUND_SPACE_LONG);
	t->SetStartTime(fxlabClientD->GetTime());
	t->SetPosition(Position);
	t->SetSoundID(EFF_SLIME_FURY);
	t->SetProcessInterface(&SoundPoint);
	t->Start();

	t = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_SOUND_SPACE_IMPULSE);
	t->SetStartTime(fxlabClientD->GetTime());
	t->SetSoundID(EFF_SLIME_FURY_ADD);
	t->SetPosition(Position);
	t->Start();

	WaitTimer.start(1500);
};


void fxlabArcaneRageSlime::Close()
{
	if(ServerPoint.Process)
		ServerPoint.Process->SetAlive(0);

	if(ClientPoint.Process)
		ClientPoint.Process->SetAlive(0);

	if(SoundPoint.Process)
		SoundPoint.Process->SetAlive(0);

	fxlabExternalPowerArcane::Close();
};

void fxlabArcaneRageSlime::Quant()
{
	fxlabExternalPowerArcane::Quant();
	if(!WaitTimer && (!fxlabGetWorldReflectionEnable()) && Position.z < vMap->LevelH2O && (GetAt(XCYCL(round(Position.x)),YCYCL(round(Position.y))) & At_WATER))
		SetAlive(0);
};

//-------------------------------------------

void fxlabArcaneFuryFungus::Open()
{
	fxlabExternalPowerArcane::Open();
	CheckLife = 0;
};

void fxlabArcaneFuryFungus::Start()
{
	int i;
	float a,r;
	float da;
	Vect3f v;
	fxlabGeneralObjectType* t;

	fxlabExternalPowerArcane::Start();

	Position = Owner->R();
	Position.z += Owner->radius();
	fxlabGetArcanePosition(Owner,Vect3f(0,Owner->radius()*1.5f,0),Position);

	a = fxlabClientRND.UnitRND() * M_PI * 2.0f;
	da = M_PI * 2.0f / 7.0f;
	r = FXLAB_FURY_FUNGUS_RADIUS;

	for(i = 0;i < FXLAB_FURY_FUNGUS_NUM;i++){
		if(i < 7){
			v = Position;
			v += Vect3f(r * cosf(a),r * sinf(a),0);
			CYCLE(v);
			v.z = fxlabGetLevel(v.x,v.y,v.z + Owner->radius()) + 10.0f;
			a += da;

			t = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_LASH_MODEL);
			t->SetStartTime(fxlabClientD->GetTime());
			t->SetKeyID(FXLAB_ID_KEY_FURY_FUNGUS_LASH);
			t->SetPosition(v);
			t->SetProcessInterface(&LashPoint[i]);
			t->Start();
		}else{
			v = Position;
			a = fxlabClientRND.UnitRND() * M_PI * 2.0f;
			r = FXLAB_FURY_FUNGUS_RADIUS * fxlabClientRND.UnitRND();
			v += Vect3f(r * cosf(a),r * sinf(a),0);
			CYCLE(v);
			v.z = fxlabGetLevel(v.x,v.y,v.z + Owner->radius()) + 10.0f;

			t = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_LASH_MODEL);
			t->SetStartTime(fxlabClientD->GetTime());
			t->SetKeyID(FXLAB_ID_KEY_FURY_FUNGUS_LASH);
			t->SetPosition(v);
			t->SetProcessInterface(&LashPoint[i]);
			t->Start();
		};
	};

	v = Position;
	CYCLE(v);
	v.z = fxlabGetLevel(v.x,v.y,v.z + Owner->radius());

	t = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_SPARK_GENERATOR);
	t->SetStartTime(fxlabClientD->GetTime());
	t->SetKeyID(FXLAB_ID_KEY_FURY_FUNGUS_FOG);
	t->SetPosition(v);
	t->SetProcessInterface(&FogPoint);
	t->Start();

	t = fxlabServerD->CreateObject(FXLAB_SERVER_PROCESS_WILD_THICKET_CIRCLE);
	t->SetStartTime(fxlabServerD->GetTime());
	t->SetKeyID(FXLAB_ID_KEY_FURY_FUNGUS_CIRCLE);
	t->SetPosition(v);
	t->SetProcessInterface(&CirclePoint);
	t->SetProtection(ProtectionID,ProtectionTime);
	t->SetStatistic(GetOwnerID(),GetArcaneID(),GetArcaneOwner(),GetObjectID());
	t->Start();

	t = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_SOUND_SPACE_LONG);
	t->SetStartTime(fxlabClientD->GetTime());
	t->SetSoundID(EFF_WILD_FUNGUS);
	t->SetPosition(v);
	t->SetProcessInterface(&SoundPoint);
	t->Start();
};

void fxlabArcaneFuryFungus::Close()
{
	int i;
	for(i = 0;i < FXLAB_FURY_FUNGUS_NUM;i++){
		if(LashPoint[i].Process)
			LashPoint[i].Process->SetAlive(0);
	};

	if(FogPoint.Process)
		FogPoint.Process->SetAlive(0);

	if(CirclePoint.Process)
		CirclePoint.Process->SetAlive(0);

	if(SoundPoint.Process)
		SoundPoint.Process->SetAlive(0);

	fxlabExternalPowerArcane::Close();
};

void fxlabArcaneFuryFungus::ArcaneAction()
{
	int i;
	if(!CheckLife){
		for(i = 0;i < FXLAB_FURY_FUNGUS_NUM;i++){
			if(LashPoint[i].Process)
				LashPoint[i].Process->SetScaleAnimation(1.0f,8.0f,2000);
		};

		LifeTimer.start(4000);
		CheckLife = 1;
	};
};

void fxlabArcaneFuryFungus::Quant()
{
//	int i;
	fxlabExternalPowerArcane::Quant();

	if(CheckLife && !LifeTimer())
		SetAlive(0);

	if((!fxlabGetWorldReflectionEnable()) && Position.z < vMap->LevelH2O && (GetAt(XCYCL(round(Position.x)),YCYCL(round(Position.y))) & At_WATER))
		SetAlive(0);

/*	for(i = 0;i < FXLAB_FURY_FUNGUS_NUM;i++){
		if(LashPoint[i].Process)
			break;
	};
	if(i >= FXLAB_FURY_FUNGUS_NUM)
		SetAlive(0);*/
};

//-----------------------------------------

void fxlabArcaneShiftTrap::Start()
{
	fxlabGeneralObjectType* t;
	fxlabExternalPowerArcane::Start();

	Position = Owner->R();
	Position.z = fxlabGetLevel(XCYCL(round(Position.x)),YCYCL(round(Position.y)),Owner->R().z + Owner->radius());
	fxlabGetArcanePosition(Owner,Vect3f(0,Owner->radius()*1.5f,0),Position);

	t = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_GROUND_MODEL);
	t->SetStartTime(fxlabClientD->GetTime());
	t->SetKeyID(FXLAB_ID_KEY_SHIFT_TRAP_MODEL);
	t->SetPosition(Position);
	MatXf pose = getPlacementPose(Position);
	t->SetMatrix(pose);
	Position = pose.trans();
	t->SetProcessInterface(&ClientPoint);
	t->Start();

	t = fxlabServerD->CreateObject(FXLAB_SERVER_PROCESS_TRAP_SPHERE);
	t->SetStartTime(fxlabServerD->GetTime());
	t->SetKeyID(FLXAB_ID_KEY_SHIFT_TRAP_SPHERE);
	t->SetPosition(Position);
	t->SetProcessInterface(&ServerPoint);
	t->SetProtection(ProtectionID,ProtectionTime);
	t->SetStatistic(GetOwnerID(),GetArcaneID(),GetArcaneOwner(),GetObjectID());
	t->Start();

	t = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_SOUND_SPACE_LONG);
	t->SetStartTime(fxlabClientD->GetTime());
	t->SetSoundID(EFF_TELEPORT);
	t->SetPosition(Position);
	t->SetProcessInterface(&SoundPoint);
	t->Start();
};

void fxlabArcaneShiftTrap::Close()
{
	if(ServerPoint.Process)
		ServerPoint.Process->SetAlive(0);
	if(ClientPoint.Process)
		ClientPoint.Process->SetAlive(0);
	if(SoundPoint.Process)
		SoundPoint.Process->SetAlive(0);
	fxlabExternalPowerArcane::Close();
};

//------------------------------------

void fxlabExternalMindArcane::Open()
{
	fxlabGeneralArcaneType::Open();
	InterruptCount = 0;
	InterruptEnable = 1;
	ExternalTime = 1;
};

void fxlabExternalMindArcane::Start()
{
	fxlabGeneralArcaneType::Start();
	PowerTime = ExternalTime / fxlabServerQuantPeriod;
};

void fxlabExternalMindArcane::Condition()
{
	if(!Owner->completed()){
		if(InterruptEnable){
			LinkPoint->Status = 1;
			InterruptEnable = 0;
		};
	};
};

void fxlabExternalMindArcane::InterruptUse()
{
	if(InterruptEnable){
		InterruptTime += 1.0f;
		while(InterruptTime >= InterruptBorder){
			InterruptAction();
			InterruptCount++;
			if(InterruptCount >= InterruptNum){
				LinkPoint->Status = 1;
				InterruptEnable = 0;
				break;
			};
			InterruptTime -= InterruptBorder;
		};
	};
};

void fxlabExternalMindArcane::PowerUse()
{
	if(InterruptEnable){
		LinkPoint->Power -= DeltaPower;
		if(LinkPoint->Power <= 0)
			LinkPowerZero();
	};

	PowerTime -= 1.0f;
	if(PowerTime <= 0)
		PowerZero();
};


void fxlabExternalMindArcane::LinkPowerZero()
{
	if(InterruptEnable){
		LinkPoint->Power = 0;
		LinkPoint->Status = 1;
		InterruptEnable = 0;
	};
};

void fxlabExternalMindArcane::ActionZero()
{
	SetAlive(0);
};

void fxlabExternalMindArcane::PowerZero()
{
	SetAlive(0);
};

void fxlabExternalMindArcane::Close()
{
	if(InterruptEnable){
		LinkPoint->Status = 1;
		InterruptEnable = 0;
	};
	fxlabGeneralArcaneType::Close();
};

//-----------------------------------------------------

void fxlabArcaneLightningSeed::Start()
{
	fxlabExternalMindArcane::Start();
	if(Owner->completed())
		Position = Owner->part_coords(M3D_ENGINE);
	ServerPoint = new fxlabProcessInterface[InterruptNum];
};

void fxlabArcaneLightningSeed::Close()
{
	int i;
	for(i = 0;i < InterruptNum;i++){
		if(ServerPoint[i].Process)
			ServerPoint[i].Process->SetAlive(0);
	};
	delete ServerPoint;
	fxlabExternalMindArcane::Close();
};

void fxlabArcaneLightningSeed::Quant()
{
	if(Owner->completed())
		Position = Owner->part_coords(M3D_ENGINE);
	fxlabExternalMindArcane::Quant();
};

void fxlabArcaneLightningSeed::InterruptAction()
{
	fxlabGeneralObjectType* ct;
	fxlabGeneralObjectType* st;

	Position = Owner->R();
	Position.z = fxlabGetLevel(XCYCL(round(Position.x)),YCYCL(round(Position.y)),Owner->R().z + Owner->radius());
	fxlabGetArcanePosition(Owner,Vect3f(0,Owner->radius()*1.5f,0),Position);

	ct = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_LIGHTNING_SEED);
	ct->SetStartTime(fxlabClientD->GetTime());
	ct->SetKeyID(FXLAB_ID_KEY_LIGHTNING_SEEDS);
	ct->SetPosition(Position + Vect3f(0,0,Owner->radius() * 0.33f));
	ct->Start();

	st = fxlabServerD->CreateObject(FXLAB_SERVER_PROCESS_LIGHTNING_SEED_DAMAGE);
	st->SetStartTime(fxlabServerD->GetTime());
	st->SetKeyID(FXLAB_ID_KEY_LIGHTNING_SEEDS_DAMAGE);
	st->SetPosition(Position + Vect3f(0,0,Owner->radius() * 0.33f));
	st->SetRemoteObject(ct);
	st->SetProtection(ProtectionID,ProtectionTime);
	st->SetStatistic(GetOwnerID(),GetArcaneID(),GetArcaneOwner(),GetObjectID());
	st->SetProcessInterface(&ServerPoint[InterruptCount]);
	st->Start();
};

void fxlabArcaneLightningSeed::Condition()
{
	int i,cnt;

	fxlabExternalMindArcane::Condition();
	if(!InterruptEnable){
		cnt = 0;
		for(i = 0;i < InterruptNum;i++){
			if(ServerPoint[i].Process)
				cnt++;
		};
		if(!cnt)
			SetAlive(0);
	};
};

//---------------------------------------------------------

void fxlabArcaneCrystalSpike::Start()
{
	fxlabExternalMindArcane::Start();
	if(Owner->completed())
		Position = Owner->part_coords(M3D_ENGINE);
	SpikePoint.reserve(InterruptNum);
};

void fxlabArcaneCrystalSpike::Close()
{
	std::vector<ShareHandle<Body> >::iterator it;

	FOR_EACH(SpikePoint, it){
		if(*it && (*it)->alive())
			(*it) -> kill();
	};
	SpikePoint.clear();
	fxlabExternalMindArcane::Close();
};

void fxlabArcaneCrystalSpike::Quant()
{
	if(Owner->completed())
		Position = Owner->part_coords(M3D_ENGINE);
	fxlabExternalMindArcane::Quant();
};

void fxlabArcaneCrystalSpike::InterruptAction()
{
	Body* p;
	OwnerProtection* op;
	fxlabGeneralObjectType* ct;

	Position = Owner->R();
	fxlabGetArcanePosition(Owner,Vect3f(0,Owner->radius()*1.5f,0),Position);

	ArcaneStatisticsService::setCurrentArcane(*this);
	net_fx_set_pending_body("Crystal");
	p = Mdisp -> createBody("Crystal",Position + Vect3f(0,0,FXLAB_REALOBJECT_OFFSET_Z));
	op = dynamic_cast<OwnerProtection*>(p);
	if(op) op->set_owner(ProtectionID,ProtectionTime);
//	Mdisp -> attachBody(p);	

	SpikePoint.push_back(p);

	ct = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_SOUND_SPACE_IMPULSE);
	ct->SetStartTime(fxlabClientD->GetTime());
	ct->SetSoundID(EFF_CRYSTALLIZATION);
	ct->SetPosition(Position);
	ct->Start();
};

void fxlabArcaneCrystalSpike::Condition()
{
	int cnt;
	std::vector<ShareHandle<Body> >::iterator it;

	fxlabExternalMindArcane::Condition();
	if(!InterruptEnable){
		cnt = 0;
		FOR_EACH(SpikePoint,it){
			if(*it && (*it) -> alive())
				cnt++;
		};
		
		if(!cnt)
			SetAlive(0);
	};
};

//----------------------------

void fxlabArcaneBubbleRush::InterruptAction()
{
	Body* p;
	OwnerProtection* op;
	fxlabGeneralObjectType* t;

	Position = Owner->R();
	fxlabGetArcanePosition(Owner,Vect3f(0,Owner->radius()*1.5f,0),Position);

	ArcaneStatisticsService::setCurrentArcane(*this);
	net_fx_set_pending_body("Bubble");
	p = Mdisp -> createBody("Bubble",Position + Vect3f(0,0,FXLAB_REALOBJECT_OFFSET_Z));
	op = dynamic_cast<OwnerProtection*>(p);
	if(op) op->set_owner(ProtectionID,ProtectionTime);
//	Mdisp -> attachBody(p);	

	t = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_SOUND_SPACE_IMPULSE);
	t->SetStartTime(fxlabClientD->GetTime());
	t->SetSoundID(EFF_SOAP_BUBBLE);
	t->SetPosition(Position);
	t->Start();
};

//---------------------------------------------------------

void fxlabArcaneMortarMine::Start()
{
	fxlabExternalMindArcane::Start();
	if(Owner->completed())
		Position = Owner->part_coords(M3D_ENGINE);

	MortarPoint.reserve(InterruptNum);
	ClientPoint = new fxlabProcessInterface[InterruptNum];
};

void fxlabArcaneMortarMine::Close()
{
	int i;
	std::vector<ShareHandle<Body> >::iterator it;

	for(i = 0;i < InterruptNum;i++){
		if(ClientPoint[i].Process)
			ClientPoint[i].Process->SetAlive(0);
	};

	FOR_EACH(MortarPoint, it){
		if(*it && (*it)->alive())
			(*it) -> kill();
	};
	MortarPoint.clear();
	delete ClientPoint;

	fxlabExternalMindArcane::Close();
};

void fxlabArcaneMortarMine::Quant()
{
	if(Owner->completed())
		Position = Owner->part_coords(M3D_ENGINE);
	fxlabExternalMindArcane::Quant();
};

#include "Meteorite.h"

void fxlabArcaneMortarMine::InterruptAction()
{
	Body* p;
	OwnerProtection* op;
	fxlabGeneralObjectType* ct;

	ArcaneStatisticsService::setCurrentArcane(*this);

	p = new fxlabSleepyMortar(M3D_MORTAR,Position,Vect3f(10.0f - fxlabServerRND.UnitRND() * 20.0f,10.0f - fxlabServerRND.UnitRND() * 20.0f,30.0f),33.3f,-1);
	op = dynamic_cast<OwnerProtection*>(p);
	if(op) op->set_owner(ProtectionID,ProtectionTime);
	net_fx_set_pending_link(FXLAB_CLIENT_PROCESS_MORTAR_BODY_LINK);
	Mdisp -> attachBody(p);

	MortarPoint.push_back(p);

	ct = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_MORTAR_BODY_LINK);
	ct->SetStartTime(fxlabClientD->GetTime());
//!!!!!!!
//	ct->SetKeyID(FXLAB_ID_KEY_MINE_CLIENT_LINK);
	ct->SetPosition(Position);
	ct->SetBody(p);
	ct->SetProcessInterface(&ClientPoint[InterruptCount]);
	ct->Start();

	ct = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_SOUND_SPACE_IMPULSE);
	ct->SetStartTime(fxlabClientD->GetTime());
	ct->SetSoundID(EFF_MINES);
	ct->SetPosition(Position);
	ct->Start();
};

void fxlabArcaneMortarMine::Condition()
{
	int cnt;
	std::vector<ShareHandle<Body> >::iterator it;

	fxlabExternalMindArcane::Condition();
	if(!InterruptEnable){
		cnt = 0;
		FOR_EACH(MortarPoint,it){
			if(*it && (*it) -> alive())
				cnt++;
		};
		if(!cnt)
			SetAlive(0);
	};
};

//---------------------------------

void fxlabArcanePetard::Start()
{
	fxlabExternalMindArcane::Start();
	if(Owner->completed())
		Position = Owner->part_coords(M3D_FRONT);
};

void fxlabArcanePetard::Close()
{
	int i;
	std::vector<ShareHandle<Body> >::iterator it;

	for(i = 0;i < FXLAB_PETARD_BONUS_NUM;i++){
		if(ClientPoint[i].Process)
			ClientPoint[i].Process->SetAlive(0);
	};

	FOR_EACH(BonusPoint, it){
		if(*it && (*it)->alive())
			(*it) -> kill();
	};
	BonusPoint.clear();

	if(PetardPoint.Process)
		PetardPoint.Process->SetAlive(0);

	fxlabExternalMindArcane::Close();
};

void fxlabArcanePetard::Quant()
{
	if(Owner->completed())
		Position = Owner->part_coords(M3D_FRONT);
	fxlabExternalMindArcane::Quant();
};

void fxlabArcanePetard::InterruptAction()
{
	Body* p;
	OwnerProtection* op;
	fxlabGeneralObjectType* ct;
	int i;
	float a,da;

	if(InterruptCount){
		if(PetardPoint.Process)
			PetardPoint.Process->SetAlive(0);

		ExplodePosition += Velocity * (CurrentTime * fxlabGlobalTimeRate);
		ExplodePosition = CYCLE(ExplodePosition);

		ct = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_SPARK_GENERATOR);
		ct->SetStartTime(fxlabClientD->GetTime());
		ct->SetPosition(ExplodePosition);
		ct->SetKeyID(FXLAB_ID_KEY_PETARD_BURN);
		ct->Start();

		ct = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_CONTROL_OMNI);
		ct->SetStartTime(fxlabClientD->GetTime());
		ct->SetPosition(ExplodePosition);
		ct->SetKeyID(FXLAB_ID_KEY_PETARD_BURN_OMNI);
		ct->Start();

		ct = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_CONTROL_MODEL);
		ct->SetStartTime(fxlabClientD->GetTime());
		ct->SetPosition(ExplodePosition);
		ct->SetKeyID(FXLAB_ID_KEY_PETARD_BURN_MODEL);
		ct->Start();

		ct = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_SOUND_SPACE_IMPULSE);
		ct->SetStartTime(fxlabClientD->GetTime());
		ct->SetSoundID(EFF_PETARD_EXPLOSION);
		ct->SetPosition(ExplodePosition);
		ct->Start();

		a = fxlabServerRND.UnitRND() * M_PI * 2.0f;
		da = 2.0f * M_PI / FXLAB_PETARD_BONUS_NUM;
		for(i = 0;i < FXLAB_PETARD_BONUS_NUM;i++){
			if(!fxlabServerRND.RND(2)){
				ArcaneStatisticsService::setCurrentArcane(*this);

				p = new fxlabMinorBonus(M3D_TOTAL_TYPE(FXLAB_MODEL_INTERFACE_ID_BOX_START + fxlabServerRND.RND(FXLAB_MODEL_INTERFACE_ID_BOX_NOISE),M3D_XREAL),ExplodePosition,Vect3f(20.0f * cosf(a),20.0f * sinf(a),0.0),0,-1);
				op = dynamic_cast<OwnerProtection*>(p);
				if(op) op->set_owner(ProtectionID,ProtectionTime);
				Mdisp -> attachBody(p);	

				ct = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_BONUS_BODY_LINK);
				ct->SetStartTime(fxlabClientD->GetTime());
//!!!
//				ct->SetKeyID(FXLAB_ID_KEY_MINOR_BONUS_CLIENT_LINK);
				ct->SetPosition(ExplodePosition);
				ct->SetBody(p);
				ct->SetProcessInterface(&ClientPoint[i]);
				ct->Start();
			}else{
				ArcaneStatisticsService::setCurrentArcane(*this);

				p = new fxlabMajorBonus(M3D_TOTAL_TYPE(FXLAB_MODEL_INTERFACE_ID_BOX_START + fxlabServerRND.RND(FXLAB_MODEL_INTERFACE_ID_BOX_NOISE),M3D_XREAL),ExplodePosition,Vect3f(20.0f * cosf(a),20.0f * sinf(a),0.0),0,-1);
				op = dynamic_cast<OwnerProtection*>(p);
				if(op) op->set_owner(ProtectionID,ProtectionTime);
				Mdisp -> attachBody(p);

				ct = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_BONUS_BODY_LINK);
				ct->SetStartTime(fxlabClientD->GetTime());
//!!!!!
//				ct->SetKeyID(FXLAB_ID_KEY_MAJOR_BONUS_CLIENT_LINK);
				ct->SetPosition(ExplodePosition);
				ct->SetBody(p);
				ct->SetProcessInterface(&ClientPoint[i]);
				ct->Start();
			};
			BonusPoint.push_back(p);
			a += da;
		};
	}else{
		Velocity = Owner->Yglobal() * 3.0f;
		Velocity += Owner->Zglobal();
		Velocity.Normalize(20.f);
//		Velocity += Owner->Yglobal() * Owner->actualSpeed() / 20.0f;

		ExplodePosition = Position;
		ct = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_PETARD_MODEL);
		ct->SetStartTime(fxlabClientD->GetTime());
		ct->SetKeyID(FXLAB_ID_KEY_PETARD_MODEL);
		ct->SetPosition(Position);
		ct->SetVelocity(Velocity);
		ct->SetProcessInterface(&PetardPoint);
		ct->Start();

		ct = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_SOUND_SPACE_IMPULSE);
		ct->SetStartTime(fxlabClientD->GetTime());
		ct->SetSoundID(EFF_PETARD_START);
		ct->SetPosition(Position);
		ct->Start();
	};
};

void fxlabArcanePetard::Condition()
{
	int cnt;
	std::vector<ShareHandle<Body> >::iterator it;

	fxlabExternalMindArcane::Condition();
	if(!InterruptEnable){
		cnt = 0;

		FOR_EACH(BonusPoint,it){
			if(*it && (*it) -> alive())
				cnt++;
		};

		if(!cnt)
			SetAlive(0);
	};
};

//------------------------------

const float FXLAB_SCARLET_TRACK_DENSITY = 20.0f;

void fxlabArcaneScarletTrack::Start()
{
	fxlabExternalMindArcane::Start();
	if(Owner->completed())
		Position = Owner->part_coords(M3D_ENGINE);
	LastPosition = Position;
	ServerPoint = new fxlabProcessInterface[InterruptNum];
	ClientPoint = new fxlabProcessInterface[InterruptNum];
};

void fxlabArcaneScarletTrack::Close()
{
	int i;
	for(i = 0;i < InterruptNum;i++){
		if(ServerPoint[i].Process)
			ServerPoint[i].Process->SetAlive(0);
		if(ClientPoint[i].Process)
			ClientPoint[i].Process->SetAlive(0);
	};
	delete ServerPoint;
	delete ClientPoint;
	fxlabExternalMindArcane::Close();
};

void fxlabArcaneScarletTrack::Quant()
{
	if(Owner->completed())
		Position = Owner->part_coords(M3D_ENGINE);
	fxlabExternalMindArcane::Quant();
};

void fxlabArcaneScarletTrack::InterruptUse()
{
	Vect3f v;
	fxlabGeneralObjectType* t;

	if(InterruptEnable){
		if(InterruptCount < InterruptNum){
			v = getDist(LastPosition,Position);
			if(v.norm() > FXLAB_SCARLET_TRACK_DENSITY){
				t = fxlabClientD->CreateObject(FXLAB_CLIENT_PROCESS_SPARK_LINE);
				t->SetStartTime(fxlabClientD->GetTime());
				t->SetPosition(Vect3f(Position.x,Position.y,fxlabGetLevel(XCYCL(round(Position.x)),YCYCL(round(Position.y)),Position.z + Owner->radius())));
				t->SetVelocity(v);
				t->SetKeyID(FXLAB_ID_KEY_SCARLET_TRACK_LINE);
				t->SetProcessInterface(&(ClientPoint[InterruptCount]));
				t->Start();


  				t = fxlabServerD->CreateObject(FXLAB_SERVER_PROCESS_RED_TRACK_CIRCLE);
				t->SetStartTime(fxlabServerD->GetTime());
				t->SetPosition(Vect3f(Position.x,Position.y,fxlabGetLevel(XCYCL(round(Position.x)),YCYCL(round(Position.y)),Position.z + Owner->radius())));
				t->SetKeyID(FXLAB_ID_KEY_SCARLET_TRACK_DAMAGE);
				t->SetProcessInterface(&(ServerPoint[InterruptCount]));
				t->SetProtection(ProtectionID,ProtectionTime);
				t->SetStatistic(GetOwnerID(),GetArcaneID(),GetArcaneOwner(),GetObjectID());
				t->Start();

				LastPosition = Position;
				InterruptCount++;
				if(InterruptCount >= InterruptNum){
					LinkPoint->Status = 1;
					InterruptEnable = 0;
				};
			};
		}else{
			LinkPoint->Status = 1;
			InterruptEnable = 0;
		};
	};
};

//---------------------------------------------------------

void fxlabArcaneRedTrack::Start()
{
	fxlabInternalArcane::Start();
	ArcaneStatisticsService::setCurrentArcane(*this);
	Owner->set_feature(Mechos::RED_TRACK_FEATURE);
};

void fxlabArcaneRedTrack::Close()
{
	Owner->reset_feature(Mechos::RED_TRACK_FEATURE);
	fxlabInternalArcane::Close();
};


void fxlabGetArcanePosition(Mechos* owner,const Vect3f& offset,Vect3f& pos)
{
	Vect3f AxisX,AxisY,AxisZ;

	AxisX = owner->Xglobal();
	AxisY = owner->Yglobal();
	AxisZ = owner->Zglobal();

	pos.x += offset.x * AxisX.x + offset.y * AxisY.x + offset.z * AxisZ.x;
	pos.y += offset.x * AxisX.y + offset.y * AxisY.y + offset.z * AxisZ.y;
	pos.z += offset.x * AxisX.z + offset.y * AxisY.z + offset.z * AxisZ.z;

	pos = CYCLE(pos);
};
