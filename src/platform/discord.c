/* Discord Rich Presence: what the player is doing, shown on their Discord
   profile ("Playing NFL Street 2 Recomp", "In a match").

   Talks to the Discord app on this computer over its local connection (a
   named pipe on Windows, a socket elsewhere), no SDK: each message is a
   little-endian opcode and length followed by JSON. All of it runs on its
   own thread, so a slow or missing Discord never holds up the game; with
   Discord closed it simply tries again now and then. */
#include "platform/discord.h"

#include <SDL3/SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
typedef HANDLE Conn;
#define NO_CONN INVALID_HANDLE_VALUE
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <errno.h>
typedef int Conn;
#define NO_CONN (-1)
#endif

#define DISCORD_IMAGE "logo"           /* the Rich Presence art asset's name (Developer Portal) */
#define DISCORD_IMAGE_TEXT "NFL Street 2 Recomp"
#define RETRY_MS 15000                 /* Discord not there: try again this often */

enum { OP_HANDSHAKE = 0, OP_FRAME = 1, OP_CLOSE = 2 };

static char g_client_id[32];
static SDL_Thread* g_thread;
static SDL_Mutex* g_mx;
static volatile bool g_quit;
static char g_details[128], g_state[128];
static bool g_enabled = true;
static long long g_start;              /* when the game started (Unix seconds) */
static unsigned g_nonce;

/* ------------------------------------------------------- connection --- */
static Conn conn_open(void) {
    for (int i = 0; i < 10; i++) {
#ifdef _WIN32
        char name[64];
        snprintf(name, sizeof name, "\\\\.\\pipe\\discord-ipc-%d", i);
        HANDLE h = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) return h;
#else
        const char* dirs[4] = {getenv("XDG_RUNTIME_DIR"), getenv("TMPDIR"), getenv("TMP"), "/tmp"};
        for (int d = 0; d < 4; d++) {
            if (!dirs[d] || !dirs[d][0]) continue;
            struct sockaddr_un a;
            memset(&a, 0, sizeof a);
            a.sun_family = AF_UNIX;
            snprintf(a.sun_path, sizeof a.sun_path, "%s/discord-ipc-%d", dirs[d], i);
            int s = socket(AF_UNIX, SOCK_STREAM, 0);
            if (s < 0) continue;
            if (connect(s, (struct sockaddr*)&a, sizeof a) == 0) return s;
            close(s);
        }
#endif
    }
    return NO_CONN;
}

static void conn_close(Conn c) {
    if (c == NO_CONN) return;
#ifdef _WIN32
    CloseHandle(c);
#else
    close(c);
#endif
}

static bool conn_write(Conn c, const void* p, size_t n) {
#ifdef _WIN32
    DWORD done = 0;
    return WriteFile(c, p, (DWORD)n, &done, NULL) && done == (DWORD)n;
#else
    const char* b = (const char*)p;
    while (n) {
        ssize_t k = send(c, b, n, 0);
        if (k <= 0) return false;
        b += k; n -= (size_t)k;
    }
    return true;
#endif
}

/* Bytes waiting to be read (-1: the connection is gone). */
static long conn_waiting(Conn c) {
#ifdef _WIN32
    DWORD avail = 0;
    if (!PeekNamedPipe(c, NULL, 0, NULL, &avail, NULL)) return -1;
    return (long)avail;
#else
    char tmp[1];
    ssize_t k = recv(c, tmp, 1, MSG_PEEK | MSG_DONTWAIT);
    if (k > 0) return 1;
    if (k == 0) return -1;
    return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
#endif
}

static bool conn_read(Conn c, void* p, size_t n) {
#ifdef _WIN32
    char* b = (char*)p;
    while (n) {
        DWORD done = 0;
        if (!ReadFile(c, b, (DWORD)n, &done, NULL) || !done) return false;
        b += done; n -= done;
    }
    return true;
#else
    char* b = (char*)p;
    while (n) {
        ssize_t k = recv(c, b, n, 0);
        if (k <= 0) return false;
        b += k; n -= (size_t)k;
    }
    return true;
#endif
}

static bool send_msg(Conn c, uint32_t op, const char* json) {
    uint32_t len = (uint32_t)strlen(json);
    unsigned char head[8] = {(unsigned char)op, (unsigned char)(op >> 8), (unsigned char)(op >> 16), (unsigned char)(op >> 24),
                             (unsigned char)len, (unsigned char)(len >> 8), (unsigned char)(len >> 16), (unsigned char)(len >> 24)};
    return conn_write(c, head, 8) && conn_write(c, json, len);
}

/* Reads one message (its opcode; -1: gone). The text isn't needed. */
static int read_msg(Conn c) {
    unsigned char head[8];
    if (!conn_read(c, head, 8)) return -1;
    uint32_t op = head[0] | (uint32_t)head[1] << 8 | (uint32_t)head[2] << 16 | (uint32_t)head[3] << 24;
    uint32_t len = head[4] | (uint32_t)head[5] << 8 | (uint32_t)head[6] << 16 | (uint32_t)head[7] << 24;
    char buf[512];
    while (len) {
        uint32_t k = len < sizeof buf ? len : (uint32_t)sizeof buf;
        if (!conn_read(c, buf, k)) return -1;
        len -= k;
    }
    return (int)op;
}

