#include "lobby_server.h"

#include "net_socket.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdarg>
#include <string>
#include <chrono>

namespace {

unsigned int now_ms(void)
{
	using namespace std::chrono;

	return (unsigned int)duration_cast<milliseconds>(
		steady_clock::now().time_since_epoch()).count();
}

// Past the current word and any run of spaces, so a line can be walked with
// "value = atoi(p); p = next_word(p);". A trailing free-form field is whatever
// is left after the last next_word().
char* next_word(char* p)
{
	while(*p && *p != ' ') p ++;
	while(*p == ' ') p ++;

	return p;
}

} /* anonymous namespace */

/* ------------------------------------------------------------------------- */

struct lobbyServer::Impl
{
	struct Conn
	{
		netSocket sock;
		unsigned int peer_ip;
		unsigned short peer_port;
		char token[LOBBY_TOKEN_LEN];
		char inbuf[LOBBY_MAX_LINE * 4];
		int inlen;
		int is_host;
		unsigned short match_id;	// 0 when not registered
		unsigned int udp_ip;
		unsigned short udp_port;
		int has_udp;
		unsigned int last_ms;
		Conn* next;
	};

	struct Match
	{
		unsigned short id;
		unsigned short players;
		unsigned short max_players;
		unsigned short world;
		unsigned short laps;
		char name[LOBBY_MATCH_NAME_LEN];
		Conn* host;
		unsigned int last_ms;
		Match* next;
	};

	netSocket tcp;
	netSocket udp;
	unsigned short port;

	Conn* conns;
	int conn_count;

	Match* matches;
	unsigned short match_seq;

	unsigned int token_seq;

	lobbyLogFn log;
	void* log_user;

	Impl(void)
	{
		tcp = NET_SOCKET_INVALID;
		udp = NET_SOCKET_INVALID;
		port = 0;
		conns = NULL;
		conn_count = 0;
		matches = NULL;
		match_seq = 0;
		token_seq = 0;
		log = NULL;
		log_user = NULL;
	}

	void note(const char* fmt,...)
	{
		char buf[256];
		va_list arg;

		if(!log) return;

		va_start(arg,fmt);
		vsnprintf(buf,sizeof(buf),fmt,arg);
		va_end(arg);

		log(buf,log_user);
	}

	/* ------------------------------ sending ------------------------------ */

	void send_line(Conn* c,const char* fmt,...)
	{
		char buf[LOBBY_MAX_LINE];
		va_list arg;
		int n;

		if(!c || c -> sock == NET_SOCKET_INVALID) return;

		va_start(arg,fmt);
		n = vsnprintf(buf,sizeof(buf) - 2,fmt,arg);
		va_end(arg);

		if(n < 0) return;

		if(n > (int)sizeof(buf) - 2) n = (int)sizeof(buf) - 2;
		buf[n] = '\n';
		buf[n + 1] = 0;

		// Control lines are tiny and the kernel send buffer is far larger, so
		// one send() places the whole line. A would-block here would be a
		// client that stopped reading, which the timeout collects anyway.
		netSocketTcpSend(c -> sock,buf,n + 1);
	}

	/* ------------------------------ lookup -------------------------------- */

	Conn* conn_by_token(const char* token)
	{
		Conn* c = conns;

		while(c){
			if(!strcmp(c -> token,token)) return c;
			c = c -> next;
		}

		return NULL;
	}

	Match* match_by_id(unsigned short id)
	{
		Match* m = matches;

		while(m){
			if(m -> id == id) return m;
			m = m -> next;
		}

		return NULL;
	}

	Match* match_by_host(Conn* host)
	{
		Match* m = matches;

		while(m){
			if(m -> host == host) return m;
			m = m -> next;
		}

		return NULL;
	}

	/* ------------------------------ teardown ------------------------------ */

	void remove_match(Match* m)
	{
		if(!m) return;

		if(m -> host) m -> host -> match_id = 0;

		if(matches == m) matches = m -> next;
		else {
			Match* p = matches;
			while(p && p -> next != m) p = p -> next;
			if(p) p -> next = m -> next;
		}

		note("match %u closed: %s",(unsigned)m -> id,m -> name);

		delete m;
	}

	void remove_conn(Conn* c)
	{
		if(!c) return;

		if(c -> is_host){
			Match* m = match_by_host(c);
			if(m) remove_match(m);
		}

		if(conns == c) conns = c -> next;
		else {
			Conn* p = conns;
			while(p && p -> next != c) p = p -> next;
			if(p) p -> next = c -> next;
		}

		netSocketClose(c -> sock);
		conn_count --;

		delete c;
	}

