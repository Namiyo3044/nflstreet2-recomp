/* Renderer comparison test (Linux, headless): draws a set of scenes built
 * from raw GameCube graphics commands, fed through the real command parser,
 * with either the software renderer or the GPU backend, and writes each
 * scene's TV-buffer output (YUYV) to a file. tests/gpu_compare.py runs both
 * and compares them.
 *
 *   gpu_compare soft|gpu OUTDIR [scale]
 *
 * The GPU run uses an EGL surfaceless context (Mesa's llvmpipe works). No
 * game data is involved: every scene is made up here.
 */
#include "gx/fifo.h"
#include "gx/raster.h"
#include "gx/state.h"
#include "gx/raster_gl.h"
#include "gx/gl_min.h"

#include <dlfcn.h>
#include <unistd.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ----------------------------------------------------- stubs (port) --- */
volatile int g_prof_cat;
void mmio_pe_finish(void) {}
void mmio_pe_token(u16 token, bool interrupt) { (void)token; (void)interrupt; }
void mmio_pe_display_copy(void) {}
double interrupts_game_seconds(void) { return 0.0; }

/* --------------------------------------------------------------- EGL --- */
typedef void* EP;
static void* (*egl_gpa)(const char*);
static EP g_dpy, g_ctx, g_ctx2;
static unsigned (*egl_make)(EP, EP, EP, EP);

static void* getproc(const char* name) { return egl_gpa(name); }
static void make_current(void) { egl_make(g_dpy, NULL, NULL, g_ctx); }

static bool egl_init(void) {
    void* l = dlopen("libEGL.so.1", RTLD_NOW);
    if (!l) return false;
    egl_gpa = (void* (*)(const char*))dlsym(l, "eglGetProcAddress");
    EP (*getpd)(int, void*, const int*) = (EP(*)(int, void*, const int*))egl_gpa("eglGetPlatformDisplayEXT");
    unsigned (*init)(EP, int*, int*) = (unsigned (*)(EP, int*, int*))dlsym(l, "eglInitialize");
    unsigned (*bind)(unsigned) = (unsigned (*)(unsigned))dlsym(l, "eglBindAPI");
    EP (*cc)(EP, EP, EP, const int*) = (EP(*)(EP, EP, EP, const int*))dlsym(l, "eglCreateContext");
    egl_make = (unsigned (*)(EP, EP, EP, EP))dlsym(l, "eglMakeCurrent");
    g_dpy = getpd(0x31DD /* surfaceless */, NULL, NULL);
    int a, b;
    if (!g_dpy || !init(g_dpy, &a, &b)) return false;
    bind(0x30A2 /* OpenGL */);
    const int attrs[] = {0x3098, 3, 0x30FB, 3, 0x30FD, 1, 0x3038};   /* 3.3 core */
    g_ctx = cc(g_dpy, NULL, NULL, attrs);
    if (!g_ctx) return false;
    g_ctx2 = cc(g_dpy, NULL, g_ctx, attrs);   /* shares textures: shows the picture */
    make_current();
    return true;
}

/* ---------------------------------------------------- command writing --- */
static CPUState g_cpu;

static void w8(u8 v) { gx_fifo_write(&g_cpu, v, 1); }
static void w32(u32 v) { gx_fifo_write(&g_cpu, v, 4); }
static void wf(float f) { u32 u; memcpy(&u, &f, 4); w32(u); }
static void bp(u32 reg, u32 v) { w8(0x61); w32((reg << 24) | (v & 0xFFFFFFu)); }
static void cp(u32 reg, u32 v) { w8(0x08); w8((u8)reg); w32(v); }
static void xf(u32 addr, u32 n, const u32* v) {
    w8(0x10);
    w32(((n - 1) << 16) | addr);
    for (u32 i = 0; i < n; i++) w32(v[i]);
}
static void xf1(u32 addr, u32 v) { xf(addr, 1, &v); }
static void xff(u32 addr, float f) { u32 u; memcpy(&u, &f, 4); xf1(addr, u); }

/* RAM layout of the test: textures at 1 MB, TV buffer at 4 MB, copies at 6 MB. */
#define TEX_ADDR 0x00100000u
#define TEX2_ADDR 0x00180000u
#define XFB_ADDR 0x00400000u
#define COPY_ADDR 0x00600000u

static void be16(u8* p, u32 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }

/* An RGBA8 texture (4x4 tiles: 32 bytes AR, then 32 bytes GB). */
static void put_rgba8(u32 addr, u32 w, u32 h, u32 (*fn)(u32 x, u32 y)) {
    u8* out = g_cpu.ram + addr;
    for (u32 ty = 0; ty < h; ty += 4)
        for (u32 tx = 0; tx < w; tx += 4) {
            for (u32 i = 0; i < 16; i++) {
                u32 c = fn(tx + (i & 3), ty + (i >> 2));    /* 0xAARRGGBB */
                out[i * 2] = (u8)(c >> 24); out[i * 2 + 1] = (u8)(c >> 16);
                out[32 + i * 2] = (u8)(c >> 8); out[32 + i * 2 + 1] = (u8)c;
            }
            out += 64;
        }
}

static u32 tex_checker(u32 x, u32 y) {
    bool on = ((x / 8) ^ (y / 8)) & 1;
    u32 r = on ? 230 : 30, g = (x * 255) / 63, b = (y * 255) / 63;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}
static u32 tex_alpha(u32 x, u32 y) {
    float dx = (float)x - 31.5f, dy = (float)y - 31.5f;
    float d = sqrtf(dx * dx + dy * dy) / 32.0f;
    u32 a = d >= 1.0f ? 0 : (u32)((1.0f - d) * 255.0f);
    return (a << 24) | 0x00FFC040u;
}
static u32 tex_stripes(u32 x, u32 y) {
    (void)y;
    return (x & 1) ? 0xFFFFFFFFu : 0xFF000000u;   /* fine stripes: shows mipmapping */
}

