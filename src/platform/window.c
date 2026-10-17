#include "platform/window.h"
#include "gx/raster.h"
#include "runtime/interrupts.h"
#include "runtime/mmio.h"
#include "runtime/pad.h"

#include <SDL3/SDL.h>

#include <stdio.h>

#define TEX_W 720
#define TEX_H 576

static SDL_Window* g_window;
static SDL_Renderer* g_renderer;
static SDL_Texture* g_texture;
static SDL_Gamepad* g_pad;
static bool g_quit;
static bool g_fullscreen;
static u32 g_pixels[TEX_W * TEX_H];
static u32 g_shown_w = 640, g_shown_h = 480;

/* Real-time pacing: game fields are held to the wall clock. */
static Uint64 g_base_ns;               /* wall time of field g_base_field */
static u64 g_base_field;
static u64 g_field;
static Uint64 g_stat_ns;
static u64 g_stat_fields;
static u32 g_frames_at_stat;

#define FIELD_NS (1001000000.0 / 60.0) /* NTSC: 59.94 fields per second */

bool window_open(int scale) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "error: SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }
    if (scale < 1) scale = 1;
    g_window = SDL_CreateWindow("NFL Street 2", 640 * scale, 480 * scale, SDL_WINDOW_RESIZABLE);
    if (!g_window) {
        fprintf(stderr, "error: couldn't create the window: %s\n", SDL_GetError());
        return false;
    }
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
    printf("window: %s renderer, keyboard: arrows=stick X=A Z=B C=X S=Y D=Z Q/W=L/R Enter=START, Esc quits, F11 fullscreen\n",
           SDL_GetRendererName(g_renderer));
    g_base_ns = g_stat_ns = SDL_GetTicksNS();
    return true;
}

bool window_quit_requested(void) {
    return g_quit;
}

void window_close(void) {
    if (g_pad) SDL_CloseGamepad(g_pad);
    if (g_texture) SDL_DestroyTexture(g_texture);
    if (g_renderer) SDL_DestroyRenderer(g_renderer);
    if (g_window) SDL_DestroyWindow(g_window);
    SDL_Quit();
    g_pad = NULL;
    g_texture = NULL;
    g_renderer = NULL;
    g_window = NULL;
}

/* ------------------------------------------------------------- input --- */
static u8 axis_to_gc(Sint16 v, bool invert) {
    int x = (int)v * 100 / 32767;      /* a real stick reaches about +/-100 */
    if (x > -8 && x < 8) x = 0;        /* small dead zone */
    if (invert) x = -x;
    int r = 128 + x;
    return (u8)(r < 0 ? 0 : r > 255 ? 255 : r);
}

static void poll_events(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_EVENT_QUIT:
            g_quit = true;
            break;
        case SDL_EVENT_KEY_DOWN:
            if (e.key.key == SDLK_ESCAPE) {
                g_quit = true;
            } else if (e.key.key == SDLK_F11 && !e.key.repeat) {
                g_fullscreen = !g_fullscreen;
                SDL_SetWindowFullscreen(g_window, g_fullscreen);
            }
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

static void read_input(void) {
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
    pad_set_live(&p);
}

/* ------------------------------------------------------------ output --- */
static void present(CPUState* cpu) {
    u32 w, h;
    if (gx_raster_xfb_rgb(cpu, mmio_reg(0x201C, 4), mmio_reg(0x2048, 2), mmio_reg(0x2000, 2), g_pixels, TEX_W,
                          TEX_H, &w, &h)) {
        SDL_Rect r = {0, 0, (int)w, (int)h};
        SDL_UpdateTexture(g_texture, &r, g_pixels, (int)(w * 4));
        if (w != g_shown_w || h != g_shown_h) {
            g_shown_w = w;
            g_shown_h = h;
        }
    }
    SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);
    SDL_RenderClear(g_renderer);

    /* Scale to fit the window, keeping the 4:3 TV shape. */
    int ww, wh;
    SDL_GetRenderOutputSize(g_renderer, &ww, &wh);
    float aspect = 4.0f / 3.0f;
    float dw = (float)ww, dh = (float)ww / aspect;
    if (dh > (float)wh) { dh = (float)wh; dw = dh * aspect; }
    SDL_FRect src = {0, 0, (float)g_shown_w, (float)g_shown_h};
    SDL_FRect dst = {((float)ww - dw) * 0.5f, ((float)wh - dh) * 0.5f, dw, dh};
    SDL_RenderTexture(g_renderer, g_texture, &src, &dst);
    SDL_RenderPresent(g_renderer);
}

static void pace_and_stats(void) {
    Uint64 now = SDL_GetTicksNS();
    double target = (double)g_base_ns + (double)(g_field - g_base_field) * FIELD_NS;
    if ((double)now < target) {
        SDL_DelayNS((Uint64)(target - (double)now));
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
        char title[128];
        snprintf(title, sizeof(title), "NFL Street 2 - %.0f fps - %.0f%% speed", fps, speed);
        SDL_SetWindowTitle(g_window, title);
        g_stat_ns = now;
        g_stat_fields = 0;
        g_frames_at_stat = frames;
    }
}

void window_field(CPUState* cpu) {
    g_field++;
    poll_events();
    read_input();
    present(cpu);
    pace_and_stats();
}