	void add_conn(netSocket sock,unsigned int ip,unsigned short port_no)
	{
		if(sock == NET_SOCKET_INVALID) return;

		Conn* c = new Conn;

		memset(c,0,sizeof(*c));
		c -> sock = sock;
		c -> peer_ip = ip;
		c -> peer_port = port_no;
		c -> inlen = 0;
		c -> is_host = 0;
		c -> match_id = 0;
		c -> has_udp = 0;
		c -> last_ms = now_ms();
		c -> next = conns;

		conns = c;
		conn_count ++;
	}

	/* ------------------------------ protocol ------------------------------ */

	int handle_hello(Conn* c,char* args)
	{
		int version = atoi(args);

		if(version != LOBBY_PROTO_VERSION){
			send_line(c,"ERROR protocol version %d, server is %d",
				  version,LOBBY_PROTO_VERSION);
			remove_conn(c);
			return 0;
		}

		snprintf(c -> token,LOBBY_TOKEN_LEN,"%05x%05x",
			 (unsigned)(token_seq ++) & 0xFFFFF,
			 (unsigned)(now_ms() & 0xFFFFF));

		send_line(c,"HELLO %d %s",LOBBY_PROTO_VERSION,c -> token);

		note("connection %s:%u hello",ip_string(c -> peer_ip).c_str(),
		     (unsigned)c -> peer_port);

		return 1;
	}

	void handle_register(Conn* c,char* args)
	{
		int world,laps,players,max;
		char name[LOBBY_MATCH_NAME_LEN];
		char* p = args;
		Match* m;

		if(!c -> token[0]){
			send_line(c,"ERROR say hello first");
			return;
		}

		world = atoi(p);		p = next_word(p);
		laps = atoi(p);			p = next_word(p);
		players = atoi(p);		p = next_word(p);
		max = atoi(p);			p = next_word(p);

		strncpy(name,p,sizeof(name) - 1);
		name[sizeof(name) - 1] = 0;

		// A host that re-registers is updating its own entry, not opening a
		// second one: the player count moves as people join its roster.
		m = match_by_host(c);

		if(!m){
			m = new Match;
			memset(m,0,sizeof(*m));
			m -> id = (unsigned short)(++ match_seq);
			m -> host = c;
			m -> next = matches;
			matches = m;

			c -> is_host = 1;
			c -> match_id = m -> id;
		}

		m -> world = (unsigned short)world;
		m -> laps = (unsigned short)laps;
		m -> players = (unsigned short)players;
		m -> max_players = (unsigned short)max;
		strncpy(m -> name,name,sizeof(m -> name) - 1);
		m -> name[sizeof(m -> name) - 1] = 0;
		m -> last_ms = now_ms();

		send_line(c,"REGISTERED %u",(unsigned)m -> id);

		note("match %u \"%s\" world %d laps %d players %d/%d",
		     (unsigned)m -> id,m -> name,world,laps,players,max);
	}

	void handle_list(Conn* c)
	{
		Match* m;
		int count = 0;

		for(m = matches; m; m = m -> next)
			count ++;

		send_line(c,"LIST %d",count);

		for(m = matches; m; m = m -> next){
			send_line(c,"MATCH %u %u %u %u %u %s",
				  (unsigned)m -> id,
				  (unsigned)m -> players,(unsigned)m -> max_players,
				  (unsigned)m -> world,(unsigned)m -> laps,
				  m -> name);
		}

		send_line(c,"END");

		note("list sent to %s:%u: %d match(es)",
		     ip_string(c -> peer_ip).c_str(),(unsigned)c -> peer_port,count);
	}

	void handle_join(Conn* c,char* args)
	{
		unsigned short id = (unsigned short)atoi(args);
		Match* m = match_by_id(id);

		if(!m){
			send_line(c,"ERROR no match %u",(unsigned)id);
			return;
		}

		if(!c -> has_udp){
			send_line(c,"ERROR no udp probe received");
			return;
		}

		if(!m -> host || !m -> host -> has_udp){
			send_line(c,"ERROR host is not reachable yet");
			return;
		}

		// Both sides get the other's public UDP endpoint in the same breath,
		// so both punch outward at once and whichever NAT is strict finds the
		// other already knocking.
		send_line(c,"PEER %s %u",
			  ip_string(m -> host -> udp_ip).c_str(),
			  (unsigned)m -> host -> udp_port);

		send_line(m -> host,"PEER %s %u",
			  ip_string(c -> udp_ip).c_str(),
			  (unsigned)c -> udp_port);

		note("match %u: %s:%u joined, introduced to the host",
		     (unsigned)id,ip_string(c -> peer_ip).c_str(),
		     (unsigned)c -> peer_port);
	}

