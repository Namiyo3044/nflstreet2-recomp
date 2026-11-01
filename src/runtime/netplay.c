#include "runtime/netplay.h"
#include "runtime/interrupts.h"
#include "runtime/netsync.h"
#include "cpu/cpu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define BAD_SOCK INVALID_SOCKET
#define sock_close closesocket
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
typedef int sock_t;
#define BAD_SOCK (-1)
#define sock_close close
#endif

#define MAGIC "NS2NET01"
#define PROTOCOL 2u               /* 2: ping packets */
#define DEFAULT_PORT 7120
#define RING 256u                 /* fields of input kept (well past any delay) */
#define HASH_EVERY 60u            /* fields between memory checks (one second) */

static sock_t g_sock = BAD_SOCK;
static bool g_active, g_host, g_lost, g_desync;
static int g_delay = 3;
static PadLive g_local[RING], g_remote[RING];
static bool g_have_remote[RING];
static unsigned long long g_remote_frame[RING];
static PadLive g_now[2];          /* this field's controllers: port 1 (host), port 2 (joiner) */
static unsigned long long g_waits, g_wait_ms_total, g_checks;
static int g_ping_ms = -1;                /* the last round trip (-1: none yet) */
static unsigned long long g_wait_since;   /* waiting for the other player since (0: not waiting) */
static unsigned long long g_last_wait_end;
static uint64_t g_my_hash[64], g_their_hash[64];
static unsigned long long g_my_hash_field[64], g_their_hash_field[64];
static uint8_t* g_card;
static uint32_t g_card_size;

static const PadLive k_idle = {0, 0x80, 0x80, 0x80, 0x80, 0, 0};

static unsigned long long now_ms(void) {
#ifdef _WIN32
    return (unsigned long long)GetTickCount64();
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long long)t.tv_sec * 1000ull + (unsigned long long)t.tv_nsec / 1000000ull;
#endif
}

/* ------------------------------------------------------------ sockets --- */
static bool net_init(void) {
#ifdef _WIN32
    static bool done;
    if (!done) {
        WSADATA w;
        if (WSAStartup(MAKEWORD(2, 2), &w) != 0) { fprintf(stderr, "netplay: no network (WSAStartup)\n"); return false; }
        done = true;
    }
#endif
    return true;
}

static void set_nodelay(sock_t s) {
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof one);
}

static bool send_all(const void* data, size_t n) {
    const char* p = (const char*)data;
    while (n) {
        int k = send(g_sock, p, (int)n, 0);
        if (k <= 0) return false;
        p += k; n -= (size_t)k;
    }
    return true;
}

static bool recv_all(void* data, size_t n) {
    char* p = (char*)data;
    while (n) {
        int k = recv(g_sock, p, (int)n, 0);
        if (k <= 0) return false;
        p += k; n -= (size_t)k;
    }
    return true;
}

/* Whether data is waiting (within ms milliseconds). */
static bool readable(int ms) {
    fd_set r;
    FD_ZERO(&r);
    FD_SET(g_sock, &r);
    struct timeval tv = {ms / 1000, (ms % 1000) * 1000};
    return select((int)g_sock + 1, &r, NULL, NULL, &tv) > 0;
}

