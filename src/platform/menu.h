#ifndef NFLS2_MENU_H
#define NFLS2_MENU_H

#include <stdbool.h>
#include <stdint.h>

/* The port's video settings menu, drawn over the game (F1 or the
 * controller's Back / View button). It edits g_settings; the window applies
 * what changed. Drawn into a picture the size of the window (0xAARRGGBB,
 * premultiplied alpha), redrawn only when something changes.
 */

typedef struct {
    bool gpu_running;                  /* the graphics card renderer is in use */
    int res_running;                   /* its internal resolution */
    bool textures_found;               /* the custom texture folder exists */
} MenuInfo;

enum { MENU_UP, MENU_DOWN, MENU_LEFT, MENU_RIGHT, MENU_OK, MENU_BACK };

/* Settings items, as bits in menu_take_changes(). */
enum { MI_SHAPE, MI_DISPLAY, MI_WINSIZE, MI_RENDERER, MI_RES, MI_TEXTURES, MI_CLOSE, MI_COUNT };

void menu_open(const MenuInfo* info);
bool menu_is_open(void);
void menu_action(int action);

/* Items changed since the last call (1 << MI_...). */
unsigned menu_take_changes(void);

/* The menu picture for a w x h window; *dirty is set when it changed since
   the last call (or the size did). */
const uint32_t* menu_draw(int w, int h, bool* dirty);

#endif
