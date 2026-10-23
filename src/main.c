/* NFL Street 2 native port - bring-up runner.
 *
 * Milestone 1: load the game's own main.dol into guest memory, start at its
 * entry point and run the recompiled code until it stops, reporting where and
 * why. Nothing is drawn yet; this proves the CPU side works end to end.
 */
#include "generated.h"
#include "runtime/disc.h"
#include "runtime/dol.h"
#include "runtime/hle_cpu.h"
#include "runtime/hle_exceptions.h"
#include "runtime/interrupts.h"
#include "runtime/pad.h"
#include "gx/fifo.h"
#include "gx/raster.h"
#include "gx/hires.h"
#include "platform/settings.h"
#include "audio/ax_hle.h"
#include "runtime/mmio.h"
#include "runtime/symbols.h"
#include "runtime/prof.h"
#include "runtime/interp.h"
#include "runtime/textscan.h"
#include "runtime/textpatch.h"
#include "runtime/exi.h"
#include "runtime/uismenu.h"
#include "runtime/uicolors.h"
#ifdef NFLS2_HAVE_WINDOW
#include "platform/window.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TRACE_LEN 24

static u32 g_trace[TRACE_LEN];
static u32 g_trace_pos;
static bool g_have_symbols;
static bool g_verbose_entries = true;
/* --debug: the diagnostics that cost time while playing (per-call counts,
   sound engine checks, file and stream logs, the end-of-run profile). */
static bool g_debug;

/* Bitmap of functions already announced, indexed by (start - 0x80000000) / 4. */
static u8* g_seen;

/* Profile of the last part of the run: dispatched calls counted per function,
   indexed by (start - 0x80000000) / 4. Shows what a stuck game is looping on. */
#define PROFILE_STEPS 20000000ull
static u32* g_profile;
static bool g_profiling;
static unsigned long long g_profiled;

/* Calls counted for the whole run, indexed like the profile by
   (address - 0x80000000) / 4; only function entry points are reported. */
static u32* g_calls;

/* Idle skipping (see the main loop): how often and how much time was skipped. */
static unsigned long long g_idle_skips, g_idle_ticks;

/* File-open calls (path in r3) logged with the game time. */
static u32 g_dvd_open, g_dvd_path, g_dvd_read, g_dvd_close;

#ifdef NFLS2_HAVE_WINDOW
/* Replacement movies: which part of MOVIEDAT.DAT the game is reading tells
   which movie it is playing (the file's own directory gives each movie's
   place); closing the file ends it. */
static u32 g_movie_fi;                 /* the game's DVDFileInfo for MOVIEDAT.DAT */
static int g_movie_count = -1;         /* -1: directory not read yet */
static u32 g_movie_off[16], g_movie_len[16];
static int g_movie_cur = -1;

static u32 rd_be32(const u8* p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }

static void movie_read_directory(void) {
    g_movie_count = 0;
    const char* path = disc_host_path("moviedat.dat");
    FILE* f = path ? fopen(path, "rb") : NULL;
    if (!f) return;
    u8 h[0xC0];
    if (fread(h, 1, sizeof(h), f) == sizeof(h) && memcmp(h, "TERF", 4) == 0 && memcmp(h + 0x40, "DIR1", 4) == 0) {
        u32 n = ((u32)h[0x0E] << 8) | h[0x0F];
        for (u32 i = 0; i < n && i < 16 && 0x48 + 8 * i + 8 <= 0x80; i++) {
            g_movie_off[i] = 0x80u + rd_be32(h + 0x48 + 8 * i);
            g_movie_len[i] = rd_be32(h + 0x4C + 8 * i);
            g_movie_count = (int)i + 1;
        }
    }
    fclose(f);
}
#endif
static u32 g_open_lines;
static bool g_skip_intros;             /* --skip-thx: fast-forward through the THX intro movie */
static u32 g_movie_opens;

/* Audio stream timeline: the engine's stream calls, logged with the game time. */
static u32 g_refills_this_second;
static u32 g_stream_lines;
static void print_streams_brief(CPUState* cpu, double now);

/* Sound-engine callback list at 0x803A6BCC (head word; nodes: +0 next,
   +8 function, +12 argument), run by 0x80269C70. A pass crashes the game
   when that list holds a node whose function pointer is garbage. Log the
   list whenever it changes, flag bad nodes, and log frees (0x8026B7D8) of
   memory holding a node that is still listed. */
#define CBL_MAX 32
static u32 g_cbl_nodes[CBL_MAX], g_cbl_funcs[CBL_MAX], g_cbl_n;
static u32 g_cbl_lines;

static void cbl_check(CPUState* ctx, const char* why) {
    u32 nodes[CBL_MAX], funcs[CBL_MAX], args[CBL_MAX], n = 0;
    u32 p = mem_read32(ctx, 0x803A6BCCu);
    while (p && n < CBL_MAX) {
        if (p < 0x80000000u || p >= 0x81800000u) { nodes[n] = p; funcs[n] = 0; args[n] = 0; n++; break; }
        nodes[n] = p;
        funcs[n] = mem_read32(ctx, p + 8);
        args[n] = mem_read32(ctx, p + 12);
        n++;
        p = mem_read32(ctx, p);
    }
    bool changed = n != g_cbl_n;
    for (u32 i = 0; i < n && !changed; i++)
        changed = nodes[i] != g_cbl_nodes[i] || funcs[i] != g_cbl_funcs[i];
    if (changed && g_cbl_lines < 300) {
        printf("  cblist: %7.2f s (%s) %u node%s:", interrupts_game_seconds(), why, n, n == 1 ? "" : "s");
        for (u32 i = 0; i < n; i++) {
            bool bad = funcs[i] < 0x80003100u || funcs[i] >= 0x80300000u;
            printf(" [%08X fn %08X arg %08X%s]", nodes[i], funcs[i], args[i], bad ? " BAD" : "");
        }
        printf("\n");
        g_cbl_lines++;
    }
    g_cbl_n = n;
    memcpy(g_cbl_nodes, nodes, n * 4);
    memcpy(g_cbl_funcs, funcs, n * 4);
}

static void cbl_free(CPUState* ctx) {
    u32 p = ctx->gpr[3];
    for (u32 i = 0; i < g_cbl_n; i++) {
        if (p + 0x40u >= g_cbl_nodes[i] && p <= g_cbl_nodes[i] + 0x10u && g_cbl_lines < 300) {
            printf("  cblist: %7.2f s  memory 0x%08X freed while node %08X (fn %08X) is still listed; called from "
                   "0x%08X\n", interrupts_game_seconds(), p, g_cbl_nodes[i], g_cbl_funcs[i], ctx->lr);
            g_cbl_lines++;
        }
    }
}

/* --trace-at: from this much game time, log calls that matter for frame
   pacing (video, OS timing and threads, GPU sync, the game's video module at
   0x801CE000-0x801CF000) across a few displayed frames. */
static double g_trace_at;
static bool g_tracing;
static u32 g_trace_lines, g_trace_frames_start;
#define TRACE_MAX_LINES 3000
#define TRACE_FRAMES 4

static bool guest_ptr_any(CPUState* cpu, u32 addr) {
    return addr >= GC_RAM_BASE && addr - GC_RAM_BASE < cpu->ram_size;
}

static bool trace_wanted(u32 address, const char* name) {
    if (address >= 0x801CE000u && address < 0x801CF000u)
        return true;
    static const char* const k_prefixes[] = {
        "VI", "__VI", "OSSleep", "OSWakeup", "OSYield", "OSGetTime", "__OSGetSystemTime", "OSWaitSemaphore",
        "OSSignalSemaphore", "OSWaitCond", "OSSignalCond", "OSReceiveMessage", "OSSendMessage",
        "OSSetAlarm", "OSCancelAlarm", "OSGetTick", "GXDrawDone", "GXSetDrawDone", "GXWaitDrawDone",
        "GXCopyDisp", "GXFlush", "GXSetDrawSync", "GXReadDrawSync", "GXSetDrawSyncCallback",
        "GXSetDrawDoneCallback", "GXSetBreakPtCallback", "GXEnableBreakPt", "GXDisableBreakPt",
        "GXSetCPUFifo", "GXSetGPFifo", "PADRead", "SelectThread", "OSResumeThread", "OSSuspendThread",
    };
    for (size_t i = 0; i < sizeof(k_prefixes) / sizeof(k_prefixes[0]); i++)
        if (strncmp(name, k_prefixes[i], strlen(k_prefixes[i])) == 0)
            return true;
    return false;
}

