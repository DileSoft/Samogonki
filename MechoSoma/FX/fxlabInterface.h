#ifndef __FXLAB_INTERFACE__
#define __FXLAB_INTERFACE__

#include <unordered_map>

#include "Base.h"
#include "ArcaneDefine.h"
#include "ArcaneStatistics.h"

void fxlabOpen();
void fxlabClose();

void fxlabInit();
void fxlabFinit();

void fxlabServerQuant();
void fxlabClientQuant();

struct fxlabProcessInterface
{
	struct fxlabGeneralObjectType* Process;

	fxlabProcessInterface(){ Process = NULL; };
	void Init(){ Process = NULL; };
	fxlabGeneralObjectType* Get(){ return Process; };
	void Set(struct fxlabGeneralObjectType* p){ Process = p; };
};

enum fxlabCheckPointMode
{
	FXLAB_CHECK_POINT_MODE_CURRENT = 0,
	FXLAB_CHECK_POINT_MODE_FADE,
	FXLAB_CHECK_POINT_MODE_PAST,
	FXLAB_CHECK_POINT_MODE_FUTURE,
	FXLAB_CHECK_POINT_MODE_MOVE,
	FXLAB_CHECK_POINT_MODE_SKIP,
	FXLAB_CHECK_POINT_MODE_ACTIVE_START,
	FXLAB_CHECK_POINT_MODE_PASSIVE_START,
	FXLAB_CHECK_POINT_MODE_FADE_START,
	FXLAB_CHECK_POINT_MODE_MOVE_START,
	FXLAB_CHECK_POINT_MODE_MAX
};

extern int fxlabCheckPointIndex;
extern int fxlabCheckPointMax;

void fxlabCreateAverageCheckPoint(fxlabProcessInterface* master,int x,int y,int z,int key,float scale,int dx,int dy,int dz,int n_camera);
void fxlabChangeCheckPoint(fxlabProcessInterface* master,int key,struct mchRacer* p = NULL,int init_flag = 0);
void fxlabCreateMasterCheckPoint(fxlabProcessInterface* master,int x,int y,int z,int key,float scale,int dx,int dy,int dz,int ind,int n_camera);

void fxlabArcaneStart(const class ArcaneStatisticsInfo* p);
void fxlabArcaneStop(const class ArcaneStatisticsInfo* p);
//void fxlabArcaneAction(const class ArcaneStatisticsInfo* p);
void fxlabArcaneAction(const class ArcaneStatisticsInfo* p,int id);
void fxlabArcaneStatisticInit();

/*
	Client-side visual for an arcane, without the game part.

	A client never runs the arcane process - mchArcaneData::fxlabStart() returns
	at the net hook - so the host is the only machine that builds the server
	side. But the look of an arcane is not on the server side: every visible
	piece is a fxlabClientD object created from a server class. This rebuilds
	just those pieces. It never creates a field, a damage sensor, a feature or
	touches LinkPoint->Status, so it cannot affect the race.

	Covered arcanes are listed in the definition; the rest cast on the host and
	stay invisible on a client for now.
*/
void fxlabClientArcaneStart(int arcane_id,int owner_id);

/*
	Client side: one later interrupt of an arcane, at the host's place.

	Sister of fxlabClientArcaneStart: a server arcane's InterruptAction() plays
	a client visual and runs a game effect at the same time, and only the first
	belongs here. Covered arcanes are listed in the definition.
*/
void fxlabClientArcaneInterrupt(int arcane_id,int owner_id,const class Vect3f& pos);

/*
	Client side: the arcane latched onto a victim on the host (a voodoo head
	reaching a racer), so one of its mirrored heads should leave its orbit and
	home in. victim_id is the racer the host's head latched onto.
*/
void fxlabClientArcaneLatch(int arcane_id,int owner_id,int victim_id);

/*
	Client side: the arcane ended on the host, so drop the visual built for it.

	Kills every client object the cast mirror tagged with this (owner, arcane),
	and puts back any feature the cast applied to the car. Start and stop are
	the only two moments a mirror object's life is decided, so this is the
	counterpart to fxlabClientArcaneStart and nothing else.
*/
void fxlabClientArcaneStop(int arcane_id,int owner_id);

int fxlabLine2SphereDist(const class Vect3f& r1,const class Vect3f& c,float rad);

int fxlabGetWorldReflectionEnable();
int fxlabGetWaterRed();
int fxlabGetWaterRedDelta();
int fxlabGetWaterGreen();
int fxlabGetWaterGreenDelta();
int fxlabGetWaterBlue();
int fxlabGetWaterBlueDelta();
int fxlabGetWaterAlpha();
int fxlabGetWaterDeltaAlpha();
int fxlabGetWaterDeltaHeight();
float fxlabGetWaterDeltaUV();

