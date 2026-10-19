#include "runtime/prof.h"
#include "platform/window.h"
#include "gx/raster.h"
#include "gx/raster_gl.h"
#include "runtime/interrupts.h"
#include "runtime/mmio.h"
#include "runtime/pad.h"
#include "platform/settings.h"
#include "platform/menu.h"
#include "gx/hires.h"
#include "gx/texture.h"
#include "platform/movie.h"
#include "runtime/disc.h"

#include <SDL3/SDL.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEX_W 720
#define TEX_H 576

static SDL_Window* g_window;
static SDL_Renderer* g_renderer;
static SDL_Texture* g_texture;
static SDL_Gamepad* g_pad;
static SDL_AudioStream* g_audio;
static u32 g_audio_rate;
static bool g_quit;
static bool g_fullscreen;
static int g_gpu_scale;                /* 0 = software renderer */
static bool g_gpu;                     /* GPU mode is running */
static u32 g_pixels[TEX_W * TEX_H];
static u32 g_shown_w = 640, g_shown_h = 480;

/* Real-time pacing: game fields are held to the wall clock. */
static Uint64 g_base_ns;               /* wall time of field g_base_field */
static u64 g_base_field;
static u64 g_field;
static Uint64 g_stat_ns;
static Uint64 g_slept_ns, g_present_ns;   /* this second: pacing sleep, picture output */
static u64 g_stat_fields;
static u32 g_frames_at_stat;

#define FIELD_NS (1001000000.0 / 60.0) /* NTSC: 59.94 fields per second */

/* Audio: finished audio-interface buffers (16-bit big-endian, right then
   left) are converted to native left/right and queued to SDL. If the queue
   runs more than ~150 ms ahead (the game ran fast for a moment), the buffer
   is dropped so sound stays in step with the picture. */
/* Fast-forward: no waiting for real time, few pictures shown, no sound.
   On while Tab is held, and set by the game side to skip the intro movies. */
static bool g_turbo_forced, g_turbo_key;

void window_set_turbo(bool on) {
    static Uint64 since;
    if (on != g_turbo_forced) {
        if (on) {
            since = SDL_GetTicksNS();
            printf("window: fast-forward on (skipping the THX intro)\n");
        } else {
            printf("startup: THX intro skipped in %.1f s\n", (double)(SDL_GetTicksNS() - since) * 1e-9);
        }
    }
    g_turbo_forced = on;
}

static bool turbo(void) {
    return g_turbo_forced || g_turbo_key;
}

static unsigned g_audio_underruns, g_audio_sec_underruns, g_audio_sec_buffers, g_audio_sec_dropped;
static int g_audio_sec_min_queue = 1 << 30;

static void audio_sink(const u8* samples, u32 frames, u32 rate) {
    if (!g_audio || !frames || turbo())
        return;
    if (rate != g_audio_rate) {
        SDL_AudioSpec spec = {SDL_AUDIO_S16, 2, (int)rate};
        SDL_SetAudioStreamFormat(g_audio, &spec, NULL);
        g_audio_rate = rate;
    }
    /* The game makes its sound in bursts: a whole video field's worth
       (three 5 ms buffers) arrives within a few milliseconds and then nothing
       until the next field. Keep a cushion of about 50 ms queued so the
       sound card never runs dry between bursts; if it does (the game fell
       behind), rebuild the cushion with a little silence. Over 200 ms
       queued (the game ran ahead), drop the buffer to stay in sync. */
    int bytes_per_ms = (int)(rate * 4u / 1000u);
    int queued = SDL_GetAudioStreamQueued(g_audio);
    /* The cushion: 50 ms, raised to 120 ms for a while after the sound ran
       dry (a loading screen or a scene change stalling the game), so the
       stalls that tend to come in groups don't each make a gap. */
    static int cushion_ms = 50;
    static Uint64 raised_at;
    if (cushion_ms > 50 && SDL_GetTicksNS() - raised_at > 20000000000ull)
        cushion_ms = 50;
    g_audio_sec_buffers++;
    if (queued < g_audio_sec_min_queue) g_audio_sec_min_queue = queued;
    if (queued > bytes_per_ms * 200) {
        g_audio_sec_dropped++;
        return;
    }
    /* When the game runs a little slow it makes sound slightly slower than
       real time. Rather than let the queue run dry (a gap), play slightly
       slower as the queue gets low, by up to 5% (a barely audible pitch
       drop), and back at normal speed once it refills. */
    {
        static float ratio = 1.0f;
        float fill = (float)queued / (float)(bytes_per_ms * cushion_ms);  /* 1 = at the target */
        float want = fill >= 1.0f ? 1.0f : 0.95f + 0.05f * fill;
        ratio += (want - ratio) * 0.05f;                                  /* glide, no sudden pitch jumps */
        SDL_SetAudioStreamFrequencyRatio(g_audio, ratio);
    }
    /* Ran dry: rebuild the cushion with silence. The silence starts with a
       short fade from the last sample played, and the sound that follows
       fades back in, so the gap doesn't click. */
    static s16 last_l, last_r;
    static u32 fade_in;                /* frames of the coming sound still fading in */
    const u32 fade = rate / 333u;      /* about 3 ms */
    if (queued < bytes_per_ms * 4) {
        s16 sil[2 * 800];
        if (g_field > 600) {           /* not for the start-up gap */
            cushion_ms = 120;
            raised_at = SDL_GetTicksNS();
        }
        int want = (bytes_per_ms * cushion_ms - queued) / 4;   /* frames */
        u32 done = 0;
        while (want > 0) {
            int n = want > 800 ? 800 : want;
            for (int i = 0; i < n; i++, done++) {
                float g = done < fade ? 1.0f - (float)done / (float)fade : 0.0f;
                sil[i * 2] = (s16)(last_l * g);
                sil[i * 2 + 1] = (s16)(last_r * g);
            }
            SDL_PutAudioStreamData(g_audio, sil, n * 4);
            want -= n;
        }
        fade_in = fade;
        g_audio_underruns++;
        g_audio_sec_underruns++;
    }
    s16 buf[2 * 4096];
    while (frames) {
        u32 n = frames > 4096 ? 4096 : frames;
        for (u32 i = 0; i < n; i++) {
            const u8* p = samples + i * 4;
            s16 r = (s16)((p[0] << 8) | p[1]);
            s16 l = (s16)((p[2] << 8) | p[3]);
            if (fade_in) {
                float g = 1.0f - (float)fade_in / (float)fade;
                l = (s16)(l * g);
                r = (s16)(r * g);
                fade_in--;
            }
            buf[i * 2] = l;
            buf[i * 2 + 1] = r;
            last_l = l;
            last_r = r;
        }
        SDL_PutAudioStreamData(g_audio, buf, (int)(n * 4));
        samples += n * 4;
        frames -= n;
    }
}