static double wall_seconds(void);
static double g_launch_wall;

/* Every dispatched call passes through here. Later this is where HLE
   replacements go; for now it only records where execution has been. */
/* After F8: for a few seconds, every function the game calls is checked for
   a string argument holding on-screen text, to find which functions build
   and draw the messages. */
static double g_text_watch_until;
static int g_text_watch_lines;
static u32 g_text_seen[512][2];
static int g_text_seen_n;

static void text_watch(CPUState* ctx, u32 address) {
    static const char* const k_needles[] = {"Device", "emory Card", "Slot A"};
    for (int r = 3; r <= 8; r++) {
        u32 p = ctx->gpr[r];
        if (p < GC_RAM_BASE || p - GC_RAM_BASE >= ctx->ram_size - 256) continue;
        const char* s = (const char*)ctx->ram + (p - GC_RAM_BASE);
        size_t n = 0;
        while (n < 240 && s[n] && ((u8)s[n] >= 0x20 || s[n] == '\n') && (u8)s[n] < 0x7F) n++;
        if (n < 4 || s[n]) continue;
        bool hit = false;
        for (size_t k = 0; k < sizeof k_needles / sizeof *k_needles && !hit; k++) {
            size_t ln = strlen(k_needles[k]);
            for (size_t i = 0; i + ln <= n && !hit; i++)
                if (!memcmp(s + i, k_needles[k], ln)) hit = true;
        }
        if (!hit) continue;
        u32 key = address ^ ((u32)r << 28), key2 = p;
        bool seen = false;
        for (int i = 0; i < g_text_seen_n && !seen; i++)
            if (g_text_seen[i][0] == key && g_text_seen[i][1] == key2) seen = true;
        if (seen) continue;
        if (g_text_seen_n < 512) { g_text_seen[g_text_seen_n][0] = key; g_text_seen[g_text_seen_n][1] = key2; g_text_seen_n++; }
        if (g_text_watch_lines++ >= 150) return;
        char text[256];
        size_t t;
        for (t = 0; t < n; t++) text[t] = s[t] == '\n' ? '|' : s[t];
        text[t] = 0;
        u32 off = 0, loff = 0;
        const char* fname = g_have_symbols ? symbols_lookup(address, &off) : "?";
        char fbuf[64];
        snprintf(fbuf, sizeof fbuf, "%s", fname);
        const char* lname = g_have_symbols ? symbols_lookup(ctx->lr, &loff) : "?";
        printf("textcall: %s (0x%08X) r%d=0x%08X from %s+0x%X (0x%08X): \"%s\"\n", fbuf, address, r, p, lname, loff,
               ctx->lr, text);
    }
}

/* --cardtrace: logs the memory card side of loading and saving (the CARD
   library calls, the save code's card-stream reads and writes with a
   checksum of the bytes moved, and the stream open/close), to find where a
   save is judged corrupted. */
static bool g_cardtrace;
static int g_ct_lines;
static u32 g_ct_buf, g_ct_len, g_ct_pos;
static bool g_ct_pending;
static const char* g_ct_kind;

static u32 ct_crc_msb(CPUState* ctx, u32 buf, u32 len) {
    u32 c = 0xFFFFFFFFu;
    for (u32 i = 0; i < len; i++) {
        c ^= (u32)mem_read8(ctx, buf + i) << 24;
        for (int k = 0; k < 8; k++) c = (c & 0x80000000u) ? (c << 1) ^ 0x04C11DB7u : c << 1;
    }
    return c;
}

static void ct_flush(CPUState* ctx) {
    if (!g_ct_pending) return;
    g_ct_pending = false;
    char hex[3 * 16 + 1] = "";
    for (u32 i = 0; i < g_ct_len && i < 16; i++) snprintf(hex + i * 3, 4, "%02X ", mem_read8(ctx, g_ct_buf + i));
    printf("cardtrace: %7.2f s  stream %s %u bytes at %u: %s crc %08X\n", interrupts_game_seconds(), g_ct_kind, g_ct_len,
           g_ct_pos, hex, ct_crc_msb(ctx, g_ct_buf, g_ct_len < 0x40000u ? g_ct_len : 0));
}

static void card_trace(CPUState* ctx, u32 address) {
    if (g_ct_lines >= 1500) return;
    if (address == 0x8002410Cu || address == 0x80024040u) {          /* card stream read / write */
        ct_flush(ctx);
        u32 posp = mem_read32(ctx, ctx->gpr[3] + 4);
        g_ct_buf = ctx->gpr[4];
        g_ct_len = ctx->gpr[5];
        g_ct_pos = posp ? mem_read32(ctx, posp) : 0;
        g_ct_kind = address == 0x8002410Cu ? "read" : "write";
        g_ct_pending = true;
        g_ct_lines++;
        if (address == 0x80024040u) ct_flush(ctx);                   /* a write's bytes are there already */
        return;
    }
    if (address == 0x80023E44u || address == 0x80023FD0u) {          /* stream open / close */
        ct_flush(ctx);
        u32 rec = ctx->gpr[address == 0x80023E44u ? 5 : 3];
        if (address == 0x80023FD0u) rec = mem_read32(ctx, ctx->gpr[3] + 8);
        printf("cardtrace: %7.2f s  stream %s (r3 %08X r4 %08X r5 %08X) record checksum %08X size %u\n",
               interrupts_game_seconds(), address == 0x80023E44u ? "open" : "close", ctx->gpr[3], ctx->gpr[4],
               ctx->gpr[5], rec ? mem_read32(ctx, rec) : 0, rec ? mem_read32(ctx, rec + 4) : 0);
        g_ct_lines++;
        return;
    }
    /* The CARD library's API-level files (not its low-level BIOS, block,
       directory and read/write internals), when entered through the
       dispatcher. CARDGetStatus only for file 0, with the status it filled
       in shown at the next call. */
    static u32 stat_buf;
    if (stat_buf && address >= 0x80254CA8u && address < 0x80259B08u && !(ctx->lr >= 0x80252000u && ctx->lr < 0x8025A000u)) {
        char hex[0x6C * 2 + 1];
        for (u32 i = 0; i < 0x6C; i++) snprintf(hex + i * 2, 3, "%02X", mem_read8(ctx, stat_buf + i));
        printf("cardtrace:          status of file 0: %s\n", hex);
        stat_buf = 0;
        g_ct_lines++;
    }
    if (address >= 0x80254CA8u && address < 0x80259B08u) {
        static const struct { u32 lo, hi; const char* name; } k_files[] = {
            {0x80254CA8u, 0x80255C38u, "CARDCheck"}, {0x80255C38u, 0x80256730u, "CARDMount"},
            {0x80256730u, 0x80256F14u, "CARDFormat"}, {0x80256F14u, 0x802574E8u, "CARDOpen"},
            {0x802574E8u, 0x80257880u, "CARDCreate"}, {0x80257880u, 0x80257CF4u, "CARDRead"},
            {0x80257CF4u, 0x80258070u, "CARDWrite"}, {0x80258070u, 0x802583E0u, "CARDDelete"},
            {0x802583E0u, 0x802588A8u, "CARDStat"}, {0x802588A8u, 0x80259B08u, "CARDUnlock"},
        };
        const char* file = NULL;
        for (size_t i = 0; i < sizeof(k_files) / sizeof(k_files[0]); i++)
            if (address >= k_files[i].lo && address < k_files[i].hi) { file = k_files[i].name; break; }
        if (!file || strcmp(file, "CARDUnlock") == 0) return;
        if (g_have_symbols && symbols_function_start(address) != address) return;
        if (ctx->lr >= 0x80252000u && ctx->lr < 0x8025A000u) return;   /* calls from the game only, not internal ones */
        if (address == 0x802585D8u) {                    /* CARDGetStatus */
            if (ctx->gpr[4] != 0) return;
            stat_buf = ctx->gpr[5];
        }
        ct_flush(ctx);
        printf("cardtrace: %7.2f s  %s %08X(%08X, %08X, %08X, %08X) from %08X\n", interrupts_game_seconds(), file,
               address, ctx->gpr[3], ctx->gpr[4], ctx->gpr[5], ctx->gpr[6], ctx->lr);
        g_ct_lines++;
        /* A file read (buffer, length, offset): its bytes are shown at the next event. */
        if (strcmp(file, "CARDRead") == 0 && ctx->gpr[4] >= 0x80000000u && ctx->gpr[5] >= 0x200u &&
            !(ctx->gpr[5] & 0x1FFu) && ctx->gpr[5] <= 0x100000u) {
            g_ct_buf = ctx->gpr[4];
            g_ct_len = ctx->gpr[5];
            g_ct_pos = ctx->gpr[6];
            g_ct_kind = "file read";
            g_ct_pending = true;
        }
    }
}

