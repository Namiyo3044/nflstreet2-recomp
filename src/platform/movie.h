#ifndef NFLS2_MOVIE_H
#define NFLS2_MOVIE_H

#include <stdbool.h>
#include <stdint.h>

/* Replacement videos for the game's movies: an MPEG-1 file (.mpg) shown in
 * place of the picture while the game plays its own movie underneath (its
 * sound, timing and skipping stay the game's). The .mpg carries the same
 * frames one for one, and each is shown when the game decodes its own copy
 * of that frame, so the video stays in step with the sound.
 */
bool movie_open(const char* path);
void movie_close(void);
bool movie_is_open(void);

/* The number of the first frame that isn't black. */
long movie_first_visible(void);

/* Frame number n (0xAARRGGBB, top row first; frames only move forward); past
   the end, the last frame for a second, then NULL. Valid until the next
   call. */
const uint32_t* movie_frame(long n, uint32_t* w, uint32_t* h);

/* A movie on its own with its sound (the Previews menu's trailers). rate:
   the sound's sample rate, 0 when it has none. */
bool preview_open(const char* path, int* rate);
void preview_close(void);
/* The next 1152 stereo frames of sound (float, interleaved), NULL at the end. */
const float* preview_audio(void);
/* The picture due at t seconds (NULL before the first); ended once the
   pictures have run out. */
const uint32_t* preview_frame(double t, uint32_t* w, uint32_t* h, bool* ended);

#endif