static void put32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint32_t get32(const uint8_t* p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* ---------------------------------------------------------- handshake --- */
static bool send_hello(const char* fingerprint) {
    uint8_t h[20];
    uint32_t n = (uint32_t)strlen(fingerprint);
    memcpy(h, MAGIC, 8);
    put32(h + 8, PROTOCOL);
    put32(h + 12, (uint32_t)g_delay);
    put32(h + 16, n);
    return send_all(h, sizeof h) && send_all(fingerprint, n);
}

static bool recv_hello(char* fingerprint, size_t cap, uint32_t* delay) {
    uint8_t h[20];
    if (!recv_all(h, sizeof h) || memcmp(h, MAGIC, 8) != 0) {
        fprintf(stderr, "netplay: the other side isn't this port's netplay\n");
        return false;
    }
    if (get32(h + 8) != PROTOCOL) {
        fprintf(stderr, "netplay: the other side has a different version of the port\n");
        return false;
    }
    *delay = get32(h + 12);
    uint32_t n = get32(h + 16);
    if (n >= cap) return false;
    if (!recv_all(fingerprint, n)) return false;
    fingerprint[n] = 0;
    return true;
}

/* Lists the settings that differ between two fingerprints ("key=value ...").
   (With differing settings the two games would part.) */
static const char* value_of(const char* fp, const char* key, char* out, size_t cap) {
    size_t kl = strlen(key);
    for (const char* p = fp; *p;) {
        while (*p == ' ') p++;
        const char* e = p;
        while (*e && *e != ' ') e++;
        if ((size_t)(e - p) > kl && strncmp(p, key, kl) == 0 && p[kl] == '=') {
            snprintf(out, cap, "%.*s", (int)(e - p - kl - 1), p + kl + 1);
            return out;
        }
        p = e;
    }
    return "(not set)";
}

static void explain_mismatch(const char* mine, const char* theirs) {
    fprintf(stderr, "netplay: your settings and the other player's differ; these have to match:\n");
    printf("netplay: settings differ from the other player's (see below)\n");
    for (const char* p = mine; *p;) {
        while (*p == ' ') p++;
        const char* e = p;
        while (*e && *e != ' ') e++;
        const char* eq = memchr(p, '=', (size_t)(e - p));
        if (eq) {
            char key[64], a[128], b[128];
            snprintf(key, sizeof key, "%.*s", (int)(eq - p), p);
            value_of(mine, key, a, sizeof a);
            const char* bv = value_of(theirs, key, b, sizeof b);
            if (strcmp(a, bv) != 0) {
                fprintf(stderr, "  %s: yours %s, theirs %s\n", key, a, bv);
                printf("netplay:   %s: yours %s, theirs %s\n", key, a, bv);
            }
        }
        p = e;
    }
}

static bool setup(bool host, const char* fingerprint) {
    char theirs[1024];
    uint32_t their_delay = 0;
    if (!send_hello(fingerprint) || !recv_hello(theirs, sizeof theirs, &their_delay)) {
        fprintf(stderr, "netplay: the connection failed while setting up\n");
        return false;
    }
    if (strcmp(theirs, fingerprint) != 0) {
        explain_mismatch(fingerprint, theirs);
        return false;
    }
    if (host) {
        /* the session: delay, clock, memory card */
        uint32_t rtc = netsync_rtc((uint32_t)(time(NULL) - 946684800));
        netsync_set_rtc(rtc);
        uint8_t s[12];
        put32(s, (uint32_t)g_delay);
        put32(s + 4, rtc);
        put32(s + 8, g_card ? g_card_size : 0);
        if (!send_all(s, sizeof s) || (g_card && !send_all(g_card, g_card_size))) return false;
        printf("netplay: session started (input delay %d fields, clock %u, memory card %s)\n", g_delay, rtc,
               g_card ? "sent" : "none");
    } else {
        uint8_t s[12];
        if (!recv_all(s, sizeof s)) return false;
        g_delay = (int)get32(s);
        uint32_t rtc = get32(s + 4), card = get32(s + 8);
        netsync_set_rtc(rtc);
        if (card) {
            g_card = (uint8_t*)malloc(card);
            if (!g_card || !recv_all(g_card, card)) return false;
            g_card_size = card;
        }
        printf("netplay: session started (input delay %d fields, clock %u, the host's memory card %s)\n", g_delay,
               rtc, card ? "in slot A" : "none");
    }
    return true;
}

bool netplay_host(int port, int delay, const char* fingerprint) {
    if (!net_init()) return false;
    if (port <= 0) port = DEFAULT_PORT;
    g_delay = delay > 0 ? delay : 3;
    sock_t ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls == BAD_SOCK) return false;
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((unsigned short)port);
    if (bind(ls, (struct sockaddr*)&a, sizeof a) != 0 || listen(ls, 1) != 0) {
        fprintf(stderr, "netplay: couldn't listen on port %d (in use?)\n", port);
        sock_close(ls);
        return false;
    }
    printf("netplay: hosting on port %d, waiting for the other player...\n", port);
    fflush(stdout);
    g_sock = accept(ls, NULL, NULL);
    sock_close(ls);
    if (g_sock == BAD_SOCK) return false;
    set_nodelay(g_sock);
    g_host = true;
    if (!setup(true, fingerprint)) { sock_close(g_sock); g_sock = BAD_SOCK; return false; }
    g_active = true;
    return true;
}

