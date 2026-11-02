#ifndef NFLS2_SETTINGS_H
#define NFLS2_SETTINGS_H

#include <stdbool.h>

/* The port's own settings, kept in a small text file (C:\nfls2\settings.ini
 * when started from play.ps1) and changed from the in-game video menu (F1).
 * Command-line switches override them for one run.
 */
/* GameCube controller inputs that can be bound to a key and a controller
   button or stick direction. */
enum {
    GCI_A, GCI_B, GCI_X, GCI_Y, GCI_Z, GCI_START, GCI_L, GCI_R,
    GCI_DUP, GCI_DDOWN, GCI_DLEFT, GCI_DRIGHT,
    GCI_SUP, GCI_SDOWN, GCI_SLEFT, GCI_SRIGHT,
    GCI_CUP, GCI_CDOWN, GCI_CLEFT, GCI_CRIGHT,
    GCI_COUNT
};
extern const char* const k_gc_input_names[GCI_COUNT];   /* "a", "dpad_up", ... (settings file keys) */

typedef struct {
    int renderer;                      /* 0 = graphics card, 1 = software */
    int resolution;                    /* graphics card's internal resolution, 1-8 x the GameCube's */
    int screen_shape;                  /* 0 = the game's setting, 1 = 4:3, 2 = 16:9 */
    int fullscreen;                    /* 0 / 1 */
    int window_size;                   /* 1-6: 480, 720, 900, 1080, 1440, 2160 lines */
    int custom_textures;               /* 0 / 1 */
    /* Bindings: 0 = the default, -1 = nothing. Keys are SDL scancodes;
       controller codes are SDL gamepad button + 1, or 64 + axis * 2
       (+1 for the positive direction). */
    int key_bind[GCI_COUNT];
    int pad_bind[GCI_COUNT];
    int keyboard_player;               /* 0-3: the player the keyboard controls */
    int nintendo_labels;               /* 1: Nintendo-style controllers by their labels (A = A), 0: by position */
    int ps2_rain;                      /* The Alley's rain: 1 PS2 / Xbox (streaks, more drops, splashes), 0 as on GameCube, 2 off */
    int screen_filters;                /* 1: the game's colour filters over the picture in matches, 0: off */
    int skip_intros;                   /* 1: boot straight to the title screen (intro movies fast-forwarded) */
    int specular;                      /* highlights on lit models (graphics card only): 0 off (as on GameCube), 1 subtle, 2 accurate (PS2) */
    int net_delay;                     /* netplay: input delay in video fields (Online menu) */
    char net_address[64];              /* netplay: the last address joined */
    int no_rumble;                     /* 1: controllers never rumble (rumble=0 in the file) */
    int no_discord;                    /* 1: no Discord status (Recomp Settings; discord=0 in the file) */
} Settings;

extern Settings g_settings;

/* Reads the file (missing values keep their defaults). Returns false if
   there is no file yet. */
bool settings_load(const char* path);

/* Writes the file given to settings_load (if any). */
void settings_save(void);

/* The window's height (lines) for a window_size. */
#define SETTINGS_WINDOW_SIZES 6
int settings_window_height(int window_size);

#endif