static void open_audio(void) {
    SDL_AudioSpec spec = {SDL_AUDIO_S16, 2, 32000};
    g_audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (!g_audio) {
        printf("window: no audio (%s)\n", SDL_GetError());
        return;
    }
    g_audio_rate = 32000;
    SDL_ResumeAudioStreamDevice(g_audio);
    mmio_set_audio_sink(audio_sink);
    printf("window: audio on\n");
}

/* Screen shape: 0 = follow the game's widescreen option, 1 = always 4:3,
   2 = always 16:9. F9 cycles. */
static int g_aspect_mode;
static float g_aspect = 4.0f / 3.0f;
static char g_title_shape[32] = "4:3 (auto)";
static char g_title_stats[64] = "";

static char g_title_cam[48] = "";

static void set_title(void) {
    char title[192];
    snprintf(title, sizeof(title), "NFL Street 2%s%s  -  %s%s", g_title_stats[0] ? "  -  " : "", g_title_stats,
             g_title_shape, g_title_cam);
    SDL_SetWindowTitle(g_window, title);
}

void window_set_aspect_mode(int mode) {
    g_aspect_mode = mode < 0 || mode > 2 ? 0 : mode;
}

/* The game's widescreen setting is only visible once it draws 3D (the title
   screen), so in auto mode the last shape it was seen using is remembered
   in a small file and used from the start of the next run: the intro movies
   and loading screens then already show at that shape. */
#define SHAPE_FILE "nfls2-screen.txt"
static int g_saved_wide = -1;          /* -1 unknown, 0 4:3, 1 16:9 */
/* The boot screens (memory card check, loading) draw some 3D at 4:3 whatever
   the setting; detection is only believed from the NFL Street 2 intro on. */
static bool g_shape_trusted;

void window_trust_screen_shape(void) {
    g_shape_trusted = true;
    gx_raster_widescreen_reset();      /* what the boot screens showed doesn't count */
}

static void load_saved_shape(void) {
    FILE* f = fopen(SHAPE_FILE, "r");
    if (!f)
        return;
    char buf[32] = {0};
    if (fgets(buf, sizeof(buf), f))
        g_saved_wide = strncmp(buf, "16:9", 4) == 0 ? 1 : strncmp(buf, "4:3", 3) == 0 ? 0 : -1;
    fclose(f);
}

static void save_shape(int wide) {
    if (wide == g_saved_wide)
        return;
    g_saved_wide = wide;
    FILE* f = fopen(SHAPE_FILE, "w");
    if (f) {
        fputs(wide ? "16:9\n" : "4:3\n", f);
        fclose(f);
    }
}

static void update_aspect(bool announce) {
    int detected = g_shape_trusted || g_saved_wide < 0 ? gx_raster_widescreen() : -1;
    if (g_aspect_mode == 0 && detected >= 0 && g_shape_trusted)
        save_shape(detected);
    int game_wide = detected >= 0 ? detected : g_saved_wide == 1;
    int wide = g_aspect_mode == 2 || (g_aspect_mode == 0 && game_wide == 1);
    float aspect = wide ? 16.0f / 9.0f : 4.0f / 3.0f;
    if (aspect == g_aspect && !announce)
        return;
    bool changed = aspect != g_aspect;
    g_aspect = aspect;
    static const char* const k_mode[] = {"auto", "fixed", "fixed"};
    snprintf(g_title_shape, sizeof(g_title_shape), "%s (%s)", wide ? "16:9" : "4:3", k_mode[g_aspect_mode]);
    set_title();
    printf("window: screen shape %s (%s)\n", wide ? "16:9" : "4:3", k_mode[g_aspect_mode]);
    /* A plain window follows the new shape: same height, new width. */
    if (changed && !g_fullscreen && !(SDL_GetWindowFlags(g_window) & SDL_WINDOW_MAXIMIZED)) {
        int w, h;
        SDL_GetWindowSize(g_window, &w, &h);
        SDL_SetWindowSize(g_window, (int)((float)h * aspect + 0.5f), h);
    }
}

/* Graphics-card rendering (window_set_gpu before window_open). Two OpenGL
   3.3 contexts share textures: one on a small hidden window, used by the
   renderer's GPU thread to draw; one on the game window, used here to show
   the finished frames. */
static SDL_Window* g_gl_window;
static SDL_GLContext g_gl_ctx, g_present_ctx;

void window_set_gpu(int scale) {
    g_gpu_scale = scale;
}

static void gl_make_current(void) {
    if (SDL_GL_GetCurrentContext() != g_gl_ctx)
        SDL_GL_MakeCurrent(g_gl_window, g_gl_ctx);
}

static void* gl_getproc(const char* name) {
    return (void*)SDL_GL_GetProcAddress(name);
}

static void gpu_teardown(void) {
    if (g_present_ctx) SDL_GL_DestroyContext(g_present_ctx);
    if (g_gl_ctx) SDL_GL_DestroyContext(g_gl_ctx);
    if (g_gl_window) SDL_DestroyWindow(g_gl_window);
    g_present_ctx = g_gl_ctx = NULL;
    g_gl_window = NULL;
}

/* Sets up both contexts and the renderer's GPU backend; g_window must have
   been created with SDL_WINDOW_OPENGL. */
static bool gpu_setup(void) {
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0);
    g_gl_window = SDL_CreateWindow("NFL Street 2 (GPU)", 64, 64, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!g_gl_window) {
        printf("gpu: couldn't make the OpenGL window: %s\n", SDL_GetError());
        return false;
    }
    g_gl_ctx = SDL_GL_CreateContext(g_gl_window);
    if (!g_gl_ctx) {
        printf("gpu: no OpenGL 3.3 context: %s\n", SDL_GetError());
        return false;
    }
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);   /* g_gl_ctx is current */
    g_present_ctx = SDL_GL_CreateContext(g_window);
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0);
    if (!g_present_ctx) {
        printf("gpu: no OpenGL context for the window: %s\n", SDL_GetError());
        return false;
    }
    SDL_GL_MakeCurrent(g_gl_window, g_gl_ctx);
    if (!gx_raster_use_gpu(gl_getproc, g_gpu_scale, gl_make_current))
        return false;
    /* From here the GPU thread owns g_gl_ctx; this thread keeps the window's. */
    SDL_GL_MakeCurrent(g_window, g_present_ctx);
    SDL_GL_SetSwapInterval(0);         /* pacing is done against the game's own clock */
    if (!glr_present_init())
        return false;
    gx_raster_present_from_gpu(true);
    return true;
}