bool netplay_join(const char* address, int delay, const char* fingerprint) {
    if (!net_init()) return false;
    char host[256];
    snprintf(host, sizeof host, "%s", address);
    int port = DEFAULT_PORT;
    char* colon = strrchr(host, ':');
    if (colon) { *colon = 0; port = atoi(colon + 1); }
    g_delay = delay > 0 ? delay : 3;
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char ps[16];
    snprintf(ps, sizeof ps, "%d", port);
    if (getaddrinfo(host, ps, &hints, &res) != 0 || !res) {
        fprintf(stderr, "netplay: couldn't find %s\n", host);
        return false;
    }
    printf("netplay: connecting to %s:%d...\n", host, port);
    fflush(stdout);
    /* (after the Online menu the host's game is restarting too: keep trying
       for half a minute) */
    bool connected = false;
    for (int tries = 0; tries < 60 && !connected; tries++) {
        g_sock = socket(AF_INET, SOCK_STREAM, 0);
        if (g_sock != BAD_SOCK && connect(g_sock, res->ai_addr, (int)res->ai_addrlen) == 0) { connected = true; break; }
        if (g_sock != BAD_SOCK) sock_close(g_sock);
        g_sock = BAD_SOCK;
#ifdef _WIN32
        Sleep(500);
#else
        usleep(500000);
#endif
    }
    freeaddrinfo(res);
    if (!connected) {
        fprintf(stderr, "netplay: couldn't connect to %s:%d\n", host, port);
        return false;
    }
    set_nodelay(g_sock);
    if (!setup(false, fingerprint)) { sock_close(g_sock); g_sock = BAD_SOCK; return false; }
    g_active = true;
    return true;
}

bool netplay_active(void) { return g_active; }

static void (*g_idle)(void);
void netplay_set_idle(void (*fn)(void)) { g_idle = fn; }

void netplay_set_card(const uint8_t* data, uint32_t size) {
    free(g_card);
    g_card = NULL;
    g_card_size = 0;
    if (data && size) {
        g_card = (uint8_t*)malloc(size);
        if (g_card) { memcpy(g_card, data, size); g_card_size = size; }
    }
}

const uint8_t* netplay_card(uint32_t* size) {
    if (size) *size = g_card_size;
    return g_card;
}

/* ---------------------------------------------------------------- lobby --- */
/* The Online menu: hosting and joining while the game runs on. Once the two
   are connected and their settings match, they agree on the input delay and
   hang up; then both games restart into the session (netplay_host /
   netplay_join at start-up, the joiner retrying until the host is back). */
static sock_t g_lsock = BAD_SOCK;      /* hosting: listening */
static sock_t g_lconn = BAD_SOCK;      /* the connection being set up */
static int g_lstate = NETPLAY_LOBBY_IDLE, g_lhost, g_ldelay = 3;
static char g_lmsg[512];
static const char* (*g_fingerprint_fn)(void);

void netplay_set_fingerprint(const char* (*fn)(void)) { g_fingerprint_fn = fn; }

