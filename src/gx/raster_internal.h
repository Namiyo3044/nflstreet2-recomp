#ifndef NFLS2_GX_RASTER_INTERNAL_H
#define NFLS2_GX_RASTER_INTERNAL_H

/* Shared between the frame recorder (raster.c), which decodes and
   transforms vertices into a per-frame command list, and the two backends
   that draw such a list: the software rasteriser (raster.c) and the
   graphics-card one (raster_gl.c). */
#include "cpu/cpu.h"
#include "gx/texture.h"

#define EFB_W 640
#define EFB_H 528

/* The emulated embedded frame buffer at native size. The software backend
   draws into it; the GPU backend reads its picture back into it when a copy
   needs the pixels in memory. */
extern u32 g_color[EFB_W * EFB_H];   /* 0xAARRGGBB */
extern u32 g_depth[EFB_W * EFB_H];   /* 24-bit, larger = farther */

/* ---------------------------------------------------------- vertices --- */
/* A vertex as decoded from the command stream, before transforming. */
typedef struct {
    float pos[3];
    u32 pos_mtx;                 /* row index into transform memory */
    u32 tex_mtx[8];
    u32 color[2];                /* 0xRRGGBBAA */
    bool has_color[2];
    float tc[8][2];              /* texture coordinates as given */
    bool has_tc[8];
    float nrm[3];                /* normal as given (0,0,1 if none) */
} Vtx;

#define MAX_TC 8

typedef struct {
    float x, y, z;              /* screen x, y; depth 0..16777215 */
    float inv_w;                /* 1/w, for perspective-correct interpolation */
    float col[2][4];            /* colour channels 0/1, RGBA 0..1 */
    float tc[MAX_TC][3];        /* s, t, q (q = 1 unless projected) */
    float clip[4];              /* clip-space x, y, z, w (before the divide) */
    float near_d;               /* distance inside the near plane (z + w); < 0 = in front of it */
    float far_d;                /* distance inside the far plane (-z); < 0 = beyond it */
    bool valid;
} SVtx;

/* ------------------------------------------------------- draw state --- */
/* TEV inputs are read from a table of rows (RGBA floats):
     0-3  PREV, REG0, REG1, REG2   (reset from the start values per pixel)
     4    texture colour of the current stage (after swizzle)
     5    rasterised colour of the current stage (after swizzle)
     6-8  constants 1, 0.5, 0
     9+   konstant colour / alpha for each stage (two rows per stage) */
enum { ROW_PREV = 0, ROW_TEX = 4, ROW_RAS = 5, ROW_ONE = 6, ROW_HALF = 7, ROW_ZERO = 8, ROW_KONST = 9,
       ROW_COUNT = ROW_KONST + 32 };

typedef struct {
    u8 ci[4];                          /* colour input rows a, b, c, d */
    bool cbc[4];                       /* input is an alpha broadcast to RGB */
    u8 ai[4];                          /* alpha input rows a, b, c, d (A component) */
    u8 cdst, adst;
    bool csub, asub, cclamp, aclamp, ccmp, acmp;
    float cbias, abias, cscale, ascale;
    bool tex_on;
    u32 efb_copy;                      /* GPU: id of a frame-buffer copy held on the GPU to use instead (0 = none) */
    u8 texcoord, chan;
    u8 tsw[4], rsw[4];
    const u32* texels;                 /* decoded texture (all mip levels); valid until the frame is drawn */
    const GxHires* hires;              /* GPU: custom replacement to draw instead (never freed) */
    u32 tw, th, wrap_s, wrap_t;
    u32 levels;
    u32 lw[GX_TEX_MAX_LEVELS], lh[GX_TEX_MAX_LEVELS], loff[GX_TEX_MAX_LEVELS];
    bool mag_linear;
    u8 min_filter;                     /* GX_NEAR .. GX_LIN_MIP_LIN */
    float lod_bias, min_lod, max_lod;
} Stage;

enum { FAST_NONE, FAST_MODULATE, FAST_REPLACE, FAST_PASSCLR };

typedef struct {
    u32 stages;
    u32 fast;                          /* one-stage standard modes (GXSetTevOp) */
    Stage st[16];
    float reg_init[4][4];
    float konst[32][4];
    u32 at_ref0, at_ref1, at_f0, at_f1, at_logic;
    bool at_always;
    /* rasteriser and pixel engine */
    int sx0, sy0, sx1, sy1;            /* scissor, inclusive */
    bool ztest, zupdate, blend, blend_alpha, subtract, color_update, alpha_update;
    u32 zfunc, sfac, dfac, ntex;
    u32 cull;                          /* 0 none, 1 front (counter-clockwise on screen), 2 back, 3 all */
} RenderState;

/* ----------------------------------------------------- command lists --- */
typedef struct {
    u32 cmd, sx, sy, w, h;             /* copy command and source rectangle */
    u32 dest, stride, lines;           /* to_xfb: destination in RAM */
    float yscale;
    bool to_xfb, clear;
    u32 clear_color, clear_z;
} CopyCmd;

typedef struct {
    u32 type;                          /* CMD_TRI or CMD_COPY */
    u32 state;                         /* RenderState index (triangles) */
    union {
        u32 vi[3];                     /* triangle: indices into the list's vertex pool */
        CopyCmd copy;
        struct { u32 prim, count, first; } draw;   /* deferred draw: decoded vertices in raw[] */
        struct { u32 addr, n, off; } xf;           /* transform unit writes: n words from xfdata[off] */
    } u;
} Cmd;

/* CMD_DRAW and CMD_XF only appear when transforming is deferred to the GPU
   thread (GPU renderer): the game thread records decoded vertices and the
   transform unit writes in stream order, and the GPU thread replays the
   writes and transforms, lights and clips each draw before drawing it. */
enum { CMD_TRI, CMD_COPY, CMD_DRAW, CMD_XF };

typedef struct {
    Cmd* cmds;
    u32 n, cap;
    RenderState* states;
    u32 ns, cap_s;
    SVtx* verts;                       /* transformed vertices, shared by the triangles */
    u32 nv, cap_v;
    Vtx* raw;                          /* deferred: decoded vertices */
    u32 nraw, cap_raw;
    u32* xfdata;                       /* deferred: values of transform unit writes */
    u32 nxf, cap_xf;
} CmdList;

/* A frame-buffer copy into a texture, as the GPU backend redoes it at its
   own resolution: source rectangle, output size and the copy's format. */
typedef struct {
    u32 id;                            /* matches Stage.efb_copy */
    u32 sx, sy, src_w, src_h;          /* source rectangle in the frame buffer (native pixels) */
    u32 w, h;                          /* texture size (half the source when halved) */
    u32 fmt;                           /* copy format (0-0xC, as efb_copy_to_texture) */
    bool intensity, keep_alpha;
} TexCopy;

/* Writes an EFB-to-TV-buffer copy into RAM (YUYV) from g_color. */
void raster_xfb_write(const CopyCmd* cp, int y0, int y1);

#endif
