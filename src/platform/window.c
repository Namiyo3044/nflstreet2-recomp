#include "runtime/netplay.h"
#include "runtime/prof.h"
#include "platform/window.h"
#include "gx/raster.h"
#include "gx/raster_gl.h"
#include "runtime/interrupts.h"
#include "runtime/mmio.h"
#include "runtime/pad.h"
#include "platform/discord.h"
#include "platform/settings.h"
#include "platform/menu.h"
#include "gx/hires.h"
#include "gx/texture.h"
#include "platform/movie.h"
#include "runtime/disc.h"
#include "runtime/uicolors.h"
#include "runtime/uismenu.h"
#include "runtime/lodfix.h"
#include "apt/ps2online.h"

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
/* Controllers, in the order they were connected: controller N plays as
   player N (the keyboard joins whichever player the settings say). A
   controller unplugged leaves its slot empty, so the others keep theirs. */
#define DISCORD_APP_ID "1566925276686852116"   /* the port's Discord application (Rich Presence) */
static SDL_Gamepad* g_pads[PAD_PORTS];
/* A player whose controller was unplugged shows as unplugged in the game
   (its "controller disconnected" screen), even if the keyboard also plays as
   that player, until a controller is connected again or a key is pressed. */
static bool g_lost[PAD_PORTS];
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
static bool g_card_focus;               /* Save/Load's Delete User ID row has the cursor */
/* Skipping all the intro movies: black picture and no sound, at normal speed. */
static bool g_hidden;
static Uint64 g_hidden_since;

void window_set_hidden(bool on) {
    if (on && !g_hidden) {
        g_hidden_since = SDL_GetTicksNS();
        printf("window: hiding the intro movies while they're skipped\n");
    } else if (!on && g_hidden) {
        printf("startup: intro movies skipped in %.1f s\n", (double)(SDL_GetTicksNS() - g_hidden_since) * 1e-9);
    }
    g_hidden = on;
}

void window_set_turbo(bool on) {
    static Uint64 since;
    if (on != g_turbo_forced) {
        if (on) {
            since = SDL_GetTicksNS();
            printf("window: fast-forward on (skipping intro movies)\n");
        } else {
            printf("startup: intro fast-forwarded in %.1f s\n", (double)(SDL_GetTicksNS() - since) * 1e-9);
        }
    }
    g_turbo_forced = on;
}

static bool turbo(void) {
    return g_turbo_forced || g_turbo_key;
}

static unsigned g_audio_underruns, g_audio_sec_underruns, g_audio_sec_buffers, g_audio_sec_dropped;
static int g_audio_sec_min_queue = 1 << 30;

/* --dump-audio: everything sent to the speakers, as a .wav file. */
static FILE* g_wav;
static u32 g_wav_bytes, g_wav_rate = 32000;

static void wav_header(FILE* f, u32 bytes, u32 rate) {
    u8 h[44];
    memcpy(h, "RIFF", 4);
    u32 v = 36 + bytes; memcpy(h + 4, &v, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    v = 16; memcpy(h + 16, &v, 4);
    u16 w = 1; memcpy(h + 20, &w, 2);          /* PCM */
    w = 2; memcpy(h + 22, &w, 2);              /* stereo */
    memcpy(h + 24, &rate, 4);
    v = rate * 4; memcpy(h + 28, &v, 4);
    w = 4; memcpy(h + 32, &w, 2);
    w = 16; memcpy(h + 34, &w, 2);
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &bytes, 4);
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, 44, f);
}

void window_set_audio_dump(const char* path) {
    g_wav = fopen(path, "wb");
    if (!g_wav) { printf("window: couldn't create %s\n", path); return; }
    wav_header(g_wav, 0, g_wav_rate);
    printf("window: recording the sound to %s\n", path);
}