static void set_blocking(sock_t s, bool on) {
#ifdef _WIN32
    u_long nb = on ? 0 : 1;
    ioctlsocket(s, FIONBIO, &nb);
#else
    int fl = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, on ? (fl & ~O_NONBLOCK) : (fl | O_NONBLOCK));
#endif
}

static void set_timeout(sock_t s, int ms) {
#ifdef _WIN32
    DWORD t = (DWORD)ms;
#else
    struct timeval t = {ms / 1000, (ms % 1000) * 1000};
#endif
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&t, sizeof t);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&t, sizeof t);
}

static const char* friendly(const char* key) {
    if (!strcmp(key, "shape")) return "ASPECT RATIO";
    if (!strcmp(key, "rain")) return "THE ALLEY RAIN";
    if (!strcmp(key, "intros")) return "SKIP INTRO VIDEOS";
    if (!strcmp(key, "thx")) return "THX INTRO";
    if (!strcmp(key, "text")) return "TEXT-REPLACEMENTS.TXT";
    if (!strcmp(key, "colors")) return "UI-COLORS.TXT";
    if (!strcmp(key, "idle")) return "IDLE SKIPPING";
    return key;
}

static const char* shape_name(const char* v) {
    return !strcmp(v, "1") ? "4:3" : !strcmp(v, "2") ? "16:9" : !strcmp(v, "3") ? "21:9" : !strcmp(v, "4") ? "32:9" : "AUTO";
}

/* What differs, for the menu: "ASPECT RATIO: YOURS 16:9, THEIRS 4:3. ..." */
static void mismatch_text(const char* mine, const char* theirs, char* out, size_t cap) {
    size_t n = (size_t)snprintf(out, cap, "YOUR SETTINGS AND THE OTHER PLAYER'S HAVE TO MATCH. ");
    for (const char* p = mine; *p && n < cap;) {
        while (*p == ' ') p++;
        const char* e = p;
        while (*e && *e != ' ') e++;
        const char* eq = memchr(p, '=', (size_t)(e - p));
        if (eq) {
            char key[64], a[128], b[128];
            snprintf(key, sizeof key, "%.*s", (int)(eq - p), p);
            value_of(mine, key, a, sizeof a);
            const char* bv = value_of(theirs, key, b, sizeof b);
            if (strcmp(a, bv) != 0) {
                bool shp = !strcmp(key, "shape"), file = !strcmp(key, "text") || !strcmp(key, "colors");
                const char* ya = shp ? shape_name(a) : file ? "" : (!strcmp(a, "1") ? "ON" : "OFF");
                const char* yb = shp ? shape_name(bv) : file ? "" : (!strcmp(bv, "1") ? "ON" : "OFF");
                if (!strcmp(key, "rain")) {
                    ya = !strcmp(a, "1") ? "PS2/XBOX" : !strcmp(a, "2") ? "OFF" : "GAMECUBE";
                    yb = !strcmp(bv, "1") ? "PS2/XBOX" : !strcmp(bv, "2") ? "OFF" : "GAMECUBE";
                }
                if (file)
                    n += (size_t)snprintf(out + n, cap - n, "%s IS DIFFERENT. ", friendly(key));
                else
                    n += (size_t)snprintf(out + n, cap - n, "%s: YOURS %s, THEIRS %s. ", friendly(key), ya, yb);
            }
        }
        p = e;
    }
}

static void lobby_close(void) {
    if (g_lconn != BAD_SOCK) { sock_close(g_lconn); g_lconn = BAD_SOCK; }
    if (g_lsock != BAD_SOCK) { sock_close(g_lsock); g_lsock = BAD_SOCK; }
}

static void lobby_fail(const char* msg) {
    lobby_close();
    snprintf(g_lmsg, sizeof g_lmsg, "%s", msg);
    g_lstate = NETPLAY_LOBBY_FAILED;
    printf("netplay: %s\n", msg);
}

