/* A Quit Game button on the game's main menu.
 *
 * The main menu is one script (EA's UIS: objects plus bytecode). Its six
 * buttons (Quick Game, Game Modes, Modify Teams / Stats and Rewards,
 * Options, Inside Street) are groups of nine objects each; the controller
 * object keeps the selected button as an index (0-5) and each button keeps
 * its neighbours (up, down, left, right) as indexes. The script turns the
 * index into a button, a highlight and an action with switches over 0-5,
 * and sets every button's look for each menu page.
 *
 * The seventh button is the Options group copied into the port's own memory
 * (past the GameCube's 24 MB), relabelled, moved to a third row and linked
 * in as index 6:
 *   - the copy: instance data, property lists, descriptors and drawing
 *     records, with the pointers, header-relative offsets and object numbers
 *     inside moved to the copy; its own label and text variables;
 *   - the object table and the root's child list, moved to the port's memory
 *     with the copy added;
 *   - neighbours: up from it Options, down Game Modes; down from the second
 *     row and up from Game Modes, it;
 *   - script: the focus and highlight switches get an index-6 case (Inside
 *     Street's, with the copy and its help line); the action switch's index 6
 *     calls UIS_FN_QUIT; each page's "look of the six buttons" list sets the
 *     copy's too. New script bytes go in the port's memory and are reached
 *     by jumps (the script's jumps are relative and reach anywhere).
 *
 * Everything is checked against the expected layout first (the disc's own
 * data, so the offsets below are fixed); anything unexpected and nothing is
 * changed.
 */
#include "runtime/uismenu.h"

#include <stdio.h>
#include <string.h>

#define G 0x80000000u

static u8* R;
static u32 RS;