/* The game's menus are small scripts (EA's UIS). A script calls the game's
   C code through one dispatcher (0x80064690: r3 = function, r4 = menu group,
   r5 = screen, r7 = the arguments, r8 = where the result goes). Function 1 answers which console
   the game runs on (2 = GameCube). The Audio/Visual screen (group 5 OPTIONS,
   screen 5) asks it to choose its layout: on Xbox (1) the screen is "Audio
   Settings" without the Video (Normal / Letterbox / Widescreen) row, since
   the Xbox sets that in its own dashboard. With the screen shape fixed in
   the port's menu, that screen is told "Xbox", so the row is gone, like
   it is there; the port keeps the setting itself in step
   (sync_game_screen_option). Other screens and questions are untouched. */
#define UIS_NATIVE_DISPATCH 0x80064690u
#define UIS_FN_PLATFORM 1u
#define UIS_FN_SCREEN_OK 0x38u          /* (screen) -> whether that screen can be shown */
static int g_uis_ok = -1;              /* the code looks as expected (checked once) */
static bool g_uis_log;                /* --uis-log */

/* Edits to the main menu's scripts (menu group 0), each a comparison with
   the console value the menu keeps (Xbox 1, GameCube 2):
   - the Options list says "Audio Settings" (and its help line "Adjust audio
     settings.") where the value is 1, else "Audio/Visual": compared with 2
     instead while the screen shape is fixed, matching the Audio Settings
     screen (uis_native_hook);
   - the main menu leaves out Online, the Credits list leaves out Previews
     and Madden NFL 2005, and picking Credits goes straight to the credits,
     where the value is 2: compared with 0x7F (never) instead, so the menus
     have the entries of the other versions. What the GameCube game does when
     they are picked: Online and Madden NFL 2005 lead to features it doesn't
     have; Previews to the trailer screen.
   Each edit: the script bytes around the compared number (byte 11). */
typedef struct { const u8 sig[26]; u8 len; u8 off, on; bool screen_shape; const char* what; } UisEdit;
static const UisEdit k_uis_edits[] = {
    {{0x55,0,0,0,0xA0,0xFF,0xFD,0x10,0,0,0,0x01,0x36,0x10,0,0,0,0x1C,0x3F,0x41,0x10,0,0,0x01,0x65,0x6F}, 26, 1, 2, true,
     "Options list: Audio Settings"},
    {{0x55,0,0,0,0xA0,0xFF,0xFA,0x10,0,0,0,0x01,0x36,0x10,0,0,0,0x26,0x3F,0x41,0x10,0,0,0x01,0x81,0x6F}, 26, 1, 2, true,
     "Options help: Adjust audio settings."},
    {{0x55,0,0,0,0xA0,0xFF,0xFD,0x10,0,0,0,0x02,0x36,0x41,0x10,0,0,0x01,0x36,0x10,0,0,0,0,0x0B}, 25, 2, 0x7F, false,
     "main menu: Online"},
    {{0x55,0,0,0,0xA0,0xFF,0xFD,0x10,0,0,0,0x02,0x36,0x10,0,0,0,0x1C,0x3F,0x41,0x10,0,0,0x01,0x67,0x6F}, 26, 2, 0x7F, false,
     "Credits list: Previews, Madden NFL 2005"},
    {{0x55,0,0,0,0xA0,0xFF,0xF7,0x10,0,0,0,0x02,0x36,0x10,0,0,0,0x68,0x3F}, 19, 2, 0x7F, false,
     "Credits: choice of the three"},
};
#define UIS_EDITS (int)(sizeof(k_uis_edits) / sizeof(k_uis_edits[0]))

static bool uis_edit_at(const u8* ram, u32 o, const UisEdit* e) {
    return memcmp(ram + o, e->sig, 11) == 0 && (ram[o + 11] == e->off || ram[o + 11] == e->on) &&
           memcmp(ram + o + 12, e->sig + 12, e->len - 12u) == 0;
}

static void uis_main_menu_edits(CPUState* ctx) {
    static u32 at[UIS_EDITS];          /* where each was found last */
    if (!ctx->ram || ctx->ram_size < 0x01000000u) return;
    u8* ram = ctx->ram;
    u32 n = ctx->ram_size - 32u;
    bool search = false;
    for (int i = 0; i < UIS_EDITS; i++)
        if (!(at[i] && at[i] <= n && uis_edit_at(ram, at[i], &k_uis_edits[i]))) { at[i] = 0; search = true; }
    if (search)                        /* menu scripts live in the upper heap */
        for (u32 o = 0x00800000u; o < n; o++)
            if (ram[o] == 0x55 && ram[o + 4] == 0xA0)
                for (int i = 0; i < UIS_EDITS; i++)
                    if (!at[i] && uis_edit_at(ram, o, &k_uis_edits[i])) at[i] = o;
    for (int i = 0; i < UIS_EDITS; i++) {
        const UisEdit* e = &k_uis_edits[i];
        if (!at[i]) continue;
        u8 want = (!e->screen_shape || g_settings.screen_shape != 0) ? e->on : e->off;
        if (ram[at[i] + 11] != want) {
            ram[at[i] + 11] = want;
            printf("uis: %s %s\n", e->what, want == e->on ? "on" : "off");
        }
    }
}

static bool g_video_screen;            /* the Options list asked for Video Settings */
static bool g_video_back;              /* ... and the cursor goes back to it */