bool window_open(int scale) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) {
        fprintf(stderr, "error: SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }
    if (scale < 1) scale = 1;
    load_saved_shape();
    bool start_wide = g_aspect_mode == 2 || (g_aspect_mode == 0 && g_saved_wide == 1);
    int win_w = start_wide ? 853 * scale : 640 * scale, win_h = 480 * scale;
    if (g_gpu_scale > 0) {
        g_window = SDL_CreateWindow("NFL Street 2", win_w, win_h, SDL_WINDOW_RESIZABLE | SDL_WINDOW_OPENGL);
        if (g_window && gpu_setup()) {
            g_gpu = true;
        } else {
            printf("gpu: falling back to the software renderer\n");
            gpu_teardown();
            if (g_window) SDL_DestroyWindow(g_window);
            g_window = NULL;
        }
    }
    if (!g_window)
        g_window = SDL_CreateWindow("NFL Street 2", win_w, win_h, SDL_WINDOW_RESIZABLE);
    if (start_wide)
        g_aspect = 16.0f / 9.0f;
    if (!g_window) {
        fprintf(stderr, "error: couldn't create the window: %s\n", SDL_GetError());
        return false;
    }
    if (!g_gpu) {
        g_renderer = SDL_CreateRenderer(g_window, NULL);
        if (!g_renderer) {
            fprintf(stderr, "error: couldn't create a renderer: %s\n", SDL_GetError());
            return false;
        }
        SDL_SetRenderVSync(g_renderer, 0); /* pacing is done against the game's own clock */
        g_texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, TEX_W, TEX_H);
        if (!g_texture) {
            fprintf(stderr, "error: couldn't create a texture: %s\n", SDL_GetError());
            return false;
        }
        SDL_SetTextureScaleMode(g_texture, SDL_SCALEMODE_LINEAR);
    }
    printf("window: %s, keyboard: arrows=stick X=A Z=B C=X S=Y D=Z Q/W=L/R Enter=START, Esc quits, "
           "Tab fast-forward, F1 video menu, F7 free camera, F9 screen shape (auto/4:3/16:9), F10 screenshot, F11 fullscreen, F12 draw log\n",
           g_gpu ? "picture from the graphics card" : SDL_GetRendererName(g_renderer));
    open_audio();
    gx_raster_set_target_shape(g_aspect_mode);
    update_aspect(true);
    if (g_settings.fullscreen) {
        g_fullscreen = true;
        SDL_SetWindowFullscreen(g_window, true);
    }
    g_base_ns = g_stat_ns = SDL_GetTicksNS();
    return true;
}

bool window_quit_requested(void) {
    return g_quit;
}

void window_close(void) {
    printf("window: sound card ran dry %u times\n", g_audio_underruns);
    mmio_set_audio_sink(NULL);
    if (g_audio) SDL_DestroyAudioStream(g_audio);
    g_audio = NULL;
    if (g_pad) SDL_CloseGamepad(g_pad);
    if (g_texture) SDL_DestroyTexture(g_texture);
    if (g_renderer) SDL_DestroyRenderer(g_renderer);
    gx_raster_sync();
    gpu_teardown();
    if (g_window) SDL_DestroyWindow(g_window);
    SDL_Quit();
    g_pad = NULL;
    g_texture = NULL;
    g_renderer = NULL;
    g_window = NULL;
}

/* Saves what is on screen (the last presented TV picture) as shot_NNN.bmp. */
static void save_screenshot(void) {
    static int n;
    char path[64];
    snprintf(path, sizeof(path), "shot_%03d.bmp", ++n);
    FILE* f = fopen(path, "wb");
    if (!f)
        return;
    u32 w = g_shown_w, h = g_shown_h, row = (w * 3 + 3) & ~3u, size = 54 + row * h;
    u8 hdr[54] = {'B', 'M'};
    hdr[2] = (u8)size; hdr[3] = (u8)(size >> 8); hdr[4] = (u8)(size >> 16); hdr[5] = (u8)(size >> 24);
    hdr[10] = 54; hdr[14] = 40;
    hdr[18] = (u8)w; hdr[19] = (u8)(w >> 8);
    hdr[22] = (u8)h; hdr[23] = (u8)(h >> 8);
    hdr[26] = 1; hdr[28] = 24;
    fwrite(hdr, 1, 54, f);
    u8 line[TEX_W * 3 + 4] = {0};
    for (u32 y = 0; y < h; y++) {
        const u32* src = g_pixels + (h - 1 - y) * w;
        for (u32 x = 0; x < w; x++) {
            line[x * 3] = (u8)src[x];
            line[x * 3 + 1] = (u8)(src[x] >> 8);
            line[x * 3 + 2] = (u8)(src[x] >> 16);
        }
        fwrite(line, 1, row, f);
    }
    fclose(f);
    printf("window: screenshot saved to %s\n", path);
}

/* ------------------------------------------------------------- input --- */
static u8 axis_to_gc(Sint16 v, bool invert) {
    int x = (int)v * 100 / 32767;      /* a real stick reaches about +/-100 */
    if (x > -8 && x < 8) x = 0;        /* small dead zone */
    if (invert) x = -x;
    int r = 128 + x;
    return (u8)(r < 0 ? 0 : r > 255 ? 255 : r);
}

static bool g_text_search;
static bool g_shot_pending;            /* GPU mode: save the next picture shown */

bool window_take_text_search(void) {
    bool r = g_text_search;
    g_text_search = false;
    return r;
}

/* ------------------------------------------------------- free camera --- */
/* F7: fly the camera around the 3D scene (debug, like Dolphin's free look).
   While it is on, keys and the controller move the camera instead of
   playing: W/S forward/back, A/D sideways, Q/E down/up, arrows or the mouse
   with the right button held to look around, Shift faster, mouse wheel to
   change the speed, R back to the game's camera. */
static bool g_freecam;
static float g_cam_pos[3], g_cam_yaw, g_cam_pitch, g_cam_speed = 0.5f;
static int g_cam_wheel;

static void freecam_title(void) {
    if (g_freecam)
        snprintf(g_title_cam, sizeof(g_title_cam), "  -  FREE CAMERA (F7), speed %.3g", g_cam_speed);
    else
        g_title_cam[0] = 0;
    set_title();
}

static void freecam_toggle(void) {
    g_freecam = !g_freecam;
    if (!g_freecam)
        gx_raster_set_free_camera(false, NULL);
    printf("window: free camera %s\n", g_freecam ? "on (W/S/A/D/Q/E move, arrows or right mouse look, R reset, F7 off)"
                                                  : "off");
    freecam_title();
}

