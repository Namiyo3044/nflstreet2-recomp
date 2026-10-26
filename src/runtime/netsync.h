#pragma once
/* Groundwork for netplay: two copies of the game kept in step by giving both
   the same inputs. These tools check that a run can be repeated exactly:
   - an input recording: every controller reply (and "is a controller
     there?") the game gets, in order, plus the real-time clock it started
     with, written by one run and played back by the next;
   - a sync log: a hash of each 64 KB of game memory every second of game
     time, so two runs can be compared to find where they part. */
#include <stdbool.h>
#include <stdint.h>

struct CPUState;

/* Start recording to / playing back from a file. Playback returns false if
   the file isn't a recording. */
bool netsync_record(const char* path);
bool netsync_play(const char* path);
bool netsync_playing(void);

/* The real-time clock (seconds since 2000): fixed for playback (the
   recording's), with --rtc, or while recording (the PC's at the start,
   stored in the recording); else the PC's clock as it goes. */
uint32_t netsync_rtc(uint32_t pc_clock);
void netsync_set_rtc(uint32_t seconds);

/* Called by the controller code for every answer the game gets: records it,
   or replaces it with the recorded one. */
bool netsync_connected(int port, bool live);
void netsync_poll(int port, uint8_t out[8]);

/* Sync log: a line every `every` video fields. */
bool netsync_log_open(const char* path, unsigned every);
void netsync_field(struct CPUState* cpu, unsigned long long field);

void netsync_report(void);