static int uis_native_hook(CPUState* ctx) {
    if (g_uis_ok < 0)
        g_uis_ok = mem_read32(ctx, UIS_NATIVE_DISPATCH) == 0x9421FFE8u &&    /* stwu r1, -0x18(r1) */
                   mem_read32(ctx, 0x8017FDF0u) == 0x38600002u;               /* platform: li r3, 2 */
    if (!g_uis_ok)
        return 0;
    u32 group = ctx->gpr[4], screen = ctx->gpr[5];
#ifdef NFLS2_HAVE_WINDOW
    if (group == 0 || group == 5) window_note_front_end();   /* a menu is up (main menu, options screens) */
#endif
    if (g_uis_log) {                   /* --uis-log: each distinct call once */
        static u32 seen_calls[1024];
        static int n_seen;
        u32 key = (group & 0xFFu) << 24 | (screen & 0xFFu) << 16 | (ctx->gpr[3] & 0xFFFFu) |
                  (ctx->gpr[3] & 0x80000000u ? 0x8000u : 0);
        int i = 0;
        while (i < n_seen && seen_calls[i] != key) i++;
        if (i == n_seen && n_seen < 1024) {
            seen_calls[n_seen++] = key;
            u32 argc = ctx->gpr[6];
            printf("uis: call %s%X  group %u screen %u  args %u:", ctx->gpr[3] & 0x80000000u ? "K" : "",
                   ctx->gpr[3] & 0x7FFFFFFFu, group, screen, argc);
            for (u32 k = 0; k < argc && k < 4; k++) printf(" %d", (int)mem_read32(ctx, ctx->gpr[7] + 4 * k));
            printf("  (%.1f s)\n", interrupts_game_seconds());
        }
    }
    if (ctx->gpr[3] == UIS_FN_QUIT && group == 0) {
        /* Yes in the game's own "quit?" dialog (uismenu.c). */
        printf("uis: Quit Game confirmed\n");
#ifdef NFLS2_HAVE_WINDOW
        window_quit_now();
#endif
        mem_write32(ctx, ctx->gpr[8], 0);
        ctx->gpr[3] = 1;
        ctx->pc = ctx->lr;
        return 1;
    }
    if ((ctx->gpr[3] == UIS_FN_VIDEO || ctx->gpr[3] == UIS_FN_AUDIO) && group == 0) {
        /* The Options list opens Audio Settings for both entries; this says
           which (Video Settings: the same screen in video mode). */
        g_video_screen = g_video_back = ctx->gpr[3] == UIS_FN_VIDEO;
        mem_write32(ctx, ctx->gpr[8], 0);
        ctx->gpr[3] = 1;
        ctx->pc = ctx->lr;
        return 1;
    }
    if (group == 5 && screen == 5 && ctx->gpr[3] == 0x80000002u) {
        /* The screen's leaving (its function 2, run as it closes): from here
           its memory is on its way out, so the port stops touching it. */
        uismenu_video_closed();
        return 0;
    }
    if (group == 5 && screen == 5 && ctx->gpr[3] == 0x80000003u && ctx->gpr[6] == 1 && uismenu_video_active(ctx)) {
        /* Video Settings: the screen's function 3 is the rows' sound
           preview (8-10: speech, effects, music; anything else pauses the
           music, as the screen's start-up did). Each is made the music row's,
           which keeps the music playing. */
        mem_write32(ctx, ctx->gpr[7], 10);
        return 0;
    }
    if (group == 5 && screen == 5 && ctx->gpr[3] == 0x80000005u && ctx->gpr[6] >= 1 && uismenu_video_active(ctx)) {
        /* ... and nothing written to the game's audio / screen settings (its
           rows set the port's): the setting number is made one the game
           doesn't have, so the call goes through and does nothing. */
        mem_write32(ctx, ctx->gpr[7], 0x7Fu);
        uismenu_video_closed();        /* sent as the screen is left (Back): it's closing */
        return 0;
    }
    if (ctx->gpr[3] == UIS_FN_VIDEO_BACK && group == 0) {
        /* Back on Options from Audio Settings' screen: was it Video Settings? */
        mem_write32(ctx, ctx->gpr[8], g_video_back ? 1u : 0u);
        g_video_back = false;
        ctx->gpr[3] = 1;
        ctx->pc = ctx->lr;
        return 1;
    }
    if (ctx->gpr[3] == UIS_FN_SCREEN_OK && group == 0) {
        /* The restored main menu entries whose screens aren't in the GameCube
           game (uis_main_menu_edits): Online (0x12C; going on would also award
           the "online match" feat, then hang) and Madden NFL 2005 (0x9C, the
           PS2 demo). "Not available", so picking them does nothing. */
        u32 target = mem_read32(ctx, ctx->gpr[7]);
        if (target == 0x12Cu || target == 0x9Cu) {
            mem_write32(ctx, ctx->gpr[8], 0);
            ctx->gpr[3] = 1;
            ctx->pc = ctx->lr;
            return 1;
        }
        return 0;
    }
    if (group == 5 && screen == 3 && ctx->gpr[3] == 0x80000003u && ctx->gpr[6] == 1) {
        /* The Previews menu (OPTIONS screen 3, restored above): each entry
           calls the screen's function 3 with n (1-11), which hands n - 1 to the
           trailer player the GameCube version left empty (0x8001F500). The
           window plays the trailer instead, from its place in the Xbox / PS2
           MOVIEDAT.DAT. */
        /* n numbers the games about alphabetically (FIFA Soccer, FIFA Street,
           Madden, March Madness, NASCAR, NBA Street, NBA Live, NCAA Football,
           Need for Speed, NHL, Tiger Woods); the movie file has its own order. */
        static const int k_trailer[12] = {-1, 4, 3, 0, 6, 7, 2, 1, 5, 10, 8, 9};
        u32 n = mem_read32(ctx, ctx->gpr[7]);
        int idx = n >= 1 && n <= 11 ? k_trailer[n] : -1;
        printf("uis: Previews entry %u -> trailer %d\n", n, idx);
#ifdef NFLS2_HAVE_WINDOW
        if (idx >= 0) window_preview_request(idx);
#endif
        mem_write32(ctx, ctx->gpr[8], 1);
        ctx->gpr[3] = 1;
        ctx->pc = ctx->lr;
        return 1;
    }
    if (ctx->gpr[3] != UIS_FN_PLATFORM)
        return 0;
    if (!(group == 5 && screen == 5)) uismenu_video_closed();   /* another screen loading */
    if (group == 0) { uis_main_menu_edits(ctx); uismenu_add_quit(ctx); }
    if (group == 0 && (screen == 2 || screen == 3)) uismenu_hide_licence(ctx);   /* the title screen */
    static u32 seen[16];
    if (group < 16 && screen < 32 && !(seen[group] & (1u << screen))) {
        seen[group] |= 1u << screen;
        printf("uis: menu group %u screen %u checks the console\n", group, screen);
    }
    if (group == 5 && screen == 5 && g_video_screen) {
        /* Video Settings: the screen in video mode, with its GameCube
           layout (the Video row) kept. */
        g_video_screen = false;
        uismenu_video_screen(ctx);
        return 0;
    }
    if (!(group == 5 && screen == 5 && g_settings.screen_shape != 0))
        return 0;
    mem_write32(ctx, ctx->gpr[8], 1);  /* "Xbox" */
    ctx->gpr[3] = 1;                   /* handled */
    ctx->pc = ctx->lr;
    return 1;
}