static void freecam_update(void) {
    const bool* k = SDL_GetKeyboardState(NULL);
    float dx = 0, dy = 0;
    SDL_MouseButtonFlags b = SDL_GetRelativeMouseState(&dx, &dy);
    if (g_cam_wheel) {
        g_cam_speed *= powf(1.25f, (float)g_cam_wheel);
        if (g_cam_speed < 0.001f) g_cam_speed = 0.001f;
        if (g_cam_speed > 1000.0f) g_cam_speed = 1000.0f;
        g_cam_wheel = 0;
        freecam_title();
    }
    if (k[SDL_SCANCODE_R]) {
        g_cam_pos[0] = g_cam_pos[1] = g_cam_pos[2] = 0;
        g_cam_yaw = g_cam_pitch = 0;
    }
    if (b & SDL_BUTTON_RMASK) {
        g_cam_yaw -= dx * 0.004f;
        g_cam_pitch -= dy * 0.004f;
    }
    if (k[SDL_SCANCODE_LEFT]) g_cam_yaw += 0.03f;
    if (k[SDL_SCANCODE_RIGHT]) g_cam_yaw -= 0.03f;
    if (k[SDL_SCANCODE_UP]) g_cam_pitch += 0.03f;
    if (k[SDL_SCANCODE_DOWN]) g_cam_pitch -= 0.03f;
    if (g_cam_pitch > 1.55f) g_cam_pitch = 1.55f;
    if (g_cam_pitch < -1.55f) g_cam_pitch = -1.55f;

    /* Orientation in the game camera's view space (looking down -z, y up). */
    float cy = cosf(g_cam_yaw), sy = sinf(g_cam_yaw), cp = cosf(g_cam_pitch), sp = sinf(g_cam_pitch);
    float R[3][3] = {{cy, sy * sp, sy * cp}, {0, cp, -sp}, {-sy, cy * sp, cy * cp}};
    float fwd[3] = {-R[0][2], -R[1][2], -R[2][2]}, right[3] = {R[0][0], R[1][0], R[2][0]};
    float v = g_cam_speed * ((k[SDL_SCANCODE_LSHIFT] || k[SDL_SCANCODE_RSHIFT]) ? 5.0f : 1.0f);
    for (int i = 0; i < 3; i++) {
        if (k[SDL_SCANCODE_W]) g_cam_pos[i] += fwd[i] * v;
        if (k[SDL_SCANCODE_S]) g_cam_pos[i] -= fwd[i] * v;
        if (k[SDL_SCANCODE_D]) g_cam_pos[i] += right[i] * v;
        if (k[SDL_SCANCODE_A]) g_cam_pos[i] -= right[i] * v;
    }
    if (k[SDL_SCANCODE_E]) g_cam_pos[1] += v;
    if (k[SDL_SCANCODE_Q]) g_cam_pos[1] -= v;

    /* view = R^T (p - pos) */
    float m[12];
    for (int r = 0; r < 3; r++) {
        m[r * 4 + 0] = R[0][r];
        m[r * 4 + 1] = R[1][r];
        m[r * 4 + 2] = R[2][r];
        m[r * 4 + 3] = -(R[0][r] * g_cam_pos[0] + R[1][r] * g_cam_pos[1] + R[2][r] * g_cam_pos[2]);
    }
    gx_raster_set_free_camera(true, m);
}

/* ------------------------------------------------------- RAM search --- */
/* F6: finds where in memory the game keeps a setting. Press F6, change the
   setting in the game's menu, press F6 again, change it back, F6, and so on:
   each press keeps only the bytes that switch between the same two values in
   step with the setting. Ctrl+F6 without changing the setting keeps only the
   bytes that stayed the same (drops things that just animate). Shift+F6
   starts over. Results go to the log ("ramsearch:" lines). */
static CPUState* g_cpu_ref;
static u8 *g_rs_a, *g_rs_b, *g_rs_cand;
static int g_rs_step, g_rs_state;      /* g_rs_state: 0 = setting as in snapshot A, 1 = as in B */

static void ram_search(bool restart, bool same) {
    CPUState* cpu = g_cpu_ref;
    if (!cpu || !cpu->ram) return;
    u32 n = cpu->ram_size;
    if (restart || !g_rs_a) {
        free(g_rs_a); free(g_rs_b); free(g_rs_cand);
        g_rs_a = (u8*)malloc(n);
        g_rs_b = (u8*)malloc(n);
        g_rs_cand = (u8*)malloc(n);
        g_rs_step = g_rs_state = 0;
        if (!g_rs_a || !g_rs_b || !g_rs_cand) return;
        if (restart) { printf("ramsearch: started over\n"); return; }
    }
    if (same && g_rs_step < 2) {
        printf("ramsearch: Ctrl+F6 works after the first two snapshots\n");
        return;
    }
    const u8* ram = cpu->ram;
    u32 count = 0;
    if (g_rs_step == 0) {
        memcpy(g_rs_a, ram, n);
        printf("ramsearch: snapshot 1 taken; change the setting, then press F6 again\n");
    } else if (g_rs_step == 1) {
        memcpy(g_rs_b, ram, n);
        g_rs_state = 1;
        for (u32 i = 0; i < n; i++) count += (g_rs_cand[i] = g_rs_a[i] != g_rs_b[i]);
    } else {
        if (!same) g_rs_state ^= 1;
        const u8* want = g_rs_state ? g_rs_b : g_rs_a;
        for (u32 i = 0; i < n; i++) {
            if (g_rs_cand[i] && ram[i] != want[i]) g_rs_cand[i] = 0;
            count += g_rs_cand[i];
        }
    }
    if (g_rs_step > 0) {
        printf("ramsearch: snapshot %d (%s, setting as in snapshot %c): %u bytes still match\n", g_rs_step + 1,
               same ? "unchanged" : "changed", g_rs_state ? 'B' : 'A', count);
        if (count <= 300) {
            /* Likely flags first: both values small. */
            for (int pass = 0; pass < 2; pass++)
                for (u32 i = 0; i < n; i++)
                    if (g_rs_cand[i] && ((g_rs_a[i] <= 3 && g_rs_b[i] <= 3) == (pass == 0)))
                        printf("ramsearch:   0x%08X: %u <-> %u\n", 0x80000000u | i, g_rs_a[i], g_rs_b[i]);
        }
    }
    g_rs_step++;
}

/* The game's own screen option ("option 6" of its option record, kept in
   the record at 0x8030BAAC, 16 bytes per option: value, table tag, column
   tag): 0 = 4:3, 1 = letterbox, 2 = 16:9. With the screen shape fixed to 4:3
   or 16:9 in the port's menu, the game's option is kept at the same choice,
   so the game draws for the screen the port shows. The game reads it when it
   sets up a screen, so some parts follow from the next screen or match. */
#define GAME_OPT_SCREEN 0x0030BB0Cu

static u32 be32(const u8* p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }

