#ifndef NFLS2_WINDOW_H
#define NFLS2_WINDOW_H

#include "cpu/cpu.h"

/* Live window (SDL3): shows the TV picture every video field, reads the
 * keyboard and gamepads into the GameCube controller, and keeps the game at
 * real-time speed.
 *
 * Keyboard (Dolphin's default layout):
 *   arrows = main stick   X = A   Z = B   C = X   S = Y   D = Z
 *   Q / W = L / R         Enter = START   I J K L = C-stick   T F G H = D-pad
 *   Esc = quit            F11 = fullscreen
 */

/* Opens the window (scale = initial size as a multiple of 640x480).
   Returns false if SDL or the window can't be started. */
bool window_open(int scale);

/* Once per video field: present the frame, read input, wait for real time. */
void window_field(CPUState* cpu);

/* True once the user has closed the window or pressed Esc. */
bool window_quit_requested(void);

void window_close(void);

#endif