int dolrecomp_dispatch_replacement(CPUState* ctx, u32 address) {
    if (address == UIS_NATIVE_DISPATCH && uis_native_hook(ctx)) return 1;
    g_trace[g_trace_pos++ % TRACE_LEN] = address;
    if (g_cardtrace) card_trace(ctx, address);
    if (g_text_watch_until > 0) {
        if (interrupts_game_seconds() < g_text_watch_until) text_watch(ctx, address);
        else { g_text_watch_until = 0; printf("textcall: watch ended\n"); }
    }
    if (g_debug) {
        u32 slot = (address - GC_RAM_BASE) >> 2;
        if (g_calls && slot < (GC_MAIN_RAM_SIZE >> 2) && g_calls[slot] != 0xFFFFFFFFu)
            g_calls[slot]++;
    }
    if (g_debug && address >= 0x80273040u && address <= 0x80274F5Cu && g_stream_lines < 400) {
        const char* what = NULL;
        switch (address) {
        case 0x802743A0u: what = "stream create   (stream r3)"; break;
        case 0x80274464u: what = "stream start    (stream r3, channel r4)"; break;
        case 0x80274F5Cu: what = "channel stop    (channel r3)"; break;
        case 0x8027474Cu: what = "stream free     (stream r3)"; break;
        case 0x80273040u: g_refills_this_second++; break;
        }
        if (what) {
            printf("  stream: %7.2f s  %s  r3 %d r4 %d\n", interrupts_game_seconds(), what, (int)ctx->gpr[3],
                   (int)ctx->gpr[4]);
            g_stream_lines++;
        }
    }
    if (g_debug) {
        if (address == 0x80269C70u)
            cbl_check(ctx, "run");
        else if (address == 0x8026B7D8u && g_cbl_n)
            cbl_free(ctx);
    }
    if ((address == g_dvd_open || address == g_dvd_path) && address && g_open_lines < 300) {
        char path[96];
        u32 p = ctx->gpr[3];
        u32 n = 0;
        while (n + 1 < sizeof(path) && guest_ptr_any(ctx, p + n)) {
            u8 c = mem_read8(ctx, p + n);
            if (!c) break;
            path[n++] = (char)c;
        }
        path[n] = 0;
        if (g_debug)
            printf("  file open at %7.2f s: %s\n", interrupts_game_seconds(), path);
        g_open_lines++;
        {
            /* Loading-time markers: the NFL Street 2 intro starting (3rd
               movie open) and the title screen's data (Streamed.db). */
            static int movie_opens, title_seen;
            bool movie = strstr(path, "MOVIEDAT") || strstr(path, "moviedat");
            if (movie && address == g_dvd_open) movie_opens++;
            if (address == g_dvd_open &&
                ((movie && movie_opens == 3) || (!title_seen && (strstr(path, "Streamed") || strstr(path, "streamed"))))) {
                if (!movie) title_seen = 1;
                if (g_debug) printf("startup: %s at %.1f s after launch (game time %.1f s)\n",
                       movie ? "NFL Street 2 intro starts" : "title screen data loaded", wall_seconds() - g_launch_wall,
                       interrupts_game_seconds());
            }
        }
#ifdef NFLS2_HAVE_WINDOW
        /* Skipping the THX intro: the intro movies are opens of MOVIEDAT.DAT
           in turn (EA SPORTS BIG, THX, NFL Street 2). Fast-forward, unseen
           and unheard, from the second open to the third. */
        if (address == g_dvd_open && (strstr(path, "MOVIEDAT") || strstr(path, "moviedat"))) {
            g_movie_fi = ctx->gpr[4];
            if (g_movie_count < 0) movie_read_directory();
            g_movie_opens++;
            if (g_skip_intros && g_movie_opens == 2) window_set_turbo(true);
            if (g_movie_opens == 3) {
                if (g_skip_intros) window_set_turbo(false);
                g_skip_intros = false;
                window_trust_screen_shape();   /* past the boot screens: the game's own setting applies now */
            }
        }
#endif
    }

#ifdef NFLS2_HAVE_WINDOW
    if (g_movie_fi && address && address == g_dvd_read && ctx->gpr[3] == g_movie_fi) {
        u32 off = ctx->gpr[6];
        for (int i = 0; i < g_movie_count; i++)
            if (off >= g_movie_off[i] && off < g_movie_off[i] + g_movie_len[i]) {
                if (i != g_movie_cur) {
                    g_movie_cur = i;
                    window_movie_begin(i);
                }
                break;
            }
    }
    if (g_movie_fi && address && address == g_dvd_close && ctx->gpr[3] == g_movie_fi) {
        g_movie_fi = 0;
        g_movie_cur = -1;
        window_movie_end();
    }
#endif

    if (g_tracing && g_have_symbols && symbols_function_start(address) == address) {
        const char* name = symbols_lookup(address, NULL);
        if (trace_wanted(address, name)) {
            printf("  trace: %-28s 0x%08X  lr 0x%08X  field %.0f  frame %u\n", name, address, ctx->lr,
                   interrupts_game_seconds() * 60000.0 / 1001.0, gx_raster_frames());
            if (++g_trace_lines >= TRACE_MAX_LINES)
                g_tracing = false;
        }
    }

    if (g_profiling) {
        u32 start = g_have_symbols ? symbols_function_start(address) : 0;
        u32 slot = ((start ? start : address) - GC_RAM_BASE) >> 2;
        if (slot < (GC_MAIN_RAM_SIZE >> 2))
            g_profile[slot]++;
        g_profiled++;
    }

    if (g_have_symbols && g_verbose_entries && g_seen) {
        u32 start = symbols_function_start(address);
        if (start == address) {
            u32 slot = (address - GC_RAM_BASE) >> 2;
            if (slot < (GC_MAIN_RAM_SIZE >> 2) && !(g_seen[slot >> 3] & (1u << (slot & 7)))) {
                g_seen[slot >> 3] |= (u8)(1u << (slot & 7));
                const char* name = symbols_lookup(address, NULL);
                if (strncmp(name, "fn_", 3) != 0)
                    printf("  -> %s (0x%08X)\n", name, address);
            }
        }
    }
    return 0;
}

