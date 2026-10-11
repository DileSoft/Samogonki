/* ---------------------------- INCLUDE SECTION ----------------------------- */
#include "StdAfx.h"

#include "mechosoma.h"

#include "ACI_IDS.H"
#include "ACI_EVNT.H"
#include "ACI_SCR.H"
#include "KEYS.H"
#include "sound.h"
#include "sound_api.h"

#include "race.h"
#include "TrackDispatcher.h"
#include "parts_pool.h"

#include "filesystem.h"

/* ----------------------------- EXTERN SECTION ----------------------------- */

extern char mch_playerNames[15][HS_NAME_LEN + 2];
extern int mchDefaultFigure;
extern int acsMaxEssenceID;
extern int mchNumLaps;
extern int acsNumSaves;

extern unsigned char mch_excludeFlags[];

void mchInitPartIDs(void);

/* --------------------------- PROTOTYPE SECTION ---------------------------- */

void acsInitNetMenu(void);
int acsNetMenuKey(int key);
int acsNetMenuExtEvent(int row,int dir);
void acsNetMenuQuant(void);

/* --------------------------- DEFINITION SECTION --------------------------- */

/*
	The LAN menu, hardcoded.

	The menu strings and screens live in iscreen.scb, and that data cannot be
	rebuilt from the repository's .SCR right now (the two no longer match, see
	docs/net-multiplayer-plan.md, phase 6). So the network game gets a screen
	of its own, built here in C++ and drawn by the same engine as every other
	menu: the same item look, mouse highlighting, keyboard navigation and
	button sound.

	The screen is reached from a "Сетевая игра" item on the main menu, added
	in the slot the website used to take (ACS_EXT.CPP). The original hot-seat
	multiplayer screen (ACS_MULTIPLAYER_SCR, "Поочередный хот-сит" and friends)
	is left exactly as it shipped.

	Two phases share the one screen:

	  setup   the host picks a name, a game name, a mechos and a world and
	          presses "Create"; a client picks an address, a name and a
	          mechos and presses "Connect";

	  lobby   the session is up, the roster fills in live, and everybody sees
	          who has joined and what they are driving - the row layout, one
	          player per line, mirrors the hot-seat player list. The host
	          presses "Start", the clients load the race the moment the host
	          says so.

	The rows are text, not the hot-seat's little mechos sprites: the sprite
	path is tied to the arcane shop's redraw and pulling it in here would drag
	the whole constructor in with it.
*/

// Row object ids. Far from anything the script uses, so a hidden leftover
// object can never shadow ours.
#define NM_BASE			9001
enum {
	NM_TITLE = NM_BASE,
	NM_HOST,
	NM_ADDR,
	NM_NAME,
	NM_FIG,
	NM_SP1,
	NM_SP2,
	NM_SP3,
	NM_WORLD,
	NM_GAMENAME,
	NM_PLAYHDR,
	NM_LIST0,
	NM_LIST1,
	NM_LIST2,
	NM_LIST3,
	NM_LIST4,
	NM_STATUS,
	NM_ACTION,
	NM_BACK,

	NM_LAST
};

// A row's label sits 100 above its value object.
#define NM_LBL(x)	((x) + 100)

// Keep in step with net_protocol_types.h NET_SERVER_NAME_LEN.
#define NET_GAME_NAME_LEN 24

// What the menu holds until the action is pressed. nm_host/nm_addr default to
// the command line so -nethost / -netjoin keep working as a shortcut for
// testing.
static int  nm_host = 1;
static char nm_addr[128] = "127.0.0.1";
static char nm_name[32]  = "";
static char nm_game[NET_GAME_NAME_LEN] = "";
static int  nm_fig = 1;			// mechos figure, 1..acsMaxEssenceID
static int  nm_sp[3] = { 1, 1, 1 };	// spell part ids, slots front/middle/rear
static int  nm_world = 1;

static int  nm_built = 0;
static int  nm_lobby = 0;		// the session is up, the roster is shown
static char nm_game_pushed[NET_GAME_NAME_LEN] = "";

// The three spell slots are part indices 1..3 of the mechos: index 0 is the
// figure itself, the rest carry one arcane each.
static const int nm_spell_slot[3] = { 1, 2, 3 };