static void sync_game_screen_option(CPUState* cpu) {
    if (!cpu || !cpu->ram || GAME_OPT_SCREEN + 12 > cpu->ram_size || g_settings.screen_shape == 0)
        return;
    u8* p = cpu->ram + GAME_OPT_SCREEN;
    if (be32(p + 4) != 0x5354504Fu || be32(p + 8) != 0x5349444Fu)
        return;                        /* the record isn't set up yet */
    u32 want = g_settings.screen_shape == 2 ? 2u : 0u;
    u32 have = be32(p);
    if (have == want)
        return;
    p[0] = p[1] = p[2] = 0;
    p[3] = (u8)want;
    printf("window: game's screen option set to %s (was %u)\n", want ? "16:9" : "4:3", have);
}

/* ------------------------------------------------------ settings menu --- */
static void set_window_size(void) {
    if (g_fullscreen) return;
    if (SDL_GetWindowFlags(g_window) & SDL_WINDOW_MAXIMIZED) SDL_RestoreWindow(g_window);
    int s = g_settings.window_size;
    SDL_SetWindowSize(g_window, (int)(480.0f * (float)s * g_aspect + 0.5f), 480 * s);
    SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
}

static void apply_settings(unsigned ch) {
    if (ch & (1u << MI_SHAPE)) {
        g_aspect_mode = g_settings.screen_shape;
        gx_raster_set_target_shape(g_aspect_mode);
        update_aspect(true);
    }
    if (ch & (1u << MI_DISPLAY)) {
        g_fullscreen = g_settings.fullscreen != 0;
        SDL_SetWindowFullscreen(g_window, g_fullscreen);
    }
    if (ch & (1u << MI_WINSIZE))
        set_window_size();
    if (ch & (1u << MI_TEXTURES)) {
        /* Turning them on picks up files added, changed or removed since. */
        if (g_settings.custom_textures && !hires_enabled())
            gx_tex_reload_hires();
        hires_set_enabled(g_settings.custom_textures != 0);
    }
}

static void open_menu(void) {
    MenuInfo mi = {g_gpu, g_gpu_scale, hires_has_folder()};
    menu_open(&mi);
}

/* Keys and controller buttons while the menu is open. */
static void menu_event(const SDL_Event* e) {
    static int axis_dir[2];
    if (e->type == SDL_EVENT_KEY_DOWN) {
        switch (e->key.key) {
        case SDLK_UP: menu_action(MENU_UP); break;
        case SDLK_DOWN: menu_action(MENU_DOWN); break;
        case SDLK_LEFT: menu_action(MENU_LEFT); break;
        case SDLK_RIGHT: menu_action(MENU_RIGHT); break;
        case SDLK_RETURN: case SDLK_SPACE: case SDLK_X: if (!e->key.repeat) menu_action(MENU_OK); break;
        case SDLK_ESCAPE: case SDLK_F1: case SDLK_BACKSPACE: case SDLK_Z:
            if (!e->key.repeat) menu_action(MENU_BACK);
            break;
        default: break;
        }
    } else if (e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        switch (e->gbutton.button) {
        case SDL_GAMEPAD_BUTTON_DPAD_UP: menu_action(MENU_UP); break;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN: menu_action(MENU_DOWN); break;
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT: menu_action(MENU_LEFT); break;
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: menu_action(MENU_RIGHT); break;
        case SDL_GAMEPAD_BUTTON_SOUTH: menu_action(MENU_OK); break;
        case SDL_GAMEPAD_BUTTON_EAST: case SDL_GAMEPAD_BUTTON_BACK: case SDL_GAMEPAD_BUTTON_START:
            menu_action(MENU_BACK);
            break;
        default: break;
        }
    } else if (e->type == SDL_EVENT_GAMEPAD_AXIS_MOTION &&
               (e->gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX || e->gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY)) {
        int i = e->gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY;
        int d = e->gaxis.value > 20000 ? 1 : e->gaxis.value < -20000 ? -1 : 0;
        if (d && d != axis_dir[i])
            menu_action(i ? (d < 0 ? MENU_UP : MENU_DOWN) : (d < 0 ? MENU_LEFT : MENU_RIGHT));
        axis_dir[i] = d;
    }
}

static void poll_events(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (menu_is_open() && (e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ||
                               e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION)) {
            menu_event(&e);
            continue;
        }
        switch (e.type) {
        case SDL_EVENT_QUIT:
            g_quit = true;
            break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            if (e.gbutton.button == SDL_GAMEPAD_BUTTON_BACK) open_menu();
            break;
        case SDL_EVENT_KEY_DOWN:
            if (e.key.key == SDLK_F1 && !e.key.repeat) {
                open_menu();
            } else if (e.key.key == SDLK_F6 && !e.key.repeat) {
                ram_search((e.key.mod & SDL_KMOD_SHIFT) != 0, (e.key.mod & SDL_KMOD_CTRL) != 0);
            } else if (e.key.key == SDLK_ESCAPE) {
                g_quit = true;
            } else if (e.key.key == SDLK_F7 && !e.key.repeat) {
                freecam_toggle();
            } else if (e.key.key == SDLK_F8 && !e.key.repeat) {
                g_text_search = true;
            } else if (e.key.key == SDLK_F10 && !e.key.repeat) {
                if (g_gpu) g_shot_pending = true;
                else save_screenshot();
            } else if (e.key.key == SDLK_F12 && !e.key.repeat) {
                gx_raster_draw_log(interrupts_game_seconds(), 2);
                printf("window: logging the next two frames to drawlog.txt and drawlog-short.txt\n");
            } else if (e.key.key == SDLK_F9 && !e.key.repeat) {
                static const int k_next[] = {1, 2, 0};   /* auto -> 4:3 -> 16:9 -> auto */
                g_aspect_mode = k_next[g_aspect_mode];
                gx_raster_set_target_shape(g_aspect_mode);
                update_aspect(true);
                g_settings.screen_shape = g_aspect_mode;
                settings_save();
            } else if (e.key.key == SDLK_F11 && !e.key.repeat) {
                g_fullscreen = !g_fullscreen;
                SDL_SetWindowFullscreen(g_window, g_fullscreen);
                g_settings.fullscreen = g_fullscreen;
                settings_save();
            }
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            if (g_freecam) g_cam_wheel += e.wheel.y > 0 ? 1 : e.wheel.y < 0 ? -1 : 0;
            break;
        case SDL_EVENT_GAMEPAD_ADDED:
            if (!g_pad) {
                g_pad = SDL_OpenGamepad(e.gdevice.which);
                if (g_pad)
                    printf("window: controller connected: %s\n", SDL_GetGamepadName(g_pad));
            }
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            if (g_pad && SDL_GetGamepadID(g_pad) == e.gdevice.which) {
                SDL_CloseGamepad(g_pad);
                g_pad = NULL;
                printf("window: controller disconnected\n");
            }
            break;
        default:
            break;
        }
    }
}

/* After the menu closes, the key or button that closed it is usually still
   held: the game sees nothing until everything has been let go. */
static bool g_input_block;

