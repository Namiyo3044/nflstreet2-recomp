#ifndef NFLS2_DISCORD_H
#define NFLS2_DISCORD_H

#include <stdbool.h>

/* Discord Rich Presence (platform/discord.c). */
void discord_start(const char* client_id);              /* starts the background thread */
void discord_set(const char* details, const char* state);   /* what to show (cheap; sent when it changes) */
void discord_enable(bool on);                            /* off: the status is cleared */
void discord_stop(void);                                 /* clears the status and ends the thread */

#endif