/*
	The mechos parts are pooled as middle, front, rear, and the config string
	must list them in that order - GetMechos reads token i against pool slot i.
	The menu shows the slots as front, middle, rear, so each row is redirected
	to the pool slot it edits. nm_sp[] stays in pool order.
*/
static int nm_sp_idx(int row)
{
	if(row == NM_SP1) return 1;	// front: pool slot 2
	if(row == NM_SP2) return 0;	// middle: pool slot 1
	if(row == NM_SP3) return 2;	// rear: pool slot 3
	return 0;
}

#define NM_ROWS 5		// roster lines drawn

static const char* nm_world_str(int world);

/* ------------------------------ helpers ---------------------------------- */

static aciScreen* nm_screen(void)
{
	return acsScrD -> GetScreen(ACS_NET_GAME_SCR);
}

static aciScreenObject* nm_obj(int id)
{
	aciScreen* scr = nm_screen();
	return scr ? scr -> GetObject(id) : NULL;
}

static void nm_set_text(int id,const char* text)
{
	aciScreenInputField* p = (aciScreenInputField*)nm_obj(id);
	if(!p) return;

	p -> set_string((char*)text);
	p -> flags |= ACS_REDRAW_OBJECT;
}

// Localised labels. The game's menu strings live in the language's iscreen.scb,
// which we cannot add to, so the few we need are here. cp1251 for Russian, the
// rest fall back to English.
static int nm_is_ru(void)
{
	return !strcmp(localization::getLanguage(),"ru");
}

static const char* nm_pick(const char* en,const char* ru)
{
	return nm_is_ru() ? ru : en;
}

// A row label, keyed by the row's value id.
static const char* nm_label_str(int row)
{
	switch(row){
		case NM_HOST:     return nm_pick("Mode:","\xD0\xE5\xE6\xE8\xEC:");
		case NM_ADDR:     return nm_pick("Address:","\xC0\xE4\xF0\xE5\xF1:");
		case NM_NAME:     return nm_pick("Name:","\xC8\xEC\xFF:");
		case NM_FIG:      return nm_pick("Mechos:","\xCC\xE5\xF5\xEE\xF1:");
		case NM_SP1:      return nm_pick("Front:","\xCF\xE5\xF0\xE5\xE4:");
		case NM_SP2:      return nm_pick("Middle:","\xD1\xE5\xF0\xE5\xE4\xE8\xED\xE0:");
		case NM_SP3:      return nm_pick("Rear:","\xC7\xE0\xE4:");
		case NM_WORLD:    return nm_pick("World:","\xCC\xE8\xF0:");
		case NM_GAMENAME: return nm_pick("Game:","\xC8\xE3\xF0\xE0:");
		case NM_PLAYHDR:  return nm_pick("Players:","\xC8\xE3\xF0\xEE\xEA\xE8:");
		}
	return "";
}

static const char* nm_title_str(void)
{
	return nm_pick("Network Race","\xD1\xE5\xF2\xE5\xE2\xE0\xFF \xE8\xE3\xF0\xE0");
}

static const char* nm_action_str(void)
{
	if(!nm_lobby)
		return nm_host ? nm_pick("Create","\xD1\xEE\xE7\xE4\xE0\xF2\xFC \xE8\xE3\xF0\xF3")
			       : nm_pick("Connect","\xCF\xEE\xE4\xEA\xEB\xFE\xF7\xE8\xF2\xFC\xF1\xFF");

	return nm_host ? nm_pick("Start","\xCD\xE0\xF7\xE0\xF2\xFC")
		       : nm_pick("Cancel","\xCE\xF2\xEC\xE5\xED\xE0");
}

static const char* nm_back_str(void)
{
	return nm_pick("Back","\xCD\xE0\xE7\xE0\xE4");
}

static const char* nm_host_str(void)
{
	if(nm_is_ru()) return nm_host ? "\xD5\xEE\xF1\xF2" : "\xEA\xEB\xE8\xE5\xED\xF2";	// Хост / клиент
	return nm_host ? "Host" : "Client";
}

// Name of a mechos figure. The engine keeps one per figure id, 0 based.
static const char* nm_figure_str(int fig)
{
	int id = fig - 1;
	if(id >= 0 && id < acsMaxEssenceID && mch_playerNames[id][0])
		return mch_playerNames[id];
	return "-";
}

