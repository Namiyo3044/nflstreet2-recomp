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

/* Called now and then while waiting for the other player (keeps the window
   answering the system). */
void netplay_set_idle(void (*fn)(void));

void netplay_report(void);