bool netplay_lobby_host(int port, int delay) {
    lobby_close();
    if (!net_init()) { lobby_fail("The network isn't available on this PC."); return false; }
    if (port <= 0) port = DEFAULT_PORT;
    g_ldelay = delay > 0 ? delay : 3;
    g_lsock = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(g_lsock, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((unsigned short)port);
    if (g_lsock == BAD_SOCK || bind(g_lsock, (struct sockaddr*)&a, sizeof a) != 0 || listen(g_lsock, 1) != 0) {
        char m[96];
        snprintf(m, sizeof m, "COULDN'T WAIT FOR PLAYERS ON PORT %d (IS ANOTHER PROGRAM USING IT?)", port);
        lobby_fail(m);
        return false;
    }
    set_blocking(g_lsock, false);
    g_lhost = 1;
    g_lstate = NETPLAY_LOBBY_WAITING;
    g_lmsg[0] = 0;
    printf("netplay: Online menu: hosting on port %d\n", port);
    return true;
}

bool netplay_lobby_join(const char* address, int delay) {
    lobby_close();
    if (!net_init()) { lobby_fail("The network isn't available on this PC."); return false; }
    char host[256];
    snprintf(host, sizeof host, "%s", address);
    int port = DEFAULT_PORT;
    char* colon = strrchr(host, ':');
    if (colon) { *colon = 0; port = atoi(colon + 1); }
    g_ldelay = delay > 0 ? delay : 3;
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char ps[16];
    snprintf(ps, sizeof ps, "%d", port);
    if (!host[0] || getaddrinfo(host, ps, &hints, &res) != 0 || !res) {
        lobby_fail("Connection Incomplete. That address wasn't found.");
        return false;
    }
    g_lconn = socket(AF_INET, SOCK_STREAM, 0);
    set_blocking(g_lconn, false);
    int r = connect(g_lconn, res->ai_addr, (int)res->ai_addrlen);
    freeaddrinfo(res);
#ifdef _WIN32
    bool pending = r != 0 && WSAGetLastError() == WSAEWOULDBLOCK;
#else
    bool pending = r != 0 && errno == EINPROGRESS;
#endif
    if (r != 0 && !pending) { lobby_fail("Connection Incomplete. Couldn't reach that address."); return false; }
    g_lhost = 0;
    g_lstate = NETPLAY_LOBBY_CONNECTING;
    g_lmsg[0] = 0;
    printf("netplay: Online menu: connecting to %s:%d\n", host, port);
    return true;
}

void netplay_lobby_cancel(void) {
    lobby_close();
    g_lstate = NETPLAY_LOBBY_IDLE;
}

/* Both connected: settings compared, the delay agreed (the host's). */
static void lobby_handshake(void) {
    set_blocking(g_lconn, true);
    set_timeout(g_lconn, 4000);
    set_nodelay(g_lconn);
    sock_t keep = g_sock;
    g_sock = g_lconn;                  /* (the send / receive helpers use g_sock) */
    const char* fp = g_fingerprint_fn ? g_fingerprint_fn() : "";
    char theirs[1024];
    uint32_t their_delay = 0;
    int saved_delay = g_delay;
    g_delay = g_ldelay;
    bool ok = send_hello(fp) && recv_hello(theirs, sizeof theirs, &their_delay);
    g_delay = saved_delay;
    if (!ok) { g_sock = keep; lobby_fail("Connection Incomplete. The other game didn't answer (a different version of the port?)."); return; }
    if (strcmp(theirs, fp) != 0) {
        g_sock = keep;
        char m[512];
        mismatch_text(fp, theirs, m, sizeof m);
        lobby_fail(m);
        return;
    }
    uint8_t go[5];
    if (g_lhost) {
        go[0] = 'G';
        put32(go + 1, (uint32_t)g_ldelay);
        ok = send_all(go, 5);
    } else {
        ok = recv_all(go, 5) && go[0] == 'G';
        if (ok) g_ldelay = (int)get32(go + 1);
    }
    g_sock = keep;
    if (!ok) { lobby_fail("Connection Lost while setting up. Please try again."); return; }
    lobby_close();
    g_lstate = NETPLAY_LOBBY_READY;
    printf("netplay: Online menu: the other player is ready (input delay %d); restarting into the session\n", g_ldelay);
}

int netplay_lobby_poll(const char** message, int* delay) {
    if (g_lstate == NETPLAY_LOBBY_WAITING && g_lsock != BAD_SOCK) {
        fd_set r;
        FD_ZERO(&r);
        FD_SET(g_lsock, &r);
        struct timeval tv = {0, 0};
        if (select((int)g_lsock + 1, &r, NULL, NULL, &tv) > 0) {
            g_lconn = accept(g_lsock, NULL, NULL);
            sock_close(g_lsock);
            g_lsock = BAD_SOCK;
            if (g_lconn == BAD_SOCK) lobby_fail("Connection Incomplete. A player tried to join, but the connection failed.");
            else lobby_handshake();
        }
    } else if (g_lstate == NETPLAY_LOBBY_CONNECTING && g_lconn != BAD_SOCK) {
        fd_set w, e;
        FD_ZERO(&w);
        FD_ZERO(&e);
        FD_SET(g_lconn, &w);
        FD_SET(g_lconn, &e);
        struct timeval tv = {0, 0};
        if (select((int)g_lconn + 1, NULL, &w, &e, &tv) > 0) {
            int err = 0;
            socklen_t len = sizeof err;
            getsockopt(g_lconn, SOL_SOCKET, SO_ERROR, (char*)&err, &len);
            if (err || FD_ISSET(g_lconn, &e)) lobby_fail("Connection Incomplete. No game is being hosted at that address (or a firewall is in the way).");
            else lobby_handshake();
        }
    }
    if (message) *message = g_lmsg;
    if (delay) *delay = g_ldelay;
    return g_lstate;
}

/* The PC's addresses on its networks, for the host to tell the other player. */
int netplay_local_addresses(char* out, size_t cap) {
    out[0] = 0;
    if (!net_init()) return 0;
    char name[256];
    if (gethostname(name, sizeof name) != 0) return 0;
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(name, NULL, &hints, &res) != 0) return 0;
    int n = 0;
    size_t used = 0;
    for (struct addrinfo* a = res; a && n < 4; a = a->ai_next) {
        char ip[64];
        struct sockaddr_in* sa = (struct sockaddr_in*)a->ai_addr;
        snprintf(ip, sizeof ip, "%u.%u.%u.%u", (unsigned)(ntohl(sa->sin_addr.s_addr) >> 24) & 255u,
                 (unsigned)(ntohl(sa->sin_addr.s_addr) >> 16) & 255u, (unsigned)(ntohl(sa->sin_addr.s_addr) >> 8) & 255u,
                 (unsigned)ntohl(sa->sin_addr.s_addr) & 255u);
        if (!strncmp(ip, "127.", 4) || strstr(out, ip)) continue;
        used += (size_t)snprintf(out + used, cap > used ? cap - used : 0, "%s%s", n ? "   " : "", ip);
        n++;
    }
    freeaddrinfo(res);
    return n;
}