static const char* nm_spell_str(int slot)
{
	char* name = mchGetPartArcaneName(nm_spell_slot[slot],nm_sp[slot]);
	return name ? name : "-";
}

// The bare value of a selector row: the label is a separate object.
static void nm_format_value(int id,char* out,int sz)
{
	switch(id){
		case NM_HOST:  sprintf(out,"%s",nm_host_str()); break;
		case NM_FIG:   sprintf(out,"%s",nm_figure_str(nm_fig)); break;
		case NM_SP1:   sprintf(out,"%s",nm_spell_str(nm_sp_idx(NM_SP1))); break;
		case NM_SP2:   sprintf(out,"%s",nm_spell_str(nm_sp_idx(NM_SP2))); break;
		case NM_SP3:   sprintf(out,"%s",nm_spell_str(nm_sp_idx(NM_SP3))); break;
		case NM_WORLD: sprintf(out,"%s",nm_world_str(nm_world)); break;
		default:       out[0] = 0; break;
	}
	(void)sz;
}

static const char* nm_world_str(int world)
{
	const char* name = mch_trackD ? mch_trackD -> TrackName(world,0) : NULL;
	return name ? name : "-";
}

/* --------------------------- cycling values ------------------------------ */

/*
	The parts pool must exist and be built with a known part mapping before any
	of the cycle helpers are safe: mchNextPart/mchPrevPart dereference the pool
	without a null check, and a pool built before mchInitPartIDs() ran has
	empty part lists. mchInitPartIDs is idempotent, so the parts menu can call
	this as often as it likes - including after a race, which deletes the pool.
*/
static void nm_ensure_pool(void)
{
	mchInitPartIDs();

	if(!mch_partsPoolLst.size()){
		mchCreatePartsPool();
		return;
	}

	// A pool that exists but has no parts was built too early. Rebuild it.
	{
		mchMechosPartsPool* pl = mch_partsPoolLst.search(0);
		if(pl && pl -> pool[0] -> size() == 0){
			mchDeletePartsPool();
			mchCreatePartsPool();
		}
	}
}

// First part id of a spell slot that is not excluded and carries an arcane.
static int nm_first_spell(int slot)
{
	mchMechosPartsPool* pl = mch_partsPoolLst.size() ? mch_partsPoolLst.search(0) : NULL;
	mchMechosPartInfo* p;

	if(!pl) return 1;

	p = pl -> pool[nm_spell_slot[slot]] -> first();
	while(p){
		if(p -> data && p -> data -> arcaneData.ID != -1 &&
		   !mch_excludeFlags[p -> data -> arcaneData.ID])
			return p -> ID;
		p = p -> next;
	}
	return 1;
}

static void nm_cycle_figure(int dir)
{
	int id = nm_fig;

	for(int n = 0; n < acsMaxEssenceID + 1; n ++){
		id += dir;
		if(id > acsMaxEssenceID) id = 1;
		if(id < 1) id = acsMaxEssenceID;
		if(!mch_trackD || mch_trackD -> FigureEnabledHS(id - 1)) break;
	}
	nm_fig = id;
}

static void nm_cycle_spell(int slot,int dir)
{
	int id;

	nm_ensure_pool();

	id = dir ? mchNextPart(nm_spell_slot[slot],nm_sp[slot])
		 : mchPrevPart(nm_spell_slot[slot],nm_sp[slot]);
	if(id != -1) nm_sp[slot] = id;
}

static void nm_cycle_world(int dir)
{
	int id = nm_world;

	for(int n = 0; n < 32; n ++){
		id += dir;
		if(id < 0) id = 15;
		if(id > 15) id = 0;
		if(mch_trackD && mch_trackD -> GetTrack(id,0)) break;
	}
	nm_world = id;
}

/* ------------------------------- lobby list ------------------------------ */

// The candidate chain, in the order the cursor walks it. Only the visible ones
// are linked, so hiding a row drops it out of keyboard navigation for free.
static const int nm_chain[] = {
	NM_GAMENAME, NM_HOST, NM_ADDR, NM_NAME, NM_FIG,NM_SP1,NM_SP2,NM_SP3,
	NM_WORLD, NM_ACTION, NM_BACK
};

