
// Some special keys...
#define ACS_STARTUP_KEY 	0x2001

#define ACS_ALPHA_SPEED		24

// aciScreenEventCommand::code values...
#define ACS_SET_RESOURCE		0
#define ACS_EXEC_SEQUENCE		1
#define ACS_EXT_EVENT			2
#define ACS_EXIT			3
#define ACS_CHANGE_SCROLLER		4
#define ACS_INPUT_STRING		5
#define ACS_CHANGE_STR_STATE		6
#define ACS_CHANGE_SCREEN		7
#define ACS_CHANGE_SCREEN_NEXT		8
#define ACS_CHANGE_SCREEN_PREV		9
#define ACS_INC_STATE			10
#define ACS_DEC_STATE			11
#define ACS_HIDE_OBJ			12
#define ACS_SHOW_OBJ			13
#define ACS_LOCK_OBJ			14
#define ACS_UNLOCK_OBJ			15
#define ACS_INPUT_KEYNAME		16
#define ACS_ALPHA_SEQ			17
#define ACS_DISABLE_EVENTS		18
#define ACS_ENABLE_EVENTS		19
#define ACS_SET_FULL_REDRAW		20
#define ACS_IWORLD_EVENT		21
#define ACS_DEC_SCROLLER		22
#define ACS_INC_SCROLLER		23

// iSceen exit codes...
#define ACS_EXIT_GAME				1
#define ACS_NEW_GAME				2
#define ACS_NEW_HOTSEAT_S_GAME			3
#define ACS_NEW_HOTSEAT_E_GAME			4
#define ACS_CONTINUE_GAME			5

// External events...
#define	ACS_EXT_ELECTIONARY_MODE		1
#define ACS_EXT_DONE_INPUT			2
#define ACS_EXT_RESET_CONTROLS			3
#define ACS_EXT_INIT_ESSENCES			4
#define ACS_EXT_ESSENCE_EVENT			5
#define ACS_EXT_TURN_BASED_MODE			6
#define ACS_EXT_INIT_RACERS			7
#define ACS_EXT_SAVE_GAME			8
#define ACS_EXT_LOAD_GAME			9
#define ACS_EXT_CHOOSE_SLOT			10
#define ACS_EXT_INIT_MM_OPTIONS			11
#define ACS_EXT_INC_MM_OPTION			12
#define ACS_EXT_UPDATE_MM_OPTION		13
#define ACS_EXT_DEC_MM_OPTION			14
#define ACS_EXT_SET_NUM_TRACKS			15
#define ACS_EXT_INIT_WORLDS			16
#define ACS_EXT_ABORT_RACE			17
#define ACS_EXT_CLEAR_SCREEN			18
#define ACS_EXT_SAVE_OPTIONS			19
#define ACS_EXT_SET_OPTION			20
#define ACS_EXT_GET_OPTION			21
#define ACS_EXT_RESTORE_OPTION			22
#define ACS_EXT_UPDATE_OPTION			23
#define ACS_EXT_SET_GAME_MODE			24
#define ACS_EXT_NEW_GAME			25
#define ACS_EXT_SET_NEXT_SCREEN			26
#define ACS_EXT_NEXT_SCREEN			27
#define ACS_EXT_SAVE_SCREEN			28
#define ACS_EXT_RESTORE_SCREEN			29
#define ACS_EXT_FREE_RESOURCES			30
#define ACS_EXT_CHOOSE_WORLD			31
#define ACS_EXT_CHOOSE_TRACK			32
#define ACS_EXT_START_SOUND			33
#define ACS_EXT_STOP_SOUND			34
#define ACS_EXT_RND_SOUND2			35
#define ACS_EXT_RND_SOUND3			36
#define ACS_EXT_SET_NEXT_SCR_ID			37
#define ACS_EXT_SET_PREV_SCR_ID			38
#define ACS_EXT_SELECT_WORLD			39
#define ACS_EXT_KEYTRAP				40
#define ACS_EXT_INIT_SAVE_SCR			41
#define ACS_EXT_SELECT_SAVE			42
#define ACS_EXT_DELETE_SAVE			43
#define ACS_EXT_SCROLL_SAVES			44
#define ACS_EXT_QUIT				45
#define ACS_EXT_SELECT_CONTROLLER		46
#define ACS_EXT_SET_TURNBASED			47
#define ACS_EXT_CHANGE_HS_WORLD			48
#define ACS_EXT_INIT_SHOP			49
#define ACS_EXT_FINIT_SHOP			50
#define ACS_EXT_CHANGE_PART			51
#define ACS_EXT_PLAY_DEMO			52
#define ACS_EXT_SET_ZBUF			53
#define ACS_EXT_CHANGE_PLAYER			54
#define ACS_EXT_INIT_ONLINE_MENU		55

// Options
#define ACS_OPT_RESOLUTION			0
#define ACS_OPT_AUTO_3D				1
#define ACS_OPT_SOUND				2
#define ACS_OPT_SOUND_VOL			3
#define ACS_OPT_MUSIC				4
#define ACS_OPT_MUSIC_VOL			5
#define ACS_OPT_AUTO_CENTER			6
#define ACS_OPT_SCROLLING_SPEED			7
#define ACS_OPT_SCROLLING_ACC			8
#define ACS_OPT_SCROLLING_DEC			9
#define ACS_OPT_SCROLLING_MODE			10
#define ACS_OPT_GAMMA				11
#define ACS_OPT_SHADOWS				12
#define ACS_OPT_SKY				13
#define ACS_OPT_FURROWS				14
#define ACS_OPT_CHROME				15
#define ACS_OPT_COLOR_DEPTH			16
#define ACS_OPT_FULLSCREEN			17

#define ACS_NUM_OPTIONS				18

// Codes...
#define ACS_WRLD_CHOOSE_KEY			2000

// @caiiiycuk TODO: find correct constants
#define ACS_KEY_INPUT_MODE	10
#define ACS_KEYB_SELECT 0x1000
#define ACS_NO_BACKGROUND 0x2000
#define ACS_NO_RESIZE 0