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
#include "audio/ax_hle.h"
#include "runtime/mmio.h"
#include "runtime/symbols.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TRACE_LEN 24

static u32 g_trace[TRACE_LEN];
static u32 g_trace_pos;
static bool g_have_symbols;
static bool g_verbose_entries = true;

/* Bitmap of functions already announced, indexed by (start - 0x80000000) / 4. */
static u8* g_seen;

/* Profile of the last part of the run: dispatched calls counted per function,
   indexed by (start - 0x80000000) / 4. Shows what a stuck game is looping on. */
#define PROFILE_STEPS 20000000ull
static u32* g_profile;
static bool g_profiling;
static unsigned long long g_profiled;

/* --trace-at: from this much game time, log calls that matter for frame
   pacing (video, OS timing and threads, GPU sync, the game's video module at
   0x801CE000-0x801CF000) across a few displayed frames. */
static double g_trace_at;
static bool g_tracing;
static u32 g_trace_lines, g_trace_frames_start;
#define TRACE_MAX_LINES 3000
#define TRACE_FRAMES 4

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

/* Every dispatched call passes through here. Later this is where HLE
   replacements go; for now it only records where execution has been. */
int dolrecomp_dispatch_replacement(CPUState* ctx, u32 address) {
    g_trace[g_trace_pos++ % TRACE_LEN] = address;

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

static void usage(const char* argv0) {
    fprintf(stderr,
            "usage: %s <disc folder> [--map main.map] [--max-steps N] [--quiet] [--skip-hash]\n"
            "         [--dump-every N (default 60, 0 = off)] [--dump-dir DIR (default frames)]\n"
            "         [--press SECONDS:BUTTON[+BUTTON][@HOLD]]...  e.g. --press 5.5:A --press 7:DOWN@0.2\n"
            "         [--stop-at SECONDS (stop at this much game time)]\n"
            "         [--xfb-every FIELDS (also save the TV frame buffer every N video fields)]\n"
            "         [--trace-at SECONDS (log frame-pacing calls for a few frames from then)]\n"
            "         [--unpaced (let the game draw as many frames per video field as it likes)]\n"
            "  <disc folder>  the folder containing sys\\main.dol and files\\ (e.g. C:\\nfls2\\disc)\n",
            argv0);
}

int main(int argc, char** argv) {
    const char* disc_dir = NULL;
    const char* map_path = NULL;
    unsigned long long max_steps = 50000000ull;
    bool skip_hash = false;
    const char* dump_dir = "frames";
    u32 dump_every = 60;
    double stop_at = 0;
    u32 xfb_every = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--map") == 0 && i + 1 < argc) {
            map_path = argv[++i];
        } else if (strcmp(argv[i], "--max-steps") == 0 && i + 1 < argc) {
            max_steps = strtoull(argv[++i], NULL, 10);
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
        } else if (strcmp(argv[i], "--unpaced") == 0) {
            mmio_set_frame_pacing(false);
        } else if (strcmp(argv[i], "--trace-at") == 0 && i + 1 < argc) {
            g_trace_at = strtod(argv[++i], NULL);
        } else if (strcmp(argv[i], "--xfb-every") == 0 && i + 1 < argc) {
            xfb_every = (u32)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--stop-at") == 0 && i + 1 < argc) {
            stop_at = strtod(argv[++i], NULL);
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
    g_profile = (u32*)calloc(GC_MAIN_RAM_SIZE >> 2, sizeof(u32));
    gx_raster_config(dump_dir, dump_every);
    gx_raster_config_xfb(xfb_every);

    static CPUState cpu;
    if (!cpu_init(&cpu))
        return 1;
    hle_cpu_install(&cpu);
    mmio_install(&cpu);

    u32 entry = dol_load(&cpu, disc_dir, skip_hash);
    if (!entry)
        return 1;
    if (!boot_globals_init(&cpu, disc_dir))
        return 1;
    if (!disc_open(disc_dir) || !boot_place_fst(&cpu))
        return 1;

    /* State the IPL leaves behind: translation and FP on, paired singles enabled. */
    cpu.msr = 0x00002032u;
    cpu.hid2 = PPC_HID2_LSQE | PPC_HID2_PSE | 0x40000000u; /* + write-gather pipe */
    cpu.pc = entry;
    print_location("entry", entry);

    clock_t started = clock();
    const char* reason = "step limit reached";
    unsigned long long steps = 0;
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
        g_profiling = g_profile && (steps + PROFILE_STEPS >= max_steps ||
                                    (stop_at > 0 && interrupts_game_seconds() >= stop_at - 1.0));
        cpu.downcount = 0;
        if (!dolrecomp_call(&cpu, pc)) {
            reason = "no recompiled code at this address";
            break;
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
        if (stop_at > 0 && (steps & 0xFFFu) == 0 && interrupts_game_seconds() >= stop_at) {
            reason = "game time limit reached";
            break;
        }
    }

    double secs = (double)(clock() - started) / CLOCKS_PER_SEC;
    printf("\n=== stopped after %llu steps: %s ===\n", steps, reason);
    printf("run time %.1f s (%.1f million steps/s), game time %.1f s\n", secs,
           secs > 0 ? (double)steps / secs / 1e6 : 0.0, interrupts_game_seconds());
    print_location("pc   ", cpu.pc);
    if (cpu.exception) {
        printf("exception flags 0x%08X, program cause 0x%08X\n", cpu.exception, cpu.program_exception);
        print_location("srr0 ", cpu.srr0);
    }
    print_location("lr   ", cpu.lr);
    printf("r1 (stack) 0x%08X  r2 0x%08X  r13 0x%08X  r14 0x%08X\n", cpu.gpr[1], cpu.gpr[2], cpu.gpr[13], cpu.gpr[14]);
    printf("backtrace (innermost first):\n");
    print_backtrace(&cpu, cpu.gpr[1], cpu.lr, "  ");
    print_threads(&cpu);
    print_profile();

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
    cpu_free(&cpu);
    return 0;
}