// Whether the cursor may land on a row right now.
static int nm_row_visible(int id)
{
	if(id == NM_GAMENAME) return nm_host || nm_lobby;
	if(id == NM_HOST)     return !nm_lobby;
	if(id == NM_ADDR)     return !nm_host && !nm_lobby;
	if(id == NM_NAME || id == NM_FIG ||
	   id == NM_SP1 || id == NM_SP2 || id == NM_SP3) return !nm_lobby;
	if(id == NM_WORLD)    return nm_host;
	if(id == NM_ACTION || id == NM_BACK) return 1;
	return 0;
}

static void nm_show(int id,int on)
{
	aciScreenObject* p;
	aciScreenObject* l;

	p = nm_obj(id);
	if(p){
		if(on) p -> flags &= ~(ACS_HIDDEN | ACS_BLOCKED);
		else   p -> flags |=  (ACS_HIDDEN | ACS_BLOCKED);
		p -> flags |= ACS_REDRAW_OBJECT;
	}

	l = nm_obj(NM_LBL(id));
	if(l){
		if(on) l -> flags &= ~(ACS_HIDDEN | ACS_BLOCKED);
		else   l -> flags |=  (ACS_HIDDEN | ACS_BLOCKED);
		l -> flags |= ACS_REDRAW_OBJECT;
	}
}

// Show the rows the current phase needs, and relink the cursor chain over the
// visible ones only.
static void nm_apply_visibility(void)
{
	static int last_host = -1;
	static int last_lobby = -1;
	int i,k;
	int vis[32];
	int nvis = 0;

	// Hiding a row only stops it being redrawn; the pixels it left behind
	// stay until the background is cleared again. A phase switch therefore
	// asks for a forced redraw, which is what makes the list appear instead
	// of the setup rows it replaced.
	if(last_host != nm_host || last_lobby != nm_lobby){
		last_host = nm_host;
		last_lobby = nm_lobby;
		acsScrD -> flags |= ACS_FORCED_REDRAW;
	}

	// Every row that is not a plain label has a label object too.
	const int rows[] = { NM_HOST,NM_ADDR,NM_NAME,NM_FIG,NM_SP1,NM_SP2,NM_SP3,
			     NM_WORLD,NM_GAMENAME,NM_PLAYHDR };
	for(i = 0; i < (int)(sizeof(rows)/sizeof(rows[0])); i ++){
		int id = rows[i];
		int on;

		if(id == NM_PLAYHDR) on = nm_lobby;
		else on = nm_row_visible(id);

		nm_show(id,on);
	}

	for(i = 0; i < NM_ROWS; i ++)
		nm_show(NM_LIST0 + i,nm_lobby);

	nm_show(NM_TITLE,1);
	nm_show(NM_STATUS,nm_lobby);
	nm_show(NM_ACTION,1);
	nm_show(NM_BACK,1);

	for(i = 0; i < (int)(sizeof(nm_chain)/sizeof(nm_chain[0])); i ++)
		if(nm_row_visible(nm_chain[i]))
			vis[nvis ++] = nm_chain[i];

	for(k = 0; k < nvis; k ++){
		aciScreenObject* p = nm_obj(vis[k]);
		if(!p) continue;

		p -> objIndex[1] = vis[(k + nvis - 1) % nvis];	// up
		p -> objIndex[3] = vis[(k + 1) % nvis];		// down
		p -> objIndex[0] = 0;
		p -> objIndex[2] = 0;
	}
}

