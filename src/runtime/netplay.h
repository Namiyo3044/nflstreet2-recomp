#pragma once
/* Netplay: two copies of the game kept in step over the network by giving
   both the same controller input (lockstep). The game is deterministic: fed
   the same inputs, clock, settings and memory card, both run exactly the same
   (see netsync.h). The host is controller port 1, the one who joins port 2.
   Each video field, each side sends its own controller for a few fields
   ahead (the input delay, which hides the network's round trip) and waits
   for the other's for this field. */
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "runtime/pad.h"

struct CPUState;

/* Connect before the game starts. host: wait on `port` for the other
   player; join: connect to "address[:port]". `fingerprint` describes the
   settings that change the game (both must match). The host sends its clock
   and memory card; the joiner gets them through netplay_card(). Returns
   false (with a message) if no session could be set up. */
bool netplay_host(int port, int delay, const char* fingerprint);
bool netplay_join(const char* address, int delay, const char* fingerprint);
bool netplay_active(void);

/* The host's memory card for the joiner (NULL if none was sent); the host
   gives its own here before connecting. */
void netplay_set_card(const uint8_t* data, uint32_t size);
const uint8_t* netplay_card(uint32_t* size);

/* At the start of each video field (before the controllers are polled):
   sends this side's controller and waits for the other side's. */
void netplay_field(struct CPUState* cpu, unsigned long long field);

/* The controller state for a port this field, in place of the live one
   (ports 1-2; false: not a netplay port). */
bool netplay_pad(int port, PadLive* out);
bool netplay_connected(int port, bool* out);
/* This side's controller port in the session (0 host, 1 joiner; -1: none). */
int netplay_local_port(void);
/* How the session is going, for the on-screen badge. */
typedef struct {
    int player;                        /* 1 host, 2 joiner */
    int ping_ms;                       /* round trip (-1: not measured yet) */
    int waiting_ms;                    /* waiting for the other player's input right now, for so long */
    bool recent_wait;                  /* had to wait in the last moment (a hitch) */
    bool out_of_sync, lost;
    int delay;                         /* input delay in fields */
} NetplayStatus;
bool netplay_status(NetplayStatus* st);

/* Called now and then while waiting for the other player (keeps the window
   answering the system). */
void netplay_set_idle(void (*fn)(void));

/* The Online menu (the game runs on meanwhile). Poll each frame: the state,
   a message for the player (why it failed), and the agreed input delay once
   READY, when both games restart into the session. */
enum { NETPLAY_LOBBY_IDLE, NETPLAY_LOBBY_WAITING, NETPLAY_LOBBY_CONNECTING, NETPLAY_LOBBY_READY, NETPLAY_LOBBY_FAILED };
bool netplay_lobby_host(int port, int delay);
bool netplay_lobby_join(const char* address, int delay);
int netplay_lobby_poll(const char** message, int* delay);
void netplay_lobby_cancel(void);
/* The settings fingerprint, worked out when needed (the settings can change). */
void netplay_set_fingerprint(const char* (*fn)(void));
/* This PC's network addresses, for the host to give out. Returns how many. */
int netplay_local_addresses(char* out, size_t cap);

/* The connection went (the other player left or the network failed). */
bool netplay_lost(void);
/* Tells the other player this game is leaving and closes the connections
   (before the game ends or restarts: a restarted game would otherwise keep
   them open, and the other player's game would wait on it). */
void netplay_close(void);

void netplay_report(void);