static void read_input(void) {
    if (g_freecam) {                   /* the keys fly the camera; the game sees nothing pressed */
        PadLive idle = {0, 0x80, 0x80, 0x80, 0x80, 0, 0};
        pad_set_live(&idle);
        freecam_update();
        return;
    }
    PadLive p = {0, 0x80, 0x80, 0x80, 0x80, 0, 0};
    const bool* k = SDL_GetKeyboardState(NULL);

    if (k[SDL_SCANCODE_X]) p.buttons |= PAD_A;
    if (k[SDL_SCANCODE_Z]) p.buttons |= PAD_B;
    if (k[SDL_SCANCODE_C]) p.buttons |= PAD_X;
    if (k[SDL_SCANCODE_S]) p.buttons |= PAD_Y;
    if (k[SDL_SCANCODE_D]) p.buttons |= PAD_Z;
    if (k[SDL_SCANCODE_RETURN]) p.buttons |= PAD_START;
    if (k[SDL_SCANCODE_Q]) { p.buttons |= PAD_L; p.trigger_l = 0xFF; }
    if (k[SDL_SCANCODE_W]) { p.buttons |= PAD_R; p.trigger_r = 0xFF; }
    if (k[SDL_SCANCODE_T]) p.buttons |= PAD_UP;
    if (k[SDL_SCANCODE_G]) p.buttons |= PAD_DOWN;
    if (k[SDL_SCANCODE_F]) p.buttons |= PAD_LEFT;
    if (k[SDL_SCANCODE_H]) p.buttons |= PAD_RIGHT;
    if (k[SDL_SCANCODE_LEFT]) p.stick_x = 0x80 - 100;
    if (k[SDL_SCANCODE_RIGHT]) p.stick_x = 0x80 + 100;
    if (k[SDL_SCANCODE_UP]) p.stick_y = 0x80 + 100;
    if (k[SDL_SCANCODE_DOWN]) p.stick_y = 0x80 - 100;
    if (k[SDL_SCANCODE_J]) p.cstick_x = 0x80 - 100;
    if (k[SDL_SCANCODE_L]) p.cstick_x = 0x80 + 100;
    if (k[SDL_SCANCODE_I]) p.cstick_y = 0x80 + 100;
    if (k[SDL_SCANCODE_K]) p.cstick_y = 0x80 - 100;

    if (g_pad) {
        if (SDL_GetGamepadButton(g_pad, SDL_GAMEPAD_BUTTON_SOUTH)) p.buttons |= PAD_A;
        if (SDL_GetGamepadButton(g_pad, SDL_GAMEPAD_BUTTON_EAST)) p.buttons |= PAD_B;
        if (SDL_GetGamepadButton(g_pad, SDL_GAMEPAD_BUTTON_WEST)) p.buttons |= PAD_X;
        if (SDL_GetGamepadButton(g_pad, SDL_GAMEPAD_BUTTON_NORTH)) p.buttons |= PAD_Y;
        if (SDL_GetGamepadButton(g_pad, SDL_GAMEPAD_BUTTON_START)) p.buttons |= PAD_START;
        if (SDL_GetGamepadButton(g_pad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) p.buttons |= PAD_Z;
        if (SDL_GetGamepadButton(g_pad, SDL_GAMEPAD_BUTTON_DPAD_UP)) p.buttons |= PAD_UP;
        if (SDL_GetGamepadButton(g_pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN)) p.buttons |= PAD_DOWN;
        if (SDL_GetGamepadButton(g_pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT)) p.buttons |= PAD_LEFT;
        if (SDL_GetGamepadButton(g_pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) p.buttons |= PAD_RIGHT;
        Sint16 lx = SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_LEFTX);
        Sint16 ly = SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_LEFTY);
        Sint16 rx = SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_RIGHTX);
        Sint16 ry = SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_RIGHTY);
        if (p.stick_x == 0x80) p.stick_x = axis_to_gc(lx, false);
        if (p.stick_y == 0x80) p.stick_y = axis_to_gc(ly, true);     /* SDL: down is positive */
        if (p.cstick_x == 0x80) p.cstick_x = axis_to_gc(rx, false);
        if (p.cstick_y == 0x80) p.cstick_y = axis_to_gc(ry, true);
        Sint16 lt = SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
        Sint16 rt = SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
        u8 l = (u8)(lt > 0 ? lt * 255 / 32767 : 0), r = (u8)(rt > 0 ? rt * 255 / 32767 : 0);
        if (l > p.trigger_l) p.trigger_l = l;
        if (r > p.trigger_r) p.trigger_r = r;
        if (l > 230) p.buttons |= PAD_L;  /* the digital click at the end of the travel */
        if (r > 230) p.buttons |= PAD_R;
    }
    if (g_input_block) {
        bool held = p.buttons || p.trigger_l > 30 || p.trigger_r > 30 || p.stick_x < 0x80 - 30 ||
                    p.stick_x > 0x80 + 30 || p.stick_y < 0x80 - 30 || p.stick_y > 0x80 + 30 ||
                    p.cstick_x < 0x80 - 30 || p.cstick_x > 0x80 + 30 || p.cstick_y < 0x80 - 30 ||
                    p.cstick_y > 0x80 + 30;
        if (held) {
            PadLive idle = {0, 0x80, 0x80, 0x80, 0x80, 0, 0};
            p = idle;
        } else {
            g_input_block = false;
        }
    }
    pad_set_live(&p);
}

/* ------------------------------------------------------------ output --- */
static void present_gpu(CPUState* cpu) {
    update_aspect(false);
    int ww = 0, wh = 0;
    SDL_GetWindowSizeInPixels(g_window, &ww, &wh);
    if (ww <= 0 || wh <= 0)
        return;                        /* minimised */
    char path[64];
    const char* shot = NULL;
    if (g_shot_pending) {
        static int n;
        snprintf(path, sizeof(path), "shot_%03d.bmp", ++n);
        shot = path;
        g_shot_pending = false;
    }
    gx_raster_present_gpu(cpu, mmio_reg(0x201C, 4), mmio_reg(0x2048, 2), mmio_reg(0x2000, 2), ww, wh, g_aspect,
                          menu_is_open() ? NULL : shot);
    if (menu_is_open()) {
        bool dirty;
        const uint32_t* px = menu_draw(ww, wh, &dirty);
        glr_present_overlay(px, ww, wh, dirty);
        if (shot) g_shot_pending = true;   /* screenshots are of the game, not the menu */
    }
    SDL_GL_SwapWindow(g_window);
}

/* --------------------------------------------------- replacement movies --- */
/* Which frame the game's own movie is at. The game's player writes its
   frames straight into the TV picture in RAM, in step with the TV, so: the
   first change of that
   picture is the movie's first visible frame, and from there the frame
   number follows the game's clock: one frame every two fields (29.97 a
   second; the NFL Street 2 intro's 2,132 visible frames take 71.2 s). */
