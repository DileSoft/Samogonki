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
	NM_START,
	NM_BACK,

	NM_LAST
};

// What the menu holds until Start is pressed. nm_host/nm_addr default to the
// command line so -nethost / -netjoin keep working as a shortcut for testing.
static int  nm_host = 1;
static char nm_addr[128] = "127.0.0.1";
static char nm_name[32]  = "";
static int  nm_fig = 1;			// mechos figure, 1..acsMaxEssenceID
static int  nm_sp[3] = { 1, 1, 1 };	// spell part ids, slots front/middle/rear
static int  nm_world = 1;

static int  nm_built = 0;

// The three spell slots are part indices 1..3 of the mechos: index 0 is the
// figure itself, the rest carry one arcane each. The labels say front/middle/
// rear, which is how the constructor orders them on screen.
static const int nm_spell_slot[3] = { 1, 2, 3 };

// Rows in the order they are shown and navigated. Labels are drawn at lx and
// are inert; the value under each is the clickable object.
static const int nm_row_id[]   = { NM_HOST,NM_ADDR,NM_NAME,NM_FIG,NM_SP1,NM_SP2,NM_SP3,NM_WORLD };
static const int nm_label_id[] = { NM_HOST + 100,NM_ADDR + 100,NM_NAME + 100,NM_FIG + 100,
				   NM_SP1 + 100,NM_SP2 + 100,NM_SP3 + 100,NM_WORLD + 100 };

#define NM_ROWS ((int)(sizeof(nm_row_id)/sizeof(nm_row_id[0])))

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