// Refresh every value the state can change. Called on init, after a pick, and
// once a frame while the lobby is up.
static void nm_refresh(void)
{
	char buf[256];
	aciScreenInputField* f;
	int i;

	nm_apply_visibility();

	nm_format_value(NM_HOST,buf,sizeof(buf));  nm_set_text(NM_HOST,buf);
	nm_format_value(NM_FIG,buf,sizeof(buf));   nm_set_text(NM_FIG,buf);
	nm_format_value(NM_SP1,buf,sizeof(buf));   nm_set_text(NM_SP1,buf);
	nm_format_value(NM_SP2,buf,sizeof(buf));   nm_set_text(NM_SP2,buf);
	nm_format_value(NM_SP3,buf,sizeof(buf));   nm_set_text(NM_SP3,buf);
	nm_format_value(NM_WORLD,buf,sizeof(buf)); nm_set_text(NM_WORLD,buf);

	// Address and Name are edited in place: the field is the live text, so a
	// refresh must not write the state variable back into it, or picking a
	// mechos would wipe what the player typed. Seeded once at init; live in
	// the lobby they are read back into the state instead.
	//
	// The game name is the exception: it is pushed to the host every frame so
	// a rename reaches the clients without a message of its own.
	if(nm_host && (f = (aciScreenInputField*)nm_obj(NM_GAMENAME))){
		if(strcmp(f -> string,nm_game_pushed)){
			strncpy(nm_game_pushed,f -> string,sizeof(nm_game_pushed) - 1);
			nm_game_pushed[sizeof(nm_game_pushed) - 1] = 0;
			mchNetLobbySetGameName(nm_game_pushed);
		}
	}

	// A client only reads the game name; it comes from the host's roster.
	if(!nm_host && nm_lobby)
		nm_set_text(NM_GAMENAME,mchNetGameName());

	nm_set_text(NM_TITLE,nm_title_str());
	nm_set_text(NM_ACTION,nm_action_str());
	nm_set_text(NM_BACK,nm_back_str());
	for(i = 0; i < (int)(sizeof(nm_chain)/sizeof(nm_chain[0])); i ++)
		nm_set_text(NM_LBL(nm_chain[i]),nm_label_str(nm_chain[i]));

	// Roster lines and status.
	for(i = 0; i < NM_ROWS; i ++){
		const char* name = nm_lobby ? mchNetPlayerName(i) : "";
		int fig = nm_lobby ? mchNetPlayerFigure(i) : 0;

		if(name && name[0])
			sprintf(buf,"%s - %s",name,fig ? nm_figure_str(fig) : "-");
		else
			sprintf(buf,"-");

		nm_set_text(NM_LIST0 + i,buf);
	}

	if(nm_lobby){
		// The local row is the one this machine owns; mark it so a player can
		// find themselves in the list at a glance.
		sprintf(buf,"%s %d",nm_pick("Players:","\xC8\xE3\xF0\xEE\xEA\xE8:"),
			mchNetNumPlayers());
		nm_set_text(NM_STATUS,buf);
	}
}

/* ---------------------------- starting the game -------------------------- */

// Leave the menu for the loading screen. The same exit the F2 keypress used to
// produce: the script's $MCH_SINGLE_GAME, the mode a net race rides.
static void nm_leave_to_race(void)
{
	nm_lobby = 0;

	acsScrD -> QuantCode = 2;
	acsScrD -> flags |= ACS_NEED_EXIT;
}

static void nm_back_to_menu(void)
{
	// In the lobby, back means back to the setup: the host can change
	// something and create the game again, the client can edit and reconnect.
	// A second back, from the setup, leaves for the main menu.
	if(nm_lobby){
		mchNetLobbyClose();
		nm_lobby = 0;
		nm_refresh();
		return;
	}

	if(acsNumSaves < 1)
		acsScrD -> PrepareChangeScreen(ACS_MAIN_MENU_SCR0);
	else if(acsNumSaves == 1)
		acsScrD -> PrepareChangeScreen(ACS_MAIN_MENU_SCR1);
	else
		acsScrD -> PrepareChangeScreen(ACS_MAIN_MENU_SCR2);
}

// Read the live text fields back into the state, then hand the local player's
// pick to the race loader.
static void nm_capture_local(void)
{
	aciScreenInputField* f;

	if((f = (aciScreenInputField*)nm_obj(NM_ADDR))) strncpy(nm_addr,f -> string,sizeof(nm_addr) - 1);
	if((f = (aciScreenInputField*)nm_obj(NM_NAME))) strncpy(nm_name,f -> string,sizeof(nm_name) - 1);
	if((f = (aciScreenInputField*)nm_obj(NM_GAMENAME))) strncpy(nm_game,f -> string,sizeof(nm_game) - 1);
	nm_addr[sizeof(nm_addr) - 1] = 0;
	nm_name[sizeof(nm_name) - 1] = 0;
	nm_game[sizeof(nm_game) - 1] = 0;

	mchNetMenuFigure = nm_fig;
	for(int i = 0; i < 3; i ++)
		mchNetMenuSpell[i] = nm_sp[i];

	sprintf(mchNetRacerConfig,"%d %d %d %d",nm_fig,nm_sp[0],nm_sp[1],nm_sp[2]);

	strncpy(mchNetMenuName,nm_name[0] ? nm_name : "player",sizeof(mchNetMenuName) - 1);
	mchNetMenuName[sizeof(mchNetMenuName) - 1] = 0;

	mchNumLaps = 5;
	mchNetMenuWorld = nm_world;
	mchNetMenuTrack = 0;
}

