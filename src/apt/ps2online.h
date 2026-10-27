#pragma once
/* The PS2 version's online menus, run from the player's own PS2 files (the
   .DAT packs copied into a folder): EA's screens drive themselves with their
   scripts in the Apt player (avm.h) and are drawn by aptdraw.h. The engine's
   side of their messages is answered here. */
#include <stdbool.h>
#include <stdint.h>

/* The files the online menus need, all in `dir`. */
bool ps2o_files_present(const char* dir);
/* Starts the shell and shows the online main menu; false if the files are
   missing or broken. */
bool ps2o_open(const char* dir, void (*log)(const char* msg));
bool ps2o_is_open(void);
/* one frame of the menus (30 per second) */
void ps2o_tick(void);
/* the picture for a w x h window (0xAARRGGBB, premultiplied) */
void ps2o_draw(uint32_t* px, int w, int h);
void ps2o_close(void);
