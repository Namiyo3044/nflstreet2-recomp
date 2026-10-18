#ifndef NFLS2_SETTINGS_H
#define NFLS2_SETTINGS_H

#include <stdbool.h>

/* The port's own settings, kept in a small text file (C:\nfls2\settings.ini
 * when started from play.ps1) and changed from the in-game video menu (F1).
 * Command-line switches override them for one run.
 */
typedef struct {
    int renderer;                      /* 0 = graphics card, 1 = software */
    int resolution;                    /* graphics card's internal resolution, 1-8 x the GameCube's */
    int screen_shape;                  /* 0 = the game's setting, 1 = 4:3, 2 = 16:9 */
    int fullscreen;                    /* 0 / 1 */
    int window_size;                   /* 1-4 x 640x480 */
    int custom_textures;               /* 0 / 1 */
} Settings;

extern Settings g_settings;

/* Reads the file (missing values keep their defaults). Returns false if
   there is no file yet. */
bool settings_load(const char* path);

/* Writes the file given to settings_load (if any). */
void settings_save(void);

#endif
