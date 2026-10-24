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
enum { MI_SHAPE, MI_DISPLAY, MI_WINSIZE, MI_RES, MI_RENDERER, MI_TEXTURES, MI_COLORS, MI_CONTROLS, MI_CLOSE, MI_COUNT };

void menu_open(const MenuInfo* info);
/* Opens on its own page asking whether to quit (the main menu's Quit Game);
   menu_take_quit() is true once after YES. */
void menu_open_quit(void);
bool menu_take_quit(void);
bool menu_is_open(void);
/* The menu colours page is open: the game keeps running behind it (its
   input held idle) so picked colours show as they change. */
bool menu_live(void);
void menu_action(int action);

/* The controls page: names for key and controller codes (from the window). */
void menu_set_binding_names(const char* (*key_name)(int code), const char* (*pad_name)(int code));

/* Waiting for a key (1) or a controller button / stick (2) to bind, else 0.
   The window then sends the next one with menu_bind (-1: clear the binding,
   0: cancel). */
int menu_capturing(void);

/* A line about who plays as whom (controllers connected), shown on the
   controls page; the window updates it. */
void menu_set_players_text(const char* text);

/* The graphics card's name, shown for the graphics card renderer. */
void menu_set_gpu_name(const char* name);

/* The picture's shape now (width / height), for the window sizes shown. */
void menu_set_aspect(float aspect);

/* The screen shape can't be changed now (during a match). */
void menu_set_shape_locked(bool locked);
void menu_bind(int code);

/* The game's own Video Settings screen: its rows are Screen Shape, Display,
   Window Size, Renderer and Internal Resolution, values numbered from 0.
   Setting one counts as a change (menu_take_changes). */
#define MENU_GAME_ROWS 5
#define MENU_RECOMP_ROWS 3
extern const char* const k_recomp_label[MENU_RECOMP_ROWS];
/* What the menus show about the session (the GPU, textures), kept up to date
   for the game's own settings screens too. */
void menu_set_info(const MenuInfo* info);
int menu_recomp_row_count(int row);
int menu_recomp_row_get(int row);
void menu_recomp_row_set(int row, int value);
const char* menu_recomp_row_name(int row, int value);
const char* menu_recomp_row_help(int row);
bool menu_recomp_row_enabled(int row);
int menu_game_row_count(int row);
int menu_game_row_get(int row);
void menu_game_row_set(int row, int value);
const char* menu_game_row_name(int row, int value);
const char* menu_game_row_help(int row);
bool menu_game_row_enabled(int row);

/* Items changed since the last call (1 << MI_...; MI_CONTROLS for bindings,
   which are left 0 where they should be the default). */
unsigned menu_take_changes(void);

/* The menu picture for a w x h window; *dirty is set when it changed since
   the last call (or the size did). */
const uint32_t* menu_draw(int w, int h, bool* dirty);

#endif
