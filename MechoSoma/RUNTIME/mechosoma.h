#ifndef	__MECHOSOMA_H__
#define __MECHOSOMA_H__

// mchGameWindow::flags...
#define MCH_WND_FULLSCREEN		0x01
#define MCH_WND_CENTERED		0x02
#define MCH_WND_ALIGN_LEFT		0x04
#define MCH_WND_ALIGN_RIGHT		0x08
#define MCH_WND_ALIGN_TOP		0x10
#define MCH_WND_ALIGN_BOTTOM		0x20
#define MCH_WND_640x480			0x40

struct mchGameWindow
{
	int flags;

	int PosX;
	int PosY;

	int CenterX;
	int CenterY;

	int SizeX;
	int SizeY;

	int SizeX2;	// SizeX / 2...
	int SizeY2;	// SizeY / 2...

	void set(int cx,int cy,int sx,int sy);
	void set(mchGameWindow* p);

	void set_clipping(void);
	void remove_clipping(void);

	void init(int mode = -1);

	int check(int x,int y){ 
		if(x >= (CenterX - SizeX2) && x < (CenterX + SizeX2) && y >= (CenterY - SizeY2) && y < (CenterY + SizeY2)) 
			return 1;
		return 0;
	};

	mchGameWindow(void){ flags = 0 ; };
	~mchGameWindow(void){ };
};

extern mchGameWindow* gameWnd;
extern mchGameWindow* gameWnd0;
extern mchGameWindow* gameWnd1;

extern mchGameWindow* iWnd;
extern mchGameWindow* iWnd0;
extern mchGameWindow* iWnd1;

// Camera's params
extern float fCamera;
extern float mchCameraX;
extern float mchCameraY;
extern float hCamera;
extern float mchCameraAX;
extern float mchCameraAY;
extern float mchCameraAZ;


extern int mchCurrentWorld;
extern int mchCurrentTrack;
extern int mchFreezeTime;
extern int mchTrackDemoMode;

extern int mchDebugMode;

extern const char* mch_mainINI;
extern const char* mch_hotseatINI;
extern const char* mch_optionsINI;

// Global flags...
#define MCH_STOP_TIME_FLAG		0x01
#define MCH_START_TIME_FLAG		0x02
#define MCH_TOGGLE_PAUSE_FLAG		0x04
#define MCH_TIMEOUT			0x08
extern int mchGameFlags;

/* ------------------------ HotSeat ---------------------------- */

enum mchTimeModes
{
	MCH_TIME_STOPPED = 1,
	MCH_TIME_WAITING_TURN,	// 2
	MCH_TIME_RUNNING,	// 3
	MCH_TIME_GAME_OVER	// 4
};

extern int mchTimeMode;

enum mchPBEM_GameModes {
	MCH_PBEM_EDIT_TURN = 1,
	MCH_PBEM_REPLAY_TURN,
	MCH_PBEM_VIEW_TURN,
	MCH_PBEM_END_TURN
};

enum mchGameModes
{
	MCH_SINGLE_GAME = 1,
	MCH_SPLIT_CONTROL_HS,
	MCH_ENTIRE_CONTROL_HS
};

// mchPBEM_Flags
const int PBEM_CONTROL_DATA_SENT	= 0x01;
const int PBEM_SHOW_GAME		= 0x02;
const int PBEM_CONNECTED		= 0x04;
const int PBEM_ERROR			= 0x08;
const int PBEM_DATA_SENT		= 0x10;
const int PBEM_CHECK_RETURN		= 0x20;
const int PBEM_START_TIME		= 0x40;
const int PBEM_CHANGE_PLAYER		= 0x80;
const int PBEM_REPLAY_MODE		= 0x100;
const int PBEM_SET_REPLAY_MODE		= 0x200;
const int PBEM_NEED_REFRESH		= 0x400;
const int PBEM_LATE_TURN		= 0x800;