// The action button. Host: create the lobby, then start it. Client: connect,
// then leave the lobby.
static void nm_action(void)
{
	nm_capture_local();

	if(!nm_lobby){
		int ok;

		if(nm_host){
			mchNetMenuWorld = nm_world;
			mchNetMenuTrack = 0;
			ok = mchNetLobbyHost(nm_name[0] ? nm_name : "host",
					     nm_game,
					     nm_fig,nm_sp);
			}
		else {
			ok = mchNetLobbyJoin(nm_addr,
					     nm_name[0] ? nm_name : "player",
					     nm_fig,nm_sp);
			}

		if(ok){
			nm_lobby = 1;
			mchNetLobbySetGameName(nm_game);
			nm_refresh();
		}
		return;
	}

	if(nm_host){
		mchNetLobbyHostStart();
		nm_leave_to_race();
		return;
	}

	// Client: cancel and go back to setup.
	mchNetLobbyClose();
	nm_lobby = 0;
	nm_refresh();
}

/* ------------------------------- building -------------------------------- */

static void nm_add_event(aciScreenObject* obj,int key,int row,int dir)
{
	aciScreenEvent* ev = new aciScreenEvent;
	aciScreenEventCommand* c = new aciScreenEventCommand;

	ev -> AddKey(key,0);
	ev -> MaxTimer = 1;

	c -> code = ACS_EXT_EVENT;
	c -> data[0] = ACS_EXT_NET_MENU;
	c -> data[1] = row;
	c -> data[2] = dir;
	c -> data[3] = 0;
	c -> StartTimer = 0;

	ev -> AddCommand(c);
	obj -> AddEvent(ev);
}

// One text row. proto, when present, is an existing item to borrow the look
// (palette, font, colours) from, so the menu matches the rest of the game.
static aciScreenInputField* nm_add_field(aciScreen* scr,aciScreenInputField* proto,
					 int id,int x,int y,int sx,int str_len)
{
	aciScreenInputField* o = new aciScreenInputField;

	o -> ID = id;
	o -> type = ACS_INPUT_FIELD_OBJ;

	if(proto){
		o -> PalID = proto -> PalID;
		o -> fonResID = proto -> fonResID;
		o -> font = proto -> font;
		o -> Space = proto -> Space;
		o -> Color0 = proto -> Color0;
		o -> Color1 = proto -> Color1;
		o -> Color2 = proto -> Color2;
		o -> Alpha0 = proto -> Alpha0;
		o -> Alpha1 = proto -> Alpha1;
		o -> Alpha2 = proto -> Alpha2;
	}
	else {
		o -> PalID = ACS_MAIN_MENU_PAL0;
		o -> fonResID = -1;
		o -> font = 1;
		o -> Space = 1;
		o -> Color0 = 140;
		o -> Color1 = 190;
		o -> Color2 = -1;
		o -> Alpha0 = o -> Alpha1 = o -> Alpha2 = -1;
	}

	o -> PosX = x;
	o -> PosY = y;
	o -> SizeX = sx;
	o -> SizeY = 20;

	o -> MaxStrLen = str_len;
	o -> alloc_str();

	o -> AlphaCur = -1;
	o -> AlphaDest = -1;
	o -> dAlpha = 0;

	// Keyboard and mouse selection, same as the main menu items: this is what
	// lets the arrows find the next row and the hover highlight work.
	o -> flags = ACS_KEYB_SELECT | ACS_MOUSE_SELECT;

	o -> objIndex[0] = o -> objIndex[1] = o -> objIndex[2] = o -> objIndex[3] = 0;

	o -> backup_coords();
	scr -> AddObject(o);

	return o;
}