static const char* nm_tr(int id)
{
	static const char* en[] = {
		"Network Race",			// NM_TITLE
		"Mode:",			// NM_HOST
		"Address:",			// NM_ADDR
		"Name:",			// NM_NAME
		"Mechos:",			// NM_FIG
		"Front:",			// NM_SP1
		"Middle:",			// NM_SP2
		"Rear:",			// NM_SP3
		"World:",			// NM_WORLD
		"Start",			// NM_START
		"Back"				// NM_BACK
	};
	static const char* ru[] = {
		"\xD1\xE5\xF2\xE5\xE2\xE0\xFF \xE8\xE3\xF0\xE0",	// Сетевая игра
		"\xD0\xE5\xE6\xE8\xEC:",				// Режим:
		"\xC0\xE4\xF0\xE5\xF1:",				// Адрес:
		"\xC8\xEC\xFF:",					// Имя:
		"\xCC\xE5\xF5\xEE\xF1:",				// Мехос:
		"\xCF\xE5\xF0\xE5\xE4:",				// Перед:
		"\xD1\xE5\xF0\xE5\xE4\xE8\xED\xE0:",			// Середина:
		"\xC7\xE0\xE4:",					// Зад:
		"\xCC\xE8\xF0:",					// Мир:
		"\xCD\xE0\xF7\xE0\xF2\xFC",				// Начать
		"\xCD\xE0\xE7\xE0\xE4"					// Назад
	};

	return nm_is_ru() ? ru[id - NM_TITLE] : en[id - NM_TITLE];
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

static const char* nm_world_str(int world)
{
	const char* name = mch_trackD ? mch_trackD -> TrackName(world,0) : NULL;
	return name ? name : "-";
}

// The bare value of a selector row: the label is a separate object.
static void nm_format_value(int id,char* out,int sz)
{
	switch(id){
		case NM_HOST:  sprintf(out,"%s",nm_host_str()); break;
		case NM_FIG:   sprintf(out,"%s",nm_figure_str(nm_fig)); break;
		case NM_SP1:   sprintf(out,"%s",nm_spell_str(0)); break;
		case NM_SP2:   sprintf(out,"%s",nm_spell_str(1)); break;
		case NM_SP3:   sprintf(out,"%s",nm_spell_str(2)); break;
		case NM_WORLD: sprintf(out,"%s",nm_world_str(nm_world)); break;
		default:       out[0] = 0; break;
	}
	(void)sz;
}

// The address is a client's, the world a host's, so the row that does not
// apply is hidden. Navigation follows objIndex, and GetObjectXY skips a hidden
// target, so the hidden row drops out of the list on its own.
static void nm_apply_visibility(void)
{
	int i;

	for(i = 0; i < NM_ROWS; i ++){
		int show = 1;
		aciScreenObject* v;
		aciScreenObject* l;

		if(nm_row_id[i] == NM_ADDR)  show = !nm_host;
		if(nm_row_id[i] == NM_WORLD) show = nm_host;

		v = nm_obj(nm_row_id[i]);
		l = nm_obj(nm_label_id[i]);

		if(v){
			if(show) v -> flags &= ~(ACS_HIDDEN | ACS_BLOCKED);
			else     v -> flags |=  (ACS_HIDDEN | ACS_BLOCKED);
			v -> flags |= ACS_REDRAW_OBJECT;
		}
		if(l){
			if(show) l -> flags &= ~(ACS_HIDDEN | ACS_BLOCKED);
			else     l -> flags |=  (ACS_HIDDEN | ACS_BLOCKED);
			l -> flags |= ACS_REDRAW_OBJECT;
		}
	}
}

// Refresh every value the state can change. Called on init and after a pick.
static void nm_refresh(void)
{
	char buf[256];

	nm_apply_visibility();

	nm_format_value(NM_HOST,buf,sizeof(buf));  nm_set_text(NM_HOST,buf);
	nm_format_value(NM_FIG,buf,sizeof(buf));   nm_set_text(NM_FIG,buf);
	nm_format_value(NM_SP1,buf,sizeof(buf));   nm_set_text(NM_SP1,buf);
	nm_format_value(NM_SP2,buf,sizeof(buf));   nm_set_text(NM_SP2,buf);
	nm_format_value(NM_SP3,buf,sizeof(buf));   nm_set_text(NM_SP3,buf);
	nm_format_value(NM_WORLD,buf,sizeof(buf)); nm_set_text(NM_WORLD,buf);

	// Address and Name are edited in place: the field is the live text, so a
	// refresh must not write the state variable back into it, or picking a
	// mechos would wipe what the player typed. Seeded once at init.
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

/* ---------------------------- starting the game -------------------------- */

static void nm_back_to_menu(void)
{
	if(acsNumSaves < 1)
		acsScrD -> PrepareChangeScreen(ACS_MAIN_MENU_SCR0);
	else if(acsNumSaves == 1)
		acsScrD -> PrepareChangeScreen(ACS_MAIN_MENU_SCR1);
	else
		acsScrD -> PrepareChangeScreen(ACS_MAIN_MENU_SCR2);
}

static void nm_start_game(void)
{
	aciScreenInputField* f;

	// The address and the name are edited in place, so the field holds the
	// live text and the state variables do not. Read them back here.
	if((f = (aciScreenInputField*)nm_obj(NM_ADDR))) strncpy(nm_addr,f -> string,sizeof(nm_addr) - 1);
	if((f = (aciScreenInputField*)nm_obj(NM_NAME))) strncpy(nm_name,f -> string,sizeof(nm_name) - 1);
	nm_addr[sizeof(nm_addr) - 1] = 0;
	nm_name[sizeof(nm_name) - 1] = 0;

	mchNetGame = 1;
	mchNetHost = nm_host;

	if(!nm_host){
		strncpy(mchNetJoinAddr,nm_addr,sizeof(mchNetJoinAddr) - 1);
		mchNetJoinAddr[sizeof(mchNetJoinAddr) - 1] = 0;
	}

	strncpy(mchNetMenuName,nm_name,sizeof(mchNetMenuName) - 1);
	mchNetMenuName[sizeof(mchNetMenuName) - 1] = 0;

	mchNetMenuFigure = nm_fig;
	for(int i = 0; i < 3; i ++)
		mchNetMenuSpell[i] = nm_sp[i];

	sprintf(mchNetRacerConfig,"%d %d %d %d",nm_fig,nm_sp[0],nm_sp[1],nm_sp[2]);

	// Five laps, always.
	mchNumLaps = 5;
	mchNetMenuWorld = nm_world;
	mchNetMenuTrack = 0;

	// Both sides have to load the same world before the race can start, and
	// the loading screen reads mchCurrentWorld. The host's is its pick; a
	// client's must match it (a lobby that ships the world to waiting clients
	// is the next step - until then both pick the same one).
	mchCurrentWorld = nm_world;
	mchCurrentTrack = 0;

	// What ACS_EXT_SET_GAME_MODE would have cleared for a non hot-seat mode,
	// since this path skips it.
	mchSplitScreenGame = 0;
	mchTurnBasedGame = 0;

	// Same exit the F2 keypress used to produce: the script's
	// $MCH_SINGLE_GAME, the mode a net race rides.
	acsScrD -> QuantCode = 2;
	acsScrD -> flags |= ACS_NEED_EXIT;
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

	const int lx = 40, vx = 260, w = 340, dy = 30;

	// Title.
	nm_add_field(scr,proto,NM_TITLE,lx,40,w,32);

	// Label + value per row.
	y = 80;
	for(i = 0; i < NM_ROWS; i ++){
		nm_add_field(scr,proto,nm_label_id[i],lx,y,w,32);
		nm_add_field(scr,proto,nm_row_id[i],vx,y,w,110);
		y += dy;
	}

	// Start and Back, at the foot of the list.
	nm_add_field(scr,proto,NM_START,lx,y + 20,w,32);
	nm_add_field(scr,proto,NM_BACK,lx,y + 50,w,32);

	// Navigation: up/down walk the list, left/right are eaten by
	// acsNetMenuKey() while standing on a selector.
	for(i = 0; i < NM_ROWS; i ++){
		aciScreenObject* o = nm_obj(nm_row_id[i]);
		if(!o) continue;
		o -> objIndex[1] = (i > 0) ? nm_row_id[i - 1] : nm_row_id[NM_ROWS - 1];
		o -> objIndex[3] = (i + 1 < NM_ROWS) ? nm_row_id[i + 1] : NM_START;
	}

	// Events. Selectors take left/right clicks as value changes; text rows
	// open the editor; Start and Back act.
	for(i = 0; i < NM_ROWS; i ++){
		aciScreenObject* o = nm_obj(nm_row_id[i]);
		if(!o) continue;

		if(nm_row_id[i] == NM_ADDR || nm_row_id[i] == NM_NAME){
			nm_add_event(o,VK_MOUSE_LBUTTON,nm_row_id[i],0);
		}
		else {
			nm_add_event(o,VK_MOUSE_LBUTTON,nm_row_id[i],1);
			nm_add_event(o,VK_MOUSE_RBUTTON,nm_row_id[i],-1);
		}
	}

	if((p = nm_obj(NM_START))) nm_add_event(p,VK_MOUSE_LBUTTON,NM_START,0);
	if((p = nm_obj(NM_BACK)))  nm_add_event(p,VK_MOUSE_LBUTTON,NM_BACK,0);

	// Fill in the strings: title, labels, then the live values.
	nm_set_text(NM_TITLE,nm_tr(NM_TITLE));
	for(i = 0; i < NM_ROWS; i ++)
		nm_set_text(nm_label_id[i],nm_tr(nm_row_id[i]));
	nm_set_text(NM_START,nm_tr(NM_START));
	nm_set_text(NM_BACK,nm_tr(NM_BACK));

	// Seed the defaults from the command line and the engine's own defaults,
	// so the menu opens on something sensible.
	if(mchNetJoinAddr[0])
		strncpy(nm_addr,mchNetJoinAddr,sizeof(nm_addr) - 1);
	if(hsPlayers[0].name[0])
		strncpy(nm_name,hsPlayers[0].name,sizeof(nm_name) - 1);

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
			nm_host = !nm_host;
			break;
		case NM_ADDR:
			acsScrD -> PrepareInput(NM_ADDR);
			return 1;
		case NM_NAME:
			acsScrD -> PrepareInput(NM_NAME);
			return 1;
		case NM_FIG:
			nm_cycle_figure(dir ? 1 : -1);
			break;
		case NM_SP1:
			nm_cycle_spell(0,dir ? 1 : -1);
			break;
		case NM_SP2:
			nm_cycle_spell(1,dir ? 1 : -1);
			break;
		case NM_SP3:
			nm_cycle_spell(2,dir ? 1 : -1);
			break;
		case NM_WORLD:
			nm_cycle_world(dir ? 1 : -1);
			break;
		case NM_START:
			nm_start_game();
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