void fxlabClearDeltaPolygon();

extern int fxlabPolyGridAlpha;

void fxlabTakeBonus(int type,const Vect3f& pos);
void fxlabDestroyBonus(struct fxlabProcessInterface* p);

enum fxlabFirePointInterfaceType
{
	FXLAB_FIRE_POINT_INTERFACE_CLAW,
	FXLAB_FIRE_POINT_INTERFACE_PUMP,
	FXLAB_FIRE_POINT_INTERFACE_FIRE,
	FXLAB_FIRE_POINT_INTERFACE_BOSS_WATER
};

struct fxlabFirePointInterface
{
	int ID;
	fxlabProcessInterface ServerPoint;
	fxlabProcessInterface ClientPoint;

	void Open(int id);
	void Close();
	void Update(const class Vect3f& pos,const class Vect3f& vel);
	void Update(const class Vect3f& pos,const class Mat3f& vel);
};

#include "UMATH.H"

const int FXLAB_MECHOS_IMFORMATION_MANA_DELAY = 500;
const int FXLAB_MECHOS_IMFORMATION_ENERGY_DELAY = 500;

struct fxlabMechosColorInformation
{
	int ColorEnable;
	int RGBACount;
	float Red,Green,Blue,Alpha;
	sColor4f Color0,Color1;

	fxlabMechosColorInformation();
};

struct fxlabMechosProcessInformation
{
	fxlabMechosColorInformation ColorData[NUM_PARTS];
	fxlabProcessInterface ArmorPoint,SkinPoint,IsolationPoint,BioPoint;
	fxlabProcessInterface ArmorServerPoint,SkinServerPoint,IsolationServerPoint,BioServerPoint;

	int WaterEnable;

	class DurationTimer& FireTimer;
	class DurationTimer& DustTimer;
	class DurationTimer& EnergyTimer;
	class DurationTimer& ManaTimer;
	class DurationTimer& WaveTimer;
	class DurationTimer& DamageTimer;

	class DurationTimer& JumpTimer;
	class DurationTimer& NitroTimer;

	fxlabMechosProcessInformation();
	~fxlabMechosProcessInformation();
};


void fxlabCannonFire(const Vect3f& pos,const Vect3f& vel,float pow);
void fxlabRacerEvent(int type,struct mchRacer* point);

float triangleMapH(int x, int y, float z);
inline int fxlabGetLevel(int x,int y,float z)
{
	return triangleMapH(x, y, z);
};

template <class Type,int sx,int sy,int dx,int dy>
void fxTransFormBitMap(Type* sbuf,Type* dbuf,double angle)
{
	Type* pd;
	int x,y;
	int x0,y0,x1,y1,x2,y2;
	int x_add,y_add;
	int x_delta,y_delta;
	int cx,cy;
	int tx,ty;
	int cosTurn,sinTurn;
	int i,j;
	int cnt;

	pd = dbuf;

	cx = dx / 2;
	cy = dy / 2;
	tx = sx / 2;
	ty = sy / 2;

	cosTurn = round(cos(angle) * (double)(1 << 16));
	sinTurn = round(sin(angle) * (double)(1 << 16));

	x0 = (-cx)*cosTurn - (-cy)*sinTurn;
	y0 = (-cx)*sinTurn + (-cy)*cosTurn;

	x1 = (cx)*cosTurn - (-cy)*sinTurn;
	y1 = (cx)*sinTurn + (-cy)*cosTurn;

	x2 = (-cx)*cosTurn - (cy)*sinTurn;
	y2 = (-cx)*sinTurn + (cy)*cosTurn;

	x_add = (x1 - x0) / dx;
	y_add = (y1 - y0) / dx;
	x_delta = (x2 - x0) / dy;
	y_delta = (y2 - y0) / dy;

	x0 = x0 + (tx << 16);
	y0 = y0 + (ty << 16);

	cnt = 0;

	for(i = 0;i < dy;i++){
		x = x0;
		y = y0;
		for(j = 0;j < dx;j++){
			if(x >= 0 && x < (sx << 16) && y >= 0 && y < (sy << 16)){
				*pd = sbuf[(y >> 16) * sy + (x >> 16)];
				cnt++;
			}else 
				*pd = 0;
			pd++;
			x += x_add;
			y += y_add;
		};
		x0 += x_delta;
		y0 += y_delta;
	};
};


