#include "gx/raster_gl.h"
#include "gx/gl_min.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GLR_DEFINE(name, ret, args) PFN_##name glr_##name;
GLR_FUNCS(GLR_DEFINE)
#undef GLR_DEFINE

#define G(name) glr_##name

static int g_scale = 1;
static volatile int g_want_scale;      /* a new internal resolution asked for (0: none) */
static GLint g_maxtex;
/* Horizontal scale: a third more than the vertical one, so a widescreen
   picture (which the game squeezes into the 640-pixel frame for the TV to
   stretch) keeps its full horizontal detail when stretched back to 16:9.
   In 4:3 the extra width is folded back down (horizontal anti-aliasing). */
static float g_sx = 1.0f;
static int g_efb_w = EFB_W, g_efb_h = EFB_H;   /* frame buffer size in pixels */

static int PX(u32 x) { return (int)((float)x * g_sx + 0.5f); }   /* native x to frame-buffer pixels */
static void (*g_make_current)(void);
static bool g_ready;
static GLuint g_prog, g_vao, g_vbo, g_ibo, g_ubo;
static GLint g_u_solid;
static GLuint g_fbo, g_color_tex, g_depth_tex;           /* the frame buffer at scale x native */
static GLuint g_fbo_small, g_color_small, g_depth_small; /* native size, for read-back when scaled */
static GLuint g_samplers[16];
static GLint g_ubo_align = 256;
static unsigned long long g_draw_calls, g_lists, g_readbacks, g_uploads, g_hires_uploads;

/* ------------------------------------------------------------ shaders --- */
static const char* k_vs =
    "#version 330 core\n"
    "layout(location = 0) in vec4 a_pos;\n"     /* screen x, y, depth 0..16777215, 1/w */
    "layout(location = 1) in vec4 a_col0;\n"
    "layout(location = 2) in vec4 a_col1;\n"
    "layout(location = 3) in vec3 a_tc0;\n"
    "layout(location = 4) in vec3 a_tc1;\n"
    "layout(location = 5) in vec3 a_tc2;\n"
    "layout(location = 6) in vec3 a_tc3;\n"
    "layout(location = 7) in vec3 a_tc4;\n"
    "layout(location = 8) in vec3 a_tc5;\n"
    "layout(location = 9) in vec3 a_tc6;\n"
    "layout(location = 10) in vec3 a_tc7;\n"
    "out vec4 v_col0;\n"
    "out vec4 v_col1;\n"
    "out vec3 v_tc[8];\n"
    "void main() {\n"
    "    float w = 1.0 / a_pos.w;\n"
    /* Screen y runs down and frame-buffer row 0 is the top, as in the
       software frame buffer, so read-backs need no flipping. */
    "    vec3 ndc = vec3(a_pos.x * (2.0 / 640.0) - 1.0, a_pos.y * (2.0 / 528.0) - 1.0,\n"
    "                    a_pos.z * (2.0 / 16777215.0) - 1.0);\n"
    "    gl_Position = vec4(ndc * w, w);\n"
    "    v_col0 = a_col0; v_col1 = a_col1;\n"
    "    v_tc[0] = a_tc0; v_tc[1] = a_tc1; v_tc[2] = a_tc2; v_tc[3] = a_tc3;\n"
    "    v_tc[4] = a_tc4; v_tc[5] = a_tc5; v_tc[6] = a_tc6; v_tc[7] = a_tc7;\n"
    "}\n";

/* The TEV, as tev_pixel() in raster.c does it. */
static const char* k_fs =
    "#version 330 core\n"
    "in vec4 v_col0;\n"
    "in vec4 v_col1;\n"
    "in vec3 v_tc[8];\n"
    "out vec4 o_color;\n"
    "layout(std140) uniform State {\n"
    "    ivec4 g0;\n"            /* stages, alpha test always passes, alpha logic */
    "    ivec4 g1;\n"            /* alpha functions 0/1, references 0/1 */
    "    vec4 reg_init[4];\n"
    "    vec4 konst[32];\n"
    "    ivec4 s_ci[16];\n"      /* colour input rows a b c d */
    "    ivec4 s_cbc[16];\n"     /* colour input is an alpha broadcast */
    "    ivec4 s_ai[16];\n"      /* alpha input rows */
    "    ivec4 s_misc[16];\n"    /* colour dest, alpha dest, texcoord, channel */
    "    ivec4 s_flags[16];\n"   /* x: csub 1 asub 2 cclamp 4 aclamp 8 ccmp 16 acmp 32; y: texture 0 off 1 on 2 missing */
    "    vec4 s_par[16];\n"      /* colour bias, scale, alpha bias, scale */
    "    ivec4 s_tsw[16];\n"
    "    ivec4 s_rsw[16];\n"
    "};\n"
    "uniform sampler2D u_t0, u_t1, u_t2, u_t3, u_t4, u_t5, u_t6, u_t7,\n"
    "                  u_t8, u_t9, u_t10, u_t11, u_t12, u_t13, u_t14, u_t15;\n"
    "uniform int u_solid;\n"
    "vec4 samp(int n, vec2 uv) {\n"
    "    switch (n) {\n"
    "    case 0: return texture(u_t0, uv);   case 1: return texture(u_t1, uv);\n"
    "    case 2: return texture(u_t2, uv);   case 3: return texture(u_t3, uv);\n"
    "    case 4: return texture(u_t4, uv);   case 5: return texture(u_t5, uv);\n"
    "    case 6: return texture(u_t6, uv);   case 7: return texture(u_t7, uv);\n"
    "    case 8: return texture(u_t8, uv);   case 9: return texture(u_t9, uv);\n"
    "    case 10: return texture(u_t10, uv); case 11: return texture(u_t11, uv);\n"
    "    case 12: return texture(u_t12, uv); case 13: return texture(u_t13, uv);\n"
    "    case 14: return texture(u_t14, uv); default: return texture(u_t15, uv);\n"
    "    }\n"
    "    return vec4(0.0);\n"
    "}\n"
    "vec4 R[9];\n"
    "vec4 row(int i) { return i < 9 ? R[i] : konst[i - 9]; }\n"
    "float comb(float a, float b, float c, float d, bool sub, float bias, float scale, bool cl) {\n"
    "    a = clamp(a, 0.0, 1.0); b = clamp(b, 0.0, 1.0); c = clamp(c, 0.0, 1.0);\n"
    "    float l = a + (b - a) * c;\n"
    "    float r = ((sub ? d - l : d + l) + bias) * scale;\n"
    "    return cl ? clamp(r, 0.0, 1.0) : clamp(r, -4.0, 4.0);\n"
    "}\n"
    "bool acmp(int f, int v, int ref) {\n"
    "    if (f == 0) return false; if (f == 1) return v < ref; if (f == 2) return v == ref;\n"
    "    if (f == 3) return v <= ref; if (f == 4) return v > ref; if (f == 5) return v != ref;\n"
    "    if (f == 6) return v >= ref; return true;\n"
    "}\n"
    "vec4 swz(vec4 v, ivec4 s) { return vec4(v[s.x], v[s.y], v[s.z], v[s.w]); }\n"
    "void main() {\n"
    "    R[0] = reg_init[0]; R[1] = reg_init[1]; R[2] = reg_init[2]; R[3] = reg_init[3];\n"
    "    R[4] = vec4(0.0); R[5] = vec4(0.0); R[6] = vec4(1.0); R[7] = vec4(0.5); R[8] = vec4(0.0);\n"
    "    vec3 tcs[8];\n"
    "    tcs[0] = v_tc[0]; tcs[1] = v_tc[1]; tcs[2] = v_tc[2]; tcs[3] = v_tc[3];\n"
    "    tcs[4] = v_tc[4]; tcs[5] = v_tc[5]; tcs[6] = v_tc[6]; tcs[7] = v_tc[7];\n"
    "    int ns = g0.x;\n"
    "    for (int n = 0; n < 16; n++) {\n"
    "        if (n >= ns) break;\n"
    "        ivec4 m = s_misc[n];\n"
    "        int fl = s_flags[n].x, tx = s_flags[n].y;\n"
    "        if (tx == 1) {\n"
    "            vec3 t = tcs[m.z];\n"
    "            vec2 uv = abs(t.z) > 1e-9 ? t.xy / t.z : t.xy;\n"
    "            R[4] = swz(samp(n, uv), s_tsw[n]);\n"
    "        } else if (tx == 2) {\n"
    "            R[4] = vec4(0.0);\n"
    "        }\n"
    "        if (m.w <= 1) R[5] = swz(m.w == 0 ? v_col0 : v_col1, s_rsw[n]);\n"
    "        else R[5] = vec4(0.0);\n"
    "        ivec4 ci = s_ci[n], bc = s_cbc[n], ai = s_ai[n];\n"
    "        vec4 p = s_par[n];\n"
    "        vec4 i0 = row(ci.x), i1 = row(ci.y), i2 = row(ci.z), i3 = row(ci.w);\n"
    "        vec3 a = bc.x != 0 ? vec3(i0.a) : i0.rgb;\n"
    "        vec3 b = bc.y != 0 ? vec3(i1.a) : i1.rgb;\n"
    "        vec3 c = bc.z != 0 ? vec3(i2.a) : i2.rgb;\n"
    "        vec3 d = bc.w != 0 ? vec3(i3.a) : i3.rgb;\n"
    "        vec3 res;\n"
    "        if ((fl & 16) != 0) res = d;\n"
    "        else {\n"
    "            bool sub = (fl & 1) != 0, cl = (fl & 4) != 0;\n"
    "            res = vec3(comb(a.r, b.r, c.r, d.r, sub, p.x, p.y, cl), comb(a.g, b.g, c.g, d.g, sub, p.x, p.y, cl),\n"
    "                       comb(a.b, b.b, c.b, d.b, sub, p.x, p.y, cl));\n"
    "        }\n"
    "        float ares = (fl & 32) != 0 ? row(ai.w).a\n"
    "            : comb(row(ai.x).a, row(ai.y).a, row(ai.z).a, row(ai.w).a, (fl & 2) != 0, p.z, p.w, (fl & 8) != 0);\n"
    "        R[m.x].rgb = res;\n"
    "        R[m.y].a = ares;\n"
    "    }\n"
    "    vec4 o = clamp(R[0], 0.0, 1.0);\n"
    "    if (g0.w != 0) o.rgb = min(o.rgb + v_col1.rgb, 1.0);\n"
    "    if (g0.y == 0) {\n"
    "        int av = int(o.a * 255.0 + 0.5);\n"
    "        bool r0 = acmp(g1.x, av, g1.z), r1 = acmp(g1.y, av, g1.w);\n"
    "        bool pass = g0.z == 0 ? (r0 && r1) : g0.z == 1 ? (r0 || r1) : g0.z == 2 ? (r0 != r1) : (r0 == r1);\n"
    "        if (!pass) discard;\n"
    "    }\n"
    "    if (u_solid != 0 && o.a < 0.5) discard;\n"   /* (the depth pass of a solid_depth draw) */
    "    o_color = o;\n"
    "}\n";

