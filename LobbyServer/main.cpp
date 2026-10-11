/*
	The lobby server console application.

	Two modes:

	  (default)   run the server and print the match list as it changes;
	  --selftest  spin up a server and two clients on loopback, walk the whole
	              register/list/join handshake and a UDP probe, and report a
	              verdict. No game needed, no network needed.

	It links nothing from the game but the socket layer and the shared
	protocol header, which is why it lives in its own directory and target.
*/

#include "lobby_server.h"
#include "net_socket.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <chrono>
#include <thread>

/* ------------------------------------------------------------------------- */

static void sleep_ms(int ms)
{
	std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

static unsigned int now_ms(void)
{
	using namespace std::chrono;

	return (unsigned int)duration_cast<milliseconds>(
		steady_clock::now().time_since_epoch()).count();
}

static void console_log(const char* text,void* user)
{
	(void)user;
	printf("  %s\n",text);
	fflush(stdout);
}

static void print_matches(lobbyServer& srv)
{
	int count = srv.match_count();
	int i;

	if(!count){
		printf("no matches yet\n");
		fflush(stdout);
		return;
	}

	printf("%d match(es):\n",count);

	for(i = 0; i < count; i ++){
		lobbyMatchInfo info;
		if(!srv.match(i,&info)) continue;

		printf("  [%u] \"%s\"  world %u  laps %u  players %u/%u\n",
		       (unsigned)info.id,info.name,
		       (unsigned)info.world,(unsigned)info.laps,
		       (unsigned)info.players,(unsigned)info.max_players);
	}

	fflush(stdout);
}

/* ------------------------------- selftest -------------------------------- */

// A client connection plus the bytes read but not yet consumed as a line. The
// buffer has to outlive a single read: LIST, MATCH and END commonly arrive in
// one TCP segment, and a reader that drops the tail after the first newline
// loses the match it was looking for.
struct TestConn
{
	netSocket sock;
	char buf[4096];
	int len;

	TestConn(void){ sock = NET_SOCKET_INVALID; len = 0; buf[0] = 0; }
};

static netSocket tcp_connect_wait(unsigned int ip,unsigned short port)
{
	netSocket s = netSocketTcpConnect(ip,port);

	if(s == NET_SOCKET_INVALID) return s;

	for(int i = 0; i < 400; i ++){
		int st = netSocketTcpStatus(s);
		if(st == 1) return s;
		if(st < 0){ netSocketClose(s); return NET_SOCKET_INVALID; }
		sleep_ms(5);
	}

	netSocketClose(s);
	return NET_SOCKET_INVALID;
}

static void tcp_send(netSocket s,const char* line)
{
	char buf[LOBBY_MAX_LINE];
	int n = snprintf(buf,sizeof(buf) - 2,"%s",line);
	buf[n] = '\n';
	buf[n + 1] = 0;
	netSocketTcpSend(s,buf,n + 1);
}

// Reads one newline terminated line, pumping the server meanwhile and keeping
// whatever followed in the connection's buffer. Returns 0 on timeout or close.
static int wait_line(lobbyServer& srv,TestConn& c,char* out,int sz,int timeout_ms)
{
	unsigned int start = now_ms();

	while((int)(now_ms() - start) < timeout_ms){
		srv.quant();

		char* nl = strchr(c.buf,'\n');

		if(nl){
			*nl = 0;
			strncpy(out,c.buf,sz - 1);
			out[sz - 1] = 0;

			int rem = c.len - (int)(nl + 1 - c.buf);
			memmove(c.buf,nl + 1,rem);
			c.len = rem;
			c.buf[c.len] = 0;

			return 1;
		}

		int n = netSocketTcpRecv(c.sock,c.buf + c.len,(int)sizeof(c.buf) - 1 - c.len);

		if(n > 0){
			c.len += n;
			c.buf[c.len] = 0;
			continue;
		}

		if(n == 0) return 0;

		sleep_ms(5);
	}

	return 0;
}

// Sends a hello, sends the token as a UDP probe from a fresh socket, and
// returns the token. The probe socket stays open for the test's lifetime.
static int handshake(lobbyServer& srv,TestConn& c,unsigned int server_ip,
		     netSocket* udp_out,char* token,int token_sz)
{
	char line[256];
	int version = 0;
	char tok[LOBBY_TOKEN_LEN] = "";

	tcp_send(c.sock,"HELLO 1");

	if(!wait_line(srv,c,line,sizeof(line),2000)) return 0;
	if(sscanf(line,"HELLO %d %11s",&version,tok) != 2) return 0;

	strncpy(token,tok,token_sz - 1);
	token[token_sz - 1] = 0;

	*udp_out = netSocketOpen(0);
	if(*udp_out == NET_SOCKET_INVALID) return 0;

	char probe[LOBBY_MAX_LINE];
	int n = snprintf(probe,sizeof(probe),"%s%s\n",LOBBY_PROBE_PREFIX,token);

	netSocketSend(*udp_out,server_ip,(unsigned short)srv.port(),probe,n);

	// One pump so the server reads the probe before the next request.
	srv.quant();

	(void)version;

	return 1;
}

static int selftest(void)
{
	lobbyServer srv;
	unsigned int ip = 0;
	TestConn host,client;
	netSocket host_udp = NET_SOCKET_INVALID;
	netSocket client_udp = NET_SOCKET_INVALID;
	char line[512];
	char host_token[LOBBY_TOKEN_LEN],client_token[LOBBY_TOKEN_LEN];
	unsigned short host_udp_port = 0,client_udp_port = 0;
	int count = -1;
	char peer_line[256];
	char host_tcp_peer[256];
	int ok = 1;

	printf("lobby selftest\n");

	if(!srv.start(0)){
		printf("FAIL: server did not start\n");
		return 1;
	}

	unsigned short port = (unsigned short)srv.port();
	printf("  server on tcp+udp port %u\n",(unsigned)port);

	if(!netSocketResolve("127.0.0.1",&ip)){
		printf("FAIL: cannot resolve loopback\n");
		srv.stop();
		return 1;
	}

	/* ---- host ---- */
	host.sock = tcp_connect_wait(ip,port);
	if(host.sock == NET_SOCKET_INVALID){ printf("FAIL: host did not connect\n"); ok = 0; goto done; }

	if(!handshake(srv,host,ip,&host_udp,host_token,sizeof(host_token))){
		printf("FAIL: host handshake\n"); ok = 0; goto done;
	}
	netSocketLocalAddr(host_udp,NULL,&host_udp_port);

	tcp_send(host.sock,"REGISTER 1 5 1 5 Test Match");
	if(!wait_line(srv,host,line,sizeof(line),2000)){
		printf("FAIL: no REGISTERED\n"); ok = 0; goto done;
	}
	if(strncmp(line,"REGISTERED ",11)){
		printf("FAIL: expected REGISTERED, got '%s'\n",line); ok = 0; goto done;
	}
	printf("  host registered: %s\n",line);

	/* ---- client ---- */
	client.sock = tcp_connect_wait(ip,port);
	if(client.sock == NET_SOCKET_INVALID){ printf("FAIL: client did not connect\n"); ok = 0; goto done; }

	if(!handshake(srv,client,ip,&client_udp,client_token,sizeof(client_token))){
		printf("FAIL: client handshake\n"); ok = 0; goto done;
	}
	netSocketLocalAddr(client_udp,NULL,&client_udp_port);

	tcp_send(client.sock,"LIST");
	if(!wait_line(srv,client,line,sizeof(line),2000)){ printf("FAIL: no LIST\n"); ok = 0; goto done; }

	if(sscanf(line,"LIST %d",&count) != 1){ printf("FAIL: bad LIST '%s'\n",line); ok = 0; goto done; }

	if(!wait_line(srv,client,line,sizeof(line),2000)){ printf("FAIL: no MATCH\n"); ok = 0; goto done; }

	if(count != 1 || strncmp(line,"MATCH ",6)){
		printf("FAIL: list was count %d, line '%s'\n",count,line); ok = 0; goto done;
	}

	printf("  client sees: %s\n",line);

	if(!wait_line(srv,client,line,sizeof(line),2000) || strcmp(line,"END")){
		printf("FAIL: list did not end\n"); ok = 0; goto done;
	}

	/* ---- join: both sides must be told the other's public UDP endpoint ---- */
	tcp_send(client.sock,"JOIN 1");

	if(!wait_line(srv,client,peer_line,sizeof(peer_line),2000)){
		printf("FAIL: client got no PEER\n"); ok = 0; goto done;
	}

	if(!wait_line(srv,host,host_tcp_peer,sizeof(host_tcp_peer),2000)){
		printf("FAIL: host got no PEER\n"); ok = 0; goto done;
	}

	{
		char pip[64];
		unsigned pport = 0;

		if(sscanf(peer_line,"PEER %63s %u",pip,&pport) != 2){
			printf("FAIL: bad client PEER '%s'\n",peer_line); ok = 0; goto done;
		}
		if(strcmp(pip,"127.0.0.1") || pport != host_udp_port){
			printf("FAIL: client told host %s:%u, host udp is 127.0.0.1:%u\n",
			       pip,pport,(unsigned)host_udp_port);
			ok = 0; goto done;
		}

		if(sscanf(host_tcp_peer,"PEER %63s %u",pip,&pport) != 2){
			printf("FAIL: bad host PEER '%s'\n",host_tcp_peer); ok = 0; goto done;
		}
		if(strcmp(pip,"127.0.0.1") || pport != client_udp_port){
			printf("FAIL: host told client %s:%u, client udp is 127.0.0.1:%u\n",
			       pip,pport,(unsigned)client_udp_port);
			ok = 0; goto done;
		}
	}

	printf("  rendezvous: client -> %s ; host -> %s\n",peer_line,host_tcp_peer);

done:
	if(host.sock != NET_SOCKET_INVALID) netSocketClose(host.sock);
	if(client.sock != NET_SOCKET_INVALID) netSocketClose(client.sock);
	if(host_udp != NET_SOCKET_INVALID) netSocketClose(host_udp);
	if(client_udp != NET_SOCKET_INVALID) netSocketClose(client_udp);

	srv.stop();

	printf("RESULT: %s\n",ok ? "PASS" : "FAIL");

	return ok ? 0 : 1;
}

/* --------------------------------- main ---------------------------------- */

static void usage(void)
{
	printf("moonshine-lobby [--port N] [--selftest] [--help]\n");
	printf("  --port N     listen port for TCP and UDP (default %d)\n",LOBBY_DEFAULT_PORT);
	printf("  --selftest   run the protocol self test and exit\n");
}

int main(int argc,char const* argv[])
{
	unsigned short port = (unsigned short)LOBBY_DEFAULT_PORT;
	int test = 0;
	int i;

	for(i = 1; i < argc; i ++){
		if(!strcmp(argv[i],"--selftest")) test = 1;
		else if(!strcmp(argv[i],"--port") && i + 1 < argc) port = (unsigned short)atoi(argv[++ i]);
		else if(!strcmp(argv[i],"--help")){ usage(); return 0; }
		else { printf("unknown argument '%s'\n",argv[i]); usage(); return 1; }
	}

	if(test) return selftest();

	lobbyServer srv;

	srv.set_log(console_log, NULL);

	if(!srv.start(port)){
		fprintf(stderr,"lobby: failed to start on port %u\n",(unsigned)port);
		return 1;
	}

	printf("moonshine lobby server\n");
	printf("listening on tcp+udp %u\n",(unsigned)srv.port());
	printf("matches are shown below; Ctrl+C to stop\n\n");
	fflush(stdout);

	int last_count = -1;
	unsigned int last_print = 0;

	for(;;){
		srv.quant();

		unsigned int now = now_ms();

		if(srv.match_count() != last_count || now - last_print > 2000){
			last_count = srv.match_count();
			last_print = now;
			print_matches(srv);
		}

		sleep_ms(10);
	}

	return 0;
}