template <class Type>
void fxTransFormBitMap(int sx,int sy,int dx,int dy,Type* sbuf,Type* dbuf,double angle)
{
	Type* pd;
	int x,y;
	int x0,y0,x1,y1,x2,y2;
	int x_add,y_add;
	int x_delta,y_delta;
	int cx,cy;
	int tx,ty;
	int cosTurn,sinTurn;
	int i,j;
	int cnt;

	pd = dbuf;

	cx = dx / 2;
	cy = dy / 2;
	tx = sx / 2;
	ty = sy / 2;

	cosTurn = round(cos(angle) * (double)(1 << 16));
	sinTurn = round(sin(angle) * (double)(1 << 16));

	x0 = (-cx)*cosTurn - (-cy)*sinTurn;
	y0 = (-cx)*sinTurn + (-cy)*cosTurn;

	x1 = (cx)*cosTurn - (-cy)*sinTurn;
	y1 = (cx)*sinTurn + (-cy)*cosTurn;

	x2 = (-cx)*cosTurn - (cy)*sinTurn;
	y2 = (-cx)*sinTurn + (cy)*cosTurn;

	x_add = (x1 - x0) / dx;
	y_add = (y1 - y0) / dx;
	x_delta = (x2 - x0) / dy;
	y_delta = (y2 - y0) / dy;

	x0 = x0 + (tx << 16);
	y0 = y0 + (ty << 16);

	cnt = 0;

	for(i = 0;i < dy;i++){
		x = x0;
		y = y0;
		for(j = 0;j < dx;j++){
			if(x >= 0 && x < (sx << 16) && y >= 0 && y < (sy << 16)){
				*pd = sbuf[(y >> 16) * sy + (x >> 16)];
				cnt++;
			}else 
				*pd = 0;
			pd++;
			x += x_add;
			y += y_add;
		};
		x0 += x_delta;
		y0 += y_delta;
	};
};

//----------------------------------------------------

struct fxlabTeleportInterface
{
	int Status;
	int WorldID,TrackID;
	class Vect3f Position;
	fxlabProcessInterface ClientPoint1;
	fxlabProcessInterface ClientPoint2;
	fxlabProcessInterface ServerPoint;

	fxlabTeleportInterface();
	void Init(class Vect3f& p,int status,int world_id,int track_id);
	void Open();
	void Close();
	void SetStatus(int status);
};

typedef std::unordered_map<int,std::unordered_map<int,fxlabTeleportInterface> > fxlabTeleportListType;

struct fxlabTeleportDispatcher
{
	int QuantEnable;
	fxlabTeleportListType fxlabTeleportList;

	void Init();
	void Finit();
	void Open();
	void Close();
	void Add(int world_id,int track_id,class Vect3f& p,int status);
	void SetState(int world_id,int track_id,int status);
	fxlabTeleportInterface* Search(int world_id,int tarck_id);
};

extern fxlabTeleportDispatcher* fxlabTeleportD;

void fxlabCreateTeleport(int world_id,int track_id,class Vect3f& p,int status);
void fxlabChangeTeleport(int world_id,int track_id,int status);

//------------------------------------

struct fxlabMovieDataType
{
	fxlabProcessInterface ProcessPoint;
	int ProcessType;
	int ProcessKeyID;
};

struct fxlabMovieEqStr
{
  bool operator()(const char* s1, const char* s2) const
  {
	return (strcmp(s1, s2) == 0);
  }
};

struct fxlabMovieDispacther
{
	std::unordered_map<std::string,fxlabMovieDataType> MovieList;

	void Open();
	void Close();
	void Clear();
	void RegisterName(const char* name,int type,int key_id);
	void UpdateProcess(const char* name,const Vect3f& position,const Vect3f& velocity,int status);
};

extern fxlabMovieDispacther* fxlabMovieD;

void fxlabMovieUpdateProcess(const char* name,const Vect3f& position,const Vect3f& velocity,int status);
void fxlabMovieDestroy();

enum fxlabBigBossWorldType
{
	FXLAB_BIG_BOSS_WORLD_ID_NONE = 0,
	FXLAB_BIG_BOSS_WORLD_ID_MOUNTAIN = 1,
	FXLAB_BIG_BOSS_WORLD_ID_FOREST = 2,
	FXLAB_BIG_BOSS_WORLD_ID_WATCH = 4,
	FXLAB_BIG_BOSS_WORLD_ID_WATER = 8,
	FXLAB_BIG_BOSS_WORLD_ID_FIRE = 16,
	FXLAB_BIG_BOSS_WORLD_ID_TOWN = 32
};

#define FXLAB_MECHOS_IN_WATER(p) (mchCurrentWorld != 4 && p->submersion() > 0.15f)

#endif
