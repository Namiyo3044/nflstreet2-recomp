#ifndef NFLS2_UISMENU_H
#define NFLS2_UISMENU_H

#include "cpu/cpu.h"
#include <stdbool.h>

/* The port's own memory: 1 MB past the GameCube's 24 MB (guest 0x81800000),
   which the game never uses (it is told it has 24 MB). */
#define PORT_RAM_BASE 0x81800000u
#define PORT_RAM_SIZE 0x00100000u

/* Function number the added menu code calls to ask "quit?". */
#define UIS_FN_QUIT 0x7A51u
/* ... and to open the port's video settings (Options list). */
#define UIS_FN_VIDEO 0x7A52u

/* Adds a seventh button, Quit Game, to the main menu (once per load of the
   main menu's script; does nothing if the script isn't the expected one).
   Call while the main menu script is setting itself up. */
void uismenu_add_quit(CPUState* cpu);

#endif