	// Returns 0 when the connection was removed, so the caller stops touching
	// its buffer.
	int handle_line(Conn* c,char* line)
	{
		// Trim a trailing carriage return so a client that sends CRLF is not
		// mis-parsed.
		int n = (int)strlen(line);
		while(n > 0 && (line[n - 1] == '\r' || line[n - 1] == '\n')) line[-- n] = 0;

		if(!n) return 1;

		if(!strncmp(line,"HELLO",5)) return handle_hello(c,next_word(line));
		if(!strncmp(line,"REGISTER",8)){ handle_register(c,next_word(line)); return 1; }
		if(!strncmp(line,"UNREGISTER",10)){
			Match* m = match_by_host(c);
			if(m) remove_match(m);
			return 1;
		}
		if(!strncmp(line,"LIST",4)){ handle_list(c); return 1; }
		if(!strncmp(line,"JOIN",4)){ handle_join(c,next_word(line)); return 1; }
		if(!strncmp(line,"BYE",3)){ remove_conn(c); return 0; }

		send_line(c,"ERROR unknown message");
		return 1;
	}

	void read_conn(Conn* c)
	{
		for(;;){
			int room = (int)sizeof(c -> inbuf) - 1 - c -> inlen;
			int n;

			if(room <= 1){
				// A line longer than the buffer is a broken client; drop it
				// rather than grow without bound.
				remove_conn(c);
				return;
			}

			n = netSocketTcpRecv(c -> sock,c -> inbuf + c -> inlen,room);

			if(n == NET_SOCKET_WOULDBLOCK) break;
			if(n <= 0){
				// 0 is an orderly close, any error is gone too.
				remove_conn(c);
				return;
			}

			c -> inlen += n;
			c -> last_ms = now_ms();

			c -> inbuf[c -> inlen] = 0;

			// A datagram never arrives over TCP, so every complete line is
			// handled in arrival order.
			char* start = c -> inbuf;

			for(;;){
				char* nl = strchr(start,'\n');
				if(!nl) break;

				*nl = 0;

				// A hello of the wrong version or a goodbye frees us; stop at
				// once rather than touch the buffer that went with it.
				if(!handle_line(c,start)) return;

				start = nl + 1;
			}

			// Keep the unterminated remainder.
			int rem = (int)strlen(start);
			if(start != c -> inbuf) memmove(c -> inbuf,start,rem + 1);
			c -> inlen = rem;
		}
	}

	/* -------------------------------- udp --------------------------------- */

	void handle_probe(const char* data,int len)
	{
		char buf[LOBBY_MAX_LINE];
		const char* p;
		char* token;
		Conn* c;

		if(len <= 0) return;
		if(len > (int)sizeof(buf) - 1) len = (int)sizeof(buf) - 1;

		memcpy(buf,data,len);
		buf[len] = 0;

		if(strncmp(buf,LOBBY_PROBE_PREFIX,strlen(LOBBY_PROBE_PREFIX))) return;

		token = buf + strlen(LOBBY_PROBE_PREFIX);

		// The newline and any padding are not part of the token.
		for(p = token; *p && *p != '\n' && *p != '\r' && *p != ' '; ) p ++;

		char token_copy[LOBBY_TOKEN_LEN];
		int tn = (int)(p - token);
		if(tn >= LOBBY_TOKEN_LEN) tn = LOBBY_TOKEN_LEN - 1;
		memcpy(token_copy,token,tn);
		token_copy[tn] = 0;

		c = conn_by_token(token_copy);
		if(!c) return;

		c -> udp_ip = udp_ip;	// set by the caller before this runs
		c -> udp_port = udp_port;
		c -> has_udp = 1;
		c -> last_ms = now_ms();
	}

	// The source endpoint of the datagram being handled. Set by quant() right
	// before handle_probe() so the parse above can store it.
	unsigned int udp_ip;
	unsigned short udp_port;