/* -------------------------------------------------------------- packets --- */
/* 'I' field(4) pad(8): a controller for a field.  'K' field(4) hash(8): a memory check. */
static void pack_pad(uint8_t* p, const PadLive* s) {
    p[0] = (uint8_t)s->buttons; p[1] = (uint8_t)(s->buttons >> 8);
    p[2] = s->stick_x; p[3] = s->stick_y; p[4] = s->cstick_x; p[5] = s->cstick_y;
    p[6] = s->trigger_l; p[7] = s->trigger_r;
}
static void unpack_pad(const uint8_t* p, PadLive* s) {
    s->buttons = (uint16_t)(p[0] | p[1] << 8);
    s->stick_x = p[2]; s->stick_y = p[3]; s->cstick_x = p[4]; s->cstick_y = p[5];
    s->trigger_l = p[6]; s->trigger_r = p[7];
}

static void lost(const char* why) {
    if (!g_lost) {
        g_lost = true;
        printf("netplay: connection lost (%s) at %.1f s; the other player's controller is let go from here\n", why,
               interrupts_game_seconds());
    }
}

static void check_hashes(void) {
    for (int i = 0; i < 64; i++)
        for (int j = 0; j < 64; j++)
            if (g_my_hash_field[i] && g_my_hash_field[i] == g_their_hash_field[j]) {
                g_checks++;
                if (g_my_hash[i] != g_their_hash[j] && !g_desync) {
                    g_desync = true;
                    printf("netplay: OUT OF SYNC at %.1f s (video field %llu): the two games have parted\n",
                           (double)g_my_hash_field[i] * 1001.0 / 60000.0, g_my_hash_field[i]);
                }
                g_their_hash_field[j] = 0;
            }
}