// The look of the ordinary menu rows: any input field on the main menu. Falls
// back to NULL, in which case nm_add_field uses its own colours.
static aciScreenInputField* nm_find_proto(void)
{
	aciScreen* scr = acsScrD -> GetScreen(ACS_MAIN_MENU_SCR0);
	aciScreenObject* p;

	if(!scr) return NULL;

	p = (aciScreenObject*)scr -> objList -> fPtr;
	while(p){
		if(p -> type == ACS_INPUT_FIELD_OBJ) return (aciScreenInputField*)p;
		p = (aciScreenObject*)p -> next;
	}
	return NULL;
}

void acsInitNetMenu(void)
{
	aciScreen* scr;
	aciScreenInputField* proto;
	aciScreenObject* p;
	int i,y;

	if(nm_built) return;

	// The screen itself. It is not in iscreen.scb, so it is created here: id
	// ACS_NET_GAME_SCR, no background (the hot-seat screen has none either),
	// reached from the main menu's "Сетевая игра" item.
	if(nm_screen()) return;

	scr = new aciScreen;
	scr -> ID = ACS_NET_GAME_SCR;
	scr -> backResID = 0;
	acsScrD -> AddScreen(scr);

	// acsPrepare already offset every other screen; this one was created after,
	// so it gets the same shift here.
	scr -> ChangeCoords((XGR_MAXX - 640)/2,(XGR_MAXY - 480)/2);

	proto = nm_find_proto();

	// Parts must be known before any spell default or cycle touches the pool.
	nm_ensure_pool();

	const int lx = 40, vx = 260, w = 340;

	// Title.
	nm_add_field(scr,proto,NM_TITLE,lx,30,w,32);

	// Game name row.
	nm_add_field(scr,proto,NM_LBL(NM_GAMENAME),lx,70,w,32);
	nm_add_field(scr,proto,NM_GAMENAME,vx,70,w,NET_GAME_NAME_LEN);

	// Setup rows.
	y = 110;
	{
		const int rows[] = { NM_HOST,NM_ADDR,NM_NAME,NM_FIG,NM_SP1,NM_SP2,NM_SP3,NM_WORLD };
		for(i = 0; i < (int)(sizeof(rows)/sizeof(rows[0])); i ++){
			nm_add_field(scr,proto,NM_LBL(rows[i]),lx,y,w,32);
			nm_add_field(scr,proto,rows[i],vx,y,w,110);
			y += 28;
		}
	}

	// Roster list: header plus five lines, one per slot, laid over the setup
	// rows' space - only one of the two is ever visible.
	nm_add_field(scr,proto,NM_LBL(NM_PLAYHDR),lx,110,w,32);
	for(i = 0; i < NM_ROWS; i ++)
		nm_add_field(scr,proto,NM_LIST0 + i,vx - 220,138 + 28 * i,w + 220,80);

	// Status, action, back.
	nm_add_field(scr,proto,NM_STATUS,lx,330,w,80);
	nm_add_field(scr,proto,NM_ACTION,lx,370,w,32);
	nm_add_field(scr,proto,NM_BACK,lx,400,w,32);

	// Events. Selectors take left/right clicks as value changes; text rows
	// open the editor; action and back act once, below.
	for(i = 0; i < (int)(sizeof(nm_chain)/sizeof(nm_chain[0])); i ++){
		int id = nm_chain[i];
		aciScreenObject* o = nm_obj(id);
		if(!o) continue;

		// Action and back are wired separately: adding them here as well
		// would fire them twice per click, and one click would both create
		// the lobby and start the race.
		if(id == NM_ACTION || id == NM_BACK) continue;

		if(id == NM_ADDR || id == NM_NAME || id == NM_GAMENAME){
			nm_add_event(o,VK_MOUSE_LBUTTON,id,0);
		}
		else {
			nm_add_event(o,VK_MOUSE_LBUTTON,id,1);
			nm_add_event(o,VK_MOUSE_RBUTTON,id,-1);
		}
	}

	if((p = nm_obj(NM_ACTION))) nm_add_event(p,VK_MOUSE_LBUTTON,NM_ACTION,0);
	if((p = nm_obj(NM_BACK)))  nm_add_event(p,VK_MOUSE_LBUTTON,NM_BACK,0);

	// Seed the defaults from the command line and the engine's own defaults,
	// so the menu opens on something sensible.
	if(mchNetJoinAddr[0])
		strncpy(nm_addr,mchNetJoinAddr,sizeof(nm_addr) - 1);
	if(hsPlayers[0].name[0])
		strncpy(nm_name,hsPlayers[0].name,sizeof(nm_name) - 1);
	if(mchNetMenuName[0])
		strncpy(nm_name,mchNetMenuName,sizeof(nm_name) - 1);

	nm_fig = (mchDefaultFigure >= 0 ? mchDefaultFigure : 0) + 1;
	if(mch_trackD && !mch_trackD -> FigureEnabledHS(nm_fig - 1))
		nm_cycle_figure(1);

	for(i = 0; i < 3; i ++)
		nm_sp[i] = nm_first_spell(i);

	if(mchCurrentWorld >= 0)
		nm_world = mchCurrentWorld;

	// Seed the two editable rows once, now that their state is known. They are
	// never written again on refresh, so a typed name survives a mechos pick.
	nm_set_text(NM_ADDR,nm_addr);
	nm_set_text(NM_NAME,nm_name[0] ? nm_name : "-");

	nm_refresh();

	nm_built = 1;
}