	std::string ip_string(unsigned int ip)
	{
		char buf[32];
		netSocketIpToString(ip,buf,sizeof(buf));
		return std::string(buf);
	}
};

/* ------------------------------------------------------------------------- */

lobbyServer::lobbyServer(void)
{
	_impl = new Impl;
}

lobbyServer::~lobbyServer(void)
{
	stop();
	delete _impl;
}

int lobbyServer::start(unsigned short port)
{
	if(!netSocketStartup()){
		_impl -> note("socket startup failed: %s",netSocketError());
		return 0;
	}

	_impl -> tcp = netSocketTcpListen(port);
	if(_impl -> tcp == NET_SOCKET_INVALID){
		_impl -> note("TCP listen failed: %s",netSocketError());
		return 0;
	}

	unsigned short actual = port;
	netSocketLocalAddr(_impl -> tcp,NULL,&actual);

	_impl -> udp = netSocketOpen(actual);
	if(_impl -> udp == NET_SOCKET_INVALID){
		_impl -> note("UDP bind failed on port %u: %s",actual,netSocketError());
		netSocketClose(_impl -> tcp);
		_impl -> tcp = NET_SOCKET_INVALID;
		return 0;
	}

	_impl -> port = actual;

	return (int)actual;
}

void lobbyServer::stop(void)
{
	if(!_impl) return;

	if(_impl -> tcp != NET_SOCKET_INVALID){
		netSocketClose(_impl -> tcp);
		_impl -> tcp = NET_SOCKET_INVALID;
	}

	if(_impl -> udp != NET_SOCKET_INVALID){
		netSocketClose(_impl -> udp);
		_impl -> udp = NET_SOCKET_INVALID;
	}

	while(_impl -> conns)
		_impl -> remove_conn(_impl -> conns);

	while(_impl -> matches)
		_impl -> remove_match(_impl -> matches);

	_impl -> conn_count = 0;
}

void lobbyServer::quant(void)
{
	if(!_impl || _impl -> tcp == NET_SOCKET_INVALID) return;

	unsigned int now = now_ms();

	/* accept */
	for(;;){
		unsigned int ip = 0;
		unsigned short port_no = 0;
		netSocket s = netSocketTcpAccept(_impl -> tcp,&ip,&port_no);

		if(s == NET_SOCKET_INVALID) break;

		_impl -> add_conn(s,ip,port_no);
	}

	/* read tcp */
	{
		struct Impl::Conn* c = _impl -> conns;

		while(c){
			struct Impl::Conn* next = c -> next;
			_impl -> read_conn(c);
			c = next;
		}
	}

	/* read udp probes */
	for(;;){
		unsigned int ip = 0;
		unsigned short port_no = 0;
		char buf[LOBBY_MAX_LINE];
		int n = netSocketRecv(_impl -> udp,&ip,&port_no,buf,sizeof(buf));

		if(n == NET_SOCKET_WOULDBLOCK) break;
		if(n <= 0) break;

		_impl -> udp_ip = ip;
		_impl -> udp_port = port_no;
		_impl -> handle_probe(buf,n);
	}

	/* timeouts */
	{
		struct Impl::Conn* c = _impl -> conns;

		while(c){
			struct Impl::Conn* next = c -> next;

			if((int)(now - c -> last_ms) > LOBBY_TIMEOUT_MS)
				_impl -> remove_conn(c);

			c = next;
		}
	}
}

unsigned short lobbyServer::port(void) const { return _impl -> port; }

int lobbyServer::match_count(void) const
{
	int n = 0;
	struct Impl::Match* m = _impl -> matches;

	while(m){ n ++; m = m -> next; }

	return n;
}

int lobbyServer::match(int index,lobbyMatchInfo* out) const
{
	struct Impl::Match* m = _impl -> matches;

	while(m && index > 0){ m = m -> next; index --; }

	if(!m || !out) return 0;

	out -> id = m -> id;
	out -> players = m -> players;
	out -> max_players = m -> max_players;
	out -> world = m -> world;
	out -> laps = m -> laps;
	strncpy(out -> name,m -> name,sizeof(out -> name) - 1);
	out -> name[sizeof(out -> name) - 1] = 0;

	return 1;
}

int lobbyServer::connection_count(void) const { return _impl -> conn_count; }

void lobbyServer::set_log(lobbyLogFn fn,void* user)
{
	_impl -> log = fn;
	_impl -> log_user = user;
}