extern int mchGameMode;
extern int mchTurnBasedGame;
extern int mchSplitScreenGame;

/*
	Arcade multiplayer. These are set before a race starts, from the command
	line or from the network menu, and are read by GameQuantRTO::Init and
	GameQuantRTO::Quant. The implementation lives in net_game.cpp, which is
	the only place that knows how both the match layer and the game fit
	together.

	mchNetHost    1 = this machine runs the session
	mchNetGame    1 = the current race is a net race. Cleared when it ends.
*/
extern int mchNetGame;
extern int mchNetHost;
extern int mchNetRaceAuto;
extern int mchNetAutoDrive;
/*
	mchNetMenu 1 = the in-race Escape menu is up over a live net race. While it
	is, the race and the session keep running, and the menu's regular actions -
	quit, restart, abort - must not free the world under them.
*/
extern int mchNetMenu;
/*
	mchNetBots 1 = the client's own car is a bot whose steering travels to the
	host as an ordinary input sample; on the host its own car and spare slots
	are bots too. Set by -netbots. Test scaffolding.
*/
extern int mchNetBots;
/*
	mchNetDestroy 1 = the host wrecks two adjacent bot slots a moment after the
	start, so the destroy-assemble sequence runs in a test with nobody at the
	keyboard. Set by -netdestroy. Test scaffolding.
*/
extern int mchNetDestroy;
/*
	mchNetTrace 1 = write a per-tick CSV of every networked object's pose and
	parameters, next to net.log, so a host and a client trace can be diffed.
	Set by -netdump. Diagnostic only; costs nothing when off.
*/
extern int mchNetTrace;
extern char mchNetJoinAddr[128];
extern int mchNetPort;
extern const char* mchNetStatusText;

/*
	The network menu's pick, filled in before a race arms and read at race
	init. figure is the mechos figure id (1..acsMaxEssenceID), spell the part
	ids of the three spell slots, world/track the host's choice. config is the
	"figure sp1 sp2 sp3" string the race loader wants. name is the player's.
*/
extern int mchNetMenuFigure;
extern int mchNetMenuSpell[3];
extern int mchNetMenuWorld;
extern int mchNetMenuTrack;
extern char mchNetRacerConfig[30];
extern char mchNetMenuName[32];

void mchNetStart(void);
void mchNetFinit(void);
void mchNetQuant(void);
void mchNetArmRaceWorld(void);

/*
	The menu lobby. The session opens while the creation screen is still up so
	players join and see each other before the race loads. The menu drives all
	of these from iScreen/ACS_NET.CPP.
*/
int  mchNetLobbyHost(const char* host_name,const char* game_name,int figure,const int* spells);
int  mchNetLobbyJoin(const char* addr,const char* player_name,int figure,const int* spells);
void mchNetLobbyQuant(void);
void mchNetLobbyClose(void);
void mchNetLobbyHostStart(void);
void mchNetLobbyClientArm(void);
void mchNetLobbySetLocal(const char* name,int figure,const int* spells);
void mchNetLobbySetGameName(const char* name);
const char* mchNetGameName(void);
int  mchNetPlayerFigure(int slot);
int  mchNetPlayerConfig(int slot,char* out,int sz);
void mchNetLogGrid(void);
int  mchNetBotFigure(unsigned int taken_mask);
int  mchNetLobbyStarted(void);

