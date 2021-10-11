
#ifndef __ONLINE_GAME_H__
#define __ONLINE_GAME_H__

//#define _OFFLINE_DEBUG_

#ifndef _FINAL_VERSION_
#define _DUMP_PACKETS_
#endif

class ogBuffer
{
	char* buf;
	int radix;
	int digits;
	unsigned int size;
	unsigned int offset;

	unsigned char separator;

	int make_free;

	void realloc(unsigned int sz);
public:
	void write_skip(void);
	void read_skip(void);

	void set_separator(unsigned char sp){ separator = sp; }
	unsigned int token_length(void);

	void set_radix(int r) { radix = r; }
	void set_digits(int d) { digits = d; }

	void alloc(unsigned int sz);
	void free(void);
	void fill(char fc = '\0');
	void set(int off);
	unsigned int read(void* s,unsigned int len);
	unsigned int write(const void* s,unsigned int len);
	void init(void){ offset = 0; *buf = 0; }
	int end(void){ return (offset >= size); }

	unsigned int tell(void){ return offset; }
	unsigned int length(void){ return size; }
	char* address(void){ return buf; }

	ogBuffer& operator< (const char*);
	ogBuffer& operator< (char);
	ogBuffer& operator< (unsigned char);
	ogBuffer& operator< (short);
	ogBuffer& operator< (unsigned short);
	ogBuffer& operator< (int);
	ogBuffer& operator< (unsigned int);
	ogBuffer& operator< (long);
	ogBuffer& operator< (unsigned long);
	ogBuffer& operator< (float);
	ogBuffer& operator< (double);
	ogBuffer& operator< (long double);

	ogBuffer& operator> (char*);
	ogBuffer& operator> (char&);
	ogBuffer& operator> (unsigned char&);
	ogBuffer& operator> (short&);
	ogBuffer& operator> (unsigned short&);
	ogBuffer& operator> (int&);
	ogBuffer& operator> (unsigned int&);
	ogBuffer& operator> (long&);
	ogBuffer& operator> (unsigned long&);
	ogBuffer& operator> (float&);
	ogBuffer& operator> (double&);
	ogBuffer& operator> (long double&);

	ogBuffer(unsigned int sz){ alloc(sz); offset = 0; digits = 8; radix = 10; separator = ';'; }
	ogBuffer(void* p,int sz);
	~ogBuffer(void){ free(); }
};

const int WI_LID_LEN		= 3;
const int WI_PASS_LEN		= 100;
const int WI_STRINGS_LEN	= 20;
const int WI_NIC_LEN		= 10;

struct ogPlayerInfo
{
	int UID;		// ���������� ����� ������. ��� ��������� ������, ���� ����� ��������� � OWNER_UID
	char NIC[WI_NIC_LEN];	// NIC ������
	int PERS_CAR_COMP_ID;	// ID ���������
	int FRONT_CAR_COMP_ID;	// �������
	int FWHEEL_CAR_COMP_ID;	// �������� ������
	int BWHEEL_CAR_COMP_ID;	// ������ ������
	char ROBOT;		// Y/N

	void dump(char* str);
};

struct ogSeedInfo
{
	int X,Y,Z;
	int ArcaneID;

	void decode(ogBuffer& buf,int last);
	void encode(ogBuffer& buf,int last);
};

struct ogPlayerTurnInfo
{
	int UID; 		// ���������� ��� ������
	char IS_FINISHED;	// Y/N - ����������� ����� ��� ���
	int RANK;		// ����� �����, ���������� ������� � �����.
	int MOVE_TIME;		// ����� � �������� �� ������ �����. 
	int MOVE_STEPS;		// ����� ����� �����, ��������� �������. 
	int BOTTLES_CNT;	// ���������� ��������� ���������.
	int TOTAL_SEEDS_CNT;	// ����� ���������� ������������ �����. 
	int ARCANES_CNT;	// ���������� ��������������� ����������.
	int DESTROYS_CNT;	// ���������� ������������.

	int USER_SEEDS_CNT;	// ����� ����� ������� ������. 

	ogSeedInfo* seeds;

	void decode(ogBuffer& buf);
	void encode(ogBuffer& buf);

	void alloc_seeds(int num = 0);
	void free_seeds(void);

	void init_seed(int idx,int x,int y,int z,int a_id);
	void get_seed(int idx,int& x,int& y,int& z,int& a_id);

	void dump(char* str);

	ogPlayerTurnInfo(void);
	~ogPlayerTurnInfo(void);
};

struct ogTurnInfo
{
	int STEP_ID;	// ����� ����, ������� � �������
	int USERS_CNT;	// ����� ������� ��������� ���� ���

	ogPlayerTurnInfo** turns;

	void decode(ogBuffer& buf);
	void encode(ogBuffer& buf);

	void alloc_turns(int num = 0);
	void free_turns(void);

	ogPlayerTurnInfo* get_player_turn(int uid);

	void dump(char* str);

	ogTurnInfo(void);
	~ogTurnInfo(void);
};