/* Reads one packet if there is one (waiting up to ms). */
static bool pump(int ms) {
    if (g_lost || !readable(ms)) return false;
    uint8_t t;
    if (!recv_all(&t, 1)) { lost("the other side closed it"); return false; }
    uint8_t b[12];
    if (t == 'I') {
        if (!recv_all(b, 12)) { lost("cut off"); return false; }
        uint32_t f = get32(b);
        unpack_pad(b + 4, &g_remote[f % RING]);
        g_remote_frame[f % RING] = f;
        g_have_remote[f % RING] = true;
    } else if (t == 'K') {
        if (!recv_all(b, 12)) { lost("cut off"); return false; }
        static int k;
        g_their_hash_field[k] = get32(b);
        memcpy(&g_their_hash[k], b + 4, 8);
        k = (k + 1) % 64;
        check_hashes();
    } else if (t == 'T' || t == 'U') {
        /* 'T' time(4) pad(8): a ping, answered with 'U' and the same time */
        if (!recv_all(b, 12)) { lost("cut off"); return false; }
        if (t == 'T') {
            uint8_t p[13];
            p[0] = 'U'; memcpy(p + 1, b, 12);
            if (!send_all(p, sizeof p)) lost("couldn't send");
        } else {
            unsigned long long sent = get32(b), now = now_ms() & 0xFFFFFFFFull;
            unsigned long long rt = (now - sent) & 0xFFFFFFFFull;
            if (rt < 60000) g_ping_ms = g_ping_ms < 0 ? (int)rt : (g_ping_ms * 3 + (int)rt) / 4;
        }
    } else if (t == 'Q') {
        lost("the other player left the session");
        return false;
    } else {
        lost("unexpected data");
        return false;
    }
    return true;
}

bool netplay_lost(void) { return g_active && g_lost; }

void netplay_close(void) {
    if (g_active && !g_lost && g_sock != BAD_SOCK) {
        uint8_t q = 'Q';               /* "leaving": the other game restarts on its own */
        send_all(&q, 1);
    }
    if (g_sock != BAD_SOCK) { sock_close(g_sock); g_sock = BAD_SOCK; }
    lobby_close();
}

/* Game memory, without the TV frame buffer (the picture differs between
   renderers and doesn't affect the game). */
static uint64_t memory_hash(CPUState* cpu) {
    uint64_t h = 0xCBF29CE484222325ull;
    const uint32_t skip_lo = 0x004C0000u, skip_hi = 0x00550000u;
    for (uint32_t i = 0; i + 8 <= cpu->ram_size; i += 8) {
        if (i >= skip_lo && i < skip_hi) { i = skip_hi - 8; continue; }
        uint64_t w;
        memcpy(&w, cpu->ram + i, 8);
        h = (h ^ w) * 0x100000001B3ull;
    }
    return h;
}