/*
	The lobby server (rendezvous) client, layered on the menu lobby above.

	A host registers its match with the server; a client lists the matches and
	joins one. The server sees each game's public UDP endpoint from a probe on
	the game socket and hands each side the other's, so both can punch through
	their NATs and then run the ordinary direct connection.

	The match browser is queried from the same screen, in its browse phase.
*/
int  mchNetLobbyServerHost(const char* lobby_addr,const char* host_name,const char* game_name,int figure,const int* spells);
int  mchNetLobbyServerJoin(const char* lobby_addr,const char* player_name,int figure,const int* spells);
void mchNetLobbyServerHangup(void);
void mchNetMenuPump(void);
int  mchNetLobbyClientJoined(void);
int  mchNetLobbyBrowseState(void);
const char* mchNetLobbyBrowseStatus(void);
int  mchNetLobbyMatchCount(void);
const char* mchNetLobbyMatchName(int index);
int  mchNetLobbyMatchId(int index);
int  mchNetLobbyMatchPlayers(int index);
int  mchNetLobbyMatchMax(int index);
int  mchNetLobbyMatchWorld(int index);
void mchNetLobbyBrowseRefresh(void);
void mchNetLobbyBrowseJoin(int index);

/*
	Called from the arcane machinery, never from the game loop.

	mchArcaneData::fxlabStart() is the one place both arcane paths converge on -
	the HUD menu and driving over a seed - so that is where a client gives up
	and asks the host to cast instead. On a host this does nothing and the
	engine runs the arcane itself.

	  ownerID    racer the arcane belongs to, as the engine numbers racers
	  part       which part the arcane sits on, -1 if it could not be resolved
	  event      one of NET_EV_ARCANE_*
	  arcane_id  mchArcaneData::ID, so the host can log which spell it was
*/
void mchNetArcaneCast(int ownerID,int part,int event,int arcane_id);

/*
	Host side: tell every client to build the visual for an arcane, so a spell
	cast by one player is seen by the others. Called from
	mchArcaneData::fxlabStart() on the host, which covers both its own player
	and a client's replayed cast. The sender itself is skipped on arrival,
	because it already built its own visual locally.
*/
void mchNetArcaneBroadcast(int ownerID,int arcane_id);

/*
	Host side: one interrupt of an arcane, at a place. Same rule as the cast,
	but the position comes along: an interrupt happens after the cast, and an
	effect placed at the cast point (a storm strike) and one placed at the
	owner (a dropped seed) cannot be told apart from the owner alone.

	x, y, z are engine world coordinates. No-op off the host.
*/
void mchNetArcaneInterrupt(int ownerID,int arcane_id,float x,float y,float z);

/*
	Host side: an arcane has ended, so every client should drop the visual it
	built for the cast. Called from fxlabGeneralArcaneType::Close(), which runs
	both when the arcane expires on its own and when it is interrupted - the
	one place both endings meet. Without it a client-side mirror of a lasting
	effect (a link on the car, a storm's clouds) is built once and never
	removed. No-op off the host.
*/
void mchNetArcaneVisualStop(int ownerID,int arcane_id);

/*
	Host side: an arcane latched onto a victim (a voodoo head reaching a
	racer). The victim is named by racer id; the client turns that into the
	corresponding head's SetTarget. No-op off the host.
*/
void mchNetArcaneLatch(int ownerID,int arcane_id,int victimID);

/*
	Host side: the race is over. Tells the clients so they show the same end
	screen. No-op off the host. Sent once, when mchEndGame runs.
*/
void mchNetGameOver(void);

/*
	Host side: a racer crossed the line. The clients draw a remote car only
	from snapshots, so they never see the moment it finishes; this event drives
	the finish fanfare on their copy. No-op off the host.
*/
void mchNetRacerFinished(int ownerID);

/*
	Net side of a bonus pickup. On a client this reports the pickup of its
	own car - its local simulation runs it first, but the host owns every
	replicated field of that car. On the host this relays the fanfare to
	the machines that only pose the car.
*/
void mchNetBonus(int ownerID,int arcane_id);

/*
	Write one line to the running net log from a layer that has no logger of
	its own (net_fx.cpp). No-op when no race is running.
*/
void mchNetWriteLog(const char* fmt,...);

// Read only, for the UI. All safe to call with no session running.
int  mchNetIsActive(void);
int  mchNetState(void);
int  mchNetLocalSlot(void);
int  mchNetNumPlayers(void);
int  mchNetNumReady(void);
const char* mchNetPlayerName(int slot);
const char* mchNetStatus(void);