/* Reads whatever Discord has sent (its replies), so its side never fills up.
   False: the connection is gone or Discord closed it. */
static bool drain(Conn c) {
    for (;;) {
        long n = conn_waiting(c);
        if (n < 0) return false;
        if (n < 8) return true;
        int op = read_msg(c);
        if (op < 0 || op == OP_CLOSE) return false;
    }
}

/* -------------------------------------------------------------- JSON --- */
static void json_str(char* out, size_t cap, const char* s) {
    size_t o = 0;
    for (; *s && o + 7 < cap; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\') { out[o++] = '\\'; out[o++] = (char)ch; }
        else if (ch < 0x20) o += (size_t)snprintf(out + o, cap - o, "\\u%04x", ch);
        else out[o++] = (char)ch;
    }
    out[o] = 0;
}

static int my_pid(void) {
#ifdef _WIN32
    return (int)GetCurrentProcessId();
#else
    return (int)getpid();
#endif
}

static void activity_json(char* out, size_t cap, bool on, const char* details, const char* state) {
    if (!on) {
        snprintf(out, cap, "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%d,\"activity\":null},\"nonce\":\"%u\"}", my_pid(),
                 ++g_nonce);
        return;
    }
    char d[160], st[160], stfield[200] = "";
    json_str(d, sizeof d, details);
    json_str(st, sizeof st, state);
    if (st[0]) snprintf(stfield, sizeof stfield, "\"state\":\"%s\",", st);
    snprintf(out, cap,
             "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%d,\"activity\":{\"details\":\"%s\",%s"
             "\"timestamps\":{\"start\":%lld},\"assets\":{\"large_image\":\"%s\",\"large_text\":\"%s\"}}},"
             "\"nonce\":\"%u\"}",
             my_pid(), d, stfield, g_start, DISCORD_IMAGE, DISCORD_IMAGE_TEXT, ++g_nonce);
}

/* ------------------------------------------------------------ thread --- */
static int SDLCALL discord_main(void* arg) {
    (void)arg;
    Conn c = NO_CONN;
    char last[160 + 160 + 8] = "";     /* what Discord was last told (details, state, on) */
    bool told_off = false;
    Uint64 next_try = 0;
    bool logged = false;
    while (!g_quit) {
        if (c == NO_CONN) {
            if (SDL_GetTicks() < next_try) { SDL_Delay(250); continue; }
            next_try = SDL_GetTicks() + RETRY_MS;
            c = conn_open();
            if (c == NO_CONN) continue;
            char hs[96];
            snprintf(hs, sizeof hs, "{\"v\":1,\"client_id\":\"%s\"}", g_client_id);
            if (!send_msg(c, OP_HANDSHAKE, hs) || read_msg(c) != OP_FRAME) {   /* (its READY) */
                conn_close(c);
                c = NO_CONN;
                continue;
            }
            if (!logged) { printf("discord: connected (Rich Presence)\n"); logged = true; }
            last[0] = 0;
            told_off = false;
        }
        if (!drain(c)) {
            conn_close(c);
            c = NO_CONN;
            continue;
        }
        char details[128], state[128], now[160 + 160 + 8];
        bool on;
        SDL_LockMutex(g_mx);
        memcpy(details, g_details, sizeof details);
        memcpy(state, g_state, sizeof state);
        on = g_enabled && details[0];
        SDL_UnlockMutex(g_mx);
        snprintf(now, sizeof now, "%d|%s|%s", on, details, state);
        if (strcmp(now, last) != 0 && (on || !told_off)) {
            char json[1024];
            activity_json(json, sizeof json, on, details, state);
            if (!send_msg(c, OP_FRAME, json)) {
                conn_close(c);
                c = NO_CONN;
                continue;
            }
            snprintf(last, sizeof last, "%s", now);
            told_off = !on;
        }
        SDL_Delay(500);
    }
    if (c != NO_CONN) {
        char json[256];
        activity_json(json, sizeof json, false, "", "");
        send_msg(c, OP_FRAME, json);
        conn_close(c);
    }
    return 0;
}

void discord_start(const char* client_id) {
    if (g_thread || !client_id || !client_id[0]) return;
    snprintf(g_client_id, sizeof g_client_id, "%s", client_id);
    g_start = (long long)time(NULL);
    g_mx = SDL_CreateMutex();
    if (!g_mx) return;
    g_thread = SDL_CreateThread(discord_main, "discord", NULL);
}

void discord_set(const char* details, const char* state) {
    if (!g_mx) return;
    if (!details) details = "";
    if (!state) state = "";
    SDL_LockMutex(g_mx);
    snprintf(g_details, sizeof g_details, "%s", details);
    snprintf(g_state, sizeof g_state, "%s", state);
    SDL_UnlockMutex(g_mx);
}

void discord_enable(bool on) {
    if (!g_mx) { g_enabled = on; return; }
    SDL_LockMutex(g_mx);
    g_enabled = on;
    SDL_UnlockMutex(g_mx);
}

void discord_stop(void) {
    if (!g_thread) return;
    /* (not waited for: a Discord that stopped answering mustn't hold up
       quitting; closing the game closes the connection, which clears the
       status anyway) */
    g_quit = true;
    SDL_DetachThread(g_thread);
    g_thread = NULL;
}