void netplay_field(CPUState* cpu, unsigned long long field) {
    if (!g_active) return;
    /* this side's controller (player 1 on this PC) for `delay` fields ahead */
    unsigned long long ahead = field + (unsigned long long)g_delay;
    PadLive mine;
    pad_get_live(0, &mine);
    g_local[ahead % RING] = mine;
    if (!g_lost) {
        uint8_t p[13];
        p[0] = 'I';
        put32(p + 1, (uint32_t)ahead);
        pack_pad(p + 5, &mine);
        if (!send_all(p, sizeof p)) lost("couldn't send");
    }
    if (!g_lost && field % 30 == 0) {
        uint8_t p[13] = {'T'};
        put32(p + 1, (uint32_t)(now_ms() & 0xFFFFFFFFull));
        if (!send_all(p, sizeof p)) lost("couldn't send");
    }
    /* the other side's controller for this field */
    PadLive theirs = k_idle;
    if (field > (unsigned long long)g_delay) {
        unsigned slot = (unsigned)(field % RING);
        if (!(g_have_remote[slot] && g_remote_frame[slot] == field)) {
            g_waits++;
            unsigned long long t0 = now_ms(), told = 0;
            g_wait_since = t0;
            while (!g_lost && !(g_have_remote[slot] && g_remote_frame[slot] == field)) {
                pump(50);
                if (g_idle) g_idle();
                unsigned long long w = now_ms() - t0;
                if (w / 5000 > told) {
                    told = w / 5000;
                    printf("netplay: waiting for the other player (%llu s)...\n", told * 5);
                    fflush(stdout);
                }
            }
            g_wait_ms_total += now_ms() - t0;
            g_wait_since = 0;
            g_last_wait_end = now_ms();
        }
        if (g_have_remote[slot] && g_remote_frame[slot] == field) theirs = g_remote[slot];
        g_have_remote[slot] = false;
    }
    while (pump(0)) {}
    PadLive own = field > (unsigned long long)g_delay ? g_local[field % RING] : k_idle;
    g_now[0] = g_host ? own : theirs;      /* port 1: the host */
    g_now[1] = g_host ? theirs : own;      /* port 2: the one who joined */
    /* memory check once a second */
    if (field % HASH_EVERY == 0 && cpu && cpu->ram) {
        static int k;
        uint64_t h = memory_hash(cpu);
        g_my_hash[k] = h;
        g_my_hash_field[k] = field;
        k = (k + 1) % 64;
        if (!g_lost) {
            uint8_t p[13];
            p[0] = 'K';
            put32(p + 1, (uint32_t)field);
            memcpy(p + 5, &h, 8);
            if (!send_all(p, sizeof p)) lost("couldn't send");
        }
        check_hashes();
    }
}

bool netplay_pad(int port, PadLive* out) {
    if (!g_active || port < 0) return false;
    if (port > 1) { *out = k_idle; return true; }
    *out = g_now[port];
    return true;
}

int netplay_local_port(void) { return g_active ? (g_host ? 0 : 1) : -1; }

bool netplay_status(NetplayStatus* st) {
    if (!g_active) return false;
    unsigned long long now = now_ms();
    st->player = g_host ? 1 : 2;
    st->ping_ms = g_ping_ms;
    st->waiting_ms = g_wait_since ? (int)(now - g_wait_since) : 0;
    st->recent_wait = g_last_wait_end && now - g_last_wait_end < 1500;
    st->out_of_sync = g_desync;
    st->lost = g_lost;
    st->delay = g_delay;
    return true;
}

bool netplay_connected(int port, bool* out) {
    if (!g_active) return false;
    *out = port == 0 || port == 1;
    return true;
}

void netplay_report(void) {
    if (!g_active) return;
    printf("netplay: %s, %llu memory checks %s, waited for the other player %llu times (%.1f s in all)%s\n",
           g_host ? "hosted" : "joined", g_checks, g_desync ? "(WENT OUT OF SYNC)" : "all matched", g_waits,
           g_wait_ms_total / 1000.0, g_lost ? ", connection lost" : "");
}