static long g_movie_at = -1;
static u64 g_movie_last_hash;
static double g_movie_t_first;
static long g_movie_first;
static bool g_movie_have_base;

static u64 xfb_hash(CPUState* cpu) {
    static u32 px[720 * 576];
    u32 w = 0, h = 0;
    if (!gx_raster_xfb_rgb(cpu, mmio_reg(0x201C, 4), mmio_reg(0x2048, 2), mmio_reg(0x2000, 2), px, 720, 576, &w, &h))
        return 0;
    u64 v = 1469598103934665603ull;
    for (u32 i = 0; i < w * h; i += 7) v = (v ^ px[i]) * 1099511628211ull;
    return v;
}

/* How far behind the picture the sound is heard: what's queued for the sound
   card, plus the card's own buffer (about 20 ms). */
static double audio_delay(void) {
    if (!g_audio || !g_audio_rate) return 0.0;
    return (double)SDL_GetAudioStreamQueued(g_audio) / (g_audio_rate * 4.0) + 0.02;
}

static void movie_track(CPUState* cpu) {
    double now = interrupts_game_seconds();
    if (g_movie_at >= 0) {
        /* The game's picture is shown at once but its sound is heard a
           little later; show the replacement frame that goes with what's
           being heard. */
        now -= audio_delay();
        long at = g_movie_first + (long)((now - g_movie_t_first) * (30000.0 / 1001.0) + 0.25);
        g_movie_at = at > g_movie_first ? at : g_movie_first;
        return;
    }
    u64 hsh = xfb_hash(cpu);
    if (!g_movie_have_base) {          /* the picture before the movie's first frame */
        g_movie_last_hash = hsh;
        g_movie_have_base = true;
        return;
    }
    if (hsh != g_movie_last_hash) {
        g_movie_first = movie_first_visible();
        g_movie_t_first = now;
        g_movie_at = g_movie_first;
    }
}

void window_movie_begin(int index) {
    const char* dir = disc_replace_dir();
    if (!dir || !*dir) return;
    char path[1200];
    snprintf(path, sizeof(path), "%s/moviedat_%d.mpg", dir, index);
    FILE* f = fopen(path, "rb");
    if (!f) { movie_close(); return; }
    fclose(f);
    if (movie_open(path)) {
        g_movie_at = -1;
        g_movie_have_base = false;
    }
}

void window_movie_end(void) {
    if (movie_is_open())
        printf("movie: ended at frame %ld, %.2f s after its first visible frame\n", g_movie_at,
               g_movie_at >= 0 ? interrupts_game_seconds() - g_movie_t_first : 0.0);
    movie_close();
}

/* The replacement movie's frame instead of the game's picture; false when
   there's none (or it has run out). */
static bool present_movie(CPUState* cpu) {
    if (!movie_is_open() || !g_gpu)
        return false;
    movie_track(cpu);
    long at = g_movie_at;
    if (at < 0)
        return false;
    u32 w, h;
    const u32* px = movie_frame(at, &w, &h);
    if (!px) {
        movie_close();
        return false;
    }
    int ww = 0, wh = 0;
    SDL_GetWindowSizeInPixels(g_window, &ww, &wh);
    if (ww <= 0 || wh <= 0) return true;
    update_aspect(false);
    gx_raster_present_pixels(px, w, h, ww, wh, g_aspect);   /* the shape the game shows its own movie at */
    if (menu_is_open()) {
        bool dirty;
        const uint32_t* mp = menu_draw(ww, wh, &dirty);
        glr_present_overlay(mp, ww, wh, dirty);
    }
    SDL_GL_SwapWindow(g_window);
    return true;
}

/* A black window: shown while the THX intro is skipped, so the cut from one
   intro movie to the next looks like one instead of a frozen picture. */
static void present_black(void) {
    if (g_gpu) {
        int ww = 0, wh = 0;
        SDL_GetWindowSizeInPixels(g_window, &ww, &wh);
        glr_present_black(ww, wh);
        SDL_GL_SwapWindow(g_window);
        return;
    }
    SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);
    SDL_RenderClear(g_renderer);
    SDL_RenderPresent(g_renderer);
}

static void present(CPUState* cpu) {
    if (present_movie(cpu))
        return;
    if (g_gpu) {
        present_gpu(cpu);
        return;
    }
    u32 w, h;
    if (gx_raster_xfb_rgb(cpu, mmio_reg(0x201C, 4), mmio_reg(0x2048, 2), mmio_reg(0x2000, 2), g_pixels, TEX_W,
                          TEX_H, &w, &h)) {
        SDL_Rect r = {0, 0, (int)w, (int)h};
        SDL_UpdateTexture(g_texture, &r, g_pixels, (int)(w * 4));
        /* Smooth scaling blends each pixel with its neighbours, so along the
           bottom and right edges it also reads the texture just outside the
           picture, which was never written (the strip of colour noise along
           the bottom). Repeat the last row and column there. */
        static u32 edge[TEX_W > TEX_H ? TEX_W + 1 : TEX_H + 1];
        if (h < TEX_H) {
            memcpy(edge, g_pixels + (size_t)(h - 1) * w, w * 4);
            edge[w] = edge[w - 1];
            SDL_Rect row = {0, (int)h, (int)(w < TEX_W ? w + 1 : w), 1};
            SDL_UpdateTexture(g_texture, &row, edge, (int)(TEX_W * 4));
        }
        if (w < TEX_W) {
            for (u32 y = 0; y < h; y++) edge[y] = g_pixels[(size_t)y * w + w - 1];
            SDL_Rect col = {(int)w, 0, 1, (int)h};
            SDL_UpdateTexture(g_texture, &col, edge, 4);
        }
        if (w != g_shown_w || h != g_shown_h) {
            g_shown_w = w;
            g_shown_h = h;
        }
    }
    SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);
    SDL_RenderClear(g_renderer);

    /* Scale to fit the window, keeping the TV shape: 4:3, or 16:9 when the
       game draws for a widescreen TV (the picture is stretched sideways). */
    update_aspect(false);
    int ww, wh;
    SDL_GetRenderOutputSize(g_renderer, &ww, &wh);
    float aspect = g_aspect;
    float dw = (float)ww, dh = (float)ww / aspect;
    if (dh > (float)wh) { dh = (float)wh; dw = dh * aspect; }
    SDL_FRect src = {0, 0, (float)g_shown_w, (float)g_shown_h};
    SDL_FRect dst = {((float)ww - dw) * 0.5f, ((float)wh - dh) * 0.5f, dw, dh};
    SDL_RenderTexture(g_renderer, g_texture, &src, &dst);
    if (menu_is_open()) {
        static SDL_Texture* overlay;
        static int ow, oh;
        bool dirty;
        const uint32_t* px = menu_draw(ww, wh, &dirty);
        if (px && (!overlay || ow != ww || oh != wh)) {
            if (overlay) SDL_DestroyTexture(overlay);
            overlay = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, ww, wh);
            if (overlay) SDL_SetTextureBlendMode(overlay, SDL_BLENDMODE_BLEND_PREMULTIPLIED);
            ow = ww;
            oh = wh;
            dirty = true;
        }
        if (px && overlay) {
            if (dirty) SDL_UpdateTexture(overlay, NULL, px, ww * 4);
            SDL_RenderTexture(g_renderer, overlay, NULL, NULL);
        }
    }
    SDL_RenderPresent(g_renderer);
}

