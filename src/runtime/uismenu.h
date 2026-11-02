#ifndef NFLS2_UISMENU_H
#define NFLS2_UISMENU_H

#include "cpu/cpu.h"
#include <stdbool.h>

/* The port's own memory: 1 MB past the GameCube's 24 MB (guest 0x81800000),
   which the game never uses (it is told it has 24 MB). */
#define PORT_RAM_BASE 0x81800000u
#define PORT_RAM_SIZE 0x00100000u

/* Function number the added menu code calls once "quit?" is answered yes. */
#define UIS_FN_QUIT 0x7A51u
/* ... and to open the port's video settings (Options list). */
#define UIS_FN_VIDEO 0x7A52u
/* ... and to say the Options list opens Audio Settings itself (not in video mode). */
#define UIS_FN_AUDIO 0x7A53u
#define UIS_FN_VIDEO_BACK 0x7A54u   /* 1 once when back from Video Settings */
#define UIS_FN_FLICKER_DONE 0x7A55u /* after the quit dialog: the text flicker finished */
#define UIS_FN_RECOMP 0x7A56u       /* ... and to open the port's Recomp Settings (Options list) */
#define UIS_FN_ONLINE 0x7A57u       /* ... and the Online screen (Game Modes list's Online) */

/* The main menu's text-flicker pieces shown again in full (a dialog opened
   while one was flickering stopped it part way, leaving text hidden). */
void uismenu_flicker_done(CPUState* cpu);

/* Adds a seventh button, Quit Game, to the main menu (once per load of the
   main menu's script; does nothing if the script isn't the expected one).
   Call while the main menu script is setting itself up. */
void uismenu_add_quit(CPUState* cpu);

/* Hides the GameCube title screen's "Licensed by Nintendo" line, as on PS2 and
   Xbox (call while the title screen script is loaded). */
void uismenu_hide_licence(CPUState* cpu);

/* Video Settings: turns the Audio Settings screen being set up into the
   video settings screen (false if it isn't as expected). */
bool uismenu_video_screen(CPUState* cpu);

/* The Video Settings rows' values, from the window side (settings). */
typedef struct {
    int (*count)(int row);
    int (*get)(int row);
    void (*set)(int row, int value);
    const char* (*name)(int row, int value);
    const char* (*help)(int row);
    bool (*enabled)(int row);          /* can be used now (else greyed out and skipped) */
    int rows;                          /* how many (2-7) */
    const char* title;
    const char* const* labels;         /* each row's label */
} UisVideoRows;
/* The screens made from Audio Settings: Video Settings (page 0), Recomp
   Settings (page 1) and Online (page 2, the main menu's Online). */
void uismenu_set_video_rows(const UisVideoRows* video, const UisVideoRows* recomp, const UisVideoRows* online);
/* Which of them the next Audio Settings screen in video mode is. */
void uismenu_set_video_page(int page);
/* The open one's row with the cursor (-1: none open) and its page. */
int uismenu_video_focus(int* page);

/* Video Settings is open (the Audio Settings screen in video mode). */
bool uismenu_video_active(CPUState* cpu);

/* Another screen's script ran: Video Settings is closed. */
void uismenu_video_closed(void);

/* Once per video field: Video Settings' rows follow and set the settings. */
void uismenu_video_frame(CPUState* cpu);

/* Save/Load: a Delete User ID row (the Auto Save row copied under the
   buttons). Its values (row 0 of these). */
void uismenu_set_saveload_row(const UisVideoRows* row);
/* The Save/Load screen setting itself up (its platform check): adds the row
   (does nothing for another screen). */
bool uismenu_saveload_screen(CPUState* cpu);
void uismenu_saveload_closed(void);
/* Once per video field: the row follows the values; true while it has the
   cursor. */
bool uismenu_saveload_frame(CPUState* cpu);

#endif
