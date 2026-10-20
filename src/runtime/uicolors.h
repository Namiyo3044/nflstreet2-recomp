#ifndef NFLS2_UICOLORS_H
#define NFLS2_UICOLORS_H
#include "cpu/cpu.h"
#include <stdbool.h>

/* Colour swaps for the game's menu and overlay shapes: the flat-coloured
   quads its UI screens draw (the music ticker's bar, border and tab, the
   main menu's red bars), which custom textures can't reach. Read from a
   list of "RRGGBB => RRGGBB" lines (see ui-colors.txt); a missing file is
   fine (nothing changes). Also picked in the port's menu (Menu Colours),
   which saves to the same file. */
void uicolors_load(const char* path);

/* Once per video field: recolours the shapes of screens as they load, and
   again when a colour is picked (a slice of memory each field, the whole
   lot every few fields). */
void uicolors_frame(CPUState* cpu);

/* The menu's named colours ("slots"): each is one shape's colour (with its
   shading following it). Colours are 0xRRGGBB. */
int uicolors_slot_count(void);
const char* uicolors_slot_name(int slot);
const char* uicolors_slot_help(int slot);
u32 uicolors_slot_original(int slot);
u32 uicolors_slot_get(int slot);
void uicolors_slot_set(int slot, u32 rgb);
void uicolors_reset_all(void);

/* Ready-made colours for left / right: 0 is the game's own, then named hues
   at the shape's own strength and brightness, grey and white. */
int uicolors_preset_count(void);
const char* uicolors_preset_name(int preset);
u32 uicolors_preset_color(int slot, int preset);
int uicolors_slot_preset(int slot);   /* -1: a colour of its own (custom) */

/* Writes the menu's colours to the file read at start (its other lines
   kept). */
bool uicolors_save(void);

#endif