/* A field that took much longer than its 16.7 ms (the game stalled: a
   loading screen, a scene change) is logged with where the time went. */
static Uint64 g_hitch_last;           /* 0: start measuring afresh (after the menu) */

static void log_hitch(Uint64 now) {
    static Uint64 last_present;
    Uint64 took = g_hitch_last ? now - g_hitch_last : 0;
    Uint64 shown = g_present_ns >= last_present ? g_present_ns - last_present : g_present_ns;
    g_hitch_last = now;
    last_present = g_present_ns;
    if (took > 50000000ull && !turbo() && g_field > 120)
        printf("hitch: %5.1f s game time, one field took %3.0f ms (showing the picture %2.0f ms)\n",
               interrupts_game_seconds(), (double)took * 1e-6, (double)shown * 1e-6);
}

static void pace_and_stats(void) {
    Uint64 now = SDL_GetTicksNS();
    log_hitch(now);
    double target = (double)g_base_ns + (double)(g_field - g_base_field) * FIELD_NS;
    if (turbo()) {
        /* Fast-forward: run flat out, and restart the clock so normal speed
           resumes smoothly afterwards instead of trying to catch up. */
        g_base_ns = now;
        g_base_field = g_field;
    } else if ((double)now < target) {
        int prev = prof_enter(PROF_SLEEP);
        SDL_DelayNS((Uint64)(target - (double)now));
        prof_leave(prev);
        g_slept_ns += SDL_GetTicksNS() - now;
    } else if ((double)now > target + 100000000.0) {
        /* More than 0.1 s behind: don't try to catch up, just carry on from now. */
        g_base_ns = now;
        g_base_field = g_field;
    }

    g_stat_fields++;
    now = SDL_GetTicksNS();
    if (now - g_stat_ns >= 1000000000ull) {
        double secs = (double)(now - g_stat_ns) * 1e-9;
        u32 frames = gx_raster_frames();
        double fps = (double)(frames - g_frames_at_stat) / secs;
        double speed = (double)g_stat_fields * (FIELD_NS * 1e-9) / secs * 100.0;
        snprintf(g_title_stats, sizeof(g_title_stats), "%.0f fps - %.0f%% speed", fps, speed);
        set_title();
        /* When the game falls behind real time, log where the second went. */
        double wait, draw_avg, draw_max;
        u32 tris;
        gx_raster_take_perf(&wait, &draw_avg, &draw_max, &tris);
        unsigned samples[PROF_COUNT];
        unsigned total = prof_take(samples);
        if (speed < 95.0) {
            double slept = (double)g_slept_ns * 1e-9, shown = (double)g_present_ns * 1e-9;
            double game = secs - slept - shown - wait;
            printf("perf: %5.1f s game time, %3.0f%% speed, %2.0f fps | per second: game code %3.0f ms, waiting for "
                   "renderer %3.0f ms, showing %2.0f ms | frame draw avg %4.1f ms, worst %4.1f ms, %u triangles/frame\n",
                   interrupts_game_seconds(), speed, fps, game / secs * 1000.0, wait / secs * 1000.0,
                   shown / secs * 1000.0, draw_avg * 1000.0, draw_max * 1000.0, tris);
            if (total) {
                printf("perf:   game thread:");
                for (int c = 0; c < PROF_COUNT; c++)
                    if (samples[c] * 100u >= total)
                        printf(" %s %u%%", prof_name(c), samples[c] * 100u / total);
                printf("\n");
            }
        }
        if (g_audio_sec_underruns || g_audio_sec_dropped)
            printf("audio: %5.1f s game time, %u buffers this second, ran dry %u times, dropped %u, lowest queue %.1f ms\n",
                   interrupts_game_seconds(), g_audio_sec_buffers, g_audio_sec_underruns, g_audio_sec_dropped,
                   (double)g_audio_sec_min_queue / (g_audio_rate * 4.0 / 1000.0));
        g_audio_sec_underruns = g_audio_sec_buffers = g_audio_sec_dropped = 0;
        g_audio_sec_min_queue = 1 << 30;
        g_slept_ns = g_present_ns = 0;
        g_stat_ns = now;
        g_stat_fields = 0;
        g_frames_at_stat = frames;
    }
}

/* The game stands still while the menu is open: the last picture stays up
   under it, the sound pauses, and real-time pacing restarts afterwards. */
static void menu_loop(CPUState* cpu) {
    printf("window: video menu open\n");
    if (g_audio) SDL_PauseAudioStreamDevice(g_audio);
    PadLive idle = {0, 0x80, 0x80, 0x80, 0x80, 0, 0};
    pad_set_live(&idle);
    while (menu_is_open() && !g_quit) {
        poll_events();
        apply_settings(menu_take_changes());
        present(cpu);
        SDL_Delay(15);
    }
    apply_settings(menu_take_changes());
    settings_save();
    g_input_block = true;
    if (g_audio) SDL_ResumeAudioStreamDevice(g_audio);
    Uint64 now = SDL_GetTicksNS();
    g_base_ns = g_stat_ns = now;
    g_base_field = g_field;
    g_stat_fields = 0;
    g_frames_at_stat = gx_raster_frames();
    g_slept_ns = g_present_ns = 0;
    g_hitch_last = 0;
    printf("window: video menu closed\n");
}

void window_field(CPUState* cpu) {
    g_field++;
    g_cpu_ref = cpu;
    poll_events();
    sync_game_screen_option(cpu);
    if (menu_is_open()) {
        menu_loop(cpu);
        return;
    }
    read_input();
    const bool* keys = SDL_GetKeyboardState(NULL);
    g_turbo_key = keys && keys[SDL_SCANCODE_TAB];
    Uint64 t0 = SDL_GetTicksNS();
    int prev = prof_enter(PROF_PRESENT);
    /* Fast-forward: show one field in eight; while skipping the THX intro,
       a black window. */
    if (g_turbo_forced) {
        if ((g_field & 7u) == 0)
            present_black();
    } else if (!turbo() || (g_field & 7u) == 0) {
        present(cpu);
    }
    prof_leave(prev);
    g_present_ns += SDL_GetTicksNS() - t0;
    pace_and_stats();
}
