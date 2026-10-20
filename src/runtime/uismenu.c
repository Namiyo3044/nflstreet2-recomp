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

static u32 find_var(u32 vars, const char* str) {
    for (u32 i = 0; i < NVAR; i++)
        if (str_is(vars + i * 12, str)) return vars + i * 12;
    return 0;
}

/* The list texts: a new one with "|Video Settings" before "|<before>". */
static bool list_with_video(u32 var, const char* before, u32* text) {
    u32 len = rd(var + 4), p = rd(var + 8);
    char buf[160], out[192];
    if (len >= sizeof(buf)) return false;
    memcpy(buf, R + (p - G), len);
    buf[len] = 0;
    char* at = strstr(buf, before);
    if (!at) return false;
    int n = snprintf(out, sizeof(out), "%.*s|Video Settings%s", (int)(at - buf), buf, at);
    if (n <= 0 || *text + (u32)n + 4 > P_TEXT_END) return false;
    memcpy(R + (*text - G), out, (size_t)n);
    wr(var + 4, (u32)n);
    wr(var + 8, *text);
    *text += ((u32)n + 4) & ~3u;
    return true;
}

static const char k_opt_list[] = "|Game Settings|Jukebox|Audio Settings|Save/Load|Cheats and Codes";
static const char k_opt_list_gc[] = "|Game Settings|Jukebox|Audio/Visual|Save/Load|Cheats and Codes";
#define VIDEO_AT 2                     /* its place: after Jukebox, above Audio Settings */

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
    /* Help: as Game Settings' case, with the new line. */
    u32 hc = g_code;
    emit8(0x41); emit_push(v_help);    /* the variable itself, where "push n; var" gives it */
    for (u32 a = ht[0] + 7; a < ht[0] + 0x20; a++) emit8(rb(a));
    emit_jump(ht[0] + 0x26 + rd(ht[0] + 0x21));
    put_jump(hat, emit_chain(ht, 5, hd, VIDEO_AT, hc));
    /* Action: the port's video settings; then input back on (as the
       entries' "not available" paths) and on as the default does. */
    u32 ac = g_code;
    emit8(0x41); emit_push(UIS_FN_VIDEO); emit_push(0); emit8(0x0B); emit8(0x42);
    emit8(0x41); emit_push(0xFFFFFFF5u); emit8(0x1A); emit8(0x43); emit32(0x0001F87Eu); emit8(0x42); emit8(0x42);
    emit_jump(ad);
    put_jump(aat, emit_chain(at, 5, ad, VIDEO_AT, ac));
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
            memcpy(R + (out - G), R + (list - G), n * 8);
            wr(out + n * 8, 0xFFFFFFFFu);
            wr(out + n * 8 + 4, add);
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
    /* Action: index 6 asks "quit?". */
    {
        u32 c = g_code;
        emit8(0x41); emit_push(UIS_FN_QUIT); emit_push(0); emit8(0x0B); emit8(0x42);
        /* Not quitting: input back on, as the entries' "not available" path
           does (the press turned it off: call 1f86e at the handler's start). */
        emit8(0x41); emit_push(0xFFFFFFF5u); emit8(0x1A); emit8(0x43); emit32(0x0001F87Eu); emit8(0x42); emit8(0x42);
        emit_jump(s_action->dflt);
        wr(s_action->dflt_imm, c - (s_action->dflt_imm + 5));
    }
    /* Options: a Video Settings entry above Audio Settings. */
    bool video = add_video(h, code_hi);
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
            for (int k = 0; k < 3; k++) {  /* old 4, 3, 2 -> 5, 4, 3 */
                u32 old_pos = 4 - (u32)k;
                emit8(0x41); emit_push(old_pos); emit8(0x36);
                u32 next = g_code;
                emit_push(0); emit8(0x3F);
                emit8(0x42); emit_push(old_pos + 1);
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
    printf("uis: main menu: Quit Game button added%s%s (%d page looks, %u bytes of script)\n",
           bar ? " with its bar" : "", video ? ", Options: Video Settings" : "", looks, g_code - P_CODE);
}