enum ogPacketTypes {
	OG_GAME_PACKET = 1, 	  // 1 - ������, ������������ �� ������� � ���� � ���������� � ����������� � ��������� �����.
	OG_CONTROL_PACKET,	  // 2 - ������ �� ������� ������ � ��������� ������ � ����������� ������� ����.
	OG_SEEDS_PACKET,	  // 3 - ���� �������.
	OG_COMPLETED_GAME_PACKET, // 4 - ���������� �� ������������ ����.
	OG_SYS_PACKET,		  // 5 - ���������������� ��� ������� ��� �������� ������������ ����.
	OG_REFRESH_PACKET,	  // 6 - ���������� ���������� � ���� ��� ��������-����
	OG_REFRESH_ANSWER_PACKET, // 7 - ����� �� 6� �����
	OG_ARCADE_GAME_PACKET	  // 8 - ������ �������� ������ � �������
};

class ogPacket
{
	char START_STRING[WI_STRINGS_LEN];	// �����-������. ������������� ������. �������� 'KDLAB'
	int Version;				// ����� ������ API. �������� ������ 100 (������ 1.00)

	int TType;	// ��� ������:

	int ID;		// ���������� ������������� ����

	char LID[WI_LID_LEN];		// ������������� �����. � ���� ������ ������ 'ru'
	int GAME_OWNER_UID;		// ���������� ����� ������ ��������� ����.
	int OWNER_UID;			// ���������� ����� ��������� ������. �������� ��������.
	char PASSWORD[WI_PASS_LEN];	// ������ ������� ��� ��������� ������. ����� ������, ������������� ������������� ���� �� ������ ��� ������ OWNER_UID. ���������� ������ ��� ������� ������ � ������ ����� ����.

	int KD_WORLD_ID;		// ����� ����
	int KD_ROUTE_ID;		// ����� ������

	int GAME_RND;			// ��������� ����� ��� ����
	char GTYPE;			// 'W' - �� ���������� ��� 'A' - ������ ������ 

	int LAPS;			// ����� ������
	int SEEDS;			// ����� ����� �� ���

	int DURATION;			// ������������ ���� � �������
	int MOVE_CNT;			// ����� ������ ��������� �����
	int PLAYERS_CNT;		// ����� �������
	int STEPS_CNT;			// ����� �����, ������������ � ������ ������

	char EXPRESS;			// Y/N -�������� ���� ��� ���

	int URL_POST_SZ;
	char* URL_POST;			// ����� (URL) ������� ��� �������� ���������� ����
	int URL_POST_PORT;		// ����� ����� ��� ���������� � ��������
	int URL_POST_PATH_SZ;
	char* URL_POST_PATH;		// ���� � ��������� �� ������� ��� �������� ������

	int URL_RETURN_SZ;
	char* URL_RETURN;		// ����� ��� �������� ������ �� ������

	ogPlayerInfo players[5];

	ogTurnInfo** turns;

	char STOP_STRING[WI_STRINGS_LEN];	// ����������� ������������� ������. �������� "BITRIX".

	void alloc_post_url(int sz);
	void alloc_post_path_url(int sz);
	void alloc_return_url(int sz);

public:
	int post_port(void) const { return URL_POST_PORT; }
	char* post_url(void) const { return URL_POST; }
	char* post_path_url(void) const { return URL_POST_PATH; }

	char* return_url(void) const { return URL_RETURN; }

	void copy_header(ogPacket& p);

	void alloc_turns(int num = 0);
	void free_turns(void);

	int decode(void* p,int sz);
	int decode(ogBuffer& buf);

	void dump(void);

	void encode(ogBuffer& buf);

	void set_type(int t){ TType = t; }
	int get_type(void) const { return TType; }

	int num_turns(void) const { return STEPS_CNT; }
	int turn_max(void) const { return MOVE_CNT; }
	int num_players(void) const { return PLAYERS_CNT; }
	int world(void) const { return KD_WORLD_ID; }
	int track(void) const { return KD_ROUTE_ID; }
	int rnd_seed(void) const { return GAME_RND; }
	int num_laps(void) const { return LAPS;	}
	int turn_seeds(void) const { return SEEDS; }

	ogTurnInfo* get_turn_by_idx(int idx);
	ogTurnInfo* get_turn_by_num(int num);

	ogPlayerInfo* get_player(int pl_num){ return &players[pl_num]; }
	ogPlayerTurnInfo* get_player(int turn_num,int pl_num);
	int get_player_id(int uid);
	int get_player_uid(int id) const { return players[id].UID; }

	int owner_uid(void) const { return OWNER_UID; }

	int express_game(void) const { return (EXPRESS == 'Y'); }
	int full_game(void) const { return (GTYPE == 'A'); }

	int check_return(char* p);

	ogPacket(void);
	~ogPacket(void);
};

extern ogPacket og_inP;
extern ogPacket og_inP2;
extern ogPacket og_outP;

extern char* ogErrorStr;

void hsOnlineSetup(void);

void ogQuant(void);
char* ogStatus(void);

void ogSetRefreshTime(void);
int ogCheckRefreshTime(void);

void ogLogStr(char* text);
void ogLogError(char* text,int uid,int turn,int prm0,int prm1);
int ogGetErrorStr(void);

#endif /* __ONLINE_GAME_H__ */