extern int mchPBEM_Game;
extern int mchPBEM_GameMode;	// edit/view turn, etc.
extern int mchPBEM_CurPlayer;
extern int mchPBEM_CurPlayer0;
extern int mchPBEM_PlayerUID;
extern int mchPBEM_TargetTurn;
extern int mchPBEM_Flags;

inline void mchPBEM_SetFlag(int fl){ mchPBEM_Flags |= fl; }
inline void mchPBEM_DropFlag(int fl){ mchPBEM_Flags &= ~fl; }
inline int mchPBEM_CheckFlag(int fl){ return (mchPBEM_Flags & fl); }

#define HS_NAME_LEN		7
#define HS_PLAYERS_MAX		5

struct hsPlayerData 
{
	char name[256];
	int type; // 0 - disabled, 1 - human, 2 - AI-1, ..., N - AI-N-1
	int TSD_Count;
	char configStr[256];

	void SetName(const char* p){ strcpy(name,p); TSD_Count = 0; }

	hsPlayerData(void){ type = 0; }
	~hsPlayerData(void){ }
};

extern hsPlayerData hsPlayers[];

extern int hsStartAssembled;
extern int hsTurnSeedsMax;
extern int hsWaitTurnTime;
extern int hsTurnPrepareTime;
extern int hsTurnTime;
extern int hsCameraLimit;
extern int hsSeedsAccumulation;
extern int hsReturnMax;
extern int hsWorld;
extern int hsTrack;
extern int hsRndSeed;

void hsSetup(void);
void hsSaveParams(void);
void mchSetTimeMode(int m);
void mchSetGameMode(int m);

/*----------------------- Options ------------------------ */

struct mchOptionData
{
	int ID;

	int LimitMin;
	int LimitMax;
	int Value;

	const char* keyName;
	void* iScreenObj;

	int GetValue(void){ return Value; }

	void Update(void);

	void GetObjectValue(void);
	void SetObjectValue(void);

	void InitValue(void);

	void SetKey(const char* p){ keyName = strdup(p); }
	void Init(void);
	void Save(void);
	void Reset(void);

	mchOptionData(void){ keyName = NULL; iScreenObj = NULL; }
};

extern mchOptionData mchOptions[];

extern int xgrInitMode;

/*-------------------- end of Options -------------------- */

void mchInitControls(void);
int mchGetKeyID(int key,int config = -1);

int mchGetKeyID_First(int key,int config = -1);
int mchGetKeyID_Next(void);

int mchKeyPressed(int id,int config = -1);

const char* getIniKey(const char* fname,const char* section,const char* key);
void putIniKey(const char* fname,const char* section,const char* key,const char* val);
void putIniKey(const char* fname,const char* section,const char* key,int val);

unsigned mch_clock(void);

int acsOpenResource(const char* fname,XStream& fh,int res_flag = -1);
int m3dOpenResource(const char* fname,XStream& fh);
void mchXResourceInit(void);
int vmapOpenResource(const char* fname,XStream& fh);
int mch_vmapResourceInit(const char * Path2World=NULL);


void mchCreateBonusArcane(void* p);

#define _MCH_LOG_

#ifdef _MCH_LOG_
extern XStream __mch_logFile;
#define __MCH_LOG_STR(s) { __mch_logFile.open("game.log",XS_OUT | XS_APPEND | XS_NOREPLACE); __mch_logFile < s < "\r\n"; __mch_logFile.close(); }
#else
#define __MCH_LOG_STR(s) 
#endif

// User-defined groups for statistics
enum {
	STAT_TOTAL,
	STAT_RESDISP,
	STAT_BODYDISP,
	STAT_BODYDISPPRE,
	STAT_MECHOS,
	STAT_CAMERA,
	STAT_M3D,
	STAT_RACEDISP,
	STAT_NUMERIC
};

#endif