/* The State block, laid out as std140 packs it. */
typedef struct {
    s32 g0[4], g1[4];
    float reg_init[4][4];
    float konst[32][4];
    s32 ci[16][4], cbc[16][4], ai[16][4], misc[16][4], flags[16][4];
    float par[16][4];
    s32 tsw[16][4], rsw[16][4];
} GlState;

static GLuint compile(GLenum type, const char* src) {
    GLuint s = G(glCreateShader)(type);
    G(glShaderSource)(s, 1, &src, NULL);
    G(glCompileShader)(s);
    GLint ok = 0;
    G(glGetShaderiv)(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        G(glGetShaderInfoLog)(s, sizeof(log), NULL, log);
        printf("gpu: %s shader failed to compile:\n%s\n", type == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
        return 0;
    }
    return s;
}

static bool build_program(void) {
    GLuint vs = compile(GL_VERTEX_SHADER, k_vs), fs = compile(GL_FRAGMENT_SHADER, k_fs);
    if (!vs || !fs)
        return false;
    g_prog = G(glCreateProgram)();
    G(glAttachShader)(g_prog, vs);
    G(glAttachShader)(g_prog, fs);
    G(glBindFragDataLocation)(g_prog, 0, "o_color");
    G(glLinkProgram)(g_prog);
    GLint ok = 0;
    G(glGetProgramiv)(g_prog, GL_LINK_STATUS, &ok);
    G(glDeleteShader)(vs);
    G(glDeleteShader)(fs);
    if (!ok) {
        char log[4096];
        G(glGetProgramInfoLog)(g_prog, sizeof(log), NULL, log);
        printf("gpu: shader program failed to link:\n%s\n", log);
        return false;
    }
    G(glUseProgram)(g_prog);
    for (int i = 0; i < 16; i++) {
        char name[16];
        snprintf(name, sizeof(name), "u_t%d", i);
        G(glUniform1i)(G(glGetUniformLocation)(g_prog, name), i);
    }
    g_u_solid = G(glGetUniformLocation)(g_prog, "u_solid");
    G(glUniform1i)(g_u_solid, 0);
    GLuint bi = G(glGetUniformBlockIndex)(g_prog, "State");
    if (bi == GL_INVALID_INDEX) {
        printf("gpu: shader has no State block\n");
        return false;
    }
    G(glUniformBlockBinding)(g_prog, bi, 0);
    return true;
}

/* ------------------------------------------------------- frame buffer --- */
static GLuint make_tex(GLint ifmt, GLenum fmt, GLenum type, int w, int h) {
    GLuint t;
    G(glGenTextures)(1, &t);
    G(glBindTexture)(GL_TEXTURE_2D, t);
    G(glTexImage2D)(GL_TEXTURE_2D, 0, ifmt, w, h, 0, fmt, type, NULL);
    G(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    G(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    G(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    G(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    G(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    return t;
}

static bool make_fbo(GLuint* fbo, GLuint* color, GLuint* depth, int w, int h) {
    *color = make_tex(GL_RGBA8, GL_BGRA, GL_UNSIGNED_BYTE, w, h);
    *depth = make_tex(GL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, w, h);
    G(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    G(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    G(glGenFramebuffers)(1, fbo);
    G(glBindFramebuffer)(GL_FRAMEBUFFER, *fbo);
    G(glFramebufferTexture2D)(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *color, 0);
    G(glFramebufferTexture2D)(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, *depth, 0);
    GLenum st = G(glCheckFramebufferStatus)(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        printf("gpu: frame buffer %dx%d incomplete (0x%X)\n", w, h, st);
        return false;
    }
    return true;
}

/* ----------------------------------------------------------- textures --- */
/* GPU copies of decoded textures, found by the address of the decoded
   texels (a decoded buffer never changes; a changed texture is decoded into
   a new one, and the old one is reported through glr_texture_freed). */
typedef struct { const u32* key; GLuint tex; } TexSlot;
static TexSlot* g_tex;
static u32 g_tex_cap, g_tex_n, g_tex_tombs;

static u32 tex_hash(const u32* p) {
    uintptr_t v = (uintptr_t)p >> 4;
    return (u32)(v * 2654435761u);
}

static TexSlot* tex_find(const u32* key, bool insert) {
    if (!g_tex_cap || (insert && (g_tex_n + g_tex_tombs + 1) * 2 > g_tex_cap)) {
        u32 cap = g_tex_cap ? g_tex_cap : 1024;
        while ((g_tex_n + 1) * 4 > cap) cap *= 2;   /* rebuilt without tombstones */
        TexSlot* t = (TexSlot*)calloc(cap, sizeof(TexSlot));
        if (!t) return NULL;
        for (u32 i = 0; i < g_tex_cap; i++) {
            if (!g_tex[i].key || g_tex[i].key == (const u32*)1) continue;
            u32 h = tex_hash(g_tex[i].key) & (cap - 1);
            while (t[h].key) h = (h + 1) & (cap - 1);
            t[h] = g_tex[i];
        }
        free(g_tex);
        g_tex = t;
        g_tex_cap = cap;
        g_tex_tombs = 0;
    }
    u32 h = tex_hash(key) & (g_tex_cap - 1);
    TexSlot* tomb = NULL;
    while (g_tex[h].key) {
        if (g_tex[h].key == key) return &g_tex[h];
        if (g_tex[h].key == (const u32*)1 && !tomb) tomb = &g_tex[h];
        h = (h + 1) & (g_tex_cap - 1);
    }
    if (!insert) return NULL;
    TexSlot* s = tomb ? tomb : &g_tex[h];
    if (tomb) g_tex_tombs--;
    s->key = key;
    s->tex = 0;
    g_tex_n++;
    return s;
}

/* Called on the game thread while the GPU thread is idle: the slot is
   dropped at once (the buffer's address may be reused straight away), the
   GL texture is deleted by the GPU thread before it next draws. */
static GLuint* g_dead;
static u32 g_dead_n, g_dead_cap;

static void delete_dead_textures(void) {
    if (g_dead_n) {
        G(glDeleteTextures)((GLsizei)g_dead_n, g_dead);
        g_dead_n = 0;
    }
}

void glr_texture_freed(const u32* texels) {
    if (!g_ready || !g_tex_cap) return;
    TexSlot* s = tex_find(texels, false);
    if (!s) return;
    if (s->tex) {
        if (g_dead_n == g_dead_cap) {
            u32 cap = g_dead_cap ? g_dead_cap * 2 : 256;
            GLuint* d = (GLuint*)realloc(g_dead, sizeof(GLuint) * cap);
            if (d) { g_dead = d; g_dead_cap = cap; }
        }
        if (g_dead_n < g_dead_cap) g_dead[g_dead_n++] = s->tex;
    }
    s->key = (const u32*)1;            /* tombstone */
    s->tex = 0;
    g_tex_n--;
    g_tex_tombs++;
}

/* Custom textures can be big (a few MB each), and many show up at once when
   a new scene starts; uploading them all in one frame stalls it. So each
   frame uploads only so many bytes of them; the rest draw with the game's
   own texture for a frame or two until their turn. */
#define HIRES_UPLOAD_BUDGET (24u << 20)
static size_t g_hires_budget_used;
static unsigned long long g_hires_deferred;

static GLuint stage_texture(const Stage* s, bool* used_hires) {
    const GxHires* hi = s->hires;
    if (hi) {
        size_t bytes = (size_t)(hi->off[hi->levels - 1] + hi->lw[hi->levels - 1] * hi->lh[hi->levels - 1]) * 4u;
        TexSlot* have = tex_find(hi->texels, false);
        if (!(have && have->tex)) {
            if (g_hires_budget_used && g_hires_budget_used + bytes > HIRES_UPLOAD_BUDGET) {
                hi = NULL;             /* next frame */
                g_hires_deferred++;
            } else {
                g_hires_budget_used += bytes;
            }
        }
    }
    *used_hires = hi != NULL;
    TexSlot* slot = tex_find(hi ? hi->texels : s->texels, true);
    if (!slot) return 0;
    if (slot->tex) return slot->tex;
    GLuint t;
    G(glGenTextures)(1, &t);
    G(glBindTexture)(GL_TEXTURE_2D, t);
    G(glPixelStorei)(GL_UNPACK_ALIGNMENT, 4);
    GLint max_level;
    if (hi) {
        /* A custom texture: its own mip files, or a mip chain made here when
           the game's texture has one. */
        for (u32 l = 0; l < hi->levels; l++)
            G(glTexImage2D)(GL_TEXTURE_2D, (GLint)l, GL_RGBA8, (GLsizei)hi->lw[l], (GLsizei)hi->lh[l], 0, GL_BGRA,
                            GL_UNSIGNED_INT_8_8_8_8_REV, hi->texels + hi->off[l]);
        max_level = (GLint)hi->levels - 1;
        if (hi->levels == 1 && s->levels > 1) {
            u32 m = hi->w > hi->h ? hi->w : hi->h;
            max_level = 0;
            while (m >> (max_level + 1)) max_level++;
            G(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, max_level);
            G(glGenerateMipmap)(GL_TEXTURE_2D);
        }
        g_hires_uploads++;
    } else {
        for (u32 l = 0; l < s->levels; l++)
            G(glTexImage2D)(GL_TEXTURE_2D, (GLint)l, GL_RGBA8, (GLsizei)s->lw[l], (GLsizei)s->lh[l], 0, GL_BGRA,
                            GL_UNSIGNED_INT_8_8_8_8_REV, s->texels + s->loff[l]);
        max_level = (GLint)(s->levels ? s->levels - 1 : 0);
    }
    G(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    G(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, max_level);
    slot->tex = t;
    g_uploads++;
    return t;
}

/* Sampler settings per texture unit, set only when they change. */
typedef struct { GLint min, mag, ws, wt; float bias, minl, maxl; bool valid; } SamplerState;
static SamplerState g_sstate[16];

static GLint wrap_mode(u32 w) {
    return w == 0 ? GL_CLAMP_TO_EDGE : w == 2 ? GL_MIRRORED_REPEAT : GL_REPEAT;
}

static void set_sampler(int unit, const Stage* s, bool hires) {
    static const GLint k_min[8] = {GL_NEAREST, GL_LINEAR, GL_NEAREST_MIPMAP_NEAREST, GL_LINEAR_MIPMAP_NEAREST,
                                   GL_NEAREST_MIPMAP_LINEAR, GL_LINEAR_MIPMAP_LINEAR, GL_LINEAR, GL_LINEAR};
    SamplerState n = {k_min[s->min_filter & 7], s->mag_linear ? GL_LINEAR : GL_NEAREST, wrap_mode(s->wrap_s),
                      wrap_mode(s->wrap_t), s->lod_bias, s->min_lod, s->max_lod, true};
    if (hires && (s->hires->levels > 1 || s->levels > 1)) {
        n.maxl = 1000.0f;              /* a custom texture has more levels than the game's (as Dolphin does) */
    } else if (s->levels <= 1) {       /* no mipmaps: plain nearest / linear */
        n.min = (n.min == GL_NEAREST || n.min == GL_NEAREST_MIPMAP_NEAREST || n.min == GL_NEAREST_MIPMAP_LINEAR)
                    ? GL_NEAREST : GL_LINEAR;
    }
    SamplerState* o = &g_sstate[unit];
    GLuint sm = g_samplers[unit];
    if (!o->valid || o->min != n.min) G(glSamplerParameteri)(sm, GL_TEXTURE_MIN_FILTER, n.min);
    if (!o->valid || o->mag != n.mag) G(glSamplerParameteri)(sm, GL_TEXTURE_MAG_FILTER, n.mag);
    if (!o->valid || o->ws != n.ws) G(glSamplerParameteri)(sm, GL_TEXTURE_WRAP_S, n.ws);
    if (!o->valid || o->wt != n.wt) G(glSamplerParameteri)(sm, GL_TEXTURE_WRAP_T, n.wt);
    if (!o->valid || o->bias != n.bias) G(glSamplerParameterf)(sm, GL_TEXTURE_LOD_BIAS, n.bias);
    if (!o->valid || o->minl != n.minl) G(glSamplerParameterf)(sm, GL_TEXTURE_MIN_LOD, n.minl);
    if (!o->valid || o->maxl != n.maxl) G(glSamplerParameterf)(sm, GL_TEXTURE_MAX_LOD, n.maxl);
    *o = n;
}

static bool build_copy_program(void);

/* --------------------------------------------------------------- init --- */
/* The card's name as OpenGL gives it, tidied for showing in a menu: what
   comes after "/" (bus, instruction set) or in brackets dropped, (R) / (TM)
   marks and the maker's name in front taken out ("NVIDIA GeForce RTX
   4070/PCIe/SSE2" -> "GeForce RTX 4070"). */
static char g_gpu_name[48];

static void tidy_gpu_name(const char* raw) {
    char b[256];
    size_t n = 0;
    int depth = 0;
    for (const char* p = raw ? raw : ""; *p && *p != '/' && n + 1 < sizeof b; p++) {
        if (*p == '(') { depth++; continue; }
        if (*p == ')') { if (depth) depth--; continue; }
        if (!depth) b[n++] = *p;
    }
    b[n] = 0;
    static const char* const k_drop[] = {"NVIDIA Corporation ", "NVIDIA ", "AMD ", "ATI Technologies Inc. ", "ATI ",
                                         "Intel ", " Series"};
    for (size_t i = 0; i < sizeof k_drop / sizeof k_drop[0]; i++) {
        char* at = strstr(b, k_drop[i]);
        if (at) memmove(at, at + strlen(k_drop[i]), strlen(at + strlen(k_drop[i])) + 1);
    }
    /* "Graphics" at the end only when a model number is left ("Arc A770
       Graphics" -> "Arc A770", but "Radeon Graphics" stays). */
    n = strlen(b);
    while (n && b[n - 1] == ' ') b[--n] = 0;
    if (n > 9 && strcmp(b + n - 9, " Graphics") == 0 && strpbrk(b, "0123456789") &&
        strpbrk(b, "0123456789") < b + n - 9)
        b[n - 9] = 0;
    /* Spaces tidied (doubles from what was taken out, and at the ends). */
    char* w = b;
    for (char* r = b; *r; r++)
        if (*r != ' ' || (w > b && w[-1] != ' ')) *w++ = *r;
    while (w > b && w[-1] == ' ') w--;
    *w = 0;
    snprintf(g_gpu_name, sizeof g_gpu_name, "%s", b);
}

const char* glr_gpu_name(void) { return g_gpu_name; }

bool glr_init(GlrGetProc getproc, int scale, void (*make_current)(void)) {
    g_make_current = make_current;
    g_scale = scale < 1 ? 1 : scale > 8 ? 8 : scale;
    bool missing = false;
#define GLR_LOAD(name, ret, args)                                        \
    glr_##name = (PFN_##name)getproc(#name);                             \
    if (!glr_##name) { printf("gpu: OpenGL function %s is missing\n", #name); missing = true; }
    GLR_FUNCS(GLR_LOAD)
#undef GLR_LOAD
    if (missing)
        return false;
    tidy_gpu_name((const char*)G(glGetString)(GL_RENDERER));
    printf("gpu: %s / %s / OpenGL %s\n", (const char*)G(glGetString)(GL_VENDOR),
           (const char*)G(glGetString)(GL_RENDERER), (const char*)G(glGetString)(GL_VERSION));
    GLint maxtex = 0;
    G(glGetIntegerv)(GL_MAX_TEXTURE_SIZE, &maxtex);
    g_maxtex = maxtex;
    while (g_scale > 1 && ((float)EFB_W * g_scale * 4.0f / 3.0f > (float)maxtex || EFB_H * g_scale > maxtex)) g_scale--;
    g_sx = (float)g_scale * 4.0f / 3.0f;
    g_efb_w = PX(EFB_W);
    g_efb_h = EFB_H * g_scale;
    G(glGetIntegerv)(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &g_ubo_align);
    if (g_ubo_align < 16) g_ubo_align = 16;

    if (!build_program())
        return false;
    if (!build_copy_program())
        return false;
    if (!make_fbo(&g_fbo, &g_color_tex, &g_depth_tex, g_efb_w, g_efb_h))
        return false;
    if (!make_fbo(&g_fbo_small, &g_color_small, &g_depth_small, EFB_W, EFB_H))
        return false;

    G(glGenVertexArrays)(1, &g_vao);
    G(glBindVertexArray)(g_vao);
    G(glGenBuffers)(1, &g_vbo);
    G(glGenBuffers)(1, &g_ibo);
    G(glGenBuffers)(1, &g_ubo);
    G(glBindBuffer)(GL_ARRAY_BUFFER, g_vbo);
    G(glBindBuffer)(GL_ELEMENT_ARRAY_BUFFER, g_ibo);
    GLsizei st = (GLsizei)sizeof(SVtx);
    G(glVertexAttribPointer)(0, 4, GL_FLOAT, GL_FALSE, st, (const void*)offsetof(SVtx, x));
    G(glVertexAttribPointer)(1, 4, GL_FLOAT, GL_FALSE, st, (const void*)offsetof(SVtx, col[0]));
    G(glVertexAttribPointer)(2, 4, GL_FLOAT, GL_FALSE, st, (const void*)offsetof(SVtx, col[1]));
    for (int i = 0; i < 8; i++)
        G(glVertexAttribPointer)((GLuint)(3 + i), 3, GL_FLOAT, GL_FALSE, st,
                                 (const void*)(offsetof(SVtx, tc) + sizeof(float) * 3 * (size_t)i));
    for (GLuint i = 0; i < 11; i++)
        G(glEnableVertexAttribArray)(i);
    G(glGenSamplers)(16, g_samplers);
    for (int i = 0; i < 16; i++)
        G(glBindSampler)((GLuint)i, g_samplers[i]);

    /* A fresh frame buffer is black with depth at the far end, like the
       software one (zeroed memory). */
    G(glBindFramebuffer)(GL_FRAMEBUFFER, g_fbo);
    G(glClearColor)(0, 0, 0, 0);
    G(glClearDepth)(0.0);
    G(glClear)(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    G(glDisable)(GL_DITHER);
    G(glEnable)(GL_DEPTH_CLAMP);       /* depth is clamped, never clipped (near clipping happens beforehand) */
    G(glFrontFace)(GL_CW);             /* see apply_state */
    GLenum err = G(glGetError)();
    if (err != GL_NO_ERROR) {
        printf("gpu: OpenGL error 0x%X during setup\n", err);
        return false;
    }
    g_ready = true;
    printf("gpu: drawing at %dx%d (%dx the GameCube's resolution, a third wider for widescreen)\n", g_efb_w, g_efb_h,
           g_scale);
    return true;
}

/* --------------------------------------------- texture copies on GPU --- */
/* The game's copies of the frame into textures (screen transitions and the
   like), redone here at the internal resolution, in the copy's format (the
   same rounding and channel layout the RAM copy decodes to). */
#define TEXCOPY_SLOTS 16
typedef struct { u32 id; GLuint tex, fbo; u32 tw, th; } TexCopySlot;
static TexCopySlot g_tc[TEXCOPY_SLOTS];
static GLuint g_cprog, g_cvao, g_cvbo;
static GLint g_c_fmt, g_c_flags, g_c_src;
static unsigned long long g_tex_copies_gpu;

static const char* k_cvs =
    "#version 330 core\n"
    "layout(location = 0) in vec2 a_pos;\n"
    "layout(location = 1) in vec2 a_uv;\n"
    "out vec2 v_uv;\n"
    "void main() { v_uv = a_uv; gl_Position = vec4(a_pos, 0.0, 1.0); }\n";
static const char* k_cfs =
    "#version 330 core\n"
    "in vec2 v_uv;\n"
    "out vec4 o_color;\n"
    "uniform sampler2D u_efb;\n"
    "uniform int u_fmt;\n"            /* copy format 0-0xC */
    "uniform int u_flags;\n"          /* 1 intensity, 2 keep alpha */
    "float q(float v, float levels) { return floor(v * 255.0 / (256.0 / levels)) * (256.0 / levels) / 255.0; }\n"
    "float n4(float v) { float k = floor(v * 255.0 + 0.5) / 16.0; return floor(k) * 17.0 / 255.0; }\n"
    "void main() {\n"
    "    vec4 c = texture(u_efb, v_uv);\n"
    "    if ((u_flags & 2) == 0) c.a = 1.0;\n"
    "    vec3 b = floor(c.rgb * 255.0 + 0.5);\n"
    "    float lum = (u_flags & 1) != 0 ? (floor((66.0 * b.r + 129.0 * b.g + 25.0 * b.b + 128.0) / 256.0) + 16.0) / 255.0 : c.r;\n"
    "    vec4 o;\n"
    "    if (u_fmt == 0) o = vec4(n4(lum));\n"                                  /* I4 */
    "    else if (u_fmt == 1 || u_fmt == 8) o = vec4(lum);\n"                   /* I8 */
    "    else if (u_fmt == 2) o = vec4(vec3(n4(lum)), n4(c.a));\n"              /* IA4 */
    "    else if (u_fmt == 3) o = vec4(vec3(lum), c.a);\n"                      /* IA8 */
    "    else if (u_fmt == 4) {\n"                                               /* RGB565 */
    "        vec3 r = floor(b / vec3(8.0, 4.0, 8.0));\n"
    "        o = vec4(floor(r.r * 255.0 / 31.0) / 255.0, floor(r.g * 255.0 / 63.0) / 255.0, floor(r.b * 255.0 / 31.0) / 255.0, 1.0);\n"
    "    } else if (u_fmt == 5) {\n"                                             /* RGB5A3 */
    "        float a = floor(c.a * 255.0 + 0.5);\n"
    "        if (a >= 224.0) { vec3 r = floor(b / 8.0); o = vec4(floor(r * 255.0 / 31.0) / 255.0, 1.0); }\n"
    "        else { vec3 r = floor(b / 16.0); o = vec4(r * 17.0 / 255.0, floor(a / 32.0) * 36.0 / 255.0); }\n"
    "    } else if (u_fmt == 6) o = c;\n"                                       /* RGBA8 */
    "    else if (u_fmt == 7) o = vec4(c.a);\n"                                 /* A8 */
    "    else if (u_fmt == 9) o = vec4(c.g);\n"                                 /* G8 */
    "    else if (u_fmt == 10) o = vec4(c.b);\n"                                /* B8 */
    "    else if (u_fmt == 11) o = vec4(vec3(c.g), c.r);\n"                     /* RG8, read as IA8 */
    "    else o = vec4(vec3(c.b), c.g);\n"                                      /* GB8, read as IA8 */
    "    o_color = o;\n"
    "}\n";

static bool build_copy_program(void) {
    GLuint vs = compile(GL_VERTEX_SHADER, k_cvs), fs = compile(GL_FRAGMENT_SHADER, k_cfs);
    if (!vs || !fs) return false;
    g_cprog = G(glCreateProgram)();
    G(glAttachShader)(g_cprog, vs);
    G(glAttachShader)(g_cprog, fs);
    G(glBindFragDataLocation)(g_cprog, 0, "o_color");
    G(glLinkProgram)(g_cprog);
    GLint ok = 0;
    G(glGetProgramiv)(g_cprog, GL_LINK_STATUS, &ok);
    G(glDeleteShader)(vs);
    G(glDeleteShader)(fs);
    if (!ok) { printf("gpu: copy shader failed to link\n"); return false; }
    G(glUseProgram)(g_cprog);
    g_c_src = G(glGetUniformLocation)(g_cprog, "u_efb");
    g_c_fmt = G(glGetUniformLocation)(g_cprog, "u_fmt");
    g_c_flags = G(glGetUniformLocation)(g_cprog, "u_flags");
    G(glGenVertexArrays)(1, &g_cvao);
    G(glBindVertexArray)(g_cvao);
    G(glGenBuffers)(1, &g_cvbo);
    G(glBindBuffer)(GL_ARRAY_BUFFER, g_cvbo);
    G(glVertexAttribPointer)(0, 2, GL_FLOAT, GL_FALSE, 16, (const void*)0);
    G(glVertexAttribPointer)(1, 2, GL_FLOAT, GL_FALSE, 16, (const void*)8);
    G(glEnableVertexAttribArray)(0);
    G(glEnableVertexAttribArray)(1);
    G(glBindVertexArray)(g_vao);
    return true;
}

void glr_tex_copy(const TexCopy* c) {
    if (!g_ready || !c->id || !c->w || !c->h)
        return;
    if (g_make_current) g_make_current();
    /* reuse the slot with the oldest id */
    TexCopySlot* t = &g_tc[0];
    for (int i = 1; i < TEXCOPY_SLOTS; i++)
        if (g_tc[i].id < t->id) t = &g_tc[i];
    u32 tw = (u32)PX(c->w), th = c->h * (u32)g_scale;
    if (!t->tex || t->tw != tw || t->th != th) {
        if (t->tex) { G(glDeleteTextures)(1, &t->tex); G(glDeleteFramebuffers)(1, &t->fbo); }
        t->tex = make_tex(GL_RGBA8, GL_BGRA, GL_UNSIGNED_BYTE, (int)tw, (int)th);
        G(glGenFramebuffers)(1, &t->fbo);
        G(glBindFramebuffer)(GL_FRAMEBUFFER, t->fbo);
        G(glFramebufferTexture2D)(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->tex, 0);
        t->tw = tw; t->th = th;
    }
    t->id = c->id;
    /* Source rectangle in the frame buffer texture (row 0 = top, like the
       texture being made, so no flip). */
    float u0 = (float)c->sx / (float)EFB_W, u1 = (float)(c->sx + c->src_w) / (float)EFB_W;
    float v0 = (float)c->sy / (float)EFB_H, v1 = (float)(c->sy + c->src_h) / (float)EFB_H;
    float v[16] = {-1, -1, u0, v0, 1, -1, u1, v0, -1, 1, u0, v1, 1, 1, u1, v1};
    G(glBindFramebuffer)(GL_FRAMEBUFFER, t->fbo);
    G(glViewport)(0, 0, (GLsizei)tw, (GLsizei)th);
    G(glDisable)(GL_SCISSOR_TEST);
    G(glDisable)(GL_DEPTH_TEST);
    G(glDisable)(GL_BLEND);
    G(glDisable)(GL_CULL_FACE);
    G(glColorMask)(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    G(glUseProgram)(g_cprog);
    G(glUniform1i)(g_c_src, 0);
    G(glUniform1i)(g_c_fmt, (GLint)c->fmt);
    G(glUniform1i)(g_c_flags, (c->intensity ? 1 : 0) | (c->keep_alpha ? 2 : 0));
    G(glBindVertexArray)(g_cvao);
    G(glBindBuffer)(GL_ARRAY_BUFFER, g_cvbo);
    G(glBufferData)(GL_ARRAY_BUFFER, sizeof(v), v, GL_STREAM_DRAW);
    G(glActiveTexture)(GL_TEXTURE0);
    G(glBindTexture)(GL_TEXTURE_2D, g_color_tex);
    G(glBindSampler)(0, 0);
    G(glDrawArrays)(GL_TRIANGLE_STRIP, 0, 4);
    /* put back what the drawing code expects */
    G(glBindSampler)(0, g_samplers[0]);
    G(glBindVertexArray)(g_vao);
    G(glUseProgram)(g_prog);
    G(glBindFramebuffer)(GL_FRAMEBUFFER, g_fbo);
    G(glViewport)(0, 0, g_efb_w, g_efb_h);
    g_sstate[0].valid = false;
    g_tex_copies_gpu++;
}

static GLuint tex_copy_texture(u32 id) {
    for (int i = 0; i < TEXCOPY_SLOTS; i++)
        if (g_tc[i].id == id && g_tc[i].tex) return g_tc[i].tex;
    return 0;
}

/* -------------------------------------------------------------- state --- */
static void fill_state(GlState* o, const RenderState* ds) {
    memset(o, 0, sizeof(*o));
    o->g0[0] = (s32)ds->stages;
    o->g0[1] = ds->at_always;
    o->g0[2] = (s32)ds->at_logic;
    o->g0[3] = ds->spec_add;
    o->g1[0] = (s32)ds->at_f0; o->g1[1] = (s32)ds->at_f1;
    o->g1[2] = (s32)ds->at_ref0; o->g1[3] = (s32)ds->at_ref1;
    memcpy(o->reg_init, ds->reg_init, sizeof(o->reg_init));
    memcpy(o->konst, ds->konst, sizeof(o->konst));
    for (u32 n = 0; n < ds->stages && n < 16; n++) {
        const Stage* s = &ds->st[n];
        for (int j = 0; j < 4; j++) {
            o->ci[n][j] = s->ci[j];
            o->cbc[n][j] = s->cbc[j];
            o->ai[n][j] = s->ai[j];
            o->tsw[n][j] = s->tsw[j];
            o->rsw[n][j] = s->rsw[j];
        }
        o->misc[n][0] = s->cdst; o->misc[n][1] = s->adst;
        o->misc[n][2] = s->texcoord < 8 ? s->texcoord : 0;
        o->misc[n][3] = s->chan;
        o->flags[n][0] = (s->csub ? 1 : 0) | (s->asub ? 2 : 0) | (s->cclamp ? 4 : 0) | (s->aclamp ? 8 : 0) |
                         (s->ccmp ? 16 : 0) | (s->acmp ? 32 : 0);
        o->flags[n][1] = !s->tex_on ? 0 : s->texels ? 1 : 2;
        o->par[n][0] = s->cbias; o->par[n][1] = s->cscale;
        o->par[n][2] = s->abias; o->par[n][3] = s->ascale;
    }
}

static GLenum blend_factor(u32 f, bool src) {
    switch (f) {
    case 0: return GL_ZERO;
    case 1: return GL_ONE;
    case 2: return src ? GL_DST_COLOR : GL_SRC_COLOR;
    case 3: return src ? GL_ONE_MINUS_DST_COLOR : GL_ONE_MINUS_SRC_COLOR;
    case 4: return GL_SRC_ALPHA;
    case 5: return GL_ONE_MINUS_SRC_ALPHA;
    case 6: return GL_DST_ALPHA;
    default: return GL_ONE_MINUS_DST_ALPHA;
    }
}

static void apply_state(const RenderState* ds) {
    int S = g_scale;
    int x0 = PX((u32)ds->sx0), x1 = PX((u32)ds->sx1 + 1u);
    G(glEnable)(GL_SCISSOR_TEST);
    G(glScissor)(x0, ds->sy0 * S, x1 - x0, (ds->sy1 - ds->sy0 + 1) * S);

    if (ds->ztest) {
        static const GLenum k_z[8] = {GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS};
        G(glEnable)(GL_DEPTH_TEST);
        G(glDepthFunc)(k_z[ds->zfunc & 7]);
        G(glDepthMask)(ds->zupdate ? GL_TRUE : GL_FALSE);
    } else {
        G(glDisable)(GL_DEPTH_TEST);   /* no test: no depth write either, as in the software renderer */
    }

    /* Blending covers colour only; alpha is written as the TEV made it. */
    if (ds->blend) {
        G(glEnable)(GL_BLEND);
        if (ds->subtract) {
            G(glBlendEquationSeparate)(GL_FUNC_REVERSE_SUBTRACT, GL_FUNC_ADD);
            G(glBlendFuncSeparate)(GL_ONE, GL_ONE, GL_ONE, GL_ZERO);
        } else {
            G(glBlendEquationSeparate)(GL_FUNC_ADD, GL_FUNC_ADD);
            G(glBlendFuncSeparate)(blend_factor(ds->sfac, true), blend_factor(ds->dfac, false), GL_ONE, GL_ZERO);
        }
    } else {
        G(glDisable)(GL_BLEND);
    }
    GLboolean c = ds->color_update ? GL_TRUE : GL_FALSE;
    G(glColorMask)(c, c, c, ds->alpha_update ? GL_TRUE : GL_FALSE);

    /* Screen y runs down here (see the vertex shader), so the winding the
       software renderer calls a positive area is counter-clockwise to GL.
       Front faces are set to be clockwise (glFrontFace in init), which
       makes GX "cull front" (1) GL_FRONT and "cull back" (2) GL_BACK. */
    if (ds->cull == 1 || ds->cull == 2) {
        G(glEnable)(GL_CULL_FACE);
        G(glCullFace)(ds->cull == 1 ? GL_FRONT : GL_BACK);
    } else {
        G(glDisable)(GL_CULL_FACE);
    }

    for (u32 n = 0; n < ds->stages && n < 16; n++) {
        const Stage* s = &ds->st[n];
        if (!s->tex_on || !s->texels)
            continue;
        G(glActiveTexture)(GL_TEXTURE0 + n);     /* before any upload, which binds on the active unit */
        GLuint t = s->efb_copy ? tex_copy_texture(s->efb_copy) : 0;
        bool hires = false;
        if (!t) t = stage_texture(s, &hires);
        G(glBindTexture)(GL_TEXTURE_2D, t);
        set_sampler((int)n, s, hires);
    }
}

/* -------------------------------------------------------------- lists --- */
static u32* g_idx;
static u32 g_idx_cap;
static u8* g_ublob;
static size_t g_ublob_cap;

typedef struct { u32 type; u32 state; u32 first, count; const CopyCmd* copy; } Run;
static Run* g_runs;
static u32 g_runs_cap;

static void clear_efb(const CopyCmd* cp) {
    G(glBindFramebuffer)(GL_FRAMEBUFFER, g_fbo);
    G(glDisable)(GL_SCISSOR_TEST);
    G(glColorMask)(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    G(glDepthMask)(GL_TRUE);
    u32 c = cp->clear_color;
    G(glClearColor)((float)((c >> 16) & 255) / 255.0f, (float)((c >> 8) & 255) / 255.0f, (float)(c & 255) / 255.0f,
                    (float)(c >> 24) / 255.0f);
    G(glClearDepth)((double)cp->clear_z / 16777215.0);
    G(glClear)(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

/* ------------------------------------------------- TV buffers on GPU --- */
/* Each copy to a TV buffer is also kept on the GPU, at the internal
   resolution, under the buffer's address in RAM; the window shows that
   instead of the (native-size) picture in RAM. A few are kept, as the game
   alternates between two or three buffers. */
#define XFB_SLOTS 4
#define XFB_TEX_W 720
#define XFB_TEX_H 576
typedef struct { u32 dest, w, lines, age; GLuint tex, fbo; bool valid; } XfbTex;
static XfbTex g_xfb[XFB_SLOTS];
static u32 g_xfb_age;
static bool g_keep_ram = true;         /* also write TV-buffer copies to RAM (read-back) */
static GLsync g_fence_copy;            /* GPU thread: TV buffers written */
static GLsync g_fence_present;         /* presenting thread: TV buffer shown */
static unsigned long long g_xfb_gpu_copies;

void glr_keep_ram_copies(bool keep) {
    g_keep_ram = keep;
}

static XfbTex* xfb_find(u32 dest) {
    for (int i = 0; i < XFB_SLOTS; i++)
        if (g_xfb[i].valid && g_xfb[i].dest == dest) return &g_xfb[i];
    return NULL;
}

static void xfb_gpu_copy(const CopyCmd* cp) {
    XfbTex* x = xfb_find(cp->dest);
    if (!x) {                          /* the least recently written slot */
        x = &g_xfb[0];
        for (int i = 1; i < XFB_SLOTS; i++)
            if (!g_xfb[i].valid || (x->valid && g_xfb[i].age < x->age)) x = &g_xfb[i];
        if (!x->tex) {
            x->tex = make_tex(GL_RGBA8, GL_BGRA, GL_UNSIGNED_BYTE, PX(XFB_TEX_W), XFB_TEX_H * g_scale);
            G(glGenFramebuffers)(1, &x->fbo);
            G(glBindFramebuffer)(GL_FRAMEBUFFER, x->fbo);
            G(glFramebufferTexture2D)(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, x->tex, 0);
        }
        x->valid = true;
        x->dest = cp->dest;
    }
    u32 w = cp->w > XFB_TEX_W ? XFB_TEX_W : cp->w;
    u32 lines = cp->lines > XFB_TEX_H ? XFB_TEX_H : cp->lines;
    x->w = w;
    x->lines = lines;
    x->age = ++g_xfb_age;
    int S = g_scale;
    /* Source rows sy..sy+h (frame-buffer row 0 is the top, as in the TV
       buffer texture), stretched to the copy's line count. */
    G(glDisable)(GL_SCISSOR_TEST);
    G(glBindFramebuffer)(GL_READ_FRAMEBUFFER, g_fbo);
    G(glBindFramebuffer)(GL_DRAW_FRAMEBUFFER, x->fbo);
    G(glBlitFramebuffer)(PX(cp->sx), (GLint)(cp->sy * S), PX(cp->sx + w), (GLint)((cp->sy + cp->h) * S),
                         0, 0, PX(w), (GLint)(lines * S), GL_COLOR_BUFFER_BIT,
                         lines == cp->h ? GL_NEAREST : GL_LINEAR);
    G(glBindFramebuffer)(GL_FRAMEBUFFER, g_fbo);
    if (g_fence_copy) G(glDeleteSync)(g_fence_copy);
    g_fence_copy = G(glFenceSync)(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    G(glFlush)();                      /* so the presenting context sees it */
    g_xfb_gpu_copies++;
}

bool glr_xfb_on_gpu(u32 dest) {
    return g_ready && xfb_find(dest) != NULL;
}

void glr_set_scale(int scale) {
    g_want_scale = scale < 1 ? 1 : scale > 8 ? 8 : scale;
}

/* On the GPU thread, between lists: a new internal resolution. The frame
   buffer is made again at the new size (starting black) and the TV-buffer
   textures dropped (made again at the new size by the next copies); the
   texture-copy slots remake themselves when their size changes. */
static void apply_scale(void) {
    int s = g_want_scale;
    if (!s) return;
    g_want_scale = 0;
    while (s > 1 && ((float)EFB_W * s * 4.0f / 3.0f > (float)g_maxtex || EFB_H * s > g_maxtex)) s--;
    if (s == g_scale) return;
    G(glDeleteFramebuffers)(1, &g_fbo);
    G(glDeleteTextures)(1, &g_color_tex);
    G(glDeleteTextures)(1, &g_depth_tex);
    for (int i = 0; i < XFB_SLOTS; i++) {
        if (g_xfb[i].tex) {
            G(glDeleteTextures)(1, &g_xfb[i].tex);
            G(glDeleteFramebuffers)(1, &g_xfb[i].fbo);
        }
        g_xfb[i].tex = g_xfb[i].fbo = 0;
        g_xfb[i].valid = false;
    }
    g_scale = s;
    g_sx = (float)g_scale * 4.0f / 3.0f;
    g_efb_w = PX(EFB_W);
    g_efb_h = EFB_H * g_scale;
    if (!make_fbo(&g_fbo, &g_color_tex, &g_depth_tex, g_efb_w, g_efb_h)) {
        printf("gpu: couldn't make the frame buffer at %dx; graphics card drawing stopped\n", g_scale);
        g_ready = false;
        return;
    }
    G(glClearColor)(0, 0, 0, 0);
    G(glClearDepth)(0.0);
    G(glClear)(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    printf("gpu: now drawing at %dx%d (%dx the GameCube's resolution)\n", g_efb_w, g_efb_h, g_scale);
}

void glr_run_list(const CmdList* l) {
    if (!g_ready)
        return;
    if (g_make_current) g_make_current();
    apply_scale();
    if (!g_ready)
        return;
    delete_dead_textures();
    g_hires_budget_used = 0;
    if (!l->n)
        return;
    g_lists++;
    if (g_fence_present) {             /* don't overwrite a TV buffer still being shown */
        G(glWaitSync)(g_fence_present, 0, GL_TIMEOUT_IGNORED);
        G(glDeleteSync)(g_fence_present);
        g_fence_present = NULL;
    }

    /* Group the triangles into runs that share a state, with copies
       (which need everything before them drawn) as separate runs. */
    if (l->n * 3 > g_idx_cap) {
        g_idx_cap = l->n * 3 + 1024;
        g_idx = (u32*)realloc(g_idx, sizeof(u32) * g_idx_cap);
    }
    if (l->n + 1 > g_runs_cap) {
        g_runs_cap = l->n + 64;
        g_runs = (Run*)realloc(g_runs, sizeof(Run) * g_runs_cap);
    }
    if (!g_idx || !g_runs)
        return;
    u32 ni = 0, nr = 0;
    for (u32 i = 0; i < l->n; i++) {
        const Cmd* c = &l->cmds[i];
        if (c->type == CMD_TRI) {
            if (!nr || g_runs[nr - 1].type != CMD_TRI || g_runs[nr - 1].state != c->state) {
                g_runs[nr].type = CMD_TRI;
                g_runs[nr].state = c->state;
                g_runs[nr].first = ni;
                g_runs[nr].count = 0;
                nr++;
            }
            g_idx[ni++] = c->u.vi[0];
            g_idx[ni++] = c->u.vi[1];
            g_idx[ni++] = c->u.vi[2];
            g_runs[nr - 1].count += 3;
        } else {
            g_runs[nr].type = CMD_COPY;
            g_runs[nr].copy = &c->u.copy;
            nr++;
        }
    }

    /* Every state used, one after another in a uniform buffer. */
    size_t ustride = (sizeof(GlState) + (size_t)g_ubo_align - 1) / (size_t)g_ubo_align * (size_t)g_ubo_align;
    size_t ubytes = ustride * (l->ns ? l->ns : 1);
    if (ubytes > g_ublob_cap) {
        g_ublob_cap = ubytes * 2;
        g_ublob = (u8*)realloc(g_ublob, g_ublob_cap);
        if (!g_ublob) { g_ublob_cap = 0; return; }
    }
    for (u32 s = 0; s < l->ns; s++)
        fill_state((GlState*)(g_ublob + ustride * s), &l->states[s]);

    G(glBindFramebuffer)(GL_FRAMEBUFFER, g_fbo);
    G(glViewport)(0, 0, g_efb_w, g_efb_h);
    G(glUseProgram)(g_prog);
    G(glBindVertexArray)(g_vao);
    G(glBindBuffer)(GL_ARRAY_BUFFER, g_vbo);
    G(glBufferData)(GL_ARRAY_BUFFER, (GLsizeiptr)(sizeof(SVtx) * l->nv), l->verts, GL_STREAM_DRAW);
    G(glBindBuffer)(GL_ELEMENT_ARRAY_BUFFER, g_ibo);
    G(glBufferData)(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(sizeof(u32) * ni), g_idx, GL_STREAM_DRAW);
    G(glBindBuffer)(GL_UNIFORM_BUFFER, g_ubo);
    G(glBufferData)(GL_UNIFORM_BUFFER, (GLsizeiptr)ubytes, g_ublob, GL_STREAM_DRAW);

    u32 cur = 0xFFFFFFFFu;
    for (u32 r = 0; r < nr; r++) {
        const Run* run = &g_runs[r];
        if (run->type == CMD_TRI) {
            if (run->state != cur) {
                cur = run->state;
                apply_state(&l->states[cur]);
                G(glBindBufferRange)(GL_UNIFORM_BUFFER, 0, g_ubo, (GLintptr)(ustride * cur), (GLsizeiptr)sizeof(GlState));
            }
            const RenderState* rs = &l->states[cur];
            bool solid = rs->solid_depth && rs->ztest && rs->zupdate;
            if (solid) G(glDepthMask)(GL_FALSE);   /* the picture first, hiding nothing */
            G(glDrawElements)(GL_TRIANGLES, (GLsizei)run->count, GL_UNSIGNED_INT,
                              (const void*)(sizeof(u32) * (size_t)run->first));
            g_draw_calls++;
            if (solid) {
                /* then depth, from its mostly solid pixels only */
                G(glDepthMask)(GL_TRUE);
                G(glColorMask)(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
                G(glUniform1i)(g_u_solid, 1);
                G(glDrawElements)(GL_TRIANGLES, (GLsizei)run->count, GL_UNSIGNED_INT,
                                  (const void*)(sizeof(u32) * (size_t)run->first));
                g_draw_calls++;
                G(glUniform1i)(g_u_solid, 0);
                GLboolean c = rs->color_update ? GL_TRUE : GL_FALSE;
                G(glColorMask)(c, c, c, rs->alpha_update ? GL_TRUE : GL_FALSE);
            }
            continue;
        }
        const CopyCmd* cp = run->copy;
        if (cp->to_xfb && cp->lines) {
            if (g_keep_ram) {
                glr_readback(false);
                raster_xfb_write(cp, 0, EFB_H - 1);
            }
            xfb_gpu_copy(cp);
            G(glBindFramebuffer)(GL_FRAMEBUFFER, g_fbo);
        }
        if (cp->clear) {
            if (cp->to_xfb) raster_probes_before_clear();
            clear_efb(cp);
        }
        cur = 0xFFFFFFFFu;              /* scissor, masks and bindings may have changed */
    }
}

/* ---------------------------------------------------------- read-back --- */
void glr_read_point(u32 x, u32 y, bool depth, u32* out) {
    *out = 0;
    if (!g_ready || x >= EFB_W || y >= EFB_H) return;
    if (g_make_current) g_make_current();
    /* The middle of the native pixel in the scaled frame buffer (row 0 is
       the top, as in the EFB). */
    GLint px = (GLint)(((float)x + 0.5f) * g_sx), py = (GLint)(y * (u32)g_scale + (u32)g_scale / 2u);
    G(glBindFramebuffer)(GL_FRAMEBUFFER, g_fbo);
    G(glPixelStorei)(GL_PACK_ALIGNMENT, 4);
    if (depth) {
        u32 d = 0;
        G(glReadPixels)(px, py, 1, 1, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, &d);
        *out = d >> 8;
    } else {
        G(glReadPixels)(px, py, 1, 1, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, out);
    }
}

void glr_readback(bool depth) {
    if (!g_ready)
        return;
    if (g_make_current) g_make_current();
    g_readbacks++;
    GLuint src = g_fbo;
    if (g_efb_w != EFB_W || g_efb_h != EFB_H) {
        /* Scale down to native size first (colour averaged, depth sampled). */
        G(glDisable)(GL_SCISSOR_TEST);
        G(glBindFramebuffer)(GL_READ_FRAMEBUFFER, g_fbo);
        G(glBindFramebuffer)(GL_DRAW_FRAMEBUFFER, g_fbo_small);
        G(glBlitFramebuffer)(0, 0, g_efb_w, g_efb_h, 0, 0, EFB_W, EFB_H, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        if (depth)
            G(glBlitFramebuffer)(0, 0, g_efb_w, g_efb_h, 0, 0, EFB_W, EFB_H, GL_DEPTH_BUFFER_BIT,
                                 GL_NEAREST);
        src = g_fbo_small;
    }
    G(glBindFramebuffer)(GL_FRAMEBUFFER, src);
    G(glPixelStorei)(GL_PACK_ALIGNMENT, 4);
    G(glReadPixels)(0, 0, EFB_W, EFB_H, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, g_color);
    if (depth) {
        G(glReadPixels)(0, 0, EFB_W, EFB_H, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, g_depth);
        for (u32 i = 0; i < EFB_W * EFB_H; i++) g_depth[i] >>= 8;   /* 32-bit to the EFB's 24 */
    }
    G(glBindFramebuffer)(GL_FRAMEBUFFER, g_fbo);
}


/* ------------------------------------------------------------ present --- */
/* Runs on the thread that shows the picture, in a second context that
   shares textures with the GPU thread's. */
static GLuint g_pprog, g_pvao, g_pvbo, g_ptex;
static GLint g_p_max;
static bool g_pready;
static GLuint g_present_fbo;           /* tests: draw here instead of the window */

void glr_set_present_fbo(unsigned fbo) {
    g_present_fbo = fbo;
}

static const char* k_pvs =
    "#version 330 core\n"
    "layout(location = 0) in vec2 a_pos;\n"
    "layout(location = 1) in vec2 a_uv;\n"
    "out vec2 v_uv;\n"
    "void main() { v_uv = a_uv; gl_Position = vec4(a_pos, 0.0, 1.0); }\n";
static const char* k_pfs =
    "#version 330 core\n"
    "in vec2 v_uv;\n"
    "out vec4 o_color;\n"
    "uniform sampler2D u_tex;\n"
    "uniform vec2 u_max;\n"           /* last texel centre of the picture: no bleeding past its edge */
    "void main() { o_color = vec4(texture(u_tex, min(v_uv, u_max)).rgb, 1.0); }\n";

bool glr_present_init(void) {
    GLuint vs = compile(GL_VERTEX_SHADER, k_pvs), fs = compile(GL_FRAGMENT_SHADER, k_pfs);
    if (!vs || !fs)
        return false;
    g_pprog = G(glCreateProgram)();
    G(glAttachShader)(g_pprog, vs);
    G(glAttachShader)(g_pprog, fs);
    G(glBindFragDataLocation)(g_pprog, 0, "o_color");
    G(glLinkProgram)(g_pprog);
    GLint ok = 0;
    G(glGetProgramiv)(g_pprog, GL_LINK_STATUS, &ok);
    G(glDeleteShader)(vs);
    G(glDeleteShader)(fs);
    if (!ok) {
        printf("gpu: picture shader failed to link\n");
        return false;
    }
    G(glUseProgram)(g_pprog);
    G(glUniform1i)(G(glGetUniformLocation)(g_pprog, "u_tex"), 0);
    g_p_max = G(glGetUniformLocation)(g_pprog, "u_max");
    G(glGenVertexArrays)(1, &g_pvao);
    G(glBindVertexArray)(g_pvao);
    G(glGenBuffers)(1, &g_pvbo);
    G(glBindBuffer)(GL_ARRAY_BUFFER, g_pvbo);
    G(glVertexAttribPointer)(0, 2, GL_FLOAT, GL_FALSE, 16, (const void*)0);
    G(glVertexAttribPointer)(1, 2, GL_FLOAT, GL_FALSE, 16, (const void*)8);
    G(glEnableVertexAttribArray)(0);
    G(glEnableVertexAttribArray)(1);
    g_ptex = make_tex(GL_RGBA8, GL_BGRA, GL_UNSIGNED_BYTE, XFB_TEX_W, XFB_TEX_H);
    GLenum err = G(glGetError)();
    if (err != GL_NO_ERROR) {
        printf("gpu: OpenGL error 0x%X setting up the picture output\n", err);
        return false;
    }
    g_pready = true;
    return true;
}

static void write_bmp_rgba(const char* path, const u32* px, int w, int h) {
    FILE* f = fopen(path, "wb");
    if (!f) return;
    u32 row = ((u32)w * 3 + 3) & ~3u, size = 54 + row * (u32)h;
    u8 hdr[54] = {'B', 'M'};
    hdr[2] = (u8)size; hdr[3] = (u8)(size >> 8); hdr[4] = (u8)(size >> 16); hdr[5] = (u8)(size >> 24);
    hdr[10] = 54; hdr[14] = 40;
    hdr[18] = (u8)w; hdr[19] = (u8)(w >> 8); hdr[22] = (u8)h; hdr[23] = (u8)(h >> 8);
    hdr[26] = 1; hdr[28] = 24;
    fwrite(hdr, 1, 54, f);
    u8* line = (u8*)calloc(1, row);
    for (int y = 0; y < h; y++) {      /* GL rows run bottom up, as BMP's do */
        const u32* src = px + (size_t)y * (size_t)w;
        for (int x = 0; x < w; x++) {
            line[x * 3] = (u8)src[x]; line[x * 3 + 1] = (u8)(src[x] >> 8); line[x * 3 + 2] = (u8)(src[x] >> 16);
        }
        fwrite(line, 1, row, f);
    }
    free(line);
    fclose(f);
}

void glr_present(const GlrPicture* pic, unsigned target_fbo, int win_w, int win_h, float aspect, const char* shot_path) {
    if (!g_pready)
        return;
    G(glBindFramebuffer)(GL_FRAMEBUFFER, target_fbo ? target_fbo : g_present_fbo);
    G(glViewport)(0, 0, win_w, win_h);
    G(glDisable)(GL_SCISSOR_TEST);
    G(glDisable)(GL_DEPTH_TEST);
    G(glDisable)(GL_BLEND);
    G(glDisable)(GL_CULL_FACE);
    G(glColorMask)(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    G(glClearColor)(0, 0, 0, 1);
    G(glClear)(GL_COLOR_BUFFER_BIT);

    /* The picture's place in the window: as large as fits, at the TV shape. */
    float dw = (float)win_w, dh = (float)win_w / aspect;
    if (dh > (float)win_h) { dh = (float)win_h; dw = dh * aspect; }
    float x0 = ((float)win_w - dw) * 0.5f, ytop = ((float)win_h + dh) * 0.5f;   /* GL y runs up */

    GLuint tex = 0;
    float umax = 0, vmax = 0, frac_w = 1, frac_h = 1, texw = XFB_TEX_W, texh = XFB_TEX_H;
    if (pic->kind == GLR_PIC_GPU) {
        XfbTex* x = xfb_find(pic->dest);
        if (x) {
            if (g_fence_copy) G(glWaitSync)(g_fence_copy, 0, GL_TIMEOUT_IGNORED);
            tex = x->tex;
            texw = (float)PX(XFB_TEX_W);
            texh = (float)(XFB_TEX_H * g_scale);
            umax = (float)PX(x->w) / texw;
            vmax = (float)(x->lines * g_scale) / texh;
            frac_w = pic->tv_w ? (float)x->w / (float)pic->tv_w : 1.0f;
            frac_h = pic->tv_h ? (float)x->lines / (float)pic->tv_h : 1.0f;
        }
    } else if (pic->kind == GLR_PIC_PIXELS && pic->pixels && pic->w && pic->h) {
        G(glActiveTexture)(GL_TEXTURE0);
        G(glBindTexture)(GL_TEXTURE_2D, g_ptex);
        G(glPixelStorei)(GL_UNPACK_ALIGNMENT, 4);
        G(glTexSubImage2D)(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)pic->w, (GLsizei)pic->h, GL_BGRA,
                           GL_UNSIGNED_INT_8_8_8_8_REV, pic->pixels);
        tex = g_ptex;
        umax = (float)pic->w / texw;
        vmax = (float)pic->h / texh;
    }
    if (tex) {
        if (frac_w > 1) frac_w = 1;
        if (frac_h > 1) frac_h = 1;
        float x1 = x0 + dw * frac_w, ybot = ytop - dh * frac_h;
        /* to normalised device coordinates; texture row 0 is the picture's top */
        float nx0 = x0 / (float)win_w * 2 - 1, nx1 = x1 / (float)win_w * 2 - 1;
        float nyt = ytop / (float)win_h * 2 - 1, nyb = ybot / (float)win_h * 2 - 1;
        float v[16] = {nx0, nyt, 0, 0, nx1, nyt, umax, 0, nx0, nyb, 0, vmax, nx1, nyb, umax, vmax};
        G(glUseProgram)(g_pprog);
        G(glUniform2f)(g_p_max, umax - 0.5f / texw, vmax - 0.5f / texh);
        G(glBindVertexArray)(g_pvao);
        G(glBindBuffer)(GL_ARRAY_BUFFER, g_pvbo);
        G(glBufferData)(GL_ARRAY_BUFFER, sizeof(v), v, GL_STREAM_DRAW);
        G(glActiveTexture)(GL_TEXTURE0);
        G(glBindTexture)(GL_TEXTURE_2D, tex);
        G(glBindSampler)(0, 0);
        G(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        G(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        G(glDrawArrays)(GL_TRIANGLE_STRIP, 0, 4);
        if (pic->kind == GLR_PIC_GPU) {
            if (g_fence_present) G(glDeleteSync)(g_fence_present);
            g_fence_present = G(glFenceSync)(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
            G(glFlush)();
        }
    }
    if (shot_path) {
        u32* px = (u32*)malloc((size_t)win_w * (size_t)win_h * 4);
        if (px) {
            G(glPixelStorei)(GL_PACK_ALIGNMENT, 4);
            G(glReadPixels)(0, 0, win_w, win_h, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, px);
            write_bmp_rgba(shot_path, px, win_w, win_h);
            free(px);
            printf("window: screenshot saved to %s (%dx%d)\n", shot_path, win_w, win_h);
        }
    }
}

/* The port's own menu over the picture: a window-sized premultiplied
   0xAARRGGBB picture, uploaded only when it changed. */
static GLuint g_oprog, g_otex;
static int g_ow, g_oh;

void glr_present_overlay(const u32* px, int w, int h, bool dirty) { glr_present_overlay_scaled(px, w, h, w, h, dirty); }

/* The same, a picture of another size stretched (smoothly) over the window. */
void glr_present_overlay_scaled(const u32* px, int w, int h, int win_w, int win_h, bool dirty) {
    if (!g_pready || !px || w <= 0 || h <= 0 || win_w <= 0 || win_h <= 0)
        return;
    if (!g_oprog) {
        static const char* fs =
            "#version 330 core\n"
            "in vec2 v_uv;\n"
            "out vec4 o_color;\n"
            "uniform sampler2D u_tex;\n"
            "void main() { o_color = texture(u_tex, v_uv); }\n";
        GLuint v = compile(GL_VERTEX_SHADER, k_pvs), f = compile(GL_FRAGMENT_SHADER, fs);
        if (!v || !f) return;
        g_oprog = G(glCreateProgram)();
        G(glAttachShader)(g_oprog, v);
        G(glAttachShader)(g_oprog, f);
        G(glBindFragDataLocation)(g_oprog, 0, "o_color");
        G(glLinkProgram)(g_oprog);
        G(glDeleteShader)(v);
        G(glDeleteShader)(f);
        G(glUseProgram)(g_oprog);
        G(glUniform1i)(G(glGetUniformLocation)(g_oprog, "u_tex"), 0);
    }
    G(glActiveTexture)(GL_TEXTURE0);
    if (!g_otex || w != g_ow || h != g_oh) {
        if (g_otex) G(glDeleteTextures)(1, &g_otex);
        g_otex = make_tex(GL_RGBA8, GL_BGRA, GL_UNSIGNED_BYTE, w, h);
        g_ow = w;
        g_oh = h;
        dirty = true;
    }
    G(glBindTexture)(GL_TEXTURE_2D, g_otex);
    G(glBindSampler)(0, 0);
    if (dirty) {
        G(glPixelStorei)(GL_UNPACK_ALIGNMENT, 4);
        G(glTexSubImage2D)(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, px);
    }
    GLint filt = w == win_w && h == win_h ? GL_NEAREST : GL_LINEAR;
    G(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filt);
    G(glTexParameteri)(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filt);
    G(glViewport)(0, 0, win_w, win_h);
    G(glDisable)(GL_SCISSOR_TEST);
    G(glEnable)(GL_BLEND);
    G(glBlendEquationSeparate)(GL_FUNC_ADD, GL_FUNC_ADD);
    G(glBlendFuncSeparate)(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    float v[16] = {-1, 1, 0, 0, 1, 1, 1, 0, -1, -1, 0, 1, 1, -1, 1, 1};   /* row 0 of the picture at the top */
    G(glUseProgram)(g_oprog);
    G(glBindVertexArray)(g_pvao);
    G(glBindBuffer)(GL_ARRAY_BUFFER, g_pvbo);
    G(glBufferData)(GL_ARRAY_BUFFER, sizeof(v), v, GL_STREAM_DRAW);
    G(glDrawArrays)(GL_TRIANGLE_STRIP, 0, 4);
    G(glDisable)(GL_BLEND);
}

void glr_present_black(int w, int h) {
    if (!g_pready || w <= 0 || h <= 0)
        return;
    G(glBindFramebuffer)(GL_FRAMEBUFFER, g_present_fbo);
    G(glViewport)(0, 0, w, h);
    G(glDisable)(GL_SCISSOR_TEST);
    G(glColorMask)(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    G(glClearColor)(0.0f, 0.0f, 0.0f, 1.0f);
    G(glClear)(GL_COLOR_BUFFER_BIT);
}

void glr_report(void) {
    if (!g_ready)
        return;
    printf("gpu: %llu frames' lists drawn in %llu draw calls, %llu textures uploaded (%u on the GPU now), %llu "
           "read-backs, %llu TV-buffer copies kept on the GPU, %llu texture copies made on the GPU\n",
           g_lists, g_draw_calls, g_uploads, g_tex_n, g_readbacks, g_xfb_gpu_copies, g_tex_copies_gpu);
    if (g_hires_uploads)
        printf("gpu: %llu custom textures uploaded (%llu draws waited a frame for theirs)\n", g_hires_uploads,
               g_hires_deferred);
}