/* Wall-clock seconds (clock() counts CPU time of all threads on some systems). */
static double wall_seconds(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void print_location(const char* label, u32 addr) {
    if (g_have_symbols) {
        u32 off = 0;
        const char* name = symbols_lookup(addr, &off);
        printf("%s 0x%08X  %s+0x%X\n", label, addr, name, off);
    } else {
        printf("%s 0x%08X\n", label, addr);
    }
}

static bool guest_ptr(CPUState* cpu, u32 addr) {
    return addr >= GC_RAM_BASE && addr - GC_RAM_BASE < cpu->ram_size - 8 && !(addr & 3);
}

/* Walks the PowerPC stack chain: each frame starts with a pointer to the
   caller's frame, and the caller's frame holds the saved return address at +4. */
static void print_backtrace(CPUState* cpu, u32 sp, u32 lr, const char* indent) {
    print_location(indent, lr);
    for (int depth = 0; depth < 20 && guest_ptr(cpu, sp); depth++) {
        u32 caller = mem_read32(cpu, sp);
        if (!guest_ptr(cpu, caller) || caller <= sp)
            break;
        u32 ret = mem_read32(cpu, caller + 4);
        if (ret)
            print_location(indent, ret);
        sp = caller;
    }
}

/* True if some game thread is ready to run (an interrupt just woke it and
   the OS is about to leave its idle loop and switch to it). */
static bool any_thread_ready(CPUState* cpu) {
    u32 t = mem_read32(cpu, 0x800000DC);
    for (int n = 0; n < 64 && guest_ptr(cpu, t); n++) {
        if ((mem_read32(cpu, t + 0x2C8) >> 16) == 1u)
            return true;
        t = mem_read32(cpu, t + 0x2FC);
    }
    return false;
}

/* Lists the game's threads from the OS's active-thread list (0x800000DC),
   with where each one is and what its call stack looks like. */
static void print_threads(CPUState* cpu) {
    static const char* const k_state[] = {"?", "ready", "running", "?", "waiting", "?", "?", "?", "dead"};
    u32 current = mem_read32(cpu, 0x800000E4);
    u32 t = mem_read32(cpu, 0x800000DC);
    printf("\ngame threads (current 0x%08X):\n", current);
    for (int n = 0; n < 32 && guest_ptr(cpu, t); n++) {
        u32 state = mem_read32(cpu, t + 0x2C8) >> 16;
        u32 suspend = mem_read32(cpu, t + 0x2CC);
        u32 priority = mem_read32(cpu, t + 0x2D0);
        u32 queue = mem_read32(cpu, t + 0x2DC);
        printf(" thread 0x%08X  %s  priority %u  suspend %d  waiting-queue 0x%08X\n", t,
               state < 9 ? k_state[state] : "?", priority, (int)suspend, queue);
        if (t == current) {
            printf("   (running now; see pc and backtrace above)\n");
        } else {
            u32 sp = mem_read32(cpu, t + 0x04);
            u32 srr0 = mem_read32(cpu, t + 0x198);
            u32 lr = mem_read32(cpu, t + 0x84);
            print_location("   resumes at", srr0);
            print_backtrace(cpu, sp, lr, "     ");
        }
        t = mem_read32(cpu, t + 0x2FC);
    }
}

static void print_profile(void) {
    if (!g_profiled)
        return;
    printf("\nwhere the last %llu calls went (top 25 functions):\n", g_profiled);
    for (int k = 0; k < 25; k++) {
        u32 best = 0, best_slot = 0;
        for (u32 i = 0; i < (GC_MAIN_RAM_SIZE >> 2); i++)
            if (g_profile[i] > best) { best = g_profile[i]; best_slot = i; }
        if (!best)
            break;
        u32 addr = GC_RAM_BASE + best_slot * 4;
        const char* name = g_have_symbols ? symbols_lookup(addr, NULL) : "?";
        printf("  %5.1f%%  0x%08X  %s\n", 100.0 * best / (double)g_profiled, addr, name);
        g_profile[best_slot] = 0;
    }
}

/* Whole-run call counts for the game's audio engine (0x8026A000-0x8027C000)
   and the SDK's disc, ARAM and audio libraries. */
typedef struct { int printed; } CountCtx;

static void count_one(uint32_t start, const char* name, void* user) {
    CountCtx* c = (CountCtx*)user;
    u32 slot = (start - GC_RAM_BASE) >> 2;
    if (slot >= (GC_MAIN_RAM_SIZE >> 2) || !g_calls[slot])
        return;
    bool engine = start >= 0x8026A000u && start < 0x8027C000u;
    static const char* const k_prefixes[] = {"DVD", "__DVD", "AR", "__AR", "AX", "__AX", "AI", "__AI", "DSP",
                                             "__DSP", "OSCreateThread", "OSResumeThread", "OSSetThreadPriority"};
    bool sdk = false;
    for (size_t i = 0; i < sizeof(k_prefixes) / sizeof(k_prefixes[0]) && !sdk; i++)
        sdk = strncmp(name, k_prefixes[i], strlen(k_prefixes[i])) == 0;
    if (!engine && !sdk)
        return;
    printf("  0x%08X %-26.26s %9u%s", start, name, g_calls[slot], (++c->printed % 3) ? "  |" : "\n");
}

static void print_call_counts(void) {
    if (!g_calls || !g_have_symbols)
        return;
    printf("\naudio and disc calls (whole run): address, name, calls\n");
    CountCtx c = {0};
    symbols_each(count_one, &c);
    if (c.printed % 3)
        printf("\n");
}

/* The audio engine's stream blocks (table of 32 pointers at 0x803AA170). */
static void print_streams(CPUState* cpu) {
    printf("\naudio engine streams (table 0x803AA170):\n");
    int shown = 0;
    for (int i = 0; i < 32; i++) {
        u32 b = mem_read32(cpu, 0x803AA170u + i * 4);
        if (!guest_ptr(cpu, b))
            continue;
        s8 channel = (s8)mem_read8(cpu, b + 0x2598);
        printf("  stream %2d at 0x%08X: channel %d half %u countdown 0x%02X voices %u aram 0x%08X "
               "data 0x%08X size %d pos %d\n",
               i, b, channel, mem_read8(cpu, b + 0x2599), mem_read8(cpu, b + 0x259A), mem_read8(cpu, b + 0x259B),
               mem_read32(cpu, b + 0x259C), mem_read32(cpu, b + 0x25C4), (int)mem_read32(cpu, b + 0x25DC),
               (int)mem_read32(cpu, b + 0x25E0));
        if (channel >= 0 && channel < 64) {
            u32 vpb = mem_read32(cpu, 0x803A8D60u + channel * 64 + 0x3C);
            if (guest_ptr(cpu, vpb))
                printf("    voice 0x%08X: state %u  format 0x%X  loop %u  cur 0x%08X  loop 0x%08X  end 0x%08X  "
                       "src %u ratio 0x%08X  vol 0x%04X  mixL 0x%04X mixR 0x%04X\n",
                       vpb, mem_read16(cpu, vpb + 0x138 + 0x0E), mem_read16(cpu, vpb + 0x138 + 0x70),
                       mem_read16(cpu, vpb + 0x138 + 0x6E),
                       (mem_read16(cpu, vpb + 0x138 + 0x7A) << 16) | mem_read16(cpu, vpb + 0x138 + 0x7C),
                       (mem_read16(cpu, vpb + 0x138 + 0x72) << 16) | mem_read16(cpu, vpb + 0x138 + 0x74),
                       (mem_read16(cpu, vpb + 0x138 + 0x76) << 16) | mem_read16(cpu, vpb + 0x138 + 0x78),
                       mem_read16(cpu, vpb + 0x138 + 0x08),
                       (mem_read16(cpu, vpb + 0x138 + 0xA6) << 16) | mem_read16(cpu, vpb + 0x138 + 0xA8),
                       mem_read16(cpu, vpb + 0x138 + 0x64), mem_read16(cpu, vpb + 0x138 + 0x12),
                       mem_read16(cpu, vpb + 0x138 + 0x16));
        }
        shown++;
    }
    if (!shown)
        printf("  (none)\n");
}

/* Once a second: refills done and each playing stream's voice. */
static void print_streams_brief(CPUState* cpu, double now) {
    if (g_stream_lines >= 400)
        return;
    bool any = false;
    for (int i = 0; i < 32; i++) {
        u32 b = mem_read32(cpu, 0x803AA170u + i * 4);
        if (!guest_ptr(cpu, b))
            continue;
        s8 channel = (s8)mem_read8(cpu, b + 0x2598);
        if (channel < 0 || channel >= 64)
            continue;
        u32 vpb = mem_read32(cpu, 0x803A8D60u + channel * 64 + 0x3C);
        if (!guest_ptr(cpu, vpb))
            continue;
        u32 pb = vpb + 0x138;
        printf("  stream: %7.2f s  stream %d channel %d: %u refills this second, data 0x%08X size %d pos %d | voice "
               "state %u fmt 0x%X loop %u cur 0x%08X loop 0x%08X end 0x%08X ratio 0x%08X vol 0x%04X mix %04X/%04X\n",
               now, i, channel, g_refills_this_second, mem_read32(cpu, b + 0x25C4), (int)mem_read32(cpu, b + 0x25DC),
               (int)mem_read32(cpu, b + 0x25E0), mem_read16(cpu, pb + 0x0E), mem_read16(cpu, pb + 0x70),
               mem_read16(cpu, pb + 0x6E), (mem_read16(cpu, pb + 0x7A) << 16) | mem_read16(cpu, pb + 0x7C),
               (mem_read16(cpu, pb + 0x72) << 16) | mem_read16(cpu, pb + 0x74),
               (mem_read16(cpu, pb + 0x76) << 16) | mem_read16(cpu, pb + 0x78),
               (mem_read16(cpu, pb + 0xA6) << 16) | mem_read16(cpu, pb + 0xA8), mem_read16(cpu, pb + 0x64),
               mem_read16(cpu, pb + 0x12), mem_read16(cpu, pb + 0x16));
        g_stream_lines++;
        any = true;
    }
    if (!any && g_refills_this_second) {
        printf("  stream: %7.2f s  %u refills this second, no stream playing\n", now, g_refills_this_second);
        g_stream_lines++;
    }
    g_refills_this_second = 0;
}

static void usage(const char* argv0) {
    fprintf(stderr,
            "usage: %s <disc folder> [--map main.map] [--max-steps N] [--quiet] [--debug] [--skip-hash]\n"
            "         [--dump-every N (default 60, 0 = off)] [--dump-dir DIR (default frames)]\n"
            "         [--press SECONDS:BUTTON[+BUTTON][@HOLD]]...  e.g. --press 5.5:A --press 7:DOWN@0.2\n"
            "         [--stop-at SECONDS (stop at this much game time)]\n"
            "         [--xfb-every FIELDS (also save the TV frame buffer every N video fields)]\n"
            "         [--trace-at SECONDS (log frame-pacing calls for a few frames from then)]\n"
            "         [--unpaced (let the game draw as many frames per video field as it likes)]\n"
            "         [--render-threads N (threads that draw frames; 0 = draw on the game thread)]\n"
            "         [--draw-log-at SECONDS (write two frames of draw calls and GPU state to drawlog.txt)]\n"
            "         [--window (play in a window at real-time speed)] [--scale N (window size, default 2)]\n"
            "         [--skip-thx (window: skip the THX intro movie)]\n"
            "         [--replace DIR (files there are read instead of the disc's, e.g. DIR/moviedat.dat)]\n"
            "         [--memcard FILE (memory card image in slot A; created if missing)]\n"
            "         [--text FILE (game text replacements, default text-replacements.txt) | --no-text]\n"
            "         [--ui-colors FILE (menu shape colours, default ui-colors.txt) | --no-ui-colors]\n"
            "         [--aspect auto|4:3|16:9 (screen shape; auto follows the game's widescreen option)]\n"
            "         [--gpu (draw with the graphics card) [--res N (its internal resolution, N x 640x528)]]\n"
            "         [--settings FILE (saved video settings; switches override them)] [--software]\n"
            "         [--textures DIR (custom textures, Dolphin format)] [--dump-textures DIR]\n"
            "  <disc folder>  the folder containing sys\\main.dol and files\\ (e.g. C:\\nfls2\\disc)\n",
            argv0);
}

int main(int argc, char** argv) {
    const char* disc_dir = NULL;
    const char* map_path = NULL;
    unsigned long long max_steps = 50000000ull;
    bool max_steps_given = false;
    bool want_window = false;
    int window_height = 960;
    int gpu_scale = 0;                 /* 0 = software renderer */
    const char* dump_audio = NULL;
    const char* textures_dir = NULL;
    const char* dump_textures_dir = NULL;
    int aspect_mode = 0;
    bool have_settings = false;
    bool idle_skip = true;
    const char* memcard_path = NULL;
    const char* text_path = "text-replacements.txt";
    const char* colors_path = "ui-colors.txt";
    bool skip_hash = false;
    const char* dump_dir = "frames";
    u32 dump_every = 60;
    double stop_at = 0;
    u32 xfb_every = 0;
    bool draw_log = false;

    /* Saved settings first (the video menu's); switches below override them. */
    for (int i = 1; i + 1 < argc; i++)
        if (strcmp(argv[i], "--settings") == 0) {
            settings_load(argv[i + 1]);
            window_height = settings_window_height(g_settings.window_size);
            gpu_scale = g_settings.renderer == 0 ? g_settings.resolution : 0;
            aspect_mode = g_settings.screen_shape;
            have_settings = true;
        }
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--settings") == 0 && i + 1 < argc) {
            i++;
        } else if (strcmp(argv[i], "--cardtrace") == 0) {
            g_cardtrace = true;
        } else if (strcmp(argv[i], "--software") == 0) {
            gpu_scale = 0;
        } else if (strcmp(argv[i], "--map") == 0 && i + 1 < argc) {
            map_path = argv[++i];
        } else if (strcmp(argv[i], "--max-steps") == 0 && i + 1 < argc) {
            max_steps = strtoull(argv[++i], NULL, 10);
            max_steps_given = true;
        } else if (strcmp(argv[i], "--window") == 0) {
            want_window = true;
        } else if (strcmp(argv[i], "--text") == 0 && i + 1 < argc) {
            text_path = argv[++i];
        } else if (strcmp(argv[i], "--no-text") == 0) {
            text_path = NULL;
        } else if (strcmp(argv[i], "--ui-colors") == 0 && i + 1 < argc) {
            colors_path = argv[++i];
        } else if (strcmp(argv[i], "--no-ui-colors") == 0) {
            colors_path = NULL;
        } else if (strcmp(argv[i], "--memcard") == 0 && i + 1 < argc) {
            memcard_path = argv[++i];
        } else if (strcmp(argv[i], "--skip-thx") == 0) {
            g_skip_intros = true;
        } else if (strcmp(argv[i], "--no-idle-skip") == 0) {
            idle_skip = false;
        } else if (strcmp(argv[i], "--aspect") == 0 && i + 1 < argc) {
            const char* a = argv[++i];
            aspect_mode = strcmp(a, "4:3") == 0 ? 1 : strcmp(a, "16:9") == 0 ? 2 : 0;
        } else if (strcmp(argv[i], "--gpu") == 0) {
            if (!gpu_scale) gpu_scale = have_settings ? g_settings.resolution : 1;
        } else if (strcmp(argv[i], "--res") == 0 && i + 1 < argc) {
            gpu_scale = atoi(argv[++i]);
            if (gpu_scale < 1) gpu_scale = 1;
        } else if (strcmp(argv[i], "--scale") == 0 && i + 1 < argc) {
            window_height = 480 * atoi(argv[++i]);
        } else if (strcmp(argv[i], "--debug") == 0) {
            g_debug = true;
        } else if (strcmp(argv[i], "--quiet") == 0) {
            g_verbose_entries = false;
        } else if (strcmp(argv[i], "--dump-every") == 0 && i + 1 < argc) {
            dump_every = (u32)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--dump-dir") == 0 && i + 1 < argc) {
            dump_dir = argv[++i];
        } else if (strcmp(argv[i], "--press") == 0 && i + 1 < argc) {
            if (!pad_add_press(argv[++i])) {
                fprintf(stderr, "error: --press %s: expected SECONDS:BUTTON[+BUTTON][@HOLD], buttons: %s\n",
                        argv[i], pad_button_names());
                return 2;
            }
        } else if (strcmp(argv[i], "--draw-log-at") == 0 && i + 1 < argc) {
            gx_raster_draw_log(strtod(argv[++i], NULL), 2);
            draw_log = true;
        } else if (strcmp(argv[i], "--render-threads") == 0 && i + 1 < argc) {
            gx_raster_set_threads(atoi(argv[++i]));
        } else if (strcmp(argv[i], "--unpaced") == 0) {
            mmio_set_frame_pacing(false);
        } else if (strcmp(argv[i], "--trace-at") == 0 && i + 1 < argc) {
            g_trace_at = strtod(argv[++i], NULL);
        } else if (strcmp(argv[i], "--xfb-every") == 0 && i + 1 < argc) {
            xfb_every = (u32)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--stop-at") == 0 && i + 1 < argc) {
            stop_at = strtod(argv[++i], NULL);
        } else if (strcmp(argv[i], "--dump-audio") == 0 && i + 1 < argc) {
            dump_audio = argv[++i];
        } else if (strcmp(argv[i], "--uis-log") == 0) {
            g_uis_log = true;
        } else if (strcmp(argv[i], "--replace") == 0 && i + 1 < argc) {
            disc_set_replace_dir(argv[++i]);
        } else if (strcmp(argv[i], "--textures") == 0 && i + 1 < argc) {
            textures_dir = argv[++i];
        } else if (strcmp(argv[i], "--dump-textures") == 0 && i + 1 < argc) {
            dump_textures_dir = argv[++i];
        } else if (strcmp(argv[i], "--skip-hash") == 0) {
            skip_hash = true;
        } else if (argv[i][0] != '-' && !disc_dir) {
            disc_dir = argv[i];
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    if (!disc_dir) {
        usage(argv[0]);
        return 2;
    }

    if (map_path) {
        g_have_symbols = symbols_load(map_path);
        if (!g_have_symbols)
            fprintf(stderr, "warning: couldn't load symbols from %s\n", map_path);
    }
    g_seen = (u8*)calloc(GC_MAIN_RAM_SIZE >> 5, 1);
    if (g_debug) {                     /* 2 x 24 MB of counters: only when asked for */
        g_profile = (u32*)calloc(GC_MAIN_RAM_SIZE >> 2, sizeof(u32));
        g_calls = (u32*)calloc(GC_MAIN_RAM_SIZE >> 2, sizeof(u32));
    }
    disc_set_clock(interrupts_game_seconds);
    if (g_have_symbols) {
        g_dvd_open = symbols_address("DVDOpen");
        g_dvd_path = symbols_address("DVDConvertPathToEntrynum");
        g_dvd_read = symbols_address("DVDReadAsyncPrio");
        g_dvd_close = symbols_address("DVDClose");
    }
    if (textures_dir) {
        if (gpu_scale) {
            hires_init(textures_dir);
            hires_set_enabled(g_settings.custom_textures != 0);
        }
        else
            printf("custom textures: only used with the graphics card renderer (not with --software)\n");
    }
    hires_set_dump_dir(dump_textures_dir);
    gx_raster_config(dump_dir, dump_every);
    gx_raster_config_xfb(xfb_every);
    if (draw_log)
        gx_raster_render_all();        /* logged frames must be drawn */
    if (want_window) {
#ifdef NFLS2_HAVE_WINDOW
        window_set_aspect_mode(aspect_mode);
        if (dump_audio) window_set_audio_dump(dump_audio);
        window_set_gpu(gpu_scale);
        if (!window_open(window_height))
            return 1;
        gx_raster_render_all();
        interrupts_set_field_callback(window_field);
        if (!max_steps_given)
            max_steps = ~0ull;         /* play until the window is closed */
#else
        fprintf(stderr, "error: this build has no window support (configure with -DNFLS2_WINDOW=ON)\n");
        return 2;
#endif
    }

    static CPUState cpu;
    if (!cpu_init(&cpu))
        return 1;
    {
        /* The port's own memory past the GameCube's 24 MB (uismenu.h): the
           game is told it has 24 MB and never goes there. */
        u8* more = (u8*)realloc(cpu.ram, (size_t)cpu.ram_size + PORT_RAM_SIZE);
        if (more) {
            memset(more + cpu.ram_size, 0, PORT_RAM_SIZE);
            cpu.ram = more;
            cpu.ram_size += PORT_RAM_SIZE;
        }
    }

    hle_cpu_install(&cpu);
    prof_start(&cpu);
    mmio_install(&cpu);
    if (memcard_path && !exi_insert_memcard(0, memcard_path))
        fprintf(stderr, "warning: running without a memory card\n");

    u32 entry = dol_load(&cpu, disc_dir, skip_hash);
    if (!entry)
        return 1;
    if (!boot_globals_init(&cpu, disc_dir))
        return 1;
    textpatch_apply(&cpu, text_path);
    uicolors_load(colors_path);
    if (!disc_open(disc_dir) || !boot_place_fst(&cpu))
        return 1;

    /* State the IPL leaves behind: translation and FP on, paired singles enabled. */
    cpu.msr = 0x00002032u;
    cpu.hid2 = PPC_HID2_LSQE | PPC_HID2_PSE | 0x40000000u; /* + write-gather pipe */
    cpu.pc = entry;
    print_location("entry", entry);

    double started = wall_seconds();
    g_launch_wall = started;
    const char* reason = "step limit reached";
    unsigned long long steps = 0;
    unsigned long long quit_steps = 0;   /* step at which the window was closed */
    u32 idle_lo = g_have_symbols ? symbols_address("SelectThread") : 0;
    u32 idle_hi = idle_lo ? symbols_function_end(idle_lo) : 0;
    while (steps < max_steps) {
        u32 pc = cpu.pc;
        if (g_trace_at > 0 && !g_tracing && g_trace_lines == 0 && interrupts_game_seconds() >= g_trace_at) {
            g_tracing = true;
            g_trace_frames_start = gx_raster_frames();
            printf("  trace: start at %.2f s, frame %u\n", interrupts_game_seconds(), g_trace_frames_start);
        }
        if (g_tracing && gx_raster_frames() >= g_trace_frames_start + TRACE_FRAMES) {
            g_tracing = false;
            g_trace_lines = TRACE_MAX_LINES;
            printf("  trace: end at %.2f s, frame %u\n", interrupts_game_seconds(), gx_raster_frames());
        }
        g_profiling = g_debug && g_profile && (steps + PROFILE_STEPS >= max_steps || quit_steps ||
                                    (stop_at > 0 && interrupts_game_seconds() >= stop_at - 1.0));
        cpu.downcount = 0;
        if (!dolrecomp_call(&cpu, pc)) {
            /* No native version: code the game built in memory at run time
               (the sound engine makes small call stubs). Interpret it. */
            InterpResult ir = interp_run(&cpu, 4096);
            if (ir == INTERP_UNSUPPORTED) {
                reason = "unsupported instruction in run-time code";
                break;
            }
            if (ir == INTERP_BAD_ADDRESS) {
                reason = "no recompiled code at this address";
                break;
            }
        }
        s64 consumed = -cpu.downcount;
        cpu.timebase += (u64)(consumed > 0 ? consumed : 1) / 12u + 1u;
        steps++;

        if (cpu.exception && hle_handle_exception(&cpu))
            continue;
        if (cpu.exception) {
            reason = "CPU exception";
            break;
        }
        interrupts_poll(&cpu);

        /* Idle skipping: with no thread ready to run, the OS spins in
           SelectThread with interrupts on until one wakes a thread. Nothing
           can change until the next timed event, so jump straight to it
           instead of spinning (the game sees exactly the same timing). */
        if (idle_skip && idle_lo && cpu.pc >= idle_lo && cpu.pc < idle_hi && (cpu.msr & 0x8000u) &&
            mem_read32(&cpu, 0x800000E4u) == 0 && !any_thread_ready(&cpu)) {
            u64 next = interrupts_next_event();
            if (next > cpu.timebase && next != ~0ull) {
                g_idle_ticks += next - cpu.timebase;
                cpu.timebase = next;
                g_idle_skips++;
            }
        }
        if ((steps & 0x3FFu) == 0) {
            static int last_second;
            double now = interrupts_game_seconds();
            if ((int)now != last_second) {
                last_second = (int)now;
                if (g_debug)
                    print_streams_brief(&cpu, now);
            }
        }
#ifdef NFLS2_HAVE_WINDOW
        if (want_window && (steps & 0xFFFu) == 0 && window_take_text_search()) {
            static const char* const k_phrases[] = {"memory card", "storage device", "save device"};
            for (size_t i = 0; i < sizeof k_phrases / sizeof *k_phrases; i++)
                textscan_run(&cpu, k_phrases[i]);
            g_text_watch_until = interrupts_game_seconds() + 3.0;
            printf("textcall: watching game calls for 3 seconds\n");
            fflush(stdout);
        }
        if (want_window && window_quit_requested()) {
            /* Run a little longer to profile where the game spends its time
               (shows what a frozen game is stuck on), then stop. */
            if (!g_debug) {
                reason = "window closed";
                break;
            }
            if (!quit_steps) {
                quit_steps = steps;
                printf("window closed: profiling a few more steps before stopping\n");
            } else if (steps - quit_steps >= PROFILE_STEPS / 4) {
                reason = "window closed";
                break;
            }
        }
#endif
        if (stop_at > 0 && (steps & 0xFFFu) == 0 && interrupts_game_seconds() >= stop_at) {
            reason = "game time limit reached";
            break;
        }
    }

    gx_raster_sync();
    double secs = wall_seconds() - started;
    printf("\n=== stopped after %llu steps: %s ===\n", steps, reason);
    printf("run time %.1f s (%.1f million steps/s), game time %.1f s\n", secs,
           secs > 0 ? (double)steps / secs / 1e6 : 0.0, interrupts_game_seconds());
    printf("idle skipping: %llu times, %.1f s of game time skipped (%.0f%%)\n", g_idle_skips,
           (double)g_idle_ticks / 40500000.0, interrupts_game_seconds() > 0
           ? (double)g_idle_ticks / 40500000.0 / interrupts_game_seconds() * 100.0 : 0.0);
    print_location("pc   ", cpu.pc);
    if (cpu.exception) {
        printf("exception flags 0x%08X, program cause 0x%08X\n", cpu.exception, cpu.program_exception);
        print_location("srr0 ", cpu.srr0);
    }
    print_location("lr   ", cpu.lr);
    printf("r1 (stack) 0x%08X  r2 0x%08X  r13 0x%08X  r14 0x%08X\n", cpu.gpr[1], cpu.gpr[2], cpu.gpr[13], cpu.gpr[14]);
    if ((strcmp(reason, "no recompiled code at this address") == 0 ||
         strcmp(reason, "unsupported instruction in run-time code") == 0) && guest_ptr(&cpu, cpu.pc & ~3u)) {
        /* Is there code here (the game copied or loaded it at run time) or data? */
        printf("memory at pc:");
        for (int i = -4; i < 12; i++)
            printf("%s %08X", (i + 4) % 8 ? "" : "\n ", mem_read32(&cpu, cpu.pc + (u32)(i * 4)));
        printf("\n");
    }
    if (strcmp(reason, "game time limit reached") != 0 && strcmp(reason, "window closed") != 0) {
        printf("registers:");
        for (int r = 0; r < 32; r++)
            printf("%s r%-2d %08X", (r % 8) ? "" : "\n ", r, cpu.gpr[r]);
        printf("\n ctr %08X  cr %08X  xer %08X\n", cpu.ctr, cpu.cr, cpu.xer);
    }
    printf("backtrace (innermost first):\n");
    print_backtrace(&cpu, cpu.gpr[1], cpu.lr, "  ");
    print_threads(&cpu);
    if (g_debug) {
        print_profile();
        prof_report();
    }
    interp_report();
    exi_report();
    if (g_debug) {
        print_call_counts();
        print_streams(&cpu);
    }
    disc_report_files();

    printf("\nlast dispatched addresses (oldest first):\n");
    u32 n = g_trace_pos < TRACE_LEN ? g_trace_pos : TRACE_LEN;
    for (u32 i = 0; i < n; i++)
        print_location("  ", g_trace[(g_trace_pos - n + i) % TRACE_LEN]);

    printf("\n");
    hle_exceptions_report();
    interrupts_report();
    gx_fifo_report();
    gx_raster_report();
    disc_report();
    ax_hle_report();
    hle_cpu_report();
    mmio_report();
#ifdef NFLS2_HAVE_WINDOW
    if (want_window)
        window_close();
#endif
    cpu_free(&cpu);
    return 0;
}