static bool in_ram(u32 a, u32 n) { return a >= G && a - G <= RS - n; }
static u32 rd(u32 a) { const u8* p = R + (a - G); return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static void wr(u32 a, u32 v) { u8* p = R + (a - G); p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }
static u8 rb(u32 a) { return R[a - G]; }

/* The main menu script, relative to its header. */
enum { NOBJ = 0x75, NVAR = 0x1BB, NFUNC = 0x12, OPT = 14, GRP = 9, NEWIDX = NOBJ, NEWBTN = 6 };
#define INST_LO 0x4590u                /* the Options group's instance data */
#define INST_HI 0x4A5Cu
#define META_LO 0x17B0u                /* its property lists and info */
#define META_HI 0x19CCu
#define DESC_LO 0x0A5Cu                /* its property descriptors */
#define DESC_HI 0x0B04u
#define DRAW_LO 0x8EA0u                /* its drawing records */
#define DRAW_HI 0x9400u
/* Buttons, relative to the controller object's instance. */
#define B_QG 0x1870u
#define B_GM 0x0310u
#define B_MT 0x0ED8u
#define B_SR 0x1D3Cu
#define B_OPT 0x13A4u
#define B_IS 0x07DCu
/* Fields of a button: neighbour indexes. */
#define NB_UP 0x78u
#define NB_DOWN 0x7Cu
#define NB_LEFT 0x80u
#define NB_RIGHT 0x84u
#define BTN_LABEL 0x74u
/* The red bars behind the rows are four plain quads the root draws (an
   outer, see-through one and an inner one per row), listed with object 3
   in one of the root's drawing lists. */
#define QUAD_ROW2_OUT 0x7460u
#define QUAD_ROW2_IN 0x74C0u
#define QUAD_SIZE 0x60u
#define HILITE_RIGHT 143              /* the Quit Game highlight's right end (Options': 113) */
#define QUAD_VERTS 0x34u               /* 4 vertices of (x, y, z), s16 12.4 */
#define HELP_VAR_IS 0x16Fu             /* Inside Street's help line */
#define HELP_VAR_FREE 0x161u           /* GameCube-only Game Modes list text, unused (main.c) */

/* The port's memory (guest addresses). */
#define P_BASE (PORT_RAM_BASE + 0x10000u)
#define P_INST (P_BASE + 0x0000u)
#define P_META (P_BASE + 0x0500u)
#define P_DESC (P_BASE + 0x0740u)
#define P_DRAW (P_BASE + 0x0800u)
#define P_OBJTAB (P_BASE + 0x0E00u)
#define P_ROOTKIDS (P_BASE + 0x1800u)
#define P_VARS (P_BASE + 0x1900u)
#define P_LISTS (P_BASE + 0x1A00u)
#define P_QLIST (P_BASE + 0x1C00u)       /* the root's drawing list with the bar */
#define P_QOFFS (P_BASE + 0x1C80u)       /* the new quads' offsets */
#define P_QUADS (P_BASE + 0x1D00u)       /* the new quads */
#define P_CODE (P_BASE + 0x2000u)
#define P_CODE_END (P_BASE + 0x4000u)
#define P_TEXT (P_BASE + 0x4000u)        /* Options list texts with Video Settings */
#define P_TEXT_END (P_BASE + 0x4400u)
#define P_DIALOG (P_BASE + 0x4300u)      /* the quit dialog's texts (inside P_TEXT's room, past what it uses) */
#define P_XDRAW (P_BASE + 0x4800u)       /* drawing records the copy uses from outside its group (its icons) */
#define P_XDRAW_END (P_BASE + 0x5000u)

typedef struct { u32 lo, hi, nlo; } Region;
static Region g_rg[4];
static u32 g_h;                        /* script header */
static u32 g_code;                     /* next free byte of P_CODE */

static u32 reloc(u32 w, bool offsets) {
    for (int i = 0; i < 4; i++)
        if (w >= g_rg[i].lo && w < g_rg[i].hi) return w - g_rg[i].lo + g_rg[i].nlo;
    if (offsets && w >= 0xA00u && w < 0x10000u) {
        u32 a = g_h + w;
        for (int i = 0; i < 4; i++)
            if (a >= g_rg[i].lo && a < g_rg[i].hi) return a - g_rg[i].lo + g_rg[i].nlo - g_h;
    }
    return w;
}

/* --------------------------------------------------- script bytes --- */
static int op_len(u8 op) {
    switch (op) {
    case 0x10: case 0x11: case 0x43: return 5;
    case 0x55: case 0x56: case 0x57: case 0x59: return 7;
    case 0x58: return 2;
    default: return 1;
    }
}
static bool is_jump(u8 op) { return op == 0x40 || op == 0x3E || op == 0x3F; }

static void emit8(u8 v) { R[g_code - G] = v; g_code++; }
static void emit32(u32 v) { wr(g_code, v); g_code += 4; }
static void emit_push(u32 v) { emit8(0x10); emit32(v); }
/* push rel; jump (to target) */
static void emit_jump(u32 target) { emit_push(target - (g_code + 6)); emit8(0x40); }
static void put_jump(u32 at, u32 target) {   /* overwrite 6 bytes at "at" */
    R[at - G] = 0x10;
    wr(at + 1, target - (at + 6));
    R[at + 5 - G] = 0x40;
}
static bool code_room(u32 n) { return g_code + n <= P_CODE_END; }

/* Copies the script bytes [from, to) to the port's code area, fixing the
   relative jumps and swapping Inside Street's button / help line for the new
   button's; then a jump to "then". Returns where the copy starts (0: no room
   or something unexpected). */
static u32 copy_case(u32 from, u32 to, u32 then, u32 new_off) {
    u32 start = g_code;
    if (!code_room(to - from + 16)) return 0;
    for (u32 a = from; a < to;) {
        u8 op = rb(a);
        int n = op_len(op);
        if (op == 0x10 && a + 5 < to && is_jump(rb(a + 5))) {
            u32 target = a + 6 + rd(a + 1);
            emit8(0x10);
            emit32(target - (g_code + 5));
        } else if (op == 0x57 && rd(a + 1) == B_IS) {
            emit8(0x57);
            emit32(new_off);
            emit8(rb(a + 5));
            emit8(rb(a + 6));
        } else if (op == 0x10 && rd(a + 1) == HELP_VAR_IS && a + 5 < to && rb(a + 5) == 0x6F) {
            emit_push(HELP_VAR_FREE);
        } else {
            for (int i = 0; i < n; i++) emit8(rb(a + (u32)i));
        }
        a += (u32)n;
    }
    emit_jump(then);
    return start;
}

/* A "switch (selected button)": U ac; then for k = 0..5: dup, push k, ==,
   push rel, jump-if-true; then push rel, jump (default). */
typedef struct { u32 at, target[6], dflt, dflt_imm; } Switch;

static bool parse_switch(u32 a, Switch* s) {
    if (rb(a) != 0x55 || rd(a + 1) != 0xACu) return false;
    s->at = a;
    u32 p = a + 7;
    for (u32 k = 0; k < 6; k++, p += 13) {
        if (rb(p) != 0x41 || rb(p + 1) != 0x10 || rd(p + 2) != k || rb(p + 6) != 0x36 || rb(p + 7) != 0x10 ||
            rb(p + 12) != 0x3E)
            return false;
        s->target[k] = p + 13 + rd(p + 8);
    }
    if (rb(p) != 0x10 || rb(p + 5) != 0x40) return false;
    s->dflt_imm = p + 1;
    s->dflt = p + 6 + rd(p + 1);
    return true;
}

static bool contains(u32 from, u32 to, const u8* pat, u32 n) {
    for (u32 a = from; a + n <= to; a++)
        if (memcmp(R + (a - G), pat, n) == 0) return true;
    return false;
}

/* --------------------------------------------------------- the add --- */
static u32 find_header(void) {
    for (u32 o = 0x00800000u; o + 0x20 < RS && o < 0x01800000u; o += 4) {
        u32 h = G + o;
        if (rd(h) == NOBJ && rd(h + 4) == h + 0x30 && rd(h + 8) == NVAR && rd(h + 0x10) == NFUNC) return h;
        if ((rd(h) == NOBJ + GRP + 1 || rd(h) == NOBJ + GRP) && rd(h + 4) == P_OBJTAB && rd(h + 8) == NVAR && rd(h + 0x10) == NFUNC) return h;
    }
    return 0;
}

static bool str_is(u32 var, const char* s) {
    if (!in_ram(var, 12) || rd(var) != 1 || rd(var + 4) != strlen(s)) return false;
    u32 p = rd(var + 8);
    return in_ram(p, (u32)strlen(s)) && memcmp(R + (p - G), s, strlen(s)) == 0;
}

static void put_var(u32 entry, u32 buf, u32 len, const char* s, u32 buflen) {
    wr(entry, 1);
    wr(entry + 4, len);
    wr(entry + 8, buf);
    memset(R + (buf - G), 0, buflen);
    memcpy(R + (buf - G), s, strlen(s));
}

static void put_s16(u32 a, int v) { R[a - G] = (u8)((unsigned)v >> 8); R[a - G + 1] = (u8)v; }

/* Sets a quad's corners (12.4 fixed point, in sixteenths). */
static void quad_rect(u32 q, int x0, int y0, int x1, int y1) {
    const int xs[4] = {x0, x1, x1, x0}, ys[4] = {y0, y0, y1, y1};
    for (int i = 0; i < 4; i++) {
        u32 v = q + QUAD_VERTS + (u32)i * 6;
        put_s16(v, xs[i]); put_s16(v + 2, ys[i]); put_s16(v + 4, 0);
    }
}

static int s16_at(u32 a) { return (int16_t)(R[a - G] << 8 | R[a - G + 1]); }

/* Pictures (button icons, bars' textures) are shared and counted: a drawing
   record that shows one takes it when its screen sets it up and gives it
   back when the screen is taken down, and the last one back frees it. The
   port's copies of objects are made after set-up, so their records never
   took theirs, yet give them back with the rest: one short each time, and a
   picture still in use on the next screen (the A icon on Select Controller's
   footer, after Create User ID) was freed. Each copied record takes its
   picture here, as set-up would have. The game's picture manager: the
   second of the list at r13 - 0x6368; per set (0x24 bytes) a slot list
   (+0x20, count +0x1E), each slot a use count (u16) and the picture. */
#define SDA_BASE 0x803F22A0u
static int acquire_images(u32 lo, u32 hi) {
    u32 mgr = SDA_BASE - 0x6368u + 4u;
    if (!in_ram(mgr, 4) || !in_ram(rd(mgr), 0x10)) return 0;
    mgr = rd(mgr);
    u32 sets = rd(mgr + 0xC);
    int n = 0;
    for (u32 a = lo; a + 0x10 <= hi;) {
        u32 w0 = rd(a), size = (w0 >> 28) == 4 ? 0x60u : 0x40u;
        if ((w0 >> 16) & 2u) {
            int idx = s16_at(a + 4), set = s16_at(a + 6);
            if (set >= 0 && set < 16 && idx >= 0 && in_ram(sets + (u32)set * 0x24u, 0x24)) {
                u32 st = sets + (u32)set * 0x24u, slots = rd(st + 0x20);
                int count = s16_at(st + 0x1E);
                if (idx < count && in_ram(slots + (u32)idx * 8u, 8)) {
                    u32 sl = slots + (u32)idx * 8u;
                    if (rd(sl + 4)) {        /* (loaded: the original's record holds it) */
                        u32 c = (u32)(R[sl - G] << 8 | R[sl - G + 1]) + 1u;
                        R[sl - G] = (u8)(c >> 8); R[sl - G + 1] = (u8)c;
                        n++;
                    }
                }
            }
        }
        a += size;
    }
    return n;
}

static bool add_bar(u32 h, u32 tab) {
    u32 n1 = rd(tab + 4), p1 = rd(tab + 8);
    for (u32 i = 0; i < n1 && i < 16; i++) {
        u32 desc = rd(p1 + i * 4), n = rd(desc + 4), list = rd(desc + 8);
        if (n < 2 || n > 16 || !in_ram(list, n * 8)) continue;
        /* The list with the second row's quads, ending its quads where an
           object starts. */
        u32 k_out = n, last_quad = n;
        for (u32 k = 0; k < n; k++) {
            if (rd(list + k * 8) != 1) continue;
            u32 ptr = rd(list + k * 8 + 4);
            if (!in_ram(ptr, 4)) continue;
            if (rd(ptr) == QUAD_ROW2_OUT) k_out = k;
            last_quad = k;
        }
        if (k_out == n) continue;
        u32 qo = h + QUAD_ROW2_OUT, qi = h + QUAD_ROW2_IN;
        if (rd(qo) != 0x40000000u || rd(qi) != 0x40000000u || s16_at(qo + QUAD_VERTS) != 123 * 16 ||
            s16_at(qo + QUAD_VERTS + 2) != 196 * 16) {
            printf("uis: main menu's bar quads aren't as expected; Quit Game without a bar\n");
            return false;
        }
        memcpy(R + (P_QUADS - G), R + (qo - G), QUAD_SIZE);
        memcpy(R + (P_QUADS + QUAD_SIZE - G), R + (qi - G), QUAD_SIZE);
        /* The second row's: outer 123-752.5 x 196-221, inner 123.5-751 x
           197.5-216. Rows are 33 apart and step 14-16 right; the third
           row's, cut to the middle around Quit Game. */
        quad_rect(P_QUADS, 385 * 16, 229 * 16, 615 * 16, 254 * 16);
        quad_rect(P_QUADS + QUAD_SIZE, 385 * 16 + 8, 230 * 16 + 8, 613 * 16 + 8, 249 * 16);
        wr(P_QOFFS, P_QUADS - h);
        wr(P_QOFFS + 4, P_QUADS + QUAD_SIZE - h);
        /* The list: the same, the two new quads after the last old one. */
        u32 out = P_QLIST;
        for (u32 k = 0; k < n; k++) {
            wr(out, rd(list + k * 8)); wr(out + 4, rd(list + k * 8 + 4)); out += 8;
            if (k == last_quad) {
                wr(out, 1); wr(out + 4, P_QOFFS); out += 8;
                wr(out, 1); wr(out + 4, P_QOFFS + 4); out += 8;
            }
        }
        wr(desc + 4, n + 2);
        wr(desc + 8, P_QLIST);
        return true;
    }
    printf("uis: main menu's bar quads not found; Quit Game without a bar\n");
    return false;
}

/* ------------------------------------------- Options: Video Settings --- */
/* A chain "switch (index)" of n cases: dup, push k, ==, push rel,
   jump-if-true (k = 0..n-1); then push rel, jump (default). */
static bool parse_chain(u32 a, u32 n, u32* target, u32* dflt) {
    u32 p = a;
    for (u32 k = 0; k < n; k++, p += 13) {
        if (rb(p) != 0x41 || rb(p + 1) != 0x10 || rd(p + 2) != k || rb(p + 6) != 0x36 || rb(p + 7) != 0x10 ||
            rb(p + 12) != 0x3E)
            return false;
        target[k] = p + 13 + rd(p + 8);
    }
    if (rb(p) != 0x10 || rb(p + 5) != 0x40) return false;
    *dflt = p + 6 + rd(p + 1);
    return true;
}

static void emit_cond(u32 target) { emit_push(target - (g_code + 6)); emit8(0x3E); }

/* The chain again with one more case: index "at" goes to "fresh", the
   original cases from "at" on move up one. */
static u32 emit_chain(const u32* target, u32 n, u32 dflt, u32 at, u32 fresh) {
    u32 start = g_code;
    for (u32 k = 0; k <= n; k++) {
        u32 t = k < at ? target[k] : k == at ? fresh : target[k - 1];
        emit8(0x41); emit_push(k); emit8(0x36); emit_cond(t);
    }
    emit_jump(dflt);
    return start;
}

/* A switch over n cases: "dup; push k; eq; if jump target[k]", then the default. */
static u32 emit_chain_n(const u32* target, u32 n, u32 dflt) {
    u32 start = g_code;
    for (u32 k = 0; k < n; k++) { emit8(0x41); emit_push(k); emit8(0x36); emit_cond(target[k]); }
    emit_jump(dflt);
    return start;
}

static u32 find_var(u32 vars, const char* str) {
    for (u32 i = 0; i < NVAR; i++)
        if (str_is(vars + i * 12, str)) return vars + i * 12;
    return 0;
}

/* The list texts: a new one with the port's entries before "|<before>". */
static bool list_with_video(u32 var, const char* before, u32* text) {
    u32 len = rd(var + 4), p = rd(var + 8);
    char buf[160], out[192];
    if (len >= sizeof(buf)) return false;
    memcpy(buf, R + (p - G), len);
    buf[len] = 0;
    char* at = strstr(buf, before);
    if (!at) return false;
    int n = snprintf(out, sizeof(out), "%.*s|Video Settings|Recomp Settings%s", (int)(at - buf), buf, at);
    if (n <= 0 || *text + (u32)n + 4 > P_DIALOG) return false;
    memcpy(R + (*text - G), out, (size_t)n);
    wr(var + 4, (u32)n);
    wr(var + 8, *text);
    *text += ((u32)n + 4) & ~3u;
    return true;
}

static const char k_opt_list[] = "|Game Settings|Jukebox|Audio Settings|Save/Load|Cheats and Codes";
static const char k_opt_list_gc[] = "|Game Settings|Jukebox|Audio/Visual|Save/Load|Cheats and Codes";
#define VIDEO_AT 2                     /* their places: after Jukebox (Video, then Recomp), above Audio Settings */

static u32 g_audio_case;               /* the Options list's Audio Settings action (opens screen 149) */
static int g_online_pos = -1;          /* Online's place in the Game Modes list (its case number) */

static bool add_video(u32 h, u32 code_hi) {
    u32 vars = rd(h + 0xC);
    u32 v_list = find_var(vars, k_opt_list), v_gc = find_var(vars, k_opt_list_gc);
    u32 v_help0 = find_var(vars, "Adjust game settings.");
    if (!v_list || !v_gc || !v_help0) { printf("uis: Options list texts not found; no Video Settings\n"); return false; }
    u32 help0 = (v_help0 - vars) / 12;
    /* The help switch: case 0 is "dup; push <Adjust game settings.>; var;
       W ..; push 3; push 8; push 1; native; pop; pop; push rel; jump". The
       action switch: case 0 asks "screen 0x96 there?" (native 0x38). */
    u32 ht[5], hd = 0, hat = 0, at[5], ad = 0, aat = 0;
    for (u32 a = h; a + 80 < code_hi && (!hat || !aat); a++) {
        if (rb(a) != 0x41 || rb(a + 1) != 0x10 || rd(a + 2) != 0 || rb(a + 6) != 0x36) continue;
        u32 t[5], dd;
        if (!parse_chain(a, 5, t, &dd) || (rb(a + 65) == 0x41 && rd(a + 67) == 5)) continue;
        if (!hat && rb(t[0]) == 0x41 && rb(t[0] + 1) == 0x10 && rd(t[0] + 2) == help0 && rb(t[0] + 6) == 0x6F &&
            rb(t[0] + 7) == 0x57 && rb(t[0] + 0x20) == 0x10 && rb(t[0] + 0x25) == 0x40) {
            hat = a; hd = dd; memcpy(ht, t, sizeof(t));
        } else if (!aat && rb(t[0]) == 0x41 && rb(t[0] + 1) == 0x10 && rd(t[0] + 2) == 0x38 && rb(t[0] + 6) == 0x10 &&
                   rd(t[0] + 7) == 0x96) {
            aat = a; ad = dd; memcpy(at, t, sizeof(t));
        }
    }
    if (!hat || !aat || !code_room(0x100)) {
        printf("uis: Options list switches not found (help %s, action %s); no Video Settings\n", hat ? "yes" : "no",
               aat ? "yes" : "no");
        return false;
    }
    /* Texts: the lists, and the help line (a variable of the port's own). */
    u32 text = P_TEXT;
    if (!list_with_video(v_list, "|Audio Settings", &text) || !list_with_video(v_gc, "|Audio/Visual", &text)) return false;
    u32 v_help = text, help_buf = text + 12;
    put_var(v_help, help_buf, 22, "Adjust video settings.", 24);
    static const char k_rhelp[] = "Settings the PC port adds.";
    u32 v_rhelp = text + 40, rhelp_buf = text + 52;
    put_var(v_rhelp, rhelp_buf, (u32)strlen(k_rhelp), k_rhelp, 32);
    /* Help: as Game Settings' case, with the new lines. */
    u32 hcs[2];
    for (int e = 0; e < 2; e++) {
        hcs[e] = g_code;
        emit8(0x41); emit_push(e ? v_rhelp : v_help);   /* the variable itself, where "push n; var" gives it */
        for (u32 a = ht[0] + 7; a < ht[0] + 0x20; a++) emit8(rb(a));
        emit_jump(ht[0] + 0x26 + rd(ht[0] + 0x21));
    }
    u32 ht7[7] = {ht[0], ht[1], hcs[0], hcs[1], ht[2], ht[3], ht[4]};
    put_jump(hat, emit_chain_n(ht7, 7, hd));
    /* Action: the port's video settings; then input back on (as the
       entries' "not available" paths) and on as the default does. */
    /* Action: both open the Audio Settings screen (its case, old index 2),
       telling the port first which of the two it is to be: Video Settings
       is the same screen in "video mode" (uismenu_video_screen). */
    u32 ac = g_code;
    emit8(0x41); emit_push(UIS_FN_VIDEO); emit_push(0); emit8(0x0B); emit8(0x42);
    emit_jump(at[2]);
    u32 ar = g_code;
    emit8(0x41); emit_push(UIS_FN_RECOMP); emit_push(0); emit8(0x0B); emit8(0x42);
    emit_jump(at[2]);
    u32 au = g_code;
    emit8(0x41); emit_push(UIS_FN_AUDIO); emit_push(0); emit8(0x0B); emit8(0x42);
    emit_jump(at[2]);
    u32 at7[7] = {at[0], at[1], ac, ar, au, at[3], at[4]};
    put_jump(aat, emit_chain_n(at7, 7, ad));
    g_audio_case = at[2];
    return true;
}

/* Online (Game Modes list): its action asks for screen 0x12C (not in the
   GameCube game) and then starts the online front end, which isn't there
   either (the menu waits for ever). Its case in the list's switch (a chain
   like the Options list's) is pointed at code telling the port "Online",
   then on into the Options list's Audio Settings action: the Audio Settings
   screen opens, in video mode with the Online rows. */
static bool add_online(u32 h, u32 code_hi) {
    if (!g_audio_case || !code_room(64)) return false;
    /* Where it asks for screen 0x12C: "push 0x38; push 0x12C". */
    u32 x = 0;
    for (u32 a = h; a + 10 < code_hi && !x; a++)
        if (rb(a) == 0x10 && rd(a + 1) == 0x38 && rb(a + 5) == 0x10 && rd(a + 6) == 0x12Cu) x = a;
    if (!x) {
        u32 y = 0;
        for (u32 o = 0x00800000u; o + 16 < RS && !y; o++)
            if (R[o] == 0x10 && rd(G + o + 1) == 0x38 && R[o + 5] == 0x10 && rd(G + o + 6) == 0x12Cu) y = G + o;
        printf("uis: main menu: Online's screen request not in its script (script at %08X, found at %08X); Online opens the "
               "port's page instead\n", h, y);
        return false;
    }
    /* The case it's in: the nearest place at or before it that a switch
       entry ("dup; push k; ==; push rel; jump-if") jumps to. */
    u32 entry = 0, c = 0;
    for (u32 back = 0; back < 0x60 && !entry; back++) {
        u32 t = x - back;
        for (u32 a = h; a + 13 < code_hi; a++) {
            if (rb(a) != 0x41 || rb(a + 1) != 0x10 || rb(a + 6) != 0x36 || rb(a + 7) != 0x10 || rb(a + 12) != 0x3E) continue;
            if (a + 13 + rd(a + 8) == t) { entry = a; c = t; break; }
        }
    }
    if (!entry) {
        printf("uis: main menu: Online's case not found (request at script +%X); Online opens the port's page instead\n", x - h);
        return false;
    }
    /* The switch's default (after its last entry). */
    u32 e = entry;
    while (rb(e + 13) == 0x41 && rb(e + 14) == 0x10 && rd(e + 15) == rd(e + 2) + 1 && rb(e + 19) == 0x36 &&
           rb(e + 20) == 0x10 && rb(e + 25) == 0x3E)
        e += 13;
    if (rb(e + 13) != 0x10 || rb(e + 18) != 0x40) {
        printf("uis: main menu: Online's switch isn't as expected; Online opens the port's page instead\n");
        return false;
    }
    u32 dflt = e + 19 + rd(e + 14);
    /* The port answers 0: on to Audio Settings' way in (the Online screen);
       1 (in a netplay session: the port's own page instead): nothing more,
       the input back on (as the entries' "not available" paths) and on as
       the switch's default. */
    u32 stub = g_code;
    emit8(0x41); emit_push(UIS_FN_ONLINE); emit_push(0); emit8(0x0B);
    emit_push(0); emit8(0x36);
    u32 skip = g_code;
    emit_push(0); emit8(0x3F);
    emit_jump(g_audio_case);
    wr(skip + 1, g_code - (skip + 6));
    emit8(0x41); emit_push(0xFFFFFFF5u); emit8(0x1A); emit8(0x43); emit32(0x0001F87Eu); emit8(0x42); emit8(0x42);
    emit_jump(dflt);
    wr(entry + 8, stub - (entry + 13));
    g_online_pos = (int)rd(entry + 2);
    printf("uis: main menu: Online opens the Online screen (case %u at script +%X, request %u bytes in)\n", rd(entry + 2),
           c - h, x - c);
    return true;
}

void uismenu_add_quit(CPUState* cpu) {
    R = cpu->ram;
    RS = cpu->ram_size;
    if (!R || RS < (PORT_RAM_BASE - G) + PORT_RAM_SIZE) return;
    u32 h = find_header();
    if (!h || rd(h) != NOBJ) return;   /* not loaded, or done already */
    g_h = h;
    u32 tab = h + 0x30;
    u32 ctrl = rd(tab);                /* object 0: the controller */
    u32 opt = rd(tab + OPT * 20);
    u32 vars = rd(h + 0xC);
    if (opt != h + INST_LO || ctrl + B_OPT != opt || rd(tab + (OPT + GRP) * 20) != ctrl + B_IS ||
        !str_is(rd(opt + BTN_LABEL), "Options") || rd(ctrl + 0xAC) > 5 || rd(ctrl + 0xD0) != 0 || rd(ctrl + 0xE4) != 5) {
        static bool told;
        if (!told) { told = true; printf("uis: main menu isn't laid out as expected; no Quit Game button\n"); }
        return;
    }
    u32 label = rd(opt + BTN_LABEL);   /* the label's variable; the two text buffers follow */
    u32 code_lo = h, code_hi = h + 0x40000u;
    if (!in_ram(code_hi, 0)) code_hi = G + RS;

    /* Find the script places first: change nothing unless all are there. */
    Switch sw[3];
    int nsw = 0;
    u32 focus = 0, hilite = 0, action = 0;
    /* Case 0 (Quick Game) of each: focus "W 1870 ..; call 1afc8", highlight
       "push 1; W 1870 ..; call 1b0a0" then its help line (var 0x16a), action
       "native 0x38(0x6c)". */
    static const u8 k_focus[] = {0x57, 0x00, 0x00, 0x18, 0x70};
    static const u8 k_focus2[] = {0x43, 0x00, 0x01, 0xAF, 0xC8};
    static const u8 k_hil[] = {0x10, 0, 0, 0, 1, 0x57, 0x00, 0x00, 0x18, 0x70};
    static const u8 k_hil2[] = {0x10, 0, 0, 0x01, 0x6A, 0x6F};
    static const u8 k_act[] = {0x10, 0, 0, 0, 0x38, 0x10, 0, 0, 0, 0x6C};
    for (u32 a = code_lo; a + 100 < code_hi && nsw < 3; a++) {
        Switch s;
        if (rb(a) != 0x55 || !parse_switch(a, &s)) continue;
        u32 lo = s.target[0], hi = s.target[1];
        if (hi <= lo || hi - lo > 0x200) continue;
        if (!focus && contains(lo, hi, k_focus, 5) && contains(lo, hi, k_focus2, 5)) { sw[nsw] = s; focus = (u32)nsw++ + 1; }
        else if (!hilite && contains(lo, hi, k_hil, 10) && contains(lo, hi, k_hil2, 6)) { sw[nsw] = s; hilite = (u32)nsw++ + 1; }
        else if (!action && contains(lo, hi, k_act, 10)) { sw[nsw] = s; action = (u32)nsw++ + 1; }
    }
    /* The navigation set-up (after Inside Street's): ... W 7dc,-9; call
       1af93; pops; then "push -5; pick". */
    u32 nav = 0;
    for (u32 a = code_lo; a + 32 < code_hi; a++) {
        if (rb(a) != 0x57 || rd(a + 1) != B_IS || rb(a + 5) != 0xFF || rb(a + 6) != 0xF7) continue;
        if (rb(a + 7) != 0x43 || rd(a + 8) != 0x0001AF93u) continue;
        u32 b = a + 12;
        while (rb(b) == 0x42) b++;
        if (rb(b) == 0x10 && rd(b + 1) == 0xFFFFFFFBu && rb(b + 5) == 0x1A) { nav = b; break; }
    }
    if (!focus || !hilite || !action || !nav) {
        static bool told;
        if (!told) {
            told = true;
            printf("uis: main menu script places not found (focus %u highlight %u action %u navigation %s); no Quit "
                   "Game button\n", focus, hilite, action, nav ? "yes" : "no");
        }
        return;
    }
    Switch* s_focus = &sw[focus - 1];
    Switch* s_hilite = &sw[hilite - 1];
    Switch* s_action = &sw[action - 1];
    for (int k = 0; k < 2; k++) {      /* case 5 must run on into the default's target */
        Switch* s = k ? s_hilite : s_focus;
        u32 last = 0;
        for (int i = 0; i < 6; i++) if (s->target[i] > last) last = s->target[i];
        if (last != s->target[5] || s->dflt <= s->target[5] || s->dflt - s->target[5] > 0x100) {
            printf("uis: main menu switch not as expected; no Quit Game button\n");
            return;
        }
    }

    /* 1. The copy of the Options group. */
    memset(R + (P_BASE - G), 0, P_TEXT_END - P_BASE);
    g_rg[0] = (Region){h + INST_LO, h + INST_HI, P_INST};
    g_rg[1] = (Region){h + META_LO, h + META_HI, P_META};
    g_rg[2] = (Region){h + DESC_LO, h + DESC_HI, P_DESC};
    g_rg[3] = (Region){h + DRAW_LO, h + DRAW_HI, P_DRAW};
    for (int i = 0; i < 4; i++) memcpy(R + (g_rg[i].nlo - G), R + (g_rg[i].lo - G), g_rg[i].hi - g_rg[i].lo);
    for (u32 a = P_META; a + 8 <= P_META + (META_HI - META_LO); a += 4) {   /* object numbers */
        u32 w = rd(a);
        if (w == 0xC000FFFFu || w == 0xFFFFFFFFu) {
            u32 n = rd(a + 4);
            if (n >= OPT && n < OPT + GRP) wr(a + 4, NEWIDX + n - OPT);
            a += 4;
        }
    }
    for (int i = 0; i < 4; i++)
        for (u32 a = g_rg[i].nlo; a + 4 <= g_rg[i].nlo + (g_rg[i].hi - g_rg[i].lo); a += 4)
            wr(a, reloc(rd(a), i != 2));
    /* Drawing records it uses from outside the copied group (its button
       icons, records showing a picture): its own copies too. Shared, the
       main menu's teardown gave each back twice (once for each object
       using it), leaving the A and B icons one short; the footer's A icon
       was then freed on the next screen after Create User ID or Enter
       Code. */
    int xrec = 0;
    {
        u32 xmap_old[24], xmap_new[24], xd = P_XDRAW;
        int nx = 0;
        for (int i = 0; i < 2; i++)    /* the instances and property lists hold their records' offsets */
            for (u32 a = g_rg[i].nlo; a + 4 <= g_rg[i].nlo + (g_rg[i].hi - g_rg[i].lo); a += 4) {
                u32 w = rd(a), rec = h + w;
                if (w < 0xA00u || w >= 0x10000u || !in_ram(rec, 0x60)) continue;
                bool inside = false;
                for (int k = 0; k < 4; k++) inside |= rec >= g_rg[k].lo && rec < g_rg[k].hi;
                u32 w0 = rd(rec);
                if (inside || (w0 >> 28) != 4 || !((w0 >> 16) & 2u) || s16_at(rec + 4) < 0 || s16_at(rec + 6) < 0 ||
                    s16_at(rec + 6) >= 16)
                    continue;
                int k = 0;
                while (k < nx && xmap_old[k] != w) k++;
                if (k == nx) {
                    if (nx == 24 || xd + 0x60 > P_XDRAW_END) continue;
                    memcpy(R + (xd - G), R + (rec - G), 0x60);
                    acquire_images(xd, xd + 0x60);
                    xmap_old[nx] = w; xmap_new[nx] = xd - h; nx++;
                    xd += 0x60;
                }
                wr(a, xmap_new[k]);
            }
        xrec = nx;
    }
    /* Its label and text: new variables. */
    u32 v_label = P_VARS, v_text1 = P_VARS + 12, v_text2 = P_VARS + 24;
    put_var(v_label, P_VARS + 0x40, 9, "Quit Game", 16);
    put_var(v_text1, P_VARS + 0x60, 30, "Quit Game", 32);
    put_var(v_text2, P_VARS + 0x80, 30, "Quit Game", 32);
    for (int i = 0; i < 4; i++)
        for (u32 a = g_rg[i].nlo; a + 8 <= g_rg[i].nlo + (g_rg[i].hi - g_rg[i].lo); a += 4) {
            u32 w = rd(a);
            if (w == label) wr(a, v_label);
            else if (w == label + 12 || w == label + 24) {
                wr(a, w == label + 12 ? v_text1 : v_text2);
                if (rd(a + 4) == 7) wr(a + 4, 9);
            }
        }
    u32 btn = P_INST;                  /* the new button's own object */
    u32 btn_off = btn - ctrl;
    wr(btn + BTN_LABEL, v_label);
    wr(btn + 0x08, 0x43E9C000u);       /* x 467.5: centred on its bar (385-615) */
    wr(btn + 0x0C, 0x43678000u);       /* y 231.5 (a third row) */
    wr(btn + NB_UP, 4);
    wr(btn + NB_DOWN, 1);
    wr(btn + NB_LEFT, NEWBTN);
    wr(btn + NB_RIGHT, NEWBTN);
    /* Its white highlight, made for "Options": longer to the right (the
       first drawing record, a quad -47 to 113 wide). */
    if (rd(P_DRAW) == 0x40030000u && s16_at(P_DRAW + QUAD_VERTS + 6) == 113 * 16 &&
        s16_at(P_DRAW + QUAD_VERTS + 12) == 113 * 16) {
        put_s16(P_DRAW + QUAD_VERTS + 6, HILITE_RIGHT * 16);
        put_s16(P_DRAW + QUAD_VERTS + 12, HILITE_RIGHT * 16);
    }
    /* Its help line. */
    u32 hv = vars + HELP_VAR_FREE * 12;
    if (rd(hv) == 1 && rd(hv + 4) >= 18) {
        u32 p = rd(hv + 8);
        memcpy(R + (p - G), "Exit NFL STREET 2.", 19);
        wr(hv + 4, 18);
    }


    /* 2. The object table and the root's children, with the copies. */
    memcpy(R + (P_OBJTAB - G), R + (tab - G), NOBJ * 20);
    for (u32 i = 0; i < GRP; i++) {
        u32 src = tab + (OPT + i) * 20, dst = P_OBJTAB + (NOBJ + i) * 20;
        wr(dst, reloc(rd(src), false));
        wr(dst + 4, rd(src + 4));
        wr(dst + 8, reloc(rd(src + 8), false));
        wr(dst + 12, rd(src + 12));
        wr(dst + 16, reloc(rd(src + 16), false));
    }
    {
        /* Children: the button after the other groups. */
        u32 n2 = rd(tab + 12), kids = rd(tab + 16), out = P_ROOTKIDS, k = 0;
        for (; k < n2 && rd(kids + k * 8) == 0xC000FFFFu; k++) {
            wr(out, rd(kids + k * 8)); wr(out + 4, rd(kids + k * 8 + 4)); out += 8;
        }
        wr(out, 0xC000FFFFu); wr(out + 4, NEWIDX); out += 8;
        for (; k < n2; k++) { wr(out, rd(kids + k * 8)); wr(out + 4, rd(kids + k * 8 + 4)); out += 8; }
        wr(P_OBJTAB + 12, (out - P_ROOTKIDS) / 8);
        wr(P_OBJTAB + 16, P_ROOTKIDS);
    }

    /* The root's list of the top-level groups (they fade / slide in when the
       menu opens; a copy made before that and left out stays invisible):
       with the copy. */
    {
        u32 n1 = rd(tab + 4), p1 = rd(tab + 8), out = P_LISTS;
        for (u32 i = 0; i < n1 && i < 16; i++) {
            u32 desc = rd(p1 + i * 4), n = rd(desc + 4), list = rd(desc + 8);
            if (n == 0 || n > 32 || !in_ram(list, n * 8)) continue;
            u32 add = 0;
            for (u32 k = 0; k < n; k++)
                if (rd(list + k * 8) == 0xFFFFFFFFu) {
                    if (rd(list + k * 8 + 4) == OPT) add = NEWIDX;
                }
            if (!add || out + (n + 1) * 8 > P_CODE) continue;
            /* It is also the drawing order: the copy goes before the first
               of the six groups (5; they go 5, 14 (Options), 23, ... 9
               apart), so the page each one opens is drawn over it. At the
               end, its label, dimmed while a page is open, showed over
               Inside Street's and Options' lists, which open over its row. */
            u32 o = out;
            bool put = false;
            for (u32 k = 0; k < n; k++) {
                if (!put && rd(list + k * 8) == 0xFFFFFFFFu && rd(list + k * 8 + 4) == OPT - GRP) {
                    wr(o, 0xFFFFFFFFu); wr(o + 4, add); o += 8; put = true;
                }
                wr(o, rd(list + k * 8)); wr(o + 4, rd(list + k * 8 + 4)); o += 8;
            }
            if (!put) { wr(o, 0xFFFFFFFFu); wr(o + 4, add); }
            wr(desc + 4, n + 1);
            wr(desc + 8, out);
            out += (n + 1) * 8;
        }
    }

    /* 3. Script. */
    g_code = P_CODE;
    /* Focus and highlight: index 6 like Inside Street's case, with the copy. */
    Switch* two[2] = {s_focus, s_hilite};
    for (int k = 0; k < 2; k++) {
        u32 c = copy_case(two[k]->target[5], two[k]->dflt, two[k]->dflt, btn_off);
        if (!c) return;
        wr(two[k]->dflt_imm, c - (two[k]->dflt_imm + 5));
    }
    /* Action: index 6 asks "quit?" in the game's own dialog (opcode 70 with
       3 arguments after the dialog kind 0x30001: title, message, choices; the
       script waits there and gets back the choice picked, 0 = the first), as
       the entries' "not available" message does. Yes quits; either way the
       input goes back on (the press turned it off: call 1f86e at the
       handler's start). */
    {
        u32 v_title = P_DIALOG, v_msg = P_DIALOG + 12, v_choice = P_DIALOG + 24;
        put_var(v_title, P_DIALOG + 0x40, 1, " ", 4);
        static const char k_msg[] = "Quit NFL STREET 2? Anything not saved will be lost.";
        put_var(v_msg, P_DIALOG + 0x50, (u32)strlen(k_msg), k_msg, 64);
        put_var(v_choice, P_DIALOG + 0x90, 8, "|Yes|^No", 16);
        u32 c = g_code;
        emit8(0x41); emit_push(0x00030001u); emit_push(v_title); emit_push(v_msg); emit_push(v_choice);
        emit8(0x70); emit8(0x03);
        emit8(0x41); emit_push(1); emit8(0x36);   /* picked Yes? (choices count from 1, as Retry is 1 in the game's Retry/Cancel dialog) */
        u32 skip = g_code;
        emit_push(0); emit8(0x3F);
        emit8(0x41); emit_push(UIS_FN_QUIT); emit_push(0); emit8(0x0B); emit8(0x42);
        wr(skip + 1, g_code - (skip + 6));
        emit8(0x42);                       /* the choice */
        emit8(0x41); emit_push(UIS_FN_FLICKER_DONE); emit_push(0); emit8(0x0B); emit8(0x42);
        emit8(0x41); emit_push(0xFFFFFFF5u); emit8(0x1A); emit8(0x43); emit32(0x0001F87Eu); emit8(0x42); emit8(0x42);
        emit_jump(s_action->dflt);
        wr(s_action->dflt_imm, c - (s_action->dflt_imm + 5));
    }
    /* Options: a Video Settings entry above Audio Settings. */
    bool video = add_video(h, code_hi);
    g_online_pos = -1;
    if (video) add_online(h, code_hi);
    /* Neighbours, set after the script's own set-up of the six. */
    {
        u32 c = g_code;
        static const u32 k_nb[][2] = {
            {B_OPT + NB_DOWN, NEWBTN}, {B_SR + NB_DOWN, NEWBTN}, {B_IS + NB_DOWN, NEWBTN}, {B_GM + NB_UP, NEWBTN},
        };
        for (unsigned i = 0; i < sizeof(k_nb) / sizeof(k_nb[0]); i++) {
            emit_push(k_nb[i][1]); emit8(0x56); emit32(k_nb[i][0]); emit8(0xFF); emit8(0xFB);
        }
        /* The start-up handler of the copy's text shadow (object OPT+1: the
           white text that jumps around behind a highlighted label). The game
           ran the original's when the menu loaded, starting its per-tick
           function; the copy, made later, needs it run: push it, call, pop.
           The controller is at -4 here. */
        {
            u32 rec = P_OBJTAB + (NEWIDX + 1) * 20, obj = rd(rec), n2 = rd(rec + 12), p2 = rd(rec + 16), fn = 0;
            for (u32 k = 0; k < n2 && k < 8; k++)
                if (rd(p2 + k * 8) == 0x8000FFFEu) fn = rd(p2 + k * 8 + 4);
            if (fn > h && fn < code_hi && obj >= P_INST && obj < P_META) {
                /* The handler multiplies its period (+0x98) by 10 before
                   starting; the copy took the original's already multiplied
                   value, so put back the one from before. */
                u32 period = rd(obj + 0x98);
                if (period >= 10 && period % 10 == 0) wr(obj + 0x98, period / 10);
                emit8(0x57); emit32(obj - ctrl); emit8(0xFF); emit8(0xFC);
                emit8(0x43); emit32(fn - h);
                emit8(0x42);
            }
        }
        for (int i = 0; i < 6; i++) emit8(rb(nav + (u32)i));   /* the replaced "push -5; pick" */
        /* That is the list position the menu opens on (the engine's first
           argument; the second, now at -7, is the page: 4 for Options). The
           screens under Options hand back their old place, from before Video
           Settings: one down for those below it. */
        if (video) {
            emit_push(0xFFFFFFF9u); emit8(0x1A); emit_push(4); emit8(0x36);
            u32 skip = g_code;
            emit_push(0); emit8(0x3F);     /* not Options: as it is */
            u32 fix[3];
            for (int k = 0; k < 3; k++) {  /* old 4, 3, 2 -> 6, 5, 4 (two entries added above) */
                u32 old_pos = 4 - (u32)k;
                emit8(0x41); emit_push(old_pos); emit8(0x36);
                u32 next = g_code;
                emit_push(0); emit8(0x3F);
                emit8(0x42);
                if (old_pos == 2) {
                    /* Old 2 is also Video Settings (it opens Audio
                       Settings' screen): the port says which it was. */
                    /* A native call: a slot for the answer, the function, its arguments
                       and their count; 0b takes the function and count, writing the
                       answer into the slot (the arguments are left to pop). */
                    /* The answer: 0 Audio Settings, 1 Video Settings, 2 Recomp
                       Settings; their places 4, 2, 3: 4 - 2a + 3(a == 2) ... as
                       a small table: answer 1 -> 2, 2 -> 3, else 4. */
                    emit_push(0); emit_push(UIS_FN_VIDEO_BACK); emit_push(0); emit8(0x0B);   /* -> the answer */
                    emit8(0x41); emit_push(1); emit8(0x36);
                    u32 not1 = g_code;
                    emit_push(0); emit8(0x3F);
                    emit8(0x42); emit_push(2);
                    u32 j1 = g_code;
                    emit_push(0); emit8(0x40);
                    wr(not1 + 1, g_code - (not1 + 6));
                    emit8(0x41); emit_push(2); emit8(0x36);
                    u32 not2 = g_code;
                    emit_push(0); emit8(0x3F);
                    emit8(0x42); emit_push(3);
                    u32 j2 = g_code;
                    emit_push(0); emit8(0x40);
                    wr(not2 + 1, g_code - (not2 + 6));
                    /* 3: Online (it borrows Audio Settings' way in): its
                       place in the Game Modes list (the port has made the
                       page Game Modes, main.c) */
                    emit_push(3); emit8(0x36);
                    u32 not3 = g_code;
                    emit_push(0); emit8(0x3F);
                    emit_push(g_online_pos >= 0 ? (u32)g_online_pos : 2u);
                    u32 j3 = g_code;
                    emit_push(0); emit8(0x40);
                    wr(not3 + 1, g_code - (not3 + 6));
                    emit_push(4);
                    wr(j1 + 1, g_code - (j1 + 6));
                    wr(j2 + 1, g_code - (j2 + 6));
                    wr(j3 + 1, g_code - (j3 + 6));
                } else {
                    emit_push(old_pos + 2);
                }
                fix[k] = g_code;
                emit_push(0); emit8(0x40);
                wr(next + 1, g_code - (next + 6));
            }
            wr(skip + 1, g_code - (skip + 6));
            for (int k = 0; k < 3; k++) wr(fix[k] + 1, g_code - (fix[k] + 6));
        }
        emit_jump(nav + 6);
        put_jump(nav, c);
    }
    /* Each page's look of the six: after Inside Street's, the copy's (as
       Inside Street's, or "dim" where that is "active"). Pattern:
       dup; push state; W btn,-n; call 1b0a0; pops. */
    int looks = 0;
    for (u32 a = code_lo + 32; a + 40 < code_hi; a++) {
        if (rb(a) != 0x41 || rb(a + 1) != 0x10 || rb(a + 6) != 0x57 || rd(a + 7) != B_IS || rb(a + 11) != 0xFF ||
            rb(a + 13) != 0x43 || rd(a + 14) != 0x0001B0A0u)
            continue;
        u32 len = 18;
        while (rb(a + len) == 0x42 && len < 24) len++;
        /* Options' call right before (the page lists go ... Options, Inside Street). */
        u32 p = a - 1;
        int pops = 0;
        while (rb(p) == 0x42 && pops < 6) { p--; pops++; }
        if (pops == 0 || p < 17 + code_lo) continue;
        u32 q = p - 17;                /* start of Options' call */
        if (rb(q) != 0x41 || rb(q + 6) != 0x57 || rd(q + 7) != B_OPT || rd(q + 14) != 0x0001B0A0u) continue;
        if (!code_room(len * 2 + 8)) break;
        u32 c = g_code;
        for (u32 i = 0; i < len; i++) emit8(rb(a + i));
        u32 state = rd(a + 2) == 4 ? 3 : rd(a + 2);
        emit8(0x41); emit_push(state);
        emit8(0x57); emit32(btn_off); emit8(rb(a + 11)); emit8(rb(a + 12));
        emit8(0x43); emit32(0x0001B0A0u);
        for (u32 i = 18; i < len; i++) emit8(0x42);
        emit_jump(a + len);
        put_jump(a, c);
        looks++;
        a += len - 1;
    }

    /* 4. A shorter red bar for the third row, under the button: the second
       row's two quads, copied, moved down a row and cut to the button's part. */
    bool bar = add_bar(h, tab);

    /* 5. In use: the table with the copy. */
    wr(h, NOBJ + GRP);
    wr(h + 4, P_OBJTAB);
    int pics = acquire_images(P_DRAW, P_DRAW + (DRAW_HI - DRAW_LO)) + xrec;
    printf("uis: main menu: Quit Game button added%s%s (%d page looks, %u bytes of script, %d pictures taken)\n",
           bar ? " with its bar" : "", video ? ", Options: Video Settings" : "", looks, g_code - P_CODE, pics);
}

/* ------------------------------------------ title screen: no licence --- */
/* The GameCube title screen shows "Licensed by Nintendo" under "Press
   START"; the PS2 and Xbox ones don't. It is a text record in the title
   script pointing at a variable holding the words, with its own length:
   that length is set to 0, so the line isn't drawn. */
/* The main menu's flicker objects (the glitch effect that brings a list's
   text in piece by piece): a rate function counts +0x70 up and switches
   pieces on and off; each piece is a pair (its drawing record's offset, on)
   from +0x90, with +0x8C the first. The quit dialog opening part way through
   left the count stopped and pieces off (the lists' entries invisible):
   all pieces on, the count back at 0, as a finished flicker leaves them. */
#define FLICKER_START 0x1B47Bu         /* their start-up handler (script offset) */
void uismenu_flicker_done(CPUState* cpu) {
    R = cpu->ram;
    RS = cpu->ram_size;
    u32 h = find_header();
    if (!h) return;
    u32 n = rd(h), tab = rd(h + 4), fixed = 0;
    if (!in_ram(tab, n * 20) || n > 0x200) return;
    for (u32 k = 0; k < n; k++) {
        u32 inst = rd(tab + k * 20), n2 = rd(tab + k * 20 + 12), p2 = rd(tab + k * 20 + 16);
        if (!in_ram(inst, 0x100) || n2 > 64 || !in_ram(p2, n2 * 8)) continue;
        bool flicker = false;
        for (u32 j = 0; j < n2; j++)
            if (rd(p2 + j * 8) == 0x8000FFFEu && rd(p2 + j * 8 + 4) == h + FLICKER_START) flicker = true;
        if (!flicker) continue;
        bool any = false;
        if (rd(inst + 0x8C) == 0) { wr(inst + 0x8C, 1); any = true; }
        for (u32 off = 0x90; off < 0x100; off += 8) {
            u32 rec = rd(inst + off);
            if (rec < 0x100u || rec > 0x40000u) break;     /* past the pieces */
            if (rd(inst + off + 4) == 0) { wr(inst + off + 4, 1); any = true; }
        }
        if (any) { wr(inst + 0x70, 0); fixed++; }
    }
    if (fixed) printf("uis: main menu: %u text flicker(s) cut short by the quit dialog finished\n", fixed);
}

void uismenu_hide_licence(CPUState* cpu) {
    R = cpu->ram;
    RS = cpu->ram_size;
    if (!R) return;
    static const char k_text[] = "Licensed by Nintendo";
    const u32 n = sizeof(k_text) - 1;
    static u32 g_done_rec;             /* the record hidden last time (still loaded: nothing to do) */
    if (g_done_rec && in_ram(g_done_rec, 8) && rd(g_done_rec + 4) == 0) {
        u32 var = rd(g_done_rec);
        if (in_ram(var, 12) && rd(var + 4) == n && in_ram(rd(var + 8), n) && memcmp(R + (rd(var + 8) - G), k_text, n) == 0)
            return;
    }
    for (u32 o = 0x00800000u; o + n + 4 < RS && o < 0x01800000u; o++) {
        if (R[o] != 'L' || memcmp(R + o, k_text, n) != 0) continue;
        u32 str = G + o;
        /* Its variable (type 1, length 20, pointer) just before the texts. */
        for (u32 v = str - 4; v + 0x2000 > str && v >= G + 12; v -= 4) {
            if (rd(v) != str || rd(v - 4) != n || rd(v - 8) != 1) continue;
            u32 var = v - 8;
            /* The text records (pointer to the variable, then its length). */
            int hidden = 0;
            for (u32 r = var - 4; r + 0x4000 > var && r >= G; r -= 4)
                if (rd(r) == var && rd(r + 4) == n) { wr(r + 4, 0); hidden++; g_done_rec = r; }
            static bool told;
            if (hidden && !told) {
                told = true;
                printf("uis: title screen: \"Licensed by Nintendo\" hidden\n");
            }
            return;
        }
    }
}

/* ------------------------------------------- Video Settings screen --- */
/* Video Settings opens the Audio Settings screen (OPTIONS group 5, screen
   5) in "video mode": its title becomes "Video Settings" and its three
   volume rows are left out, so the choice row (the GameCube's Video row)
   is what remains. Called while that screen's script sets itself up. */
#define PV_BASE (PORT_RAM_BASE + 0x20000u)
#define PV_TEXT (PV_BASE + 0x0000u)
#define PV_KIDS (PV_BASE + 0x0100u)
#define PV_LIST (PV_BASE + 0x0200u)
#define PV_KIDS2 (PV_BASE + 0x0300u)   /* the root's children with the row copies */
#define PV_OBJTAB (PV_BASE + 0x0400u)   /* the object table with the row copies */
#define PV_ROWLIST (PV_BASE + 0x0980u)  /* the root's list of rows (seven rows: the table runs to 0x978) */
#define PV_CODE (PV_BASE + 0x0A00u)     /* script added to the screen */
#define PV_VARS (PV_BASE + 0x0C00u)     /* the copies' text variables */
#define PV_HEAP (PV_BASE + 0x1000u)     /* the copies themselves */
#define PV_END (PV_BASE + 0x8000u)
#define VROWS 7                          /* rows on the screen: the original + 6 copies */
#define VROW_DY 50.0f                    /* the rows' spacing (the volume rows' own) */
#define VROW_DY6 40.0f                   /* with six rows: closer, so they take the room five did */
#define VROW_DY7 35.0f                   /* ... and seven */
#define AUD_NOBJ 0x22u
#define AUD_NVAR 0x27u
#define AUD_NFUNC 7u

/* The choice row (object 1) and its parts (objects 2-6), copied for more
   rows. A copy takes: the parts' instances (one block), their object records'
   lists, descriptors and descriptor lists, and their drawing records; pointers
   and script-relative offsets into any of those are moved to the copy, object
   numbers to the copy's. Each copy gets its own label and value text
   variables (the two text records point at buffer variables the row writes
   its texts into). */
#define VR_FIRST 1u
#define VR_LAST 6u
#define VR_MAXREG 64
static Region g_vr[VR_MAXREG];
static int g_nvr;
static u32 g_vheap;

static u32 vr_alloc(u32 n) {
    u32 a = (g_vheap + 31u) & ~31u;
    if (a + n > PV_END) return 0;
    g_vheap = a + n;
    return a;
}

static bool vr_add(u32 lo, u32 hi) {   /* a block to copy (once) */
    for (int i = 0; i < g_nvr; i++)
        if (lo >= g_vr[i].lo && hi <= g_vr[i].hi) return true;
    if (g_nvr == VR_MAXREG || hi <= lo || !in_ram(lo, hi - lo)) return false;
    g_vr[g_nvr++] = (Region){lo, hi, 0};
    return true;
}

static u32 vr_reloc(u32 w, u32 h, bool offsets) {
    for (int i = 0; i < g_nvr; i++)
        if (w >= g_vr[i].lo && w < g_vr[i].hi) return w - g_vr[i].lo + g_vr[i].nlo;
    if (offsets && w >= 0x800u && w < 0x40000u) {
        u32 a = h + w;
        for (int i = 0; i < g_nvr; i++)
            if (a >= g_vr[i].lo && a < g_vr[i].hi) return a - g_vr[i].lo + g_vr[i].nlo - h;
    }
    return w;
}

static u32 g_vrow[VROWS];              /* the rows' instances (0: the original) */
static u32 g_vh;                       /* the video screen's script header (0: not open) */
static u32 g_vhelp;                    /* its help line's text record */
static u32 g_vhelp_var, g_vhelp_buf;
static u32 g_vval_var[VROWS], g_vval_buf[VROWS];
static bool g_vstarted;                /* its rows have been given their values */
static int g_vlast[VROWS];
static const UisVideoRows* g_vcb;      /* the open screen's rows */
static const UisVideoRows* g_vpages[3]; /* Video Settings, Recomp Settings, Online */
static int g_vpage;                    /* which the next one is */
static int g_vn = VROWS;               /* the open screen's row count */
static u32 g_vrow_text[VROWS];         /* their value text records */

static int video_rows(u32 h) {
    u32 tab = h + 0x30, nobj = rd(h);
    if (nobj != AUD_NOBJ) return 1;
    u32 ctrl = rd(tab);
    u32 row = rd(tab + VR_FIRST * 20);
    g_vrow[0] = row;
    /* The parts' instances: from the row's to the next object's. */
    u32 inst_lo = row, inst_hi = 0xFFFFFFFFu;
    for (u32 o = 0; o < nobj; o++) {
        u32 a = rd(tab + o * 20);
        if (o >= VR_FIRST && o <= VR_LAST) continue;
        if (a > inst_lo && a < inst_hi) inst_hi = a;
    }
    for (u32 o = VR_FIRST; o <= VR_LAST; o++)
        if (rd(tab + o * 20) >= inst_hi) return 1;
    /* The last word before the next object is the root's (a draw list entry):
       copy up to the row's text slots only. */
    if (inst_hi - inst_lo > 0x800u) return 1;
    g_nvr = 0;
    vr_add(inst_lo, inst_hi - 4);
    u32 draw_lo = 0xFFFFFFFFu, draw_hi = 0;
    for (u32 o = VR_FIRST; o <= VR_LAST; o++) {
        u32 r = tab + o * 20, n1 = rd(r + 4), p1 = rd(r + 8), n2 = rd(r + 12), p2 = rd(r + 16);
        if (n1) vr_add(p1, p1 + n1 * 4);
        if (n2) vr_add(p2, p2 + n2 * 8);
        for (u32 i = 0; i < n1; i++) {
            u32 q = rd(p1 + i * 4), c = rd(q + 4), l = rd(q + 8);
            vr_add(q, q + 12);
            if (c > 16) return 1;
            vr_add(l, l + c * 8);
            for (u32 j = 0; j < c; j++) {
                u32 k = rd(l + j * 8), v = rd(l + j * 8 + 4);
                if (k == 0xFFFFFFFFu || !in_ram(v, 4)) continue;
                u32 off = rd(v);
                if (off < 0x800u || off >= 0x40000u) continue;
                u32 size = (rd(h + off) >> 28) == 4 ? 0x60u : 0x40u;
                if (h + off < draw_lo) draw_lo = h + off;
                if (h + off + size > draw_hi) draw_hi = h + off + size;
            }
        }
    }
    /* Each part's focus links (+0x64 / +0x68 / +0x6c: script-relative
       offsets of a small block naming the part and its neighbours). */
    for (u32 o = VR_FIRST; o <= VR_LAST; o++) {
        u32 inst = rd(tab + o * 20), lo = 0xFFFFFFFFu, hi = 0;
        for (u32 x = 0x64; x <= 0x6C; x += 4) {
            u32 off = rd(inst + x);
            if (off < 0x40u || off >= 0x40000u) { lo = 0xFFFFFFFFu; break; }
            if (h + off < lo) lo = h + off;
            if (h + off + 12 > hi) hi = h + off + 12;
        }
        if (lo != 0xFFFFFFFFu && hi - lo <= 0x40u) vr_add(lo, hi);
    }
    /* The row's own text slots (+0x368 value, +0x370 label) point at its two
       text records, which follow the parts' quads. */
    u32 vslot = row + 0x368, lslot = row + 0x370;
    u32 voff = rd(vslot), loff = rd(lslot);
    if (voff < 0x800u || loff < 0x800u || voff >= 0x40000u || loff >= 0x40000u) return 1;
    if (h + voff + 0x40 > draw_hi) draw_hi = h + voff + 0x40;
    if (h + loff + 0x40 > draw_hi) draw_hi = h + loff + 0x40;
    if (draw_lo > h + voff) draw_lo = h + voff;
    if (draw_hi <= draw_lo || draw_hi - draw_lo > 0x1000u) return 1;
    vr_add(draw_lo, draw_hi);
    int nreg = g_nvr;

    /* The new object table: the screen's objects, then the copies. */
    u32 per = VR_LAST - VR_FIRST + 1, nnew = nobj + (u32)(g_vn - 1) * per;
    if (PV_OBJTAB + nnew * 20 > PV_ROWLIST) return 1;
    memcpy(R + (PV_OBJTAB - G), R + (tab - G), nobj * 20);
    g_vheap = PV_HEAP;
    u32 vars = PV_VARS;
    for (int c = 1; c < g_vn; c++) {
        u32 base = nobj + (u32)(c - 1) * per;   /* the copy's first object number */
        for (int i = 0; i < nreg; i++) {
            u32 n = g_vr[i].hi - g_vr[i].lo, a = vr_alloc(n);
            if (!a) return c;
            g_vr[i].nlo = a;
            memcpy(R + (a - G), R + (g_vr[i].lo - G), n);
        }
        for (int i = 0; i < nreg; i++) {
            u32 n = g_vr[i].hi - g_vr[i].lo;
            bool is_draw = g_vr[i].lo == draw_lo;
            for (u32 a = g_vr[i].nlo; a + 4 <= g_vr[i].nlo + n; a += 4) {
                u32 w = rd(a);
                if (w == 0xC000FFFFu || w == 0xFFFFFFFFu) {   /* object numbers in lists */
                    if (a + 8 <= g_vr[i].nlo + n && !is_draw) {
                        u32 idx = rd(a + 4);
                        if (idx >= VR_FIRST && idx <= VR_LAST) { wr(a + 4, base + idx - VR_FIRST); a += 4; continue; }
                    }
                    continue;
                }
                wr(a, vr_reloc(w, h, !is_draw));
            }
        }
        for (int i = 0; i < nreg; i++)   /* its pictures, taken (acquire_images) */
            if (g_vr[i].lo == draw_lo) acquire_images(g_vr[i].nlo, g_vr[i].nlo + (g_vr[i].hi - g_vr[i].lo));
        /* The focus-link offsets are small (below what vr_reloc takes as an
           offset): moved here, field by field. */
        for (u32 o = VR_FIRST; o <= VR_LAST; o++) {
            u32 inst = rd(tab + o * 20), cinst = vr_reloc(inst, h, false);
            for (u32 x = 0x64; x <= 0x6C; x += 4) {
                u32 off = rd(inst + x), a = h + off;
                for (int i = 0; i < nreg; i++)
                    if (a >= g_vr[i].lo && a < g_vr[i].hi) { wr(cinst + x, a - g_vr[i].lo + g_vr[i].nlo - h); break; }
            }
        }
        for (u32 o = VR_FIRST; o <= VR_LAST; o++) {
            u32 src = tab + o * 20, dst = PV_OBJTAB + (base + o - VR_FIRST) * 20;
            wr(dst, vr_reloc(rd(src), h, false));
            wr(dst + 4, rd(src + 4));
            wr(dst + 8, vr_reloc(rd(src + 8), h, false));
            wr(dst + 12, rd(src + 12));
            wr(dst + 16, vr_reloc(rd(src + 16), h, false));
        }
        u32 crow = vr_reloc(row, h, false);
        g_vrow[c] = crow;
        /* One row lower. */
        union { u32 u; float f; } y;
        y.u = rd(row + 0x0C); y.f += (g_vn >= 7 ? VROW_DY7 : g_vn >= 6 ? VROW_DY6 : VROW_DY) * (float)c; wr(crow + 0x0C, y.u);
        /* Its own text buffers: the text records point at variables. */
        u32 vrec = h + rd(crow + 0x368), lrec = h + rd(crow + 0x370);
        if (!in_ram(vrec, 0x40) || !in_ram(lrec, 0x40)) return c;
        u32 vvar = vars, lvar = vars + 12;
        u32 vbuf = vr_alloc(100), lbuf = vr_alloc(40);
        if (!vbuf || !lbuf) return c;
        put_var(vvar, vbuf, rd(rd(vrec + 0x20) + 4), (const char*)(R + (rd(rd(vrec + 0x20) + 8) - G)), 100);
        put_var(lvar, lbuf, rd(rd(lrec + 0x20) + 4), (const char*)(R + (rd(rd(lrec + 0x20) + 8) - G)), 40);
        wr(vrec + 0x20, vvar);
        wr(lrec + 0x20, lvar);
        g_vrow_text[c] = vrec;
        vars += 24;
    }
    g_vrow_text[0] = h + voff;
    /* Up / down: each row's focus links (+0x6c next, +0x68 previous: has
       one, the row itself, the other row; script-relative instances) chain
       the rows; the copies still pointed at the volume rows. */
    /* The link blocks are packed against the next data in the script (a
       block with no link has no third word: what follows is something
       else, the row's box among it), so each link gets a block of its own.
       The first row keeps its "no previous" block. */
    for (int r = 0; r < g_vn; r++) {
        u32 self = g_vrow[r] - h;
        u32 nb = vr_alloc(12);
        wr(nb, r < g_vn - 1 ? 1u : 0u); wr(nb + 4, self); wr(nb + 8, r < g_vn - 1 ? g_vrow[r + 1] - h : self);
        wr(g_vrow[r] + 0x6C, nb - h);
        if (r > 0) {
            u32 pb = vr_alloc(12);
            wr(pb, 1); wr(pb + 4, self); wr(pb + 8, g_vrow[r - 1] - h);
            wr(g_vrow[r] + 0x68, pb - h);
        }
    }
    /* Labels: written into each row's label text (its buffer variable and
       the record's length) and its label variable (+0x94, which the row's
       start-up uses). */
    for (int r = 0; r < g_vn; r++) {
        u32 lrec = h + rd(g_vrow[r] + 0x370), lv = rd(lrec + 0x20);
        const char* label = g_vcb->labels[r];
        u32 n = (u32)strlen(label);
        if (!in_ram(lv, 12) || rd(lv + 4) == 0 || !in_ram(rd(lv + 8), 40)) continue;
        memset(R + (rd(lv + 8) - G), 0, 40);
        memcpy(R + (rd(lv + 8) - G), label, n);
        wr(lv + 4, n);
        wr(lrec + 0x24, n);
        u32 lab = vars;                    /* a variable for +0x94 */
        put_var(lab, vr_alloc(32), n, label, 32);
        wr(g_vrow[r] + 0x94, lab);
        vars += 12;
    }
    wr(h, nnew);
    wr(h + 4, PV_OBJTAB);
    /* On screen and in the up / down order: after the original row, in the
       root's children and its list of rows. */
    {
        u32 r0 = PV_OBJTAB, n2 = rd(r0 + 12), kids = rd(r0 + 16), out = PV_KIDS2;
        for (u32 k = 0; k < n2 && k < 24; k++) {
            wr(out, rd(kids + k * 8)); wr(out + 4, rd(kids + k * 8 + 4)); out += 8;
            if (rd(kids + k * 8) == 0xC000FFFFu && rd(kids + k * 8 + 4) == VR_FIRST)
                for (int c = 1; c < g_vn; c++) { wr(out, 0xC000FFFFu); wr(out + 4, nobj + (u32)(c - 1) * per); out += 8; }
        }
        wr(r0 + 12, (out - (PV_KIDS2)) / 8);
        wr(r0 + 16, PV_KIDS2);
        u32 n1 = rd(r0 + 4), p1 = rd(r0 + 8);
        for (u32 i = 0; i < n1 && i < 8; i++) {
            u32 desc = rd(p1 + i * 4), n = rd(desc + 4), list = rd(desc + 8);
            if (n != 1 || rd(list) != 0xFFFFFFFFu || rd(list + 4) != VR_FIRST) continue;
            u32 lo = PV_ROWLIST;
            wr(lo, 0xFFFFFFFFu); wr(lo + 4, VR_FIRST);
            for (int c = 1; c < g_vn; c++) { wr(lo + (u32)c * 8, 0xFFFFFFFFu); wr(lo + (u32)c * 8 + 4, nobj + (u32)(c - 1) * per); }
            wr(desc + 4, (u32)g_vn);
            wr(desc + 8, lo);
        }
    }
    /* The copies' start-up handler (the game ran the original's when the
       screen loaded): called at the end of the controller's own start-up,
       in place of its last call. */
    u32 init = 0, cinit = 0;
    {
        u32 r = tab + VR_FIRST * 20, n2 = rd(r + 12), p2 = rd(r + 16);
        for (u32 k = 0; k < n2; k++) if (rd(p2 + k * 8) == 0x8000FFFEu) init = rd(p2 + k * 8 + 4);
        r = tab; n2 = rd(r + 12); p2 = rd(r + 16);
        for (u32 k = 0; k < n2 && k < 32; k++) if (rd(p2 + k * 8) == 0x8000FFFEu) cinit = rd(p2 + k * 8 + 4);
    }
    u32 call = 0;
    for (u32 a = cinit; cinit && a < cinit + 0x200; a++)
        if (rb(a) == 0x43 && rb(a + 5) == 0x42 && rb(a + 6) == 0x42 && rb(a + 7) == 0x42 && rb(a + 8) == 0x44) { call = a; break; }
    if (init && call) {
        u32 orig = rd(call + 1);
        g_code = PV_CODE;
        for (int c = 1; c < g_vn; c++) {
            emit8(0x57); emit32(g_vrow[c] - ctrl); emit8(0xFF); emit8(0xFE);
            emit8(0x43); emit32(init - h);
            emit8(0x42);
        }
        /* The screen's start-up paused the music (as Audio Settings does
           until its Music Volume row is picked); its function 3 with 10 (the
           music row's) starts it again. */
        emit_push(0); emit_push(0x80000003u); emit_push(10); emit_push(1); emit8(0x0B);
        emit8(0x42); emit8(0x42);   /* the argument, then the answer's slot */
        emit8(0x41); emit_push(0xFFFFFFFDu); emit8(0x1A); emit8(0x43); emit32(orig); emit8(0x42); emit8(0x42);
        emit8(0x44);
        wr(call + 1, PV_CODE - h);
    }
    return g_vn;
}

void uismenu_set_video_rows(const UisVideoRows* video, const UisVideoRows* recomp, const UisVideoRows* online) {
    g_vpages[0] = video;
    g_vpages[1] = recomp;
    g_vpages[2] = online;
    if (!g_vh) g_vcb = g_vpages[g_vpage];
}

void uismenu_set_video_page(int page) { g_vpage = page < 0 || page > 2 ? 0 : page; }

static int g_vfocus = -1;              /* the row with the cursor, last field */

int uismenu_video_focus(int* page) {
    if (page) {
        *page = -1;
        for (int k = 0; k < 3 && g_vh && g_vcb; k++)
            if (g_vcb == g_vpages[k]) { *page = k; break; }
    }
    return g_vh && g_vstarted ? g_vfocus : -1;
}

static int g_ven[VROWS];               /* each row usable (1), greyed out (0), not set yet (-1) */

/* Rows that can't be used now (Window Resolution in full screen): their
   texts greyed (the game's own "disabled" grey) and the up / down links
   stepping over them. Only redone when one changes. */
#define TEXT_NORMAL 0xDCDCDCFFu
#define TEXT_GREY 0x646464FFu
static void video_enable_rows(u32 h) {
    bool changed = false;
    for (int r = 0; r < g_vn; r++) {
        int en = !g_vcb->enabled || g_vcb->enabled(r) ? 1 : 0;
        if (en != g_ven[r]) { g_ven[r] = en; changed = true; }
    }
    /* Texts: kept grey every field (the game recolours a row as the
       cursor leaves it), put back once when usable again. */
    for (int r = 0; r < g_vn; r++) {
        if (g_ven[r] && !changed) continue;
        u32 vrec = g_vrow_text[r], lrec = h + rd(g_vrow[r] + 0x370);
        if (in_ram(lrec, 0x20)) wr(lrec + 0x18, g_ven[r] ? TEXT_NORMAL : TEXT_GREY);
        if (in_ram(vrec, 0x20) && rd(g_vrow[r] + 0x84) != 1) wr(vrec + 0x18, g_ven[r] ? TEXT_NORMAL : TEXT_GREY);
    }
    if (!changed) return;
    /* Links: each usable row to the usable rows either side. The first
       row's "no previous" block is the game's own (left alone). */
    for (int r = 0; r < g_vn; r++) {
        if (!g_ven[r]) continue;
        u32 self = g_vrow[r] - h;
        int nx = r + 1, pv = r - 1;
        while (nx < g_vn && !g_ven[nx]) nx++;
        while (pv >= 0 && !g_ven[pv]) pv--;
        u32 nb = h + rd(g_vrow[r] + 0x6C);
        if (nb >= PV_HEAP && nb + 12 <= PV_END) {
            wr(nb, nx < g_vn ? 1u : 0u); wr(nb + 4, self); wr(nb + 8, nx < g_vn ? g_vrow[nx] - h : self);
        }
        u32 pb = h + rd(g_vrow[r] + 0x68);
        if (r > 0 && pb >= PV_HEAP && pb + 12 <= PV_END) {
            wr(pb, pv >= 0 ? 1u : 0u); wr(pb + 4, self); wr(pb + 8, pv >= 0 ? g_vrow[pv] - h : self);
        }
    }
}

static bool video_open(void) {
    u32 h = g_vh;
    if (!h || !in_ram(h, 0x40) || rd(h) != AUD_NOBJ + (u32)(g_vn - 1) * (VR_LAST - VR_FIRST + 1) || rd(h + 4) != PV_OBJTAB)
        return false;
    /* Still the screen's memory (not freed and reused): its title and the
       first row's link to the second, both set by the port. */
    u32 vars = rd(h + 0xC);
    if (!in_ram(vars, 12) || rd(vars + 8) != PV_TEXT || !in_ram(g_vrow[0], 0x400) ||
        h + rd(g_vrow[0] + 0x6C) < PV_HEAP || h + rd(g_vrow[0] + 0x6C) >= PV_END)
        return false;
    if (g_vhelp && (!in_ram(g_vhelp_var, 12) || rd(g_vhelp_var + 8) != g_vhelp_buf)) return false;
    for (int r = 0; r < g_vn; r++)
        if (!in_ram(g_vrow_text[r], 0x40) || !in_ram(g_vval_var[r], 12) || rd(g_vval_var[r] + 8) != g_vval_buf[r])
            return false;
    return true;
}

void uismenu_video_closed(void) { g_vh = 0; }

bool uismenu_video_active(CPUState* cpu) {
    R = cpu->ram;
    RS = cpu->ram_size;
    if (!R) return false;
    if (!video_open()) { g_vh = 0; return false; }
    return true;
}

/* A text record showing one of the port's buffer variables: its text. The
   game's own scripts point the record at other variables (the value names
   "Normal" etc.), so it's pointed back first. */
static void set_text(u32 rec, u32 var, const char* t, u32 cap) {
    if (!in_ram(rec, 0x40) || !in_ram(var, 12) || !in_ram(rd(var + 8), cap)) return;
    u32 n = (u32)strlen(t);
    if (n >= cap) n = cap - 1;
    u32 buf = rd(var + 8);
    if (rd(rec + 0x20) == var && rd(rec + 0x24) == n && rd(var + 4) == n && memcmp(R + (buf - G), t, n) == 0) return;
    memset(R + (buf - G), 0, cap);
    memcpy(R + (buf - G), t, n);
    wr(var + 4, n);
    wr(rec + 0x20, var);
    wr(rec + 0x24, n);
}

/* Every video field while Video Settings is open: the rows show the port's
   settings, a value changed with left / right is applied, and the help line
   follows the row with the cursor. */
void uismenu_video_frame(CPUState* cpu) {
    if (!g_vh || !g_vcb) return;
    if (!uismenu_video_active(cpu)) return;
    u32 h = g_vh;
    /* The copies' start-up (run at the end of the screen's) sets their
       range; after it, the values go in. */
    if (!g_vstarted) {
        for (int r = 0; r < g_vn; r++)
            if (rd(g_vrow[r] + 0x80) != 1) return;   /* step not set yet: start-up still to run */
        for (int r = 0; r < g_vn; r++) {
            int n = g_vcb->count(r), v = g_vcb->get(r);
            wr(g_vrow[r] + 0x78, 0);
            wr(g_vrow[r] + 0x7C, (u32)(n - 1));
            wr(g_vrow[r] + 0x8C, 1);                  /* wraps round */
            wr(g_vrow[r] + 0x74, (u32)v);
            g_vlast[r] = v;
        }
        g_vstarted = true;
        for (int r = 0; r < g_vn; r++) g_ven[r] = -1;
        printf("uis: %s: rows show the port's settings\n", g_vcb->title);
    }
    video_enable_rows(h);
    int focus = -1;
    for (int r = 0; r < g_vn; r++) {
        u32 row = g_vrow[r];
        int v = (int)rd(row + 0x74), n = g_vcb->count(r);
        /* Only the row with the cursor changes (left / right); anything else
           moving a value isn't the player, so it's put back. */
        if (v < 0 || v >= n || (v != g_vlast[r] && (rd(row + 0x84) != 1 || g_ven[r] == 0))) {
            v = g_vlast[r];
            wr(row + 0x74, (u32)v);
        }
        int cur = g_vcb->get(r);
        if (v == g_vlast[r] && cur != v) {   /* changed elsewhere (F11, the port's menu) */
            v = g_vlast[r] = cur;
            wr(row + 0x74, (u32)v);
        }
        if (v != g_vlast[r]) {
            printf("uis: %s: row %d set to %s\n", g_vcb->title, r + 1, g_vcb->name(r, v));
            g_vcb->set(r, v);
            g_vlast[r] = v;
        }
        set_text(g_vrow_text[r], g_vval_var[r], g_vcb->name(r, v), 100);
        if (rd(row + 0x84) == 1) focus = r;
    }
    g_vfocus = focus;
    if (focus >= 0 && g_vhelp) set_text(g_vhelp, g_vhelp_var, g_vcb->help(focus), 200);
}

bool uismenu_video_screen(CPUState* cpu) {
    R = cpu->ram;
    RS = cpu->ram_size;
    if (!R || RS < (PV_BASE - G) + 0x1000u) return false;
    u32 h = 0;
    for (u32 o = 0x00800000u; o + 0x20 < RS && o < 0x01800000u; o += 4) {
        u32 a = G + o;
        if (rd(a) == AUD_NOBJ && rd(a + 4) == a + 0x30 && rd(a + 8) == AUD_NVAR && rd(a + 0x10) == AUD_NFUNC &&
            str_is(rd(a + 0xC), "Audio Settings")) { h = a; break; }
    }
    if (!h) { printf("uis: Video Settings: the Audio Settings screen isn't as expected\n"); return false; }
    u32 vars = rd(h + 0xC), tab = h + 0x30;
    g_vcb = g_vpages[g_vpage];
    if (!g_vcb) return false;
    g_vn = g_vcb->rows < 1 ? 1 : g_vcb->rows > VROWS ? VROWS : g_vcb->rows;
    /* Titles: var 0 ("Audio Settings", Xbox / PS2 layout) and var 2
       ("Audio/Visual", GameCube). */
    u32 tl = (u32)strlen(g_vcb->title);
    memset(R + (PV_TEXT - G), 0, 0x100);
    memcpy(R + (PV_TEXT - G), g_vcb->title, tl);
    wr(vars + 0 * 12 + 4, tl); wr(vars + 0 * 12 + 8, PV_TEXT);
    wr(vars + 2 * 12 + 4, tl); wr(vars + 2 * 12 + 8, PV_TEXT);
    /* The volume rows (objects 7, 14, 21) out of the root's children and its
       list of rows. */
    static const u32 k_vol[3] = {0x07, 0x0E, 0x15};
    #define IS_VOL(n) ((n) == k_vol[0] || (n) == k_vol[1] || (n) == k_vol[2])
    u32 n2 = rd(tab + 12), kids = rd(tab + 16), out = PV_KIDS;
    for (u32 k = 0; k < n2 && k < 24; k++) {
        u32 key = rd(kids + k * 8), val = rd(kids + k * 8 + 4);
        if (key == 0xC000FFFFu && IS_VOL(val)) continue;
        wr(out, key); wr(out + 4, val); out += 8;
    }
    wr(tab + 12, (out - PV_KIDS) / 8);
    wr(tab + 16, PV_KIDS);
    u32 n1 = rd(tab + 4), p1 = rd(tab + 8), lout = PV_LIST;
    for (u32 i = 0; i < n1 && i < 8; i++) {
        u32 desc = rd(p1 + i * 4), n = rd(desc + 4), list = rd(desc + 8);
        if (!n || n > 16 || !in_ram(list, n * 8)) continue;
        bool any = false;
        for (u32 k = 0; k < n; k++) if (rd(list + k * 8) == 0xFFFFFFFFu && IS_VOL(rd(list + k * 8 + 4))) any = true;
        if (!any) continue;
        u32 start = lout, m = 0;
        for (u32 k = 0; k < n; k++) {
            if (rd(list + k * 8) == 0xFFFFFFFFu && IS_VOL(rd(list + k * 8 + 4))) continue;
            wr(lout, rd(list + k * 8)); wr(lout + 4, rd(list + k * 8 + 4)); lout += 8; m++;
        }
        wr(desc + 4, m);
        wr(desc + 8, start);
    }
    #undef IS_VOL
    /* The help line: the text record showing var 34 (the focused row's
       help, copied there by the rows' focus handlers). */
    g_vhelp = 0;
    for (u32 a = h + 0x3000u; a < h + 0x5000u; a += 0x20)
        if (rd(a + 0x20) == vars + 34 * 12 && (rd(a) & 0xFFFFu) == 0x0402u) { g_vhelp = a; break; }
    g_vhelp_var = vars + 34 * 12;
    int rows = video_rows(h);
    for (int r = 0; r < g_vn && rows == g_vn; r++) {
        g_vrow_text[r] = h + rd(g_vrow[r] + 0x368);
        g_vval_var[r] = rd(g_vrow_text[r] + 0x20);
        g_vval_buf[r] = rd(g_vval_var[r] + 8);
    }
    g_vhelp_buf = rd(g_vhelp_var + 8);
    g_vh = rows == g_vn ? h : 0;
    g_vstarted = false;
    printf("uis: %s: Audio Settings screen opened in video mode (%d rows)\n", g_vcb->title, rows);
    return true;
}

/* ------------------------------------- Save/Load: Delete User ID row --- */
/* The Save/Load screen (Auto Save, a choice row, then five buttons) gets a
   second choice row under its buttons: Delete User ID. It is the Auto Save
   row (object 1 and its parts 2-6) copied the way the video screens copy
   the Audio Settings row (video_rows), into the port's memory: the parts'
   instances, their object records' lists and descriptors, drawing records
   and focus link blocks, with pointers and offsets into those moved to the
   copy. Up / down: the last button (Remove NFL Gauntlet Team) leads to it
   and it to Auto Save (the screen's list wraps round). Its start-up runs at
   the end of the screen's own (as for the video rows); left / right change
   its value as the row does Auto Save's, but the screen only reads Auto
   Save's own row (and has nothing for an A on this one), so the port gives
   it its User IDs, help line and A. */
#define SL_NOBJ 0x26u
#define SL_BASE (PORT_RAM_BASE + 0x30000u)
#define SL_TEXT (SL_BASE + 0x0000u)      /* the label */
#define SL_KIDS (SL_BASE + 0x0100u)      /* the root's children with the copy */
#define SL_LIST (SL_BASE + 0x0300u)      /* the root's list of rows and buttons with the copy */
#define SL_OBJTAB (SL_BASE + 0x0400u)    /* the object table with the copy (0x2C objects) */
#define SL_CODE (SL_BASE + 0x0A00u)      /* script added to the screen */
#define SL_VARS (SL_BASE + 0x0C00u)      /* the copy's text variables */
#define SL_HEAP (SL_BASE + 0x1000u)      /* the copy itself */
#define SL_END (SL_BASE + 0x8000u)

static u32 g_sh;                       /* the Save/Load screen's script header (0: not open / no row) */
static u32 g_srow;                     /* the copy's instance */
static u32 g_stext, g_sval_var, g_sval_buf;   /* its value text record and variable */
static u32 g_shelp, g_shelp_var;       /* the screen's help line: text record, variable (the game's) */
static bool g_sstarted;
static int g_slast;
static const UisVideoRows* g_scb;
static u32 g_sheap;

void uismenu_set_saveload_row(const UisVideoRows* row) { g_scb = row; }

static u32 sl_alloc(u32 n) {
    u32 a = (g_sheap + 31u) & ~31u;
    if (a + n > SL_END) return 0;
    g_sheap = a + n;
    return a;
}

static bool sl_is_screen(u32 h) {
    if (!in_ram(h, 0x40) || rd(h) != SL_NOBJ || rd(h + 4) != h + 0x30) return false;
    u32 vars = rd(h + 0xC);
    return str_is(vars + 42 * 12, "Auto Save") && str_is(vars + 58 * 12, "Save/Load") &&
           str_is(vars + 50 * 12, "Remove NFL Gauntlet Team");
}

static bool sl_build(u32 h) {
    u32 tab = h + 0x30, nobj = SL_NOBJ, vars = rd(h + 0xC);
    u32 ctrl = rd(tab), row = rd(tab + VR_FIRST * 20), last = rd(tab + 0x1B * 20), prev_btn = rd(tab + 0x16 * 20);
    /* The parts' instances: from the row's to the next object's. */
    u32 inst_lo = row, inst_hi = 0xFFFFFFFFu;
    for (u32 o = 0; o < nobj; o++) {
        u32 a = rd(tab + o * 20);
        if (o >= VR_FIRST && o <= VR_LAST) continue;
        if (a > inst_lo && a < inst_hi) inst_hi = a;
    }
    for (u32 o = VR_FIRST; o <= VR_LAST; o++)
        if (rd(tab + o * 20) >= inst_hi) return false;
    if (inst_hi - inst_lo > 0x800u) return false;
    g_nvr = 0;
    /* (all of it: here the row's label slot, +0x370, is the last word) */
    vr_add(inst_lo, inst_hi);
    u32 draw_lo = 0xFFFFFFFFu, draw_hi = 0;
    for (u32 o = VR_FIRST; o <= VR_LAST; o++) {
        u32 r = tab + o * 20, n1 = rd(r + 4), p1 = rd(r + 8), n2 = rd(r + 12), p2 = rd(r + 16);
        if (n1) vr_add(p1, p1 + n1 * 4);
        if (n2) vr_add(p2, p2 + n2 * 8);
        for (u32 i = 0; i < n1; i++) {
            u32 q = rd(p1 + i * 4), c = rd(q + 4), l = rd(q + 8);
            vr_add(q, q + 12);
            if (c > 16) return false;
            vr_add(l, l + c * 8);
            for (u32 j = 0; j < c; j++) {
                u32 k = rd(l + j * 8), v = rd(l + j * 8 + 4);
                if (k == 0xFFFFFFFFu || !in_ram(v, 4)) continue;
                u32 off = rd(v);
                if (off < 0x800u || off >= 0x40000u) continue;
                u32 size = (rd(h + off) >> 28) == 4 ? 0x60u : 0x40u;
                if (h + off < draw_lo) draw_lo = h + off;
                if (h + off + size > draw_hi) draw_hi = h + off + size;
            }
        }
    }
    for (u32 o = VR_FIRST; o <= VR_LAST; o++) {
        u32 inst = rd(tab + o * 20), lo = 0xFFFFFFFFu, hi = 0;
        for (u32 x = 0x64; x <= 0x6C; x += 4) {
            u32 off = rd(inst + x);
            if (off < 0x40u || off >= 0x40000u) { lo = 0xFFFFFFFFu; break; }
            if (h + off < lo) lo = h + off;
            if (h + off + 12 > hi) hi = h + off + 12;
        }
        if (lo != 0xFFFFFFFFu && hi - lo <= 0x40u) vr_add(lo, hi);
    }
    u32 voff = rd(row + 0x368), loff = rd(row + 0x370);
    if (voff < 0x800u || loff < 0x800u || voff >= 0x40000u || loff >= 0x40000u) return false;
    if (h + voff + 0x40 > draw_hi) draw_hi = h + voff + 0x40;
    if (h + loff + 0x40 > draw_hi) draw_hi = h + loff + 0x40;
    if (draw_lo > h + voff) draw_lo = h + voff;
    if (draw_hi <= draw_lo || draw_hi - draw_lo > 0x1000u) return false;
    vr_add(draw_lo, draw_hi);
    int nreg = g_nvr;

    /* The new object table: the screen's objects, then the copy. */
    u32 per = VR_LAST - VR_FIRST + 1, nnew = nobj + per, base = nobj;
    if (SL_OBJTAB + nnew * 20 > SL_CODE) return false;
    memcpy(R + (SL_OBJTAB - G), R + (tab - G), nobj * 20);
    g_sheap = SL_HEAP;
    for (int i = 0; i < nreg; i++) {
        u32 n = g_vr[i].hi - g_vr[i].lo, a = sl_alloc(n);
        if (!a) return false;
        g_vr[i].nlo = a;
        memcpy(R + (a - G), R + (g_vr[i].lo - G), n);
    }
    for (int i = 0; i < nreg; i++) {
        u32 n = g_vr[i].hi - g_vr[i].lo;
        bool is_draw = g_vr[i].lo == draw_lo;
        for (u32 a = g_vr[i].nlo; a + 4 <= g_vr[i].nlo + n; a += 4) {
            u32 w = rd(a);
            if (w == 0xC000FFFFu || w == 0xFFFFFFFFu) {
                if (a + 8 <= g_vr[i].nlo + n && !is_draw) {
                    u32 idx = rd(a + 4);
                    if (idx >= VR_FIRST && idx <= VR_LAST) { wr(a + 4, base + idx - VR_FIRST); a += 4; continue; }
                }
                continue;
            }
            wr(a, vr_reloc(w, h, !is_draw));
        }
    }
    for (u32 o = VR_FIRST; o <= VR_LAST; o++) {
        u32 inst = rd(tab + o * 20), cinst = vr_reloc(inst, h, false);
        for (u32 x = 0x64; x <= 0x6C; x += 4) {
            u32 off = rd(inst + x), a = h + off;
            for (int i = 0; i < nreg; i++)
                if (a >= g_vr[i].lo && a < g_vr[i].hi) { wr(cinst + x, a - g_vr[i].lo + g_vr[i].nlo - h); break; }
        }
    }
    for (int i = 0; i < nreg; i++)       /* its pictures, taken (acquire_images) */
        if (g_vr[i].lo == draw_lo) acquire_images(g_vr[i].nlo, g_vr[i].nlo + (g_vr[i].hi - g_vr[i].lo));
    for (u32 o = VR_FIRST; o <= VR_LAST; o++) {
        u32 src = tab + o * 20, dst = SL_OBJTAB + (base + o - VR_FIRST) * 20;
        wr(dst, vr_reloc(rd(src), h, false));
        wr(dst + 4, rd(src + 4));
        wr(dst + 8, vr_reloc(rd(src + 8), h, false));
        wr(dst + 12, rd(src + 12));
        wr(dst + 16, vr_reloc(rd(src + 16), h, false));
    }
    u32 crow = vr_reloc(row, h, false);
    /* Under the last button, a button's step lower (the buttons' own spacing). */
    union { u32 u; float f; } y, yl, yp;
    yl.u = rd(last + 0x0C); yp.u = rd(prev_btn + 0x0C);
    float step = yl.f - yp.f;
    if (step < 20.0f || step > 60.0f) step = 32.0f;
    y.f = yl.f + step;
    wr(crow + 0x0C, y.u);
    /* Its own text buffers. */
    u32 vrec = h + rd(crow + 0x368), lrec = h + rd(crow + 0x370);
    if (!in_ram(vrec, 0x40) || !in_ram(lrec, 0x40)) return false;
    u32 vvar = SL_VARS, lvar = SL_VARS + 12, labvar = SL_VARS + 24;
    u32 vbuf = sl_alloc(100), lbuf = sl_alloc(40), labbuf = sl_alloc(32);
    if (!vbuf || !lbuf || !labbuf) return false;
    put_var(vvar, vbuf, rd(rd(vrec + 0x20) + 4), (const char*)(R + (rd(rd(vrec + 0x20) + 8) - G)), 100);
    static const char k_label[] = "Delete User ID";
    u32 ln = (u32)strlen(k_label);
    put_var(lvar, lbuf, ln, k_label, 40);
    put_var(labvar, labbuf, ln, k_label, 32);
    wr(vrec + 0x20, vvar);
    wr(lrec + 0x20, lvar);
    wr(lrec + 0x24, ln);
    wr(crow + 0x94, labvar);            /* (the row's start-up writes its label from this) */
    /* Up / down: the last button down to it, it down to Auto Save (the list
       wraps round, as the last button did), Auto Save up to it. */
    {
        u32 self = crow - h;
        u32 nb = sl_alloc(12), pb = sl_alloc(12);
        if (!nb || !pb) return false;
        wr(nb, 1); wr(nb + 4, self); wr(nb + 8, row - h);
        wr(pb, 1); wr(pb + 4, self); wr(pb + 8, last - h);
        wr(crow + 0x6C, nb - h);
        wr(crow + 0x68, pb - h);
        u32 ln6 = h + rd(last + 0x6C), rp = h + rd(row + 0x68);
        if (!in_ram(ln6, 12) || rd(ln6) != 1 || rd(ln6 + 4) != last - h) return false;
        if (!in_ram(rp, 12) || rd(rp) != 1 || rd(rp + 4) != row - h) return false;
        wr(ln6 + 8, self);
        wr(rp + 8, self);
    }
    wr(h, nnew);
    wr(h + 4, SL_OBJTAB);
    /* On screen and in the up / down order: after the last button, in the
       root's children and its list of rows and buttons. */
    {
        u32 r0 = SL_OBJTAB, n2 = rd(r0 + 12), kids = rd(r0 + 16), out = SL_KIDS;
        if (n2 > 60) return false;
        for (u32 k = 0; k < n2; k++) {
            wr(out, rd(kids + k * 8)); wr(out + 4, rd(kids + k * 8 + 4)); out += 8;
            if (rd(kids + k * 8) == 0xC000FFFFu && rd(kids + k * 8 + 4) == 0x1Bu) { wr(out, 0xC000FFFFu); wr(out + 4, base); out += 8; }
        }
        wr(r0 + 12, (out - SL_KIDS) / 8);
        wr(r0 + 16, SL_KIDS);
        u32 n1 = rd(r0 + 4), p1 = rd(r0 + 8);
        for (u32 i = 0; i < n1 && i < 8; i++) {
            u32 desc = rd(p1 + i * 4), n = rd(desc + 4), list = rd(desc + 8);
            if (n == 0 || n > 16 || rd(list + (n - 1) * 8) != 0xFFFFFFFFu || rd(list + (n - 1) * 8 + 4) != 0x1Bu) continue;
            u32 lo = SL_LIST;
            for (u32 k = 0; k < n; k++) { wr(lo + k * 8, rd(list + k * 8)); wr(lo + k * 8 + 4, rd(list + k * 8 + 4)); }
            wr(lo + n * 8, 0xFFFFFFFFu); wr(lo + n * 8 + 4, base);
            wr(desc + 4, n + 1);
            wr(desc + 8, lo);
        }
    }
    /* The copy's start-up (the game ran the row's as the screen loaded):
       called at the end of the root's own, in place of its last call
       ("dup; push -3; pick; call x; pop; pop; return"). */
    u32 init = 0, cinit = 0;
    {
        u32 r = tab + VR_FIRST * 20, n2 = rd(r + 12), p2 = rd(r + 16);
        for (u32 k = 0; k < n2; k++) if (rd(p2 + k * 8) == 0x8000FFFEu) init = rd(p2 + k * 8 + 4);
        r = tab; n2 = rd(r + 12); p2 = rd(r + 16);
        for (u32 k = 0; k < n2 && k < 64; k++) if (rd(p2 + k * 8) == 0x8000FFFEu) cinit = rd(p2 + k * 8 + 4);
    }
    u32 call = 0;
    for (u32 a = cinit; cinit && a < cinit + 0x100; a++)
        if (rb(a) == 0x43 && rb(a + 5) == 0x42 && rb(a + 6) == 0x42 && rb(a + 7) == 0x44 && rb(a - 1) == 0x1A) { call = a; break; }
    if (!init || !call) return false;
    {
        u32 orig = rd(call + 1);
        g_code = SL_CODE;
        emit_push(crow); emit8(0x43); emit32(init - h); emit8(0x42);
        emit8(0x41); emit_push(0xFFFFFFFDu); emit8(0x1A); emit8(0x43); emit32(orig); emit8(0x42); emit8(0x42);
        emit8(0x44);
        wr(call + 1, SL_CODE - h);
    }
    (void)ctrl;
    g_srow = crow;
    g_stext = vrec;
    g_sval_var = vvar;
    g_sval_buf = vbuf;
    /* The help line: the text record showing var 57. */
    g_shelp_var = vars + 57 * 12;
    g_shelp = 0;
    for (u32 a = h + 0x2000u; a < h + 0x6000u; a += 4)
        if (rd(a + 0x20) == g_shelp_var && (rd(a) & 0xFFFFu) == 0x0402u) { g_shelp = a; break; }
    return true;
}

/* The Save/Load screen is setting itself up (its platform check): add the row. */
bool uismenu_saveload_screen(CPUState* cpu) {
    R = cpu->ram;
    RS = cpu->ram_size;
    g_sh = 0;
    if (!R || !g_scb || RS < (SL_END - G)) return false;
    u32 h = 0;
    for (u32 o = 0x00800000u; o + 0x40 < RS && o < 0x01800000u; o += 4) {
        u32 a = G + o;
        if (rd(a) == SL_NOBJ && rd(a + 4) == a + 0x30 && sl_is_screen(a)) { h = a; break; }
    }
    if (!h) return false;
    if (!sl_build(h)) { printf("uis: Save/Load: the screen isn't as expected; no Delete User ID row\n"); return false; }
    g_sh = h;
    g_sstarted = false;
    printf("uis: Save/Load: Delete User ID row added\n");
    return true;
}

void uismenu_saveload_closed(void) { g_sh = 0; }

static bool sl_open(void) {
    u32 h = g_sh;
    if (!h || !in_ram(h, 0x40) || rd(h) != SL_NOBJ + (VR_LAST - VR_FIRST + 1) || rd(h + 4) != SL_OBJTAB) return false;
    if (!in_ram(g_srow, 0x400) || !in_ram(g_stext, 0x40) || !in_ram(g_sval_var, 12) || rd(g_sval_var + 8) != g_sval_buf)
        return false;
    return true;
}

/* Every video field: the row shows the User IDs; true while it has the cursor. */
bool uismenu_saveload_frame(CPUState* cpu) {
    if (!g_sh || !g_scb) return false;
    R = cpu->ram;
    RS = cpu->ram_size;
    if (!R || !sl_open()) { g_sh = 0; return false; }
    u32 row = g_srow;
    if (!g_sstarted) {
        if (rd(row + 0x80) != 1) return false;   /* its start-up still to run */
        int n = g_scb->count(0), v = g_scb->get(0);
        wr(row + 0x78, 0);
        wr(row + 0x7C, (u32)(n - 1));
        wr(row + 0x8C, 1);
        wr(row + 0x74, (u32)v);
        g_slast = v;
        g_sstarted = true;
    }
    int n = g_scb->count(0), v = (int)rd(row + 0x74);
    if ((int)rd(row + 0x7C) != n - 1) wr(row + 0x7C, (u32)(n - 1));
    /* (+0x84: 1 with the cursor; 2 too, after A: the row's own "picked"
       state, which it keeps until the cursor moves) */
    u32 st = rd(row + 0x84);
    bool focus = st == 1 || st == 2;
    bool en = !g_scb->enabled || g_scb->enabled(0);
    if (v < 0 || v >= n || (v != g_slast && (!focus || !en))) { v = g_slast < n ? g_slast : 0; wr(row + 0x74, (u32)v); }
    if (v != g_slast) { g_scb->set(0, v); g_slast = v; }
    int cur = g_scb->get(0);
    if (cur != v) { v = g_slast = cur; wr(row + 0x74, (u32)v); }
    set_text(g_stext, g_sval_var, g_scb->name(0, v), 100);
    u32 lrec = g_sh + rd(row + 0x370);
    /* Its label greyed while it can't be used (no User IDs, online); the
       value stays as the row draws it (dark on its light box). */
    if (in_ram(lrec, 0x20) && !focus) wr(lrec + 0x18, en ? TEXT_NORMAL : TEXT_GREY);
    if (focus && g_shelp) set_text(g_shelp, g_shelp_var, g_scb->help(0), 120);
    return focus;
}
