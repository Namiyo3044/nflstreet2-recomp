#pragma once
/* The PS2 version's online menus, run from the player's own PS2 files (the
   .DAT packs copied into a folder): EA's screens drive themselves with their
   scripts in the Apt player (avm.h) and are drawn by aptdraw.h. The engine's
   side of their messages is answered here. */
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* The files the online menus need, all in `dir`. */
bool ps2o_files_present(const char* dir);
/* The first of those missing (NULL: none). */
const char* ps2o_missing_file(const char* dir);
/* Starts the shell and shows the online main menu; false if the files are
   missing or broken. */
bool ps2o_open(const char* dir, void (*log)(const char* msg));
bool ps2o_is_open(void);
/* one frame of the menus (every ps2o_frame_ms: the movies' own rate, 60 a second) */
double ps2o_frame_ms(void);
void ps2o_tick(void);
/* the picture for a w x h window (0xAARRGGBB, premultiplied) */
void ps2o_draw(uint32_t* px, int w, int h);
/* a pad button pressed or let go */
enum { PS2O_UP, PS2O_DOWN, PS2O_LEFT, PS2O_RIGHT, PS2O_X, PS2O_CIRCLE, PS2O_TRIANGLE, PS2O_SQUARE, PS2O_START, PS2O_SELECT,
       PS2O_L1, PS2O_R1, PS2O_L2, PS2O_R2 };
void ps2o_button(int button, bool down);
/* The screens asked the game to move on (once): "f_MainFE" leaves online mode,
   "f_playnow" is Quick Game, others are the online screens' own. */
bool ps2o_take_flow(char* out, size_t cap);
void ps2o_close(void);
