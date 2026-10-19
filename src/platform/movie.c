#include "platform/movie.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PL_MPEG_IMPLEMENTATION
#include "third_party/pl_mpeg.h"

static plm_t* g_plm;
static uint32_t* g_px;
static uint32_t g_w, g_h;
static long g_shown = -1;              /* index of the frame in g_px */
static long g_first_visible;

bool movie_open(const char* path) {
    movie_close();
    g_plm = plm_create_with_filename(path);
    if (!g_plm)
        return false;
    plm_set_audio_enabled(g_plm, 0);
    plm_set_loop(g_plm, 0);
    g_w = (uint32_t)plm_get_width(g_plm);
    g_h = (uint32_t)plm_get_height(g_plm);
    if (!g_w || !g_h || g_w > 720 || g_h > 576) {
        printf("movie: %s is %ux%u, larger than 720x576\n", path, g_w, g_h);
        movie_close();
        return false;
    }
    g_px = (uint32_t*)malloc((size_t)g_w * g_h * 4);
    if (!g_px) { movie_close(); return false; }
    for (size_t i = 0; i < (size_t)g_w * g_h; i++) g_px[i] = 0xFF000000u;
    /* The first frame that isn't black (movies often start with a few). */
    g_first_visible = 0;
    for (long n = 0; n < 300; n++) {
        plm_frame_t* f = plm_decode_video(g_plm);
        if (!f) break;
        int bright = 0;
        for (unsigned y = 0; y < f->y.height && !bright; y += 8)
            for (unsigned x = 0; x < f->y.width; x += 8)
                if (f->y.data[y * f->y.width + x] > 40) { bright = 1; break; }
        if (bright) { g_first_visible = n; break; }
    }
    plm_rewind(g_plm);
    g_shown = -1;
    printf("movie: playing %s (%ux%u)\n", path, g_w, g_h);
    return true;
}

void movie_close(void) {
    if (g_plm) plm_destroy(g_plm);
    g_plm = NULL;
    free(g_px);
    g_px = NULL;
    g_shown = -1;
}

long movie_first_visible(void) {
    return g_first_visible;
}

bool movie_is_open(void) {
    return g_plm != NULL;
}

const uint32_t* movie_frame(long want, uint32_t* w, uint32_t* h) {
    if (!g_plm)
        return NULL;
    if (want < 0) want = 0;
    plm_frame_t* f = NULL;
    while (g_shown < want) {
        plm_frame_t* next = plm_decode_video(g_plm);
        if (!next) {
            /* Ran out: hold the last frame for up to a second (the game's
               own copy may run a few frames longer), then stop. */
            if (want > g_shown + 30 || g_shown < 0)
                return NULL;
            break;
        }
        f = next;
        g_shown++;
    }
    if (f)
        plm_frame_to_bgra(f, (uint8_t*)g_px, (int)(g_w * 4));
    *w = g_w;
    *h = g_h;
    return g_px;
}

/* ------------------------------------------------------------ trailers --- */
/* A movie played on its own, with its sound: the trailers of the Previews
   menu, which the GameCube game doesn't have (window.c plays them). */
static plm_t* g_pv;
static uint32_t* g_pv_px;
static uint32_t g_pv_w, g_pv_h;
static plm_frame_t* g_pv_next;         /* decoded, not yet due */
static bool g_pv_video_done, g_pv_have;

bool preview_open(const char* path, int* rate) {
    preview_close();
    g_pv = plm_create_with_filename(path);
    if (!g_pv)
        return false;
    plm_set_loop(g_pv, 0);
    g_pv_w = (uint32_t)plm_get_width(g_pv);
    g_pv_h = (uint32_t)plm_get_height(g_pv);
    if (!g_pv_w || !g_pv_h || g_pv_w > 1920 || g_pv_h > 1080) {
        printf("movie: %s is %ux%u, not a size that can be shown\n", path, g_pv_w, g_pv_h);
        preview_close();
        return false;
    }
    g_pv_px = (uint32_t*)malloc((size_t)g_pv_w * g_pv_h * 4);
    if (!g_pv_px) { preview_close(); return false; }
    bool sound = plm_get_num_audio_streams(g_pv) > 0;
    plm_set_audio_enabled(g_pv, sound ? 1 : 0);
    *rate = sound ? plm_get_samplerate(g_pv) : 0;
    g_pv_next = NULL;
    g_pv_video_done = g_pv_have = false;
    printf("movie: playing trailer %s (%ux%u, %s)\n", path, g_pv_w, g_pv_h, sound ? "with sound" : "no sound");
    return true;
}

void preview_close(void) {
    if (g_pv) plm_destroy(g_pv);
    g_pv = NULL;
    free(g_pv_px);
    g_pv_px = NULL;
    g_pv_next = NULL;
}

const float* preview_audio(void) {
    if (!g_pv) return NULL;
    plm_samples_t* s = plm_decode_audio(g_pv);
    return s ? s->interleaved : NULL;
}

const uint32_t* preview_frame(double t, uint32_t* w, uint32_t* h, bool* ended) {
    *ended = true;
    if (!g_pv) return NULL;
    while (!g_pv_video_done) {
        if (!g_pv_next) {
            g_pv_next = plm_decode_video(g_pv);
            if (!g_pv_next) { g_pv_video_done = true; break; }
        }
        if (g_pv_next->time > t) break;
        plm_frame_to_bgra(g_pv_next, (uint8_t*)g_pv_px, (int)(g_pv_w * 4));
        g_pv_have = true;
        g_pv_next = NULL;
    }
    *ended = g_pv_video_done;
    *w = g_pv_w;
    *h = g_pv_h;
    return g_pv_have ? g_pv_px : NULL;
}