/* --------------------------------------------------------- GX setup --- */
static void set_identity_matrix(u32 base) {
    float m[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    for (int i = 0; i < 12; i++) xff(base + (u32)i, m[i]);
}

/* Orthographic: x 0..640, y 0..480 (down), z 0..-1. */
static void ortho(void) {
    float p[6] = {2.0f / 640.0f, -1.0f, -2.0f / 480.0f, 1.0f, 1.0f, 0.0f};
    for (int i = 0; i < 6; i++) xff(0x1020 + (u32)i, p[i]);
    xf1(0x1026, 1);
}

/* Perspective, 60 degree vertical field of view, 4:3, near 1, far 100. */
static void perspective(void) {
    float f = 1.0f / tanf(30.0f * 3.14159265f / 180.0f), n = 1.0f, fa = 100.0f;
    float p[6] = {f / (4.0f / 3.0f), 0.0f, f, 0.0f, -n / (fa - n), -(fa * n) / (fa - n)};
    for (int i = 0; i < 6; i++) xff(0x1020 + (u32)i, p[i]);
    xf1(0x1026, 0);
}

static void viewport(void) {
    xff(0x101A, 320.0f); xff(0x101B, -240.0f); xff(0x101C, 16777215.0f);
    xff(0x101D, 320.0f + 342.0f); xff(0x101E, 240.0f + 342.0f); xff(0x101F, 16777215.0f);
}

/* Vertex format 0: f32 position xyz, RGBA8 colour 0, f32 texcoord 0. */
static void vertex_format(void) {
    cp(0x50, (1u << 9) | (1u << 13));
    cp(0x60, 1u);
    cp(0x70, 1u | (4u << 1) | (1u << 13) | (5u << 14) | (1u << 21) | (4u << 22));
    cp(0x80, 0); cp(0x90, 0);
}

static void reset_state(void) {
    xf1(0x1009, 1);                    /* one colour channel */
    xf1(0x100E, 1); xf1(0x1010, 1);    /* material from the vertex colour, no lighting */
    xf1(0x103F, 1);                    /* one texture coordinate */
    xf1(0x1040, 5u << 7);              /* texgen 0 from texcoord 0, regular */
    xf1(0x1018, 0); xf1(0x1019, 0);
    set_identity_matrix(0);
    viewport();
    ortho();
    vertex_format();
    for (u32 t = 0; t < 4; t++) {      /* identity swap tables */
        bp(0xF6 + t * 2, (1u << 2));
        bp(0xF7 + t * 2, 2u | (3u << 2));
    }
    bp(0x20, (342u) | (342u << 12));                 /* scissor: whole screen */
    bp(0x21, (342u + 479u) | ((342u + 639u) << 12));
    bp(0x43, 0);                                       /* RGB8 / Z24 */
    bp(0x40, 1u | (3u << 1) | (1u << 4));              /* depth: test LEQUAL, write */
    bp(0x41, (1u << 3) | (1u << 4));                   /* colour + alpha update, no blending */
    bp(0xF3, (7u << 16) | (7u << 19));                 /* alpha test always */
    bp(0x00, 1u | (1u << 4));                          /* 1 texgen, 1 colour, 1 stage, no culling */
}

/* One stage: texture map 0 x colour 0 (GXSetTevOp MODULATE), or colour only. */
static void tev_modulate(int stage, bool tex) {
    u32 order = (tex ? (1u << 6) : 0) | (0u << 7);     /* map 0, texcoord 0, colour channel 0 */
    u32 reg = 0x28 + (u32)stage / 2;
    u32 shift = (stage & 1) ? 12 : 0;
    bp(reg, (gx_bp[reg] & ~(0xFFFu << shift)) | (order << shift));
    if (tex) {
        bp(0xC0 + stage * 2, 0xF8AFu | (1u << 19));
        bp(0xC1 + stage * 2, ((7u << 13) | (4u << 10) | (5u << 7) | (7u << 4)) | (1u << 19));
    } else {
        bp(0xC0 + stage * 2, 0xFFFAu | (1u << 19));
        bp(0xC1 + stage * 2, ((7u << 13) | (7u << 10) | (7u << 7) | (5u << 4)) | (1u << 19));
    }
}

static void texture(u32 map, u32 addr, u32 w, u32 h, u32 min_filter, bool mag_linear, u32 wrap, u32 max_lod16) {
    u32 bank = map < 4 ? 0u : 0x20u, i = map & 3u;
    static const u32 k_hw[6] = {0, 4, 1, 5, 2, 6};   /* GX API filter -> register value */
    bp(0x80 + bank + i, wrap | (wrap << 2) | ((mag_linear ? 1u : 0) << 4) | (k_hw[min_filter] << 5));
    bp(0x84 + bank + i, (max_lod16 & 0xFFu) << 8);
    bp(0x88 + bank + i, (w - 1) | ((h - 1) << 10) | (6u << 20));   /* RGBA8 */
    bp(0x8C + bank + i, 0);
    bp(0x94 + bank + i, addr >> 5);
}

static void vtx(float x, float y, float z, u32 rgba, float s, float t) {
    wf(x); wf(y); wf(z); w32(rgba); wf(s); wf(t);
}

static void quad(float x0, float y0, float x1, float y1, float z, u32 c0, u32 c1, u32 c2, u32 c3) {
    w8(0x80); w8(0); w8(4);
    vtx(x0, y0, z, c0, 0, 0); vtx(x1, y0, z, c1, 1, 0); vtx(x1, y1, z, c2, 1, 1); vtx(x0, y1, z, c3, 0, 1);
}

static void clear_and_copy(bool clear_only) {
    bp(0x49, 0); bp(0x4A, 639u | (479u << 10));
    bp(0x4B, XFB_ADDR >> 5); bp(0x4D, (640u * 2u) >> 5); bp(0x4E, 0x100);
    bp(0x4F, (0xFFu << 8) | 0x20u); bp(0x50, (0x30u << 8) | 0x40u); bp(0x51, 0xFFFFFFu);
    bp(0x52, clear_only ? (1u << 11) : (1u << 14) | (1u << 11));
}

/* ------------------------------------------------------------ scenes --- */
static void scene_colours(void) {
    tev_modulate(0, false);
    quad(40, 40, 600, 440, -0.5f, 0xFF0000FFu, 0x00FF00FFu, 0x0000FFFFu, 0xFFFFFFFFu);
    /* overlapping triangles, depth tested */
    w8(0x90); w8(0); w8(6);
    vtx(100, 100, -0.2f, 0xFFFF00FFu, 0, 0); vtx(500, 120, -0.2f, 0xFFFF00FFu, 0, 0); vtx(300, 400, -0.8f, 0xFF00FFFFu, 0, 0);
    vtx(120, 380, -0.3f, 0x00FFFFFFu, 0, 0); vtx(300, 60, -0.3f, 0x00FFFFFFu, 0, 0); vtx(520, 380, -0.3f, 0x00FFFFFFu, 0, 0);
}

static void scene_texture(void) {
    put_rgba8(TEX_ADDR, 64, 64, tex_checker);
    texture(0, TEX_ADDR, 64, 64, 1, true, 1, 0);
    tev_modulate(0, true);
    quad(64, 48, 576, 432, -0.5f, 0xFFFFFFFFu, 0xFF8080FFu, 0x80FF80FFu, 0x8080FFFFu);
}

static void scene_nearest_repeat(void) {
    put_rgba8(TEX_ADDR, 64, 64, tex_checker);
    texture(0, TEX_ADDR, 64, 64, 0, false, 1, 0);
    tev_modulate(0, true);
    w8(0x80); w8(0); w8(4);           /* texcoords 0..3: repeats */
    vtx(32, 32, -0.5f, 0xFFFFFFFFu, 0, 0); vtx(608, 32, -0.5f, 0xFFFFFFFFu, 3, 0);
    vtx(608, 448, -0.5f, 0xFFFFFFFFu, 3, 2); vtx(32, 448, -0.5f, 0xFFFFFFFFu, 0, 2);
}

static void scene_blend(void) {
    scene_colours();
    put_rgba8(TEX2_ADDR, 64, 64, tex_alpha);
    texture(0, TEX2_ADDR, 64, 64, 1, true, 0, 0);
    tev_modulate(0, true);
    bp(0x41, 1u | (1u << 3) | (1u << 4) | (5u << 5) | (4u << 8));   /* src alpha, 1 - src alpha */
    bp(0x40, 0);
    quad(160, 80, 480, 400, -0.1f, 0xFFFFFFFFu, 0xFFFFFFC0u, 0xFFFFFF80u, 0xFFFFFFFFu);
}

static void scene_alpha_test(void) {
    scene_colours();
    put_rgba8(TEX2_ADDR, 64, 64, tex_alpha);
    texture(0, TEX2_ADDR, 64, 64, 1, true, 0, 0);
    tev_modulate(0, true);
    bp(0xF3, 128u | (4u << 16) | (7u << 19));        /* alpha > 128 */
    quad(160, 80, 480, 400, -0.1f, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu);
}

static void scene_tev(void) {
    /* Two stages: stage 0 = texture x colour into REG0; stage 1 =
       lerp(REG0, konst colour, half) + a little, alpha from konst. */
    put_rgba8(TEX_ADDR, 64, 64, tex_checker);
    texture(0, TEX_ADDR, 64, 64, 1, true, 1, 0);
    bp(0x00, 1u | (1u << 4) | (1u << 10));
    bp(0x28, (1u << 6) | ((7u << 7) << 12) | (0u << 7));
    bp(0xC0, 0xF8AFu | (1u << 19) | (1u << 22));                         /* -> REG0 */
    bp(0xC1, ((7u << 13) | (4u << 10) | (5u << 7) | (7u << 4)) | (1u << 19) | (1u << 22));
    /* stage 1: a = REG0 (2), b = KONST (14), c = HALF (13), d = ZERO, bias +0.5? no: scale 2, clamp */
    bp(0xC2, (2u << 12) | (14u << 8) | (13u << 4) | 15u | (1u << 19));
    bp(0xC3, ((7u << 13) | (7u << 10) | (7u << 7) | (6u << 4)) | (1u << 19));
    bp(0xE7, (1u << 23) | 0x40u | (0x80u << 12));    /* K3 blue/green */
    bp(0xE6, (1u << 23) | 0xC0u | (0xFFu << 12));    /* K3 red/alpha */
    bp(0xF6, (1u << 2) | (0x0Fu << 14) | (0x1Fu << 19));   /* stage 1 konst colour K3, alpha K3_A */
    quad(64, 48, 576, 432, -0.5f, 0xFFFFFFFFu, 0xFF0000FFu, 0x00FF00FFu, 0x0000FFFFu);
}

static void scene_cull_scissor(void) {
    tev_modulate(0, false);
    bp(0x00, 1u | (1u << 4) | (2u << 14));           /* cull back */
    bp(0x20, (342u + 60u) | ((342u + 80u) << 12));   /* scissor 80,60 - 560,420 */
    bp(0x21, (342u + 420u) | ((342u + 560u) << 12));
    w8(0x90); w8(0); w8(6);
    /* one winding each way: only one survives */
    vtx(40, 40, -0.5f, 0xFF0000FFu, 0, 0); vtx(600, 40, -0.5f, 0xFF0000FFu, 0, 0); vtx(40, 440, -0.5f, 0xFF0000FFu, 0, 0);
    vtx(600, 440, -0.5f, 0x00FF00FFu, 0, 0); vtx(600, 40, -0.5f, 0x00FF00FFu, 0, 0); vtx(40, 440, -0.5f, 0x00FF00FFu, 0, 0);
}

static void scene_perspective(void) {
    perspective();
    put_rgba8(TEX_ADDR, 64, 64, tex_checker);
    texture(0, TEX_ADDR, 64, 64, 1, true, 1, 0);
    tev_modulate(0, true);
    /* a floor running into the distance, crossing the near plane */
    w8(0x80); w8(0); w8(4);
    vtx(-20, -2, 2, 0xFFFFFFFFu, 0, 0); vtx(20, -2, 2, 0xFFFFFFFFu, 8, 0);
    vtx(20, -2, -80, 0xFFFFFFFFu, 8, 20); vtx(-20, -2, -80, 0xFFFFFFFFu, 0, 20);
    /* a wall */
    w8(0x80); w8(0); w8(4);
    vtx(-3, -2, -10, 0xFF8040FFu, 0, 0); vtx(3, -2, -10, 0xFF8040FFu, 1, 0);
    vtx(3, 3, -14, 0x40FF80FFu, 1, 1); vtx(-3, 3, -14, 0x40FF80FFu, 0, 1);
}

static void scene_mipmap(void) {
    perspective();
    put_rgba8(TEX_ADDR, 64, 64, tex_stripes);
    texture(0, TEX_ADDR, 64, 64, 5, true, 1, 6 << 4);   /* LIN_MIP_LIN, max lod 6 */
    tev_modulate(0, true);
    w8(0x80); w8(0); w8(4);
    vtx(-20, -2, -1.5f, 0xFFFFFFFFu, 0, 0); vtx(20, -2, -1.5f, 0xFFFFFFFFu, 16, 0);
    vtx(20, -2, -80, 0xFFFFFFFFu, 16, 40); vtx(-20, -2, -80, 0xFFFFFFFFu, 0, 40);
}

static void scene_copy_to_texture(void) {
    /* Draw, copy a 128x128 region into an RGB565 texture, clear, then draw
       the copy back stretched (how the game does screen transitions). */
    scene_texture();
    bp(0x49, 200u | (150u << 10)); bp(0x4A, 127u | (127u << 10));
    bp(0x4B, COPY_ADDR >> 5); bp(0x4D, (128u / 4u * 32u) >> 5);
    bp(0x4F, 0); bp(0x50, 0); bp(0x51, 0xFFFFFFu);
    bp(0x52, (8u << 3) | (1u << 11));                  /* format RGB565 (copy format code 8), clear */
    bp(0x88, 127u | (127u << 10) | (4u << 20));       /* map 0: the copy, RGB565 */
    bp(0x80, 1u << 4 | (1u << 5));
    bp(0x94, COPY_ADDR >> 5);
    tev_modulate(0, true);
    quad(20, 20, 620, 460, -0.5f, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu);
}

/* A copy of the 256x192 middle of the picture into a texture of format
   `fmt` (copy format code), optionally halved or as intensity, then the
   copy drawn back over the whole screen. tex_fmt: how it is read. */
static void copy_scene(u32 fmt, bool half, bool intensity, u32 tex_fmt, u32 tile_w, u32 tile_bytes) {
    scene_texture();
    put_rgba8(TEX2_ADDR, 64, 64, tex_alpha);           /* some alpha in the frame too */
    u32 w = 256, h = 192, ow = half ? w / 2 : w, oh = half ? h / 2 : h;
    u32 tpf = ((fmt & 7u) << 1) | (fmt >> 3);
    bp(0x49, 192u | (144u << 10)); bp(0x4A, (w - 1) | ((h - 1) << 10));
    bp(0x4B, COPY_ADDR >> 5); bp(0x4D, (((ow + tile_w - 1) / tile_w) * tile_bytes) >> 5);
    bp(0x4F, 0); bp(0x50, 0); bp(0x51, 0xFFFFFFu);
    bp(0x52, (tpf << 3) | (half ? 1u << 9 : 0) | (intensity ? 1u << 15 : 0) | (1u << 11));
    bp(0x88, (ow - 1) | ((oh - 1) << 10) | (tex_fmt << 20));
    bp(0x80, (1u << 4) | (1u << 5));
    bp(0x94, COPY_ADDR >> 5);
    tev_modulate(0, true);
    quad(20, 20, 620, 460, -0.5f, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu);
}
static void scene_copy_rgba8(void) { copy_scene(0x6, false, false, 6, 4, 64); }
static void scene_copy_rgb5a3(void) { copy_scene(0x5, false, false, 5, 4, 32); }
static void scene_copy_i8(void) { copy_scene(0x8, false, true, 1, 8, 32); }
static void scene_copy_half565(void) { copy_scene(0x4, true, false, 4, 4, 32); }

/* Far plane at 100: the floor runs to 300 and must stop there; the red
   wall at 150 lies wholly beyond it and must not be drawn. */
static void scene_far_clip(void) {
    perspective();
    put_rgba8(TEX_ADDR, 64, 64, tex_checker);
    texture(0, TEX_ADDR, 64, 64, 1, true, 1, 0);
    tev_modulate(0, true);
    w8(0x80); w8(0); w8(4);
    vtx(-60, -2, -2, 0xFFFFFFFFu, 0, 0); vtx(60, -2, -2, 0xFFFFFFFFu, 12, 0);
    vtx(60, -2, -300, 0xFFFFFFFFu, 12, 60); vtx(-60, -2, -300, 0xFFFFFFFFu, 0, 60);
    tev_modulate(0, false);
    w8(0x80); w8(0); w8(4);
    vtx(-40, -2, -150, 0xFF0000FFu, 0, 0); vtx(40, -2, -150, 0xFF0000FFu, 0, 0);
    vtx(40, 60, -150, 0xFF0000FFu, 0, 0); vtx(-40, 60, -150, 0xFF0000FFu, 0, 0);
}

static void scene_lit(void);

typedef struct { const char* name; void (*fn)(void); } Scene;
static const Scene k_scenes[] = {
    {"colours", scene_colours},       {"texture", scene_texture},   {"nearest_repeat", scene_nearest_repeat},
    {"blend", scene_blend},           {"alpha_test", scene_alpha_test}, {"tev_2stage", scene_tev},
    {"cull_scissor", scene_cull_scissor}, {"perspective", scene_perspective}, {"mipmap", scene_mipmap},
    {"copy_to_texture", scene_copy_to_texture}, {"copy_rgba8", scene_copy_rgba8}, {"copy_rgb5a3", scene_copy_rgb5a3},
    {"copy_i8", scene_copy_i8},       {"copy_half565", scene_copy_half565}, {"far_clip", scene_far_clip},
    {"lit", scene_lit},
};

/* ---------------------------------------------------------- benchmark --- */
/* "bench": game-thread cost of recording a heavy field-like frame: 7000
   small skinned, lit, textured draws (16-bit indexed arrays, a matrix index
   per vertex, a matrix-index register write before each draw), 60 frames. */
#include <time.h>
#define ARR_POS 0x00200000u
#define ARR_NRM 0x00210000u
#define ARR_TC 0x00220000u

static double thread_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void put_f32(u32 addr, float f) {
    u32 u; memcpy(&u, &f, 4);
    g_cpu.ram[addr] = (u8)(u >> 24); g_cpu.ram[addr + 1] = (u8)(u >> 16);
    g_cpu.ram[addr + 2] = (u8)(u >> 8); g_cpu.ram[addr + 3] = (u8)u;
}

static void bench_setup(void) {
    for (u32 i = 0; i < 4096; i++) {
        float a = (float)i * 0.01f;
        put_f32(ARR_POS + i * 12, 100.0f + 200.0f * cosf(a)); put_f32(ARR_POS + i * 12 + 4, 240.0f + 150.0f * sinf(a * 1.3f));
        put_f32(ARR_POS + i * 12 + 8, -0.5f);
        put_f32(ARR_NRM + i * 12, 0); put_f32(ARR_NRM + i * 12 + 4, 0.6f); put_f32(ARR_NRM + i * 12 + 8, 0.8f);
        put_f32(ARR_TC + i * 8, a); put_f32(ARR_TC + i * 8 + 4, a * 0.5f);
    }
    put_rgba8(TEX_ADDR, 64, 64, tex_checker);
    texture(0, TEX_ADDR, 64, 64, 1, true, 1, 0);
    tev_modulate(0, true);
    cp(0xA0, ARR_POS); cp(0xB0, 12); cp(0xA1, ARR_NRM); cp(0xB1, 12); cp(0xA4, ARR_TC); cp(0xB4, 8);
    cp(0x50, 1u | (3u << 9) | (3u << 11));               /* matrix index, position and normal by 16-bit index */
    cp(0x60, 3u);                                        /* texcoord 0 by 16-bit index */
    cp(0x70, 1u | (4u << 1) | (4u << 10) | (1u << 21) | (4u << 22));
    for (u32 m = 0; m < 10; m++) {                       /* ten bone matrices and their normal matrices */
        float t = (float)m * 0.05f;
        float mx[12] = {cosf(t), -sinf(t), 0, (float)m * 3, sinf(t), cosf(t), 0, 0, 0, 0, 1, 0};
        for (int k = 0; k < 12; k++) xff(m * 12 + (u32)k, mx[k]);
        float nm[9] = {cosf(t), -sinf(t), 0, sinf(t), cosf(t), 0, 0, 0, 1};
        for (int k = 0; k < 9; k++) xff(0x400 + m * 9 + (u32)k, nm[k]);
    }
    xf1(0x100E, 2u | (1u << 2) | (2u << 7) | (2u << 9));   /* lit by light 0, material register */
    xf1(0x100C, 0xC0C0C0FFu); xf1(0x100A, 0x404040FFu);
    xf1(0x603, 0xFFFFFFFFu); xff(0x60A, 100); xff(0x60B, 200); xff(0x60C, 300);
}

static bool g_bench_switch;

static void bench(int frames, int draws) {
    bench_setup();
    put_rgba8(TEX2_ADDR, 64, 64, tex_alpha);
    double t0 = thread_seconds();
    u32 vi = 0;
    for (int f = 0; f < frames; f++) {
        for (int d = 0; d < draws; d++) {
            xf1(0x1018, (u32)(d % 10) * 3u);             /* the model's matrix (as the game does per draw) */
            if (g_bench_switch) {                        /* a different texture and konst colour per draw */
                bp(0x94, (d & 1 ? TEX2_ADDR : TEX_ADDR) >> 5);
                bp(0xE6, (1u << 23) | (u32)(d & 255) | (0x80u << 12));
            }
            w8(0x98); w8(0); w8(6);
            for (int v = 0; v < 6; v++, vi = (vi + 7) & 4095) {
                w8((u8)(((d + v) % 10) * 3));
                w8((u8)(vi >> 8)); w8((u8)vi);
                w8((u8)(vi >> 8)); w8((u8)vi);
                w8((u8)(vi >> 8)); w8((u8)vi);
            }
        }
        clear_and_copy(false);
    }
    gx_raster_sync();
    double t = thread_seconds() - t0;
    printf("bench: %d frames x %d draws: %.1f ms of game-thread time per frame (%.0f ns per vertex)\n", frames, draws,
           t / frames * 1000.0, t / ((double)frames * draws * 6) * 1e9);
}

/* Lit, skinned, indexed: two lights (one spot, one plain diffuse), the
   second colour channel lit too, vertex colours as material. */
static void scene_lit(void) {
    bench_setup();
    xf1(0x1009, 2);
    xf1(0x100E, 1u | 2u | (3u << 2) | (2u << 7) | (2u << 9));         /* ch0 colour: vertex material, lights 0+1 */
    xf1(0x1010, 2u | (1u << 2) | (1u << 7) | (1u << 9));               /* ch0 alpha: lit, spot */
    xf1(0x100F, 2u | 0x40u | (2u << 2) | (2u << 7) | (2u << 9));       /* ch1 colour: ambient from vertex */
    xf1(0x1011, 0);
    xf1(0x613, 0xFF8040FFu); xff(0x61A, -200); xff(0x61B, 100); xff(0x61C, 50);
    xff(0x604, 0.2f); xff(0x605, 0.5f); xff(0x606, 0.3f); xff(0x607, 1.0f); xff(0x608, 0.001f); xff(0x609, 0.00001f);
    xff(0x60D, 0.0f); xff(0x60E, 0.0f); xff(0x60F, -1.0f);
    cp(0x50, 1u | (3u << 9) | (3u << 11) | (1u << 13));                 /* + direct colour 0 */
    cp(0x70, 1u | (4u << 1) | (4u << 10) | (1u << 13) | (5u << 14) | (1u << 21) | (4u << 22));
    u32 vi = 0;
    for (int d = 0; d < 40; d++) {
        xf1(0x1018, (u32)(d % 10) * 3u);
        w8(0x98); w8(0); w8(6);
        for (int v = 0; v < 6; v++, vi = (vi + 37) & 4095) {
            w8((u8)(((d + v) % 10) * 3));
            w8((u8)(vi >> 8)); w8((u8)vi);
            w8((u8)(vi >> 8)); w8((u8)vi);
            w32(0xFF000000u | (vi * 2654435761u >> 8));
            w8((u8)(vi >> 8)); w8((u8)vi);
        }
    }
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: gpu_compare soft|gpu|present OUTDIR [scale]\n");
        return 2;
    }
    if (strcmp(argv[1], "bench") == 0) {
        cpu_init(&g_cpu);
        gx_raster_render_all();
        gx_bp[0xFE] = 0x00FFFFFFu;
        reset_state();
        g_bench_switch = argc > 4 && strcmp(argv[4], "switch") == 0;
        if (argc > 5 && strcmp(argv[5], "gpu") == 0) {
            if (!egl_init() || !gx_raster_use_gpu(getproc, 1, make_current)) return 1;
            egl_make(g_dpy, NULL, NULL, NULL);
        }
        bench(argc > 3 ? atoi(argv[3]) : 30, 7000);
        return 0;
    }
    bool present = strcmp(argv[1], "present") == 0;
    bool gpu = present || strcmp(argv[1], "gpu") == 0;
    int scale = argc > 3 ? atoi(argv[3]) : 1;
    cpu_init(&g_cpu);
    gx_raster_render_all();
    if (gpu) {
        if (!egl_init()) { fprintf(stderr, "no EGL context\n"); return 1; }
        if (!gx_raster_use_gpu(getproc, scale, make_current)) return 1;
        egl_make(g_dpy, NULL, NULL, NULL);   /* release: the renderer's GPU thread uses it */
    }
    GLuint shot_fbo = 0;
    if (present) {
        /* The window side: the second context, drawing into a 640x480 target. */
        egl_make(g_dpy, NULL, NULL, g_ctx2);
        if (!glr_present_init()) return 1;
        GLuint tex;
        glr_glGenTextures(1, &tex);
        glr_glBindTexture(GL_TEXTURE_2D, tex);
        glr_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 640, 480, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
        glr_glGenFramebuffers(1, &shot_fbo);
        glr_glBindFramebuffer(GL_FRAMEBUFFER, shot_fbo);
        glr_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        glr_set_present_fbo(shot_fbo);
        gx_raster_present_from_gpu(true);
    }
    if (getenv("FREECAM")) {           /* tests: "tx ty tz yaw" in view space */
        float tx = 0, ty = 0, tz = 0, yaw = 0;
        sscanf(getenv("FREECAM"), "%f %f %f %f", &tx, &ty, &tz, &yaw);
        float c = cosf(yaw), sn = sinf(yaw);
        float m[12] = {c, 0, -sn, -(c * tx - sn * tz), 0, 1, 0, -ty, sn, 0, c, -(sn * tx + c * tz)};
        gx_raster_set_free_camera(true, m);
    }
    if (getenv("HIRES")) hires_init(getenv("HIRES"));            /* tests: custom textures */
    if (getenv("HIRES_DUMP")) hires_set_dump_dir(getenv("HIRES_DUMP"));
    for (size_t i = 0; i < sizeof(k_scenes) / sizeof(k_scenes[0]); i++)
      for (int pass = getenv("HIRES") ? 0 : 1; pass < 2; pass++) {   /* custom textures load in the background */
        if (pass == 1 && getenv("HIRES")) usleep(300000);
        memset(gx_bp, 0, sizeof(gx_bp));
        gx_bp[0xFE] = 0x00FFFFFFu;
        memset(gx_xf_reg, 0, sizeof(gx_xf_reg));
        gx_state_serial++;
        reset_state();
        clear_and_copy(true);
        k_scenes[i].fn();
        clear_and_copy(false);         /* to the TV buffer, then clear */
        gx_raster_sync();
        char path[512];
        if (present) {
            /* what the window would show: 640x480, top row first, 0xAARRGGBB */
            static u32 px[640 * 480], flipped[640 * 480];
            gx_raster_present_gpu(&g_cpu, 0x10000000u | (XFB_ADDR >> 5), (40u << 8) | 80u, 240u << 4, 640, 480,
                                  4.0f / 3.0f, NULL);
            if (getenv("OVERLAY")) {   /* tests: a half-transparent red block in the top left quarter */
                static u32 ov[640 * 480];
                for (int y = 0; y < 480; y++)
                    for (int x = 0; x < 640; x++) ov[y * 640 + x] = (x < 320 && y < 240) ? 0x80800000u : 0;
                glr_present_overlay(ov, 640, 480, true);
            }
            glr_glBindFramebuffer(GL_FRAMEBUFFER, shot_fbo);
            glr_glReadPixels(0, 0, 640, 480, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, px);
            for (int y = 0; y < 480; y++) memcpy(flipped + y * 640, px + (479 - y) * 640, 640 * 4);
            snprintf(path, sizeof(path), "%s/%s.rgb", argv[2], k_scenes[i].name);
            FILE* f = fopen(path, "wb");
            if (f) { fwrite(flipped, 4, 640 * 480, f); fclose(f); }
            continue;
        }
        snprintf(path, sizeof(path), "%s/%s.yuv", argv[2], k_scenes[i].name);
        FILE* f = fopen(path, "wb");
        if (f) {
            fwrite(g_cpu.ram + XFB_ADDR, 1, 640 * 480 * 2, f);
            fclose(f);
        }
      }
    gx_raster_report();
    return 0;
}
