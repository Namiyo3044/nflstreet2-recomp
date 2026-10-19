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
 *   Esc = quit   F10 = screenshot (shot_NNN.bmp)   F11 = fullscreen
 *   Tab (hold) = fast-forward
 *   F9 = screen shape: auto (follows the game's widescreen option) / 4:3 / 16:9
 *   F12 = draw log of the next two frames (drawlog.txt, drawlog-short.txt)
 *   F1 (or the controller's Back / View button) = video menu (game paused)
 *   F6 = memory search for a setting (see window.c)
 */

/* Opens the window (scale = initial size as a multiple of 640x480).
   Returns false if SDL or the window can't be started. */
bool window_open(int scale);

/* Draw with the graphics card and show its frames directly (call before
   window_open). scale: internal resolution as a multiple of the GameCube's.
   If OpenGL 3.3 isn't available, window_open falls back to the software
   renderer. */
void window_set_gpu(int scale);

/* Screen shape before the window opens: 0 = auto, 1 = 4:3, 2 = 16:9. */
void window_set_aspect_mode(int mode);

/* Fast-forward on or off (also on while Tab is held). */
void window_set_turbo(bool on);

/* Records everything sent to the speakers to a .wav file. */
void window_set_audio_dump(const char* path);

/* From now on, follow the game's detected widescreen setting (until then the
   shape remembered from the last run is used). */
void window_trust_screen_shape(void);

/* Once per video field: present the frame, read input, wait for real time. */
void window_field(CPUState* cpu);

/* True once the user has closed the window or pressed Esc. */
bool window_quit_requested(void);

/* True once after F8 is pressed (search memory for on-screen text). */
bool window_take_text_search(void);

void window_close(void);

/* Replacement movies: the game started playing movie index (its place in
   MOVIEDAT.DAT), or stopped. Shows replace\moviedat_<index>.mpg meanwhile,
   if there is one. */
void window_movie_begin(int index);
void window_movie_end(void);

/* The Previews menu asked for trailer index (its place in the Xbox / PS2
   MOVIEDAT.DAT, 0-10): plays replace\previews\<name>.mpg at the next video
   field, the game standing still meanwhile. */
void window_preview_request(int index);

#endif
