#pragma once
/* Draws the Apt player's display tree (aptdraw.c): shapes, images, masks and
   EA's bitmap fonts, into a picture of 0xAARRGGBB premultiplied pixels. */
#include "apt/avm.h"
#include "apt/terf.h"

/* the fonts (FONTS.DAT unpacked; kept, not copied) */
void aptdraw_set_fonts(TerfPack* fonts);
/* every level, the stage (854 x 480) at `scale` with its top-left at (ox, oy);
   the picture is filled with bg first */
void aptdraw_levels(VM* vm, uint32_t* px, int w, int h, float scale, float ox, float oy, uint32_t bg);
void aptdraw_local_bounds(VM* vm, Obj* o, float out[4], bool* any);
/* a text field's text size in its own units (its textWidth / textHeight) */
void aptdraw_text_size(VM* vm, Obj* o, float* w, float* h);