static void audio_sink(const u8* samples, u32 frames, u32 rate) {
    if (!g_audio || !frames || turbo() || g_hidden)
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
        if (g_wav) {
            g_wav_rate = rate;
            fwrite(buf, 4, n, g_wav);
            g_wav_bytes += n * 4;
        }
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
    g_aspect_mode = mode < 0 || mode > 4 ? 0 : mode;
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

/* Screen shape changes are only taken in the game's menus: a match lays
   out its HUD when it starts, so a shape changed mid-match would leave the
   HUD in the old layout (cut off going to 4:3). The menus count as up
   while a menu script (main menu, options screens) has made a call to the
   game in the last few seconds. */
static u64 g_front_end_field;
static bool g_front_end_seen;

void window_note_front_end(void) {
    g_front_end_field = g_field;
    g_front_end_seen = true;
}

static bool in_front_end(void) { return !g_front_end_seen || g_field - g_front_end_field < 180; }

static void update_aspect(bool announce) {
    int detected = g_shape_trusted || g_saved_wide < 0 ? gx_raster_widescreen() : -1;
    if (g_aspect_mode == 0 && detected >= 0 && g_shape_trusted)
        save_shape(detected);
    int game_wide = detected >= 0 ? detected : g_saved_wide == 1;
    int wide = g_aspect_mode >= 2 || (g_aspect_mode == 0 && game_wide == 1);
    float aspect = g_aspect_mode == 4 ? 32.0f / 9.0f : g_aspect_mode == 3 ? 21.0f / 9.0f : wide ? 16.0f / 9.0f : 4.0f / 3.0f;
    const char* shape = g_aspect_mode == 4 ? "32:9" : g_aspect_mode == 3 ? "21:9" : wide ? "16:9" : "4:3";
    if (aspect == g_aspect && !announce)
        return;
    bool changed = aspect != g_aspect;
    g_aspect = aspect;
    menu_set_aspect(aspect);
    static const char* const k_mode[] = {"auto", "fixed", "fixed", "test", "test"};
    snprintf(g_title_shape, sizeof(g_title_shape), "%s (%s)", shape, k_mode[g_aspect_mode]);
    set_title();
    printf("window: screen shape %s (%s)\n", shape, k_mode[g_aspect_mode]);
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

static void bindings_resolve(void);

bool window_open(int height) {
    bindings_resolve();
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) {
        fprintf(stderr, "error: SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }
    if (height < 240) height = 240;
    load_saved_shape();
    if (g_aspect_mode == 0) {
        /* "The game's setting" is gone from the menus (its own option isn't
           on any screen now): settings saved with it, or --aspect auto, take
           the shape the game was last seen using (16:9 if not known). */
        g_aspect_mode = g_saved_wide == 0 ? 1 : 2;
        g_settings.screen_shape = g_aspect_mode;
        settings_save();
        printf("window: screen shape set to %s (was the game's setting)\n", g_aspect_mode == 2 ? "16:9" : "4:3");
    }
    bool start_wide = g_aspect_mode >= 2 || (g_aspect_mode == 0 && g_saved_wide == 1);
    float start_aspect = g_aspect_mode == 4 ? 32.0f / 9.0f : g_aspect_mode == 3 ? 21.0f / 9.0f : start_wide ? 16.0f / 9.0f : 4.0f / 3.0f;
    int win_h = height, win_w = (int)((float)height * start_aspect + 0.5f);
    if (g_gpu_scale > 0) {
        g_window = SDL_CreateWindow("NFL Street 2", win_w, win_h, SDL_WINDOW_RESIZABLE | SDL_WINDOW_OPENGL);
        if (g_window && gpu_setup()) {
            menu_set_gpu_name(gx_raster_gpu_name());
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
    discord_enable(!g_settings.no_discord);
    discord_start(DISCORD_APP_ID);
    return true;
}

bool window_quit_requested(void) {
    return g_quit;
}

void window_close(void) {
    printf("window: sound card ran dry %u times\n", g_audio_underruns);
    if (g_wav) {
        wav_header(g_wav, g_wav_bytes, g_wav_rate);
        fclose(g_wav);
        g_wav = NULL;
    }
    discord_stop();
    mmio_set_audio_sink(NULL);
    if (g_audio) SDL_DestroyAudioStream(g_audio);
    g_audio = NULL;
    for (int i = 0; i < PAD_PORTS; i++)
        if (g_pads[i]) SDL_CloseGamepad(g_pads[i]);
    if (g_texture) SDL_DestroyTexture(g_texture);
    if (g_renderer) SDL_DestroyRenderer(g_renderer);
    gx_raster_sync();
    gpu_teardown();
    if (g_window) SDL_DestroyWindow(g_window);
    SDL_Quit();
    memset(g_pads, 0, sizeof(g_pads));
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

/* Movies (replacement videos, trailers): the picture's shape, but no wider
   than 16:9 (black at the sides at ultrawide shapes rather than stretched). */
static bool g_game_movie;               /* the game is playing one of its movies */
static float movie_aspect(void) { return g_aspect > 16.0f / 9.0f ? 16.0f / 9.0f : g_aspect; }

/* A new screen shape setting (0 auto, 1 4:3, 2 16:9), from the menu or F9. */
static void set_shape_mode(int mode) {
    g_aspect_mode = mode;
    gx_raster_set_target_shape(mode);
    update_aspect(true);
}

/* The game's own screen option ("option 6" of its option record, kept in
   the record at 0x8030BAAC, 16 bytes per option: value, table tag, column
   tag): 0 = 4:3, 1 = letterbox, 2 = 16:9. With the screen shape fixed to 4:3
   or 16:9 in the port's menu, the game's option is kept at the same choice,
   so the game draws for the screen the port shows. The game reads it when it
   sets up a screen, so some parts follow from the next screen or match. */
#define GAME_OPT_SCREEN 0x0030BB0Cu

static u32 be32(const u8* p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }

/* Ultrawide (test): the game's own 16:9 camera shapes made wider, so it
   draws (and decides what is on screen) for the wider picture itself. The
   16:9 values sit next to the 4:3 ones in two tables: the match camera's
   (0x802A0128: 4:3, 16:9, then a second pair) and the menus' (0x802A83EC).
   Put back to 16:9 for the other shapes. */
static const u32 k_wide_consts[] = {0x002A012Cu, 0x002A0134u, 0x002A83F0u};
#define F32_16_9 0x3FE38E39u

static bool g_wide_camera = true;      /* Shift+F9 (test) */

static void set_game_wide_shape(CPUState* cpu) {
    /* A quarter wider than shown: the game skips what it thinks is outside
       its camera, and close players at the sides went missing a little early
       at very wide shapes. The extra width falls outside the picture (the
       renderer fits the camera to the shape shown). */
    float t = g_settings.screen_shape == 4 ? 32.0f / 9.0f : g_settings.screen_shape == 3 ? 21.0f / 9.0f : 0.0f;
    t *= 1.25f;
    if (!g_wide_camera) t = 0.0f;
    u32 want = F32_16_9;
    if (t > 0.0f) memcpy(&want, &t, 4);
    for (size_t i = 0; i < sizeof k_wide_consts / sizeof k_wide_consts[0]; i++) {
        u32 a = k_wide_consts[i];
        if (a + 4 > cpu->ram_size) return;
        u8* p = cpu->ram + a;
        u32 have = be32(p);
        /* Only where the table is as expected (16:9, or a shape put there). */
        if (have != F32_16_9 && !(have > 0x40000000u && have < 0x40A00000u)) continue;
        if (have == want) continue;
        p[0] = (u8)(want >> 24); p[1] = (u8)(want >> 16); p[2] = (u8)(want >> 8); p[3] = (u8)want;
    }
}

static void sync_game_screen_option(CPUState* cpu) {
    if (cpu && cpu->ram) set_game_wide_shape(cpu);
    if (!cpu || !cpu->ram || GAME_OPT_SCREEN + 12 > cpu->ram_size || g_settings.screen_shape == 0)
        return;
    u8* p = cpu->ram + GAME_OPT_SCREEN;
    if (be32(p + 4) != 0x5354504Fu || be32(p + 8) != 0x5349444Fu)
        return;                        /* the record isn't set up yet */
    u32 want = g_settings.screen_shape >= 2 ? 2u : 0u;   /* 21:9: the game draws for 16:9 */
    u32 have = be32(p);
    if (have == want)
        return;
    p[0] = p[1] = p[2] = 0;
    p[3] = (u8)want;
    printf("window: game's screen option set to %s (was %u)\n", want ? "16:9" : "4:3", have);
}

/* Shift+F8: writes the game's main memory to memdump.bin (debug: lets the
   menus' data be studied as the game holds it). Each press overwrites it. */
static void dump_memory(void) {
    CPUState* cpu = g_cpu_ref;
    if (!cpu || !cpu->ram) return;
    FILE* f = fopen("memdump.bin", "wb");
    if (!f) { printf("window: couldn't write memdump.bin\n"); return; }
    size_t n = fwrite(cpu->ram, 1, cpu->ram_size, f);
    fclose(f);
    printf("window: wrote memdump.bin (%u KB) at %.1f s\n", (unsigned)(n >> 10), interrupts_game_seconds());
}

/* ------------------------------------------------------------ controls --- */
/* Defaults: the keys listed in play.ps1, and a standard controller laid out
   like a GameCube pad (sticks on sticks, L / R on the triggers, Z on the
   right shoulder). Controller codes: button + 1, or 64 + axis * 2 (+1 for
   the positive direction). */
#define PAD_AXIS(a, pos) (64 + (a) * 2 + ((pos) ? 1 : 0))
static const int k_default_keys[GCI_COUNT] = {
    SDL_SCANCODE_X, SDL_SCANCODE_Z, SDL_SCANCODE_C, SDL_SCANCODE_S, SDL_SCANCODE_D, SDL_SCANCODE_RETURN,
    SDL_SCANCODE_Q, SDL_SCANCODE_W,
    SDL_SCANCODE_T, SDL_SCANCODE_G, SDL_SCANCODE_F, SDL_SCANCODE_H,
    SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT,
    SDL_SCANCODE_I, SDL_SCANCODE_K, SDL_SCANCODE_J, SDL_SCANCODE_L,
};
static const int k_default_pads[GCI_COUNT] = {
    SDL_GAMEPAD_BUTTON_SOUTH + 1, SDL_GAMEPAD_BUTTON_EAST + 1, SDL_GAMEPAD_BUTTON_WEST + 1,
    SDL_GAMEPAD_BUTTON_NORTH + 1, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER + 1, SDL_GAMEPAD_BUTTON_START + 1,
    PAD_AXIS(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 1), PAD_AXIS(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 1),
    SDL_GAMEPAD_BUTTON_DPAD_UP + 1, SDL_GAMEPAD_BUTTON_DPAD_DOWN + 1, SDL_GAMEPAD_BUTTON_DPAD_LEFT + 1,
    SDL_GAMEPAD_BUTTON_DPAD_RIGHT + 1,
    PAD_AXIS(SDL_GAMEPAD_AXIS_LEFTY, 0), PAD_AXIS(SDL_GAMEPAD_AXIS_LEFTY, 1),
    PAD_AXIS(SDL_GAMEPAD_AXIS_LEFTX, 0), PAD_AXIS(SDL_GAMEPAD_AXIS_LEFTX, 1),
    PAD_AXIS(SDL_GAMEPAD_AXIS_RIGHTY, 0), PAD_AXIS(SDL_GAMEPAD_AXIS_RIGHTY, 1),
    PAD_AXIS(SDL_GAMEPAD_AXIS_RIGHTX, 0), PAD_AXIS(SDL_GAMEPAD_AXIS_RIGHTX, 1),
};

/* Who plays as whom, for the controls page. */
static void update_players_text(void) {
    char t[256] = "";
    size_t n = 0;
    for (int i = 0; i < PAD_PORTS; i++) {
        bool keys = g_settings.keyboard_player == i;
        if (!g_pads[i] && !keys) continue;
        const char* name = g_pads[i] ? SDL_GetGamepadName(g_pads[i]) : NULL;
        char one[96];
        snprintf(one, sizeof(one), "%sPLAYER %d: %s%s%s", n ? "     " : "", i + 1, keys ? "KEYBOARD" : "",
                 keys && name ? " + " : "", name ? name : "");
        for (char* c = one; *c; c++) if (*c >= 'a' && *c <= 'z') *c = (char)(*c - 32);
        int w = snprintf(t + n, sizeof(t) - n, "%s", one);
        if (w < 0 || (size_t)w >= sizeof(t) - n) break;
        n += (size_t)w;
    }
    menu_set_players_text(t);
}

/* Bindings left at 0 ("the default") get their default code. */
static void bindings_resolve(void) {
    for (int i = 0; i < GCI_COUNT; i++) {
        if (!g_settings.key_bind[i]) g_settings.key_bind[i] = k_default_keys[i];
        if (!g_settings.pad_bind[i]) g_settings.pad_bind[i] = k_default_pads[i];
    }
}

static const char* key_name(int code) {
    static char buf[64];
    const char* n = SDL_GetScancodeName((SDL_Scancode)code);
    if (!n || !*n) { snprintf(buf, sizeof(buf), "KEY %d", code); return buf; }
    size_t i = 0;
    for (; n[i] && i + 1 < sizeof(buf); i++) buf[i] = (char)((n[i] >= 'a' && n[i] <= 'z') ? n[i] - 32 : n[i]);
    buf[i] = 0;
    return buf;
}

static const char* pad_name(int code) {
    static const char* const k_buttons[] = {
        "A / CROSS", "B / CIRCLE", "X / SQUARE", "Y / TRIANGLE", "BACK / VIEW", "GUIDE", "START / MENU",
        "LEFT STICK PRESS", "RIGHT STICK PRESS", "LB / L1", "RB / R1", "D-PAD UP", "D-PAD DOWN", "D-PAD LEFT",
        "D-PAD RIGHT", "MISC", "PADDLE 1", "PADDLE 2", "PADDLE 3", "PADDLE 4", "TOUCHPAD"};
    static const char* const k_axes[] = {"LEFT STICK LEFT", "LEFT STICK RIGHT", "LEFT STICK UP", "LEFT STICK DOWN",
                                         "RIGHT STICK LEFT", "RIGHT STICK RIGHT", "RIGHT STICK UP",
                                         "RIGHT STICK DOWN", "LT / L2", "LT / L2", "RT / R2", "RT / R2"};
    static char buf[32];
    if (code >= 64 && code < 64 + 12) return k_axes[code - 64];
    if (code >= 1 && code <= (int)(sizeof(k_buttons) / sizeof(k_buttons[0]))) return k_buttons[code - 1];
    snprintf(buf, sizeof(buf), "BUTTON %d", code);
    return buf;
}

/* Controller buttons are bound by Xbox-style names (A at the bottom, B on
   the right, X left, Y top). Nintendo controllers have A on the right and B
   at the bottom (X top, Y left): matched by label, their A plays as A. The
   swap works both ways, so it also turns a pressed button into its name. */
static int face_button(SDL_Gamepad* pad, int button) {
    if (!pad || !g_settings.nintendo_labels ||
        SDL_GetGamepadButtonLabel(pad, SDL_GAMEPAD_BUTTON_SOUTH) != SDL_GAMEPAD_BUTTON_LABEL_B)
        return button;
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return SDL_GAMEPAD_BUTTON_EAST;
    case SDL_GAMEPAD_BUTTON_EAST: return SDL_GAMEPAD_BUTTON_SOUTH;
    case SDL_GAMEPAD_BUTTON_WEST: return SDL_GAMEPAD_BUTTON_NORTH;
    case SDL_GAMEPAD_BUTTON_NORTH: return SDL_GAMEPAD_BUTTON_WEST;
    default: return button;
    }
}

/* How hard GameCube input i is pressed, 0-1: its key (all or nothing) or its
   controller binding (a stick or trigger gives the amount; buttons all or
   nothing), whichever is more. */
static float binding_amount(int i, SDL_Gamepad* pad, bool keys) {
    float a = 0.0f;
    int kc = g_settings.key_bind[i];
    if (keys && kc > 0 && kc < SDL_SCANCODE_COUNT) {
        const bool* k = SDL_GetKeyboardState(NULL);
        if (k && k[kc]) a = 1.0f;
    }
    int pc = g_settings.pad_bind[i];
    if (pad && pc > 0) {
        float v = 0.0f;
        if (pc >= 64) {
            int axis = (pc - 64) / 2, pos = (pc - 64) & 1;
            float x = (float)SDL_GetGamepadAxis(pad, (SDL_GamepadAxis)axis) / 32767.0f;
            if (!pos) x = -x;
            /* small dead zone; a stick reaches a GameCube stick's full travel at 100% */
            v = x < 0.08f ? 0.0f : x > 1.0f ? 1.0f : x;
        } else if (SDL_GetGamepadButton(pad, (SDL_GamepadButton)face_button(pad, pc - 1))) {
            v = 1.0f;
        }
        if (v > a) a = v;
    }
    return a;
}

/* A stick axis from -1..1 to the GameCube's range (a real stick reaches
   about +/-100 around the centre, 128). */
static u8 stick_value(float v) {
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    int r = 128 + (int)(v * 100.0f + (v < 0 ? -0.5f : 0.5f));
    return (u8)(r < 0 ? 0 : r > 255 ? 255 : r);
}

/* ------------------------------------------------------ settings menu --- */
#define SIDE_FILL 0x1A1B1Fu            /* ultrawide menus' sides */
static bool g_resize_pending;          /* left full screen: size the window for the shape once it has */

static void set_window_size(void) {
    if (g_fullscreen) return;
    if (SDL_GetWindowFlags(g_window) & SDL_WINDOW_MAXIMIZED) SDL_RestoreWindow(g_window);
    int h = settings_window_height(g_settings.window_size);
    SDL_SetWindowSize(g_window, (int)((float)h * g_aspect + 0.5f), h);
    SDL_SetWindowPosition(g_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
}

static void apply_settings(unsigned ch) {
    if (ch & (1u << MI_SHAPE)) {
        set_shape_mode(g_settings.screen_shape);
    }
    if (ch & (1u << MI_DISPLAY)) {
        g_fullscreen = g_settings.fullscreen != 0;
        SDL_SetWindowFullscreen(g_window, g_fullscreen);
        if (!g_fullscreen) g_resize_pending = true;   /* back to a window: the shape's own size */
    }
    if (ch & (1u << MI_WINSIZE))
        set_window_size();
    if ((ch & (1u << MI_RES)) && g_gpu) {
        gx_raster_set_gpu_scale(g_settings.resolution);
        g_gpu_scale = g_settings.resolution;
    }
    if (ch & (1u << MI_CONTROLS)) {
        bindings_resolve();            /* "reset" leaves them at 0 */
        update_players_text();
    }
    if (ch & (1u << MI_TEXTURES)) {
        /* Turning them on picks up files added, changed or removed since. */
        if (g_settings.custom_textures && !hires_enabled())
            gx_tex_reload_hires();
        hires_set_enabled(g_settings.custom_textures != 0);
    }
}

/* F5: custom textures on / off, as the menu option does (turning them on
   reloads the folder). */
static void toggle_custom_textures(void) {
    if (!g_gpu || !hires_has_folder()) {
        printf("window: no custom textures to switch (no texture folder, or the software renderer)\n");
        return;
    }
    g_settings.custom_textures = !g_settings.custom_textures;
    apply_settings(1u << MI_TEXTURES);
    settings_save();
    printf("window: custom textures %s\n", g_settings.custom_textures ? "on" : "off");
}

static void open_menu_controls_setup(void) {
    static bool done;
    if (!done) {
        menu_set_binding_names(key_name, pad_name);
        done = true;
    }
}

static void open_menu(void) {
    open_menu_controls_setup();
    update_players_text();
    MenuInfo mi = {g_gpu, g_gpu_scale, hires_has_folder()};
    menu_open(&mi);
}

/* Keys and controller buttons while the menu is open. */
static void menu_event(const SDL_Event* e) {
    static int axis_dir[2];
    int cap = menu_capturing();
    if (cap) {                         /* waiting for a key or button to bind */
        if (e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat) {
            if (e->key.key == SDLK_ESCAPE) menu_bind(0);
            else if (e->key.key == SDLK_DELETE || e->key.key == SDLK_BACKSPACE) menu_bind(-1);
            else if (cap == 1) menu_bind((int)e->key.scancode);
        } else if (cap == 2 && e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
            menu_bind(face_button(SDL_GetGamepadFromID(e->gbutton.which), e->gbutton.button) + 1);
        } else if (cap == 2 && e->type == SDL_EVENT_GAMEPAD_AXIS_MOTION &&
                   (e->gaxis.value > 24000 || e->gaxis.value < -24000)) {
            menu_bind(PAD_AXIS(e->gaxis.axis, e->gaxis.value > 0));
        }
        if (cap == 2 && !menu_capturing()) axis_dir[0] = axis_dir[1] = 2;   /* ignore the stick until it's let go */
        return;
    }
    if (menu_text_wanted() && (e->type == SDL_EVENT_TEXT_INPUT || e->type == SDL_EVENT_KEY_DOWN)) {
        /* typing an address: letters and digits are text, not shortcuts */
        if (e->type == SDL_EVENT_TEXT_INPUT) menu_text(e->text.text);
        else if (e->key.key == SDLK_V && (e->key.mod & SDL_KMOD_CTRL)) {   /* paste (an address sent in a chat) */
            char* clip = SDL_GetClipboardText();
            if (clip) {
                for (char* c = clip; *c; c++) if (*c == ' ' || *c == '\r' || *c == '\n' || *c == '\t') *c = 0;
                char* start = clip;
                while (!*start && start < clip + strlen(clip)) start++;
                menu_text(start);
                SDL_free(clip);
            }
        }
        else if (e->key.key == SDLK_BACKSPACE || e->key.key == SDLK_DELETE) menu_text_back();
        else if (e->key.key == SDLK_RETURN || e->key.key == SDLK_KP_ENTER) { if (!e->key.repeat) menu_action(MENU_OK); }
        else if (e->key.key == SDLK_ESCAPE) { if (!e->key.repeat) menu_action(MENU_BACK); }
        else if (e->key.key == SDLK_UP) menu_action(MENU_UP);
        else if (e->key.key == SDLK_DOWN) menu_action(MENU_DOWN);
        return;
    }
    if (e->type == SDL_EVENT_TEXT_INPUT) return;
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
        switch (face_button(SDL_GetGamepadFromID(e->gbutton.which), e->gbutton.button)) {   /* A confirms on any pad */
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
        if (axis_dir[i] == 2) {        /* after binding a stick: wait for it to centre */
            if (!d) axis_dir[i] = 0;
            return;
        }
        if (d && d != axis_dir[i])
            menu_action(i ? (d < 0 ? MENU_UP : MENU_DOWN) : (d < 0 ? MENU_LEFT : MENU_RIGHT));
        axis_dir[i] = d;
    }
}

/* The game's Online screen with the cursor on Join a Game: the keyboard
   types the address (its letters aren't the game's buttons then); Enter
   connects, Esc is the B button (back), the arrows still move up and down. */
static bool g_online_typing;
static int g_online_esc;               /* fields left of B held for Esc */
static bool online_key(const SDL_Event* e) {
    if (e->type == SDL_EVENT_TEXT_INPUT) { menu_online_text(e->text.text); return true; }
    SDL_Keycode k = e->key.key;
    if (k == SDLK_V && (e->key.mod & SDL_KMOD_CTRL)) {   /* paste (an address sent in a chat) */
        char* clip = SDL_GetClipboardText();
        if (clip) {
            char* start = clip;
            while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') start++;
            for (char* c = start; *c; c++) if (*c == ' ' || *c == '\r' || *c == '\n' || *c == '\t') { *c = 0; break; }
            menu_online_text(start);
            SDL_free(clip);
        }
        return true;
    }
    if (k == SDLK_BACKSPACE || k == SDLK_DELETE) { menu_online_text_back((e->key.mod & SDL_KMOD_CTRL) != 0); return true; }
    if (k == SDLK_RETURN || k == SDLK_KP_ENTER) { if (!e->key.repeat) menu_online_press(1); return true; }
    if (k == SDLK_ESCAPE) { if (!e->key.repeat) g_online_esc = 6; return true; }
    return false;                      /* F-keys etc. as always */
}

static void poll_events(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (menu_is_open() && (e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ||
                               e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION || e.type == SDL_EVENT_TEXT_INPUT)) {
            menu_event(&e);
            continue;
        }
        if (g_online_typing && (e.type == SDL_EVENT_TEXT_INPUT || e.type == SDL_EVENT_KEY_DOWN) && online_key(&e))
            continue;
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
            } else if (e.key.key == SDLK_F5 && !e.key.repeat) {
                toggle_custom_textures();
            } else if (e.key.key == SDLK_F6 && !e.key.repeat) {
                ram_search((e.key.mod & SDL_KMOD_SHIFT) != 0, (e.key.mod & SDL_KMOD_CTRL) != 0);
            } else if (e.key.key == SDLK_ESCAPE) {
                g_quit = true;
            } else if (e.key.key == SDLK_F7 && !e.key.repeat) {
                freecam_toggle();
            } else if (e.key.key == SDLK_F8 && !e.key.repeat && (e.key.mod & SDL_KMOD_SHIFT)) {
                dump_memory();
            } else if (e.key.key == SDLK_F8 && !e.key.repeat) {
                g_text_search = true;
            } else if (e.key.key == SDLK_F10 && !e.key.repeat) {
                if (g_gpu) g_shot_pending = true;
                else save_screenshot();
            } else if (e.key.key == SDLK_F12 && !e.key.repeat) {
                if (g_gpu) {
                    gx_raster_hud_log();
                    printf("window: logging two seconds of the match HUD to hudlog.txt (the full draw log needs the software renderer)\n");
                } else {
                    gx_raster_draw_log(interrupts_game_seconds(), 2);
                    printf("window: logging the next two frames to drawlog.txt and drawlog-short.txt\n");
                }
            } else if (e.key.key == SDLK_F9 && !e.key.repeat && (e.key.mod & SDL_KMOD_SHIFT)) {
                /* Ultrawide test: the game's own wider camera on / off (off:
                   it draws for 16:9 and the port widens the picture). */
                g_wide_camera = !g_wide_camera;
                printf("window: ultrawide: the game's own wider camera %s (from the next screen or match)\n",
                       g_wide_camera ? "on" : "off");
            } else if (e.key.key == SDLK_F9 && !e.key.repeat) {
                static const int k_next[] = {2, 2, 3, 4, 1};   /* 4:3 -> 16:9 -> 21:9 -> 32:9 -> 4:3 */
                if (netplay_active()) {
                    printf("window: the screen shape can't be changed during netplay (both games have to match)\n");
                } else if (!in_front_end()) {
                    printf("window: the screen shape can only be changed in the game's menus, not during a match\n");
                } else {
                    set_shape_mode(k_next[g_aspect_mode]);
                    g_settings.screen_shape = g_aspect_mode;
                    settings_save();
                }
            } else if (e.key.key == SDLK_F11 && !e.key.repeat) {
                g_fullscreen = !g_fullscreen;
                SDL_SetWindowFullscreen(g_window, g_fullscreen);
                if (!g_fullscreen) g_resize_pending = true;   /* back to a window: the shape's own size */
                g_settings.fullscreen = g_fullscreen;
                settings_save();
            }
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            if (g_freecam) g_cam_wheel += e.wheel.y > 0 ? 1 : e.wheel.y < 0 ? -1 : 0;
            break;
        case SDL_EVENT_GAMEPAD_ADDED: {
            bool have = false;
            for (int i = 0; i < PAD_PORTS; i++)
                if (g_pads[i] && SDL_GetGamepadID(g_pads[i]) == e.gdevice.which) have = true;
            for (int i = 0; i < PAD_PORTS && !have; i++)
                if (!g_pads[i]) {
                    g_pads[i] = SDL_OpenGamepad(e.gdevice.which);
                    g_lost[i] = false;
                    if (g_pads[i])
                        printf("window: controller connected as player %d: %s\n", i + 1, SDL_GetGamepadName(g_pads[i]));
                    have = true;
                }
            update_players_text();
            break;
        }
        case SDL_EVENT_GAMEPAD_REMOVED:
            for (int i = 0; i < PAD_PORTS; i++)
                if (g_pads[i] && SDL_GetGamepadID(g_pads[i]) == e.gdevice.which) {
                    SDL_CloseGamepad(g_pads[i]);
                    g_pads[i] = NULL;
                    g_lost[i] = true;
                    printf("window: player %d's controller disconnected\n", i + 1);
                }
            update_players_text();
            break;
        default:
            break;
        }
    }
}

/* After the menu closes, the key or button that closed it is usually still
   held: the game sees nothing until everything has been let go. */
static bool g_input_block;

/* One player's controller state from a gamepad and / or the keyboard. */
static PadLive player_state(SDL_Gamepad* pad, bool keys) {
    PadLive p = {0, 0x80, 0x80, 0x80, 0x80, 0, 0};
    /* How hard each GameCube input is pressed (0-1). */
    float amt[GCI_COUNT];
    for (int i = 0; i < GCI_COUNT; i++) amt[i] = binding_amount(i, pad, keys);
    static const u16 k_bits[] = {PAD_A, PAD_B, PAD_X, PAD_Y, PAD_Z, PAD_START, 0, 0,
                                 PAD_UP, PAD_DOWN, PAD_LEFT, PAD_RIGHT};
    for (int i = 0; i < 12; i++)
        if (k_bits[i] && amt[i] > 0.5f) p.buttons |= k_bits[i];
    p.trigger_l = (u8)(amt[GCI_L] * 255.0f + 0.5f);
    p.trigger_r = (u8)(amt[GCI_R] * 255.0f + 0.5f);
    if (amt[GCI_L] > 0.9f) p.buttons |= PAD_L;   /* the digital click at the end of the travel */
    if (amt[GCI_R] > 0.9f) p.buttons |= PAD_R;
    p.stick_x = stick_value(amt[GCI_SRIGHT] - amt[GCI_SLEFT]);
    p.stick_y = stick_value(amt[GCI_SUP] - amt[GCI_SDOWN]);
    p.cstick_x = stick_value(amt[GCI_CRIGHT] - amt[GCI_CLEFT]);
    p.cstick_y = stick_value(amt[GCI_CUP] - amt[GCI_CDOWN]);
    return p;
}

static bool pad_held(const PadLive* p) {
    return p->buttons || p->trigger_l > 30 || p->trigger_r > 30 || p->stick_x < 0x80 - 30 ||
           p->stick_x > 0x80 + 30 || p->stick_y < 0x80 - 30 || p->stick_y > 0x80 + 30 ||
           p->cstick_x < 0x80 - 30 || p->cstick_x > 0x80 + 30 || p->cstick_y < 0x80 - 30 ||
           p->cstick_y > 0x80 + 30;
}

static void all_idle(void) {
    PadLive idle = {0, 0x80, 0x80, 0x80, 0x80, 0, 0};
    for (int i = 0; i < PAD_PORTS; i++) pad_set_live(i, &idle);
}

static void read_input(void) {
    if (g_freecam) {                   /* the keys fly the camera; the game sees nothing pressed */
        all_idle();
        freecam_update();
        return;
    }
    /* Some controllers (Nintendo ones through SDL's own driver) can vanish
       without a "removed" event: check each one is still there. */
    bool gone = false;
    for (int i = 0; i < PAD_PORTS; i++)
        if (g_pads[i] && !SDL_GamepadConnected(g_pads[i])) {
            printf("window: player %d's controller disconnected (%s)\n", i + 1, SDL_GetGamepadName(g_pads[i]));
            SDL_CloseGamepad(g_pads[i]);
            g_pads[i] = NULL;
            g_lost[i] = true;
            gone = true;
        }
    if (gone) update_players_text();
    PadLive st[PAD_PORTS];
    bool any_held = false;
    int kb = g_settings.keyboard_player;
    for (int i = 0; i < PAD_PORTS; i++) {
        bool keys = i == kb;
        st[i] = player_state(g_pads[i], keys && !g_online_typing);
        if (keys && g_online_typing) {
            const bool* ks = SDL_GetKeyboardState(NULL);
            if (ks && ks[SDL_SCANCODE_UP] != ks[SDL_SCANCODE_DOWN]) st[i].stick_y = stick_value(ks[SDL_SCANCODE_UP] ? 1.0f : -1.0f);
            if (g_online_esc > 0) st[i].buttons |= PAD_B;
        }
        if (g_lost[i] && keys && pad_held(&st[i])) g_lost[i] = false;   /* the keyboard takes over */
        /* Port 1 is plugged in from the start (the game wants a controller
           there), the others once something plays as them. */
        pad_set_connected(i, g_pads[i] || (!g_lost[i] && (keys || i == 0)));
        if (pad_held(&st[i])) any_held = true;
    }
    if (g_input_block) {
        /* Wait for everything to be let go, but not for ever: something
           that reads as held all the time (a trigger or stick that doesn't
           rest at zero) would otherwise lock the game out. */
        static int waited;
        if (any_held && ++waited < 45) {
            all_idle();
            return;
        }
        if (any_held) {
            for (int i = 0; i < PAD_PORTS; i++)
                if (pad_held(&st[i]))
                    printf("window: player %d still reads as held after the menu (buttons %04X, stick %d/%d, "
                           "c-stick %d/%d, triggers %d/%d)\n", i + 1, st[i].buttons, st[i].stick_x, st[i].stick_y,
                           st[i].cstick_x, st[i].cstick_y, st[i].trigger_l, st[i].trigger_r);
        }
        waited = 0;
        g_input_block = false;
    }
    if (g_online_esc > 0) g_online_esc--;
    /* The Online screen: A on Host a Game / Join a Game starts (or stops) it. */
    {
        static bool was_a;
        int page = -1, focus = uismenu_video_focus(&page);
        bool a = false;
        for (int i = 0; i < PAD_PORTS; i++) if (st[i].buttons & PAD_A) a = true;
        if (page == 2 && focus >= 0 && a && !was_a) menu_online_press(focus);
        if (g_card_focus && a && !was_a) menu_card_press();   /* Save/Load's Delete User ID */
        was_a = a;
    }
    for (int i = 0; i < PAD_PORTS; i++) pad_set_live(i, &st[i]);
    /* Rumble: while the game has a port's motor on, its controller rumbles
       (renewed every field for a moment longer, so it stops by itself if the
       game stops running); off as soon as the game switches it off. */
    {
        static bool was_on[PAD_PORTS];
        for (int i = 0; i < PAD_PORTS; i++) {
            bool on = pad_rumble(i) && !g_settings.no_rumble;   /* (rumble=0 in the settings file: never) */
            if (g_pads[i] && (on || was_on[i]))
                SDL_RumbleGamepad(g_pads[i], on ? 0xC000 : 0, on ? 0x8000 : 0, on ? 100 : 0);
            was_on[i] = on;
        }
    }
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
    gx_raster_present_gpu(cpu, mmio_reg(0x201C, 4), mmio_reg(0x2048, 2), mmio_reg(0x2000, 2), ww, wh,
                          g_game_movie ? movie_aspect() : g_aspect,   /* its own movies: 16:9 at most */
                          menu_is_open() ? NULL : shot);
    if (menu_is_open()) {
        bool dirty;
        const uint32_t* px = menu_draw(ww, wh, &dirty);
        glr_present_overlay(px, ww, wh, dirty);
        if (shot) g_shot_pending = true;   /* screenshots are of the game, not the menu */
    } else {
        NetplayStatus st;
        if (netplay_status(&st)) {     /* the session badge */
            bool dirty;
            const uint32_t* px = menu_hud(ww, wh, &dirty, &st, in_front_end());
            if (px) glr_present_overlay(px, ww, wh, dirty);
        }
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
    g_game_movie = true;
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
    g_game_movie = false;
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
    gx_raster_present_pixels(px, w, h, ww, wh, movie_aspect());   /* the shape the game shows its own movie at */
    static long logged = -1000;
    if (at < logged || at >= logged + 300) {   /* now and then, to check it's shown */
        logged = at;
        printf("movie: showing replacement frame %ld (%ux%u) in a %dx%d window\n", at, w, h, ww, wh);
    }
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
    if (movie_is_open()) {
        static u64 told;
        if (g_field > told + 120) {
            told = g_field;
            printf("movie: replacement open but not shown (frame %ld)\n", g_movie_at);
        }
    }
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
    float aspect = g_game_movie ? movie_aspect() : g_aspect;
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
    static unsigned mark;              /* the profiler's samples since the last field */
    Uint64 took = g_hitch_last ? now - g_hitch_last : 0;
    Uint64 shown = g_present_ns >= last_present ? g_present_ns - last_present : g_present_ns;
    g_hitch_last = now;
    last_present = g_present_ns;
    static unsigned long long dec0, tex0;
    static double td0, th0, ti0, tt0;
    unsigned long long dec, tex;
    double td, th, ti, tt;
    gx_tex_counters(&dec, &tex);
    gx_tex_times(&td, &th, &ti, &tt);
    if (took > 50000000ull && !turbo() && g_field > 120) {
        char where[256];
        prof_since(mark, where, sizeof where);
        printf("hitch: %5.1f s game time, one field took %3.0f ms (showing the picture %2.0f ms)%s%s"
               " | %llu textures decoded (%.1f M pixels) | textures %.1f ms: decoding %.1f, custom lookups %.1f, index %.1f\n",
               interrupts_game_seconds(), (double)took * 1e-6, (double)shown * 1e-6, where[0] ? " | " : "", where,
               dec - dec0, (double)(tex - tex0) * 1e-6, (tt - tt0) * 1e3, (td - td0) * 1e3, (th - th0) * 1e3,
               (ti - ti0) * 1e3);
    }
    dec0 = dec; tex0 = tex;
    td0 = td; th0 = th; ti0 = ti; tt0 = tt;
    mark = prof_mark();
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
        static unsigned sec_mark;
        char busiest[256];
        prof_since(sec_mark, busiest, sizeof busiest);
        sec_mark = prof_mark();
        if (speed < 95.0 || turbo()) {   /* (fast-forward too: how fast it goes, and what holds it back) */
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
            const char* fns = strstr(busiest, "game code in ");
            if (fns) printf("perf:   busiest: %s\n", fns);
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

/* ------------------------------------------------------------ trailers --- */
/* The Previews menu's trailers (the game's own trailer code isn't in the
   GameCube version, main.c answers the menu): replace\previews\<name>.mpg,
   converted from the Xbox / PS2 MOVIEDAT.DAT, in its order. The game stands
   still while one plays, its sound paused; any button stops it. Shown at
   the screen's shape (stretched for 16:9, like the intro movies). */
static const char* const k_previews[11] = {
    "madden_nfl_2005", "nba_live_2005", "nba_street_v3", "fifa_street", "fifa_soccer_2005",
    "ncaa_football_2005", "ncaa_march_madness_2005", "nascar_2005", "nhl_2005", "tiger_woods_2005",
    "nfs_underground_2",
};
static int g_preview_req = -1;
static bool g_quit_req;
static bool g_video_req;
static bool g_online_req;
static char g_restart_args[160];
static bool g_restart;

void window_quit_request(void) {
    g_quit_req = true;
}

void window_quit_now(void) {
    printf("window: quitting from the game's main menu\n");
    g_quit = true;
}

void window_video_request(void) {
    g_video_req = true;
}

void window_online_request(void) {
    g_online_req = true;
}

static char g_ps2_dir[512];
static bool g_ps2_req;
void window_set_ps2_dir(const char* dir) { snprintf(g_ps2_dir, sizeof g_ps2_dir, "%s", dir ? dir : ""); }
bool window_ps2_online_available(void) {
    static int known = -1;             /* (looked for once: the files don't come and go during a run) */
    if (known < 0) {
        const char* miss = g_ps2_dir[0] ? ps2o_missing_file(g_ps2_dir) : NULL;
        known = g_ps2_dir[0] && !miss;
        if (!g_ps2_dir[0]) printf("ps2 online: no PS2 folder given (play.ps1 -Ps2, default C:\\nfls2\\ps2); Online shows the port's own page\n");
        else if (miss) printf("ps2 online: %s isn't in %s; Online shows the port's own page\n", miss, g_ps2_dir);
        else if (!g_gpu) printf("ps2 online: the PS2 files are there, but the PS2 menus need the graphics card renderer; Online shows the port's own page\n");
        else printf("ps2 online: the PS2 files are there (%s); Online shows the PS2 version's menus\n", g_ps2_dir);
    }
    return known && g_gpu;
}
void window_ps2_online_request(void) { g_ps2_req = true; }

bool window_take_restart(char* args, size_t cap) {
    if (!g_restart) return false;
    g_restart = false;
    snprintf(args, cap, "%s", g_restart_args);
    return true;
}

void window_preview_request(int index) {
    g_preview_req = index;
}

static bool any_button_held(void) {
    int kb = g_settings.keyboard_player;
    for (int i = 0; i < PAD_PORTS; i++) {
        PadLive st = player_state(g_pads[i], i == kb);
        if (st.buttons) return true;
    }
    return false;
}

static void restart_pacing(void) {
    Uint64 now = SDL_GetTicksNS();
    g_base_ns = g_stat_ns = now;
    g_base_field = g_field;
    g_stat_fields = 0;
    g_frames_at_stat = gx_raster_frames();
    g_slept_ns = g_present_ns = 0;
    g_hitch_last = 0;
}

static void menu_loop(CPUState* cpu);

static void ps2_log(const char* msg) {
    /* (not the engine's messages, many every frame, nor the buttons' "no
       method OnUP" etc.: the screens send every press to every highlighted
       button, most of which ignore most) */
    if (!strncmp(msg, "MSG ", 4) || !strncmp(msg, "no method On", 12) || !strncmp(msg, "no method Off", 13)) return;
    printf("ps2 online: %s\n", msg);
}

/* The PS2 version's online menus: the game stands still (its sound paused)
   while they run at their own 60 frames a second, drawn at up to 720 lines and
   stretched over the window. The pad and keyboard drive EA's screens as the
   PS2 pad did: A = X (select), B = Triangle (back), X = Square (Circle, the
   EA Messenger, isn't sent: it's left out).
   Leaving online mode on them (or Quick Game, which goes on to the port's
   own host / join page for now) closes them; so do Esc and F1, at once. */
static int ps2_key_button(SDL_Keycode k) {
    switch (k) {
    case SDLK_UP: return PS2O_UP;
    case SDLK_DOWN: return PS2O_DOWN;
    case SDLK_LEFT: return PS2O_LEFT;
    case SDLK_RIGHT: return PS2O_RIGHT;
    case SDLK_X: case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: return PS2O_X;
    case SDLK_Z: case SDLK_BACKSPACE: return PS2O_TRIANGLE;
    case SDLK_C: return PS2O_SQUARE;
    case SDLK_Q: return PS2O_L1;
    case SDLK_W: return PS2O_R1;
    default: return -1;
    }
}
static int ps2_pad_button(int b) {
    switch (b) {
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return PS2O_UP;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return PS2O_DOWN;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return PS2O_LEFT;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return PS2O_RIGHT;
    case SDL_GAMEPAD_BUTTON_SOUTH: return PS2O_X;
    case SDL_GAMEPAD_BUTTON_EAST: case SDL_GAMEPAD_BUTTON_BACK: return PS2O_TRIANGLE;
    case SDL_GAMEPAD_BUTTON_WEST: return PS2O_SQUARE;
    case SDL_GAMEPAD_BUTTON_START: return PS2O_START;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return PS2O_L1;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return PS2O_R1;
    default: return -1;
    }
}

static void ps2_loop(CPUState* cpu) {
    if (g_audio) SDL_PauseAudioStreamDevice(g_audio);
    all_idle();
    Uint64 t0 = SDL_GetTicksNS();
    if (!ps2o_open(g_ps2_dir, ps2_log)) {
        printf("ps2 online: the PS2 menus didn't start; the port's Online page instead\n");
        menu_open_online();
        menu_loop(cpu);
        return;
    }
    printf("ps2 online: menus started in %.0f ms\n", (SDL_GetTicksNS() - t0) / 1e6);
    u32* px = NULL;
    int pw = 0, ph = 0;
    bool leave = false, to_page = false, dirty = true;
    Uint64 next = SDL_GetTicksNS();
    double draw_ms = 0;
    int draws = 0;
    int stick[2] = {0, 0};             /* the left stick as the pad's directions */
    while (!g_quit && !leave) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) g_quit = true;
            else if ((e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_KEY_UP) && !e.key.repeat) {
                if (e.key.key == SDLK_F11 && e.type == SDL_EVENT_KEY_DOWN)
                    SDL_SetWindowFullscreen(g_window, !(SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN));
                if ((e.key.key == SDLK_F1 || e.key.key == SDLK_ESCAPE) && e.type == SDL_EVENT_KEY_DOWN)
                    leave = true;              /* (out at once, whatever the screens do) */
                int b = ps2_key_button(e.key.key);
                if (b >= 0) ps2o_button(b, e.type == SDL_EVENT_KEY_DOWN);
            } else if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || e.type == SDL_EVENT_GAMEPAD_BUTTON_UP) {
                int b = ps2_pad_button(face_button(SDL_GetGamepadFromID(e.gbutton.which), e.gbutton.button));
                if (b >= 0) ps2o_button(b, e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
            } else if (e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION &&
                       (e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX || e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY)) {
                int i = e.gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY;
                int d = e.gaxis.value > 20000 ? 1 : e.gaxis.value < -20000 ? -1 : 0;
                if (d != stick[i]) {
                    if (stick[i]) ps2o_button(i ? (stick[i] < 0 ? PS2O_UP : PS2O_DOWN) : (stick[i] < 0 ? PS2O_LEFT : PS2O_RIGHT), false);
                    if (d) ps2o_button(i ? (d < 0 ? PS2O_UP : PS2O_DOWN) : (d < 0 ? PS2O_LEFT : PS2O_RIGHT), true);
                    stick[i] = d;
                }
            }
        }
        char flow[48];
        if (ps2o_take_flow(flow, sizeof flow)) {
            printf("ps2 online: the menus ask for %s\n", flow);
            if (!strcmp(flow, "f_MainFE")) leave = true;                     /* Exit Online */
            else if (!strcmp(flow, "f_playnow")) leave = to_page = true;     /* Quick Game: hosting / joining */
        }
        Uint64 now = SDL_GetTicksNS();
        int steps = 0;
        Uint64 frame_ns = (Uint64)(ps2o_frame_ms() * 1e6);
        while (now >= next && steps < 4) { ps2o_tick(); next += frame_ns; steps++; dirty = true; }
        if (now >= next) next = now + frame_ns;   /* (far behind: no catching up) */
        int ww = 0, wh = 0;
        SDL_GetWindowSizeInPixels(g_window, &ww, &wh);
        if (ww > 0 && wh > 0) {
            int h = wh > 720 ? 720 : wh, w = (int)((long long)ww * h / wh);
            if (w != pw || h != ph || !px) {
                free(px);
                px = (u32*)malloc((size_t)w * h * 4);
                pw = w; ph = h; dirty = true;
            }
            if (px && dirty) {
                Uint64 d0 = SDL_GetTicksNS();
                ps2o_draw(px, pw, ph);
                draw_ms += (SDL_GetTicksNS() - d0) / 1e6;
                draws++;
            }
            glr_present_black(ww, wh);
            if (px) glr_present_overlay_scaled(px, pw, ph, ww, wh, dirty);
            dirty = false;
            SDL_GL_SwapWindow(g_window);
        }
        SDL_Delay(1);
    }
    double secs = (SDL_GetTicksNS() - t0) / 1e9;
    if (draws) printf("ps2 online: closed (%d pictures at %dx%d, %.1f ms each, %.0f a second)\n", draws, pw, ph, draw_ms / draws,
                      secs > 0 ? draws / secs : 0);
    ps2o_close();
    free(px);
    if (to_page && !g_quit) {
        menu_open_online();
        menu_loop(cpu);
        return;
    }
    g_input_block = true;              /* the leaving press doesn't reach the game */
    if (g_audio) SDL_ResumeAudioStreamDevice(g_audio);
    restart_pacing();
}

static void preview_loop(CPUState* cpu) {
    (void)cpu;
    int idx = g_preview_req;
    g_preview_req = -1;
    if (idx < 0 || idx > 10) return;
    const char* dir = disc_replace_dir();
    if (!dir || !g_gpu) {
        printf("movie: trailer %s not played (%s)\n", k_previews[idx], !dir ? "no replace folder" : "needs the GPU renderer");
        return;
    }
    char path[1024];
    snprintf(path, sizeof(path), "%s/previews/%s.mpg", dir, k_previews[idx]);
    int rate = 0;
    if (!preview_open(path, &rate)) {
        printf("movie: no trailer at %s\n", path);
        return;
    }
    SDL_AudioStream* as = NULL;
    if (rate > 0) {
        SDL_AudioSpec spec = {SDL_AUDIO_F32, 2, rate};
        as = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
        if (as) SDL_ResumeAudioStreamDevice(as);
    }
    if (g_audio) SDL_PauseAudioStreamDevice(g_audio);
    all_idle();
    const double bytes_per_s = rate * 8.0;
    double fed = 0.0, t = 0.0;
    bool sound_done = !as, armed = false, stopped = false;
    Uint64 last = SDL_GetTicksNS();
    while (!g_quit) {
        poll_events();
        Uint64 now = SDL_GetTicksNS();
        double dt = (double)(now - last) * 1e-9;
        last = now;
        while (as && !sound_done && SDL_GetAudioStreamQueued(as) < (int)(bytes_per_s * 0.25)) {
            const float* s = preview_audio();
            if (!s) { sound_done = true; break; }
            SDL_PutAudioStreamData(as, s, 1152 * 8);
            fed += 1152.0 / rate;
        }
        int queued = as ? SDL_GetAudioStreamQueued(as) : 0;
        if (as && (!sound_done || queued > 0))
            t = fed - queued / bytes_per_s;   /* the picture follows the sound */
        else
            t += dt;                          /* no sound (left): the clock */
        u32 w = 0, h = 0;
        bool pictures_done;
        const u32* px = preview_frame(t, &w, &h, &pictures_done);
        if (pictures_done && sound_done && queued == 0) break;
        int ww = 0, wh = 0;
        SDL_GetWindowSizeInPixels(g_window, &ww, &wh);
        if (ww > 0 && wh > 0) {
            update_aspect(false);
            if (px) gx_raster_present_pixels(px, w, h, ww, wh, movie_aspect());   /* the shape the game's own movies get */
            else glr_present_black(ww, wh);
            SDL_GL_SwapWindow(g_window);
        }
        if (!any_button_held()) armed = true;   /* the press that picked it, let go first */
        else if (armed) { stopped = true; break; }
        if (menu_is_open()) { stopped = true; break; }
        SDL_Delay(4);
    }
    printf("movie: trailer %s %s after %.1f s\n", k_previews[idx], stopped ? "stopped" : "ended", t);
    preview_close();
    if (as) SDL_DestroyAudioStream(as);
    if (g_audio) SDL_ResumeAudioStreamDevice(g_audio);
    g_input_block = true;              /* the stopping press doesn't reach the game */
    restart_pacing();
}

/* The menu's page on screen for a moment, the game standing still (a
   notice before the game restarts). */
static void show_notice(CPUState* cpu, int ms) {
    if (g_audio) SDL_PauseAudioStreamDevice(g_audio);
    Uint64 end = SDL_GetTicksNS() + (Uint64)ms * 1000000u;
    while (!g_quit && SDL_GetTicksNS() < end) {
        SDL_PumpEvents();
        present(cpu);
        SDL_Delay(15);
    }
}

/* The game stands still while the menu is open: the last picture stays up
   under it, the sound pauses, and real-time pacing restarts afterwards. */
static void menu_loop(CPUState* cpu) {
    printf("window: video menu open\n");
    if (g_audio) SDL_PauseAudioStreamDevice(g_audio);
    all_idle();
    bool typing = false;
    while (menu_is_open() && !menu_live() && !g_quit) {
        poll_events();
        menu_tick();                   /* netplay hosting / joining going on */
        if (menu_text_wanted() != typing) {
            typing = !typing;
            if (typing) SDL_StartTextInput(g_window);
            else SDL_StopTextInput(g_window);
        }
        if (menu_take_restart(g_restart_args, sizeof g_restart_args)) {
            /* netplay: both players ready (or leaving): this game closes and
               main starts it again with the session's options */
            present(cpu);
            settings_save();
            SDL_Delay(600);            /* ("player found" on screen a moment) */
            g_restart = true;
            g_quit = true;
            printf("window: restarting the game%s%s\n", g_restart_args[0] ? " with " : " without netplay", g_restart_args);
            break;
        }
        apply_settings(menu_take_changes());
        present(cpu);
        SDL_Delay(15);
    }
    if (typing) SDL_StopTextInput(g_window);
    if (menu_live()) {                 /* the colours page: the game runs on behind it (live_field) */
        apply_settings(menu_take_changes());
        if (g_audio) SDL_ResumeAudioStreamDevice(g_audio);
        Uint64 now = SDL_GetTicksNS();
        g_base_ns = g_stat_ns = now;
        g_base_field = g_field;
        g_stat_fields = 0;
        g_frames_at_stat = gx_raster_frames();
        g_slept_ns = g_present_ns = 0;
        g_hitch_last = 0;
        return;
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

/* Keeps the window answering the system (no "not responding") while the
   game waits, e.g. for the other netplay player; the events stay queued. */
void window_keep_alive(void) {
    SDL_PumpEvents();
    /* waiting a while for the other player: the picture again, with the
       badge saying so (a few times a second) */
    NetplayStatus st;
    static Uint64 last;
    if (g_cpu_ref && g_gpu && netplay_status(&st) && st.waiting_ms > 400 && SDL_GetTicks() - last > 200) {
        last = SDL_GetTicks();
        present(g_cpu_ref);
    }
}

void window_field(CPUState* cpu) {
    g_field++;
    g_cpu_ref = cpu;
    poll_events();
    sync_game_screen_option(cpu);
    if (g_resize_pending && !(SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN)) {
        /* Out of full screen (the system puts back the window's old size,
           which can be another shape's): the size for the shape now. */
        g_resize_pending = false;
        set_window_size();
    }
    /* Ultrawide menus: a dark grey at the sides the 16:9 backgrounds don't
       reach (matches fill the whole picture themselves). */
    gx_raster_set_side_fill(g_settings.screen_shape >= 3 && in_front_end() ? SIDE_FILL : 0u);
    bool in_match = lodfix_frame(cpu);
    gx_raster_set_match(in_match);
    /* Discord: in a match or in the menus (starting up while the intro
       movies are skipped). */
    discord_set(g_hidden ? "Starting up" : in_match ? "In a match" : "In the menus", "");
    uicolors_frame(cpu);
    /* (netplay: never, the screen shape changes the game for this player only) */
    menu_set_shape_locked(!in_front_end() || netplay_active());
    if (netplay_lost() && !g_restart) {
        /* The other player left (or the connection failed): this game
           restarts on its own, out of the session, after a notice. */
        printf("window: the netplay session ended (the other player left or the connection was lost); restarting "
               "without netplay\n");
        menu_open_online_ended();
        show_notice(cpu, 2500);
        g_restart_args[0] = 0;
        settings_save();
        g_restart = true;
        g_quit = true;
    }
    {
        /* The game's Video Settings screen: its rows read and set the
           settings, applied here as the menu's are. */
        static const char* const k_video_label[MENU_GAME_ROWS] = {"Aspect Ratio", "Display", "Window Resolution",
                                                                 "Internal Resolution", "Renderer"};
        static const UisVideoRows k_rows = {menu_game_row_count, menu_game_row_get, menu_game_row_set,
                                            menu_game_row_name, menu_game_row_help, menu_game_row_enabled,
                                            MENU_GAME_ROWS, "Video Settings", k_video_label};
        static const UisVideoRows k_recomp = {menu_recomp_row_count, menu_recomp_row_get, menu_recomp_row_set,
                                              menu_recomp_row_name, menu_recomp_row_help, menu_recomp_row_enabled,
                                              MENU_RECOMP_ROWS, "Recomp Settings", k_recomp_label};
        static const UisVideoRows k_online = {menu_online_row_count, menu_online_row_get, menu_online_row_set,
                                              menu_online_row_name, menu_online_row_help, menu_online_row_enabled,
                                              MENU_ONLINE_ROWS, "Quick Game", k_online_label};
        uismenu_set_video_rows(&k_rows, &k_recomp, &k_online);
        if (!menu_is_open()) {
            MenuInfo mi = {g_gpu, g_gpu_scale, hires_has_folder()};
            menu_set_info(&mi);
        }
        uismenu_video_frame(cpu);
        {
            /* The game's Online screen: the row with the cursor, typing, and
               the connection going on (both games restart once it's made). */
            int page = -1, focus = uismenu_video_focus(&page);
            menu_online_focus(page == 2 ? focus : -1);
            {
                /* Save/Load's Delete User ID row */
                static const char* const k_card_label[1] = {"Delete User ID"};
                static const UisVideoRows k_card = {menu_card_row_count, menu_card_row_get, menu_card_row_set,
                                                    menu_card_row_name, menu_card_row_help, menu_card_row_enabled,
                                                    1, "Delete User ID", k_card_label};
                uismenu_set_saveload_row(&k_card);
                g_card_focus = uismenu_saveload_frame(cpu);
                menu_card_focus(g_card_focus);
            }
            if (menu_card_tick(g_restart_args, sizeof g_restart_args)) {
                /* a User ID deleted: the game starts again, reading the card afresh */
                settings_save();
                g_restart = true;
                g_quit = true;
                printf("window: restarting the game (a User ID was deleted)\n");
            }
            bool typing = menu_online_typing();
            if (typing != g_online_typing) {
                g_online_typing = typing;
                if (typing) SDL_StartTextInput(g_window);
                else SDL_StopTextInput(g_window);
            }
            if (menu_online_tick(g_restart_args, sizeof g_restart_args)) {
                settings_save();
                g_restart = true;
                g_quit = true;
                printf("window: restarting the game with %s\n", g_restart_args);
            }
        }
        if (!menu_is_open()) {
            unsigned ch = menu_take_changes();
            if (ch) {
                apply_settings(ch);
                settings_save();
            }
        }
    }
    static bool live;                  /* the colours page was open last field */
    if (menu_is_open() && menu_live()) {
        /* The game carries on (its controllers idle) so the colours picked
           show on it; the menu is drawn over it. */
        live = true;
        all_idle();
        if (menu_take_restart(g_restart_args, sizeof g_restart_args)) {   /* the Online page: leave the session */
            show_notice(cpu, 1500);    /* ("leaving the session") */
            settings_save();
            g_restart = true;
            g_quit = true;
            printf("window: restarting the game%s%s\n", g_restart_args[0] ? " with " : " without netplay", g_restart_args);
        }
        apply_settings(menu_take_changes());
        present(cpu);
        pace_and_stats();
        return;
    }
    if (live && !menu_is_open()) g_input_block = true;   /* closed from the colours page */
    live = false;
    if (menu_is_open()) {
        menu_loop(cpu);
        return;
    }
    if (g_preview_req >= 0) {
        preview_loop(cpu);
        return;
    }
    if (g_video_req) {                 /* the game's Options list: Video Settings */
        g_video_req = false;
        open_menu();
        menu_loop(cpu);
        return;
    }
    if (g_ps2_req) {                   /* the main menu's Online, with the PS2 files */
        g_ps2_req = false;
        ps2_loop(cpu);
        return;
    }
    if (g_online_req) {                /* the main menu's Online */
        g_online_req = false;
        menu_open_online();
        menu_loop(cpu);
        return;
    }
    if (g_quit_req) {
        g_quit_req = false;
        menu_open_quit();
        menu_loop(cpu);
        if (menu_take_quit()) {
            printf("window: quitting from the game's main menu\n");
            g_quit = true;
        }
        return;
    }
    read_input();
    const bool* keys = SDL_GetKeyboardState(NULL);
    g_turbo_key = keys && keys[SDL_SCANCODE_TAB];
    Uint64 t0 = SDL_GetTicksNS();
    int prev = prof_enter(PROF_PRESENT);
    /* Fast-forward (Tab): a picture whenever one is due on the screen (60 a
       second), the fields in between not shown. (One field in eight, as
       before, looked like 10 frames a second in a match, where the game
       can't run much faster than normal.) While skipping the intros, a black
       window now and then. */
    /* Frames not shown aren't drawn either (raster.c): while Tab is held, a
       frame is drawn about 30 times a second and shown when it's done, the
       others left out, which leaves the game's own work more of the time. */
    static Uint64 last_shown;
    static u32 shown_drawn;
    bool tab = turbo() && !g_turbo_forced && !g_hidden;
    if (g_turbo_forced || g_hidden) {
        if (!g_turbo_forced || (g_field & 7u) == 0)
            present_black();
    } else if (!tab) {
        present(cpu);
        last_shown = t0;
    } else if (gx_raster_drawn_frames() != shown_drawn) {
        present(cpu);
        shown_drawn = gx_raster_drawn_frames();
        last_shown = t0;
    }
    gx_raster_set_skip_next(tab && t0 - last_shown < 25000000ull);
    prof_leave(prev);
    g_present_ns += SDL_GetTicksNS() - t0;
    pace_and_stats();
}
