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

/* Opens the window (height: its starting height in lines; the width follows
   the screen shape). Returns false if SDL or the window can't be started. */
bool window_open(int height);

/* Draw with the graphics card and show its frames directly (call before
   window_open). scale: internal resolution as a multiple of the GameCube's.
   If OpenGL 3.3 isn't available, window_open falls back to the software
   renderer. */
void window_set_gpu(int scale);

/* Screen shape before the window opens: 0 = auto, 1 = 4:3, 2 = 16:9. */
void window_set_aspect_mode(int mode);

/* Fast-forward on or off (also on while Tab is held). */
void window_set_turbo(bool on);
/* Black picture and no sound (while the intro movies are skipped). */
void window_set_hidden(bool on);

/* Records everything sent to the speakers to a .wav file. */
void window_set_audio_dump(const char* path);

/* From now on, follow the game's detected widescreen setting (until then the
   shape remembered from the last run is used). */
void window_trust_screen_shape(void);

/* Once per video field: present the frame, read input, wait for real time. */
/* Keeps the window answering the system while the game waits. */
void window_keep_alive(void);
/* The main menu's Online: opens the port's Online page (netplay). */
void window_online_request(void);
/* (Dormant for now: Online uses the game's own screens, which custom
   textures reach.) The PS2 version's online menus (apt/ps2online.h), from the player's PS2
   files in `dir`: available once those are there (and the graphics card
   renderer is in use). The request shows them, the game standing still. */
void window_set_ps2_dir(const char* dir);
/* --perf-log: the per-second "perf:" lines every second, even at full speed (for measuring) */
void window_set_perf_log(bool on);
bool window_ps2_online_available(void);
void window_ps2_online_request(void);
/* After the window closed: true (once) if it closed to restart the game for
   netplay, with the options to restart with ("" = without netplay). */
bool window_take_restart(char* args, size_t cap);
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

/* The main menu's Quit Game was picked: asks "quit?" at the next field. */
void window_quit_request(void);

/* Quit now (the game's own Quit Game dialog said yes). */
void window_quit_now(void);

/* The game's Options list picked Video Settings: opens the port's menu. */
void window_video_request(void);

/* The game's main menu script is running (called from its function calls):
   screen shape changes there needn't wait for a new screen. */
void window_note_front_end(void);

#endif