/* ------------------------------ input hooks ------------------------------ */

/*
	Left/right on the line under the cursor changes its value. The dispatcher
	would otherwise read left/right as "move the cursor", so this runs before
	it and eats those two keys on a selector row. A text field being edited
	owns its keys, so nothing is swallowed while one is active.
*/
int acsNetMenuKey(int key)
{
	static const int rows[] = { NM_HOST,NM_FIG,NM_SP1,NM_SP2,NM_SP3,NM_WORLD };
	int mx,my;
	int i;

	if(key != VK_LEFT && key != VK_RIGHT) return 0;
	if(acsScrD -> activeInput) return 0;

	mx = XGR_MouseObj.PosX + XGR_MouseObj.SizeX/2;
	my = XGR_MouseObj.PosY + XGR_MouseObj.SizeY/2;

	for(i = 0; i < (int)(sizeof(rows)/sizeof(rows[0])); i ++){
		aciScreenObject* o = nm_obj(rows[i]);
		if(!o || !o -> CheckXY(mx,my)) continue;

		acsNetMenuExtEvent(rows[i],key == VK_RIGHT ? 1 : -1);
		return 1;
	}
	return 0;
}

int acsNetMenuExtEvent(int row,int dir)
{
	switch(row){
		case NM_HOST:
			if(nm_lobby) return 1;
			nm_host = !nm_host;
			break;
		case NM_ADDR:
			if(nm_host || nm_lobby) return 1;
			acsScrD -> PrepareInput(NM_ADDR);
			return 1;
		case NM_NAME:
			if(nm_lobby) return 1;
			acsScrD -> PrepareInput(NM_NAME);
			return 1;
		case NM_GAMENAME:
			if(!nm_host) return 1;
			acsScrD -> PrepareInput(NM_GAMENAME);
			return 1;
		case NM_FIG:
			if(nm_lobby) return 1;
			nm_cycle_figure(dir ? 1 : -1);
			break;
		case NM_SP1:
			if(nm_lobby) return 1;
			nm_cycle_spell(nm_sp_idx(NM_SP1),dir ? 1 : -1);
			break;
		case NM_SP2:
			if(nm_lobby) return 1;
			nm_cycle_spell(nm_sp_idx(NM_SP2),dir ? 1 : -1);
			break;
		case NM_SP3:
			if(nm_lobby) return 1;
			nm_cycle_spell(nm_sp_idx(NM_SP3),dir ? 1 : -1);
			break;
		case NM_WORLD:
			if(!nm_host) return 1;
			nm_cycle_world(dir ? 1 : -1);
			break;
		case NM_ACTION:
			nm_action();
			return 1;
		case NM_BACK:
			nm_back_to_menu();
			return 1;
		default:
			return 0;
	}

	nm_refresh();
	return 1;
}

/* ------------------------------ per frame -------------------------------- */

/*
	Called once a frame from acsQuant while the screen is up. Pumps the lobby
	and pulls the roster into the list. A client that sees the host's start
	leaves the menu and loads.
*/
void acsNetMenuQuant(void)
{
	if(!nm_built || !nm_lobby) return;

	mchNetLobbyQuant();

	if(!nm_host && mchNetLobbyStarted()){
		nm_capture_local();
		mchNetLobbyClientArm();
		nm_leave_to_race();
		return;
	}

	nm_refresh();
}
