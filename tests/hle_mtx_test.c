/* Checks the native matrix functions (src/runtime/hle_mtx.c) against the
 * game's own, recompiled ones: random matrices and vectors (with NaNs,
 * infinities, denormals, huge and tiny values, rounding ties and
 * overlapping outputs mixed in), each function run both ways from the same
 * state; every register, the cycle count and the memory around must come
 * out the same whenever the native one takes the call. Then how long each
 * takes.
 *
 *   cmake --build build --target nfls2_hle_mtx_test
 *   build\nfls2_hle_mtx_test.exe C:\nfls2\disc\sys\main.dol [runs]
 */
#include "generated.h"
#include "runtime/hle_mtx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* (what the port's main program provides to the game code) */
unsigned char g_nfls2_direct_off[0x02000000u >> 5];
int g_nfls2_direct_on = 1;
unsigned g_nfls2_direct_depth;
unsigned long long g_nfls2_direct_calls;
void mmio_pipe_write(CPUState* cpu, u32 value, u8 size) { (void)cpu; (void)value; (void)size; }

/* The locked cache, as the port's mmio.c has it (each run's own copy). */
#define L1_MASK 0x0003FFFFu
static u8 g_l1_a[L1_MASK + 1], g_l1_b[L1_MASK + 1];
static u8* g_l1 = g_l1_a;
u8* mmio_l1_at(u32 ea, u32 n) {
    u32 o = ea & L1_MASK;
    return o + n <= L1_MASK + 1u ? g_l1 + o : NULL;
}
static u64 l1_read(CPUState* cpu, u32 ea, u8 size) {
    (void)cpu;
    u64 v = 0;
    if ((ea & 0xF0000000u) != 0xE0000000u) return 0;
    for (u8 i = 0; i < size; i++) v = (v << 8) | g_l1[(ea + i) & L1_MASK];
    return v;
}
static void l1_write(CPUState* cpu, u32 ea, u64 value, u8 size) {
    (void)cpu;
    if ((ea & 0xF0000000u) != 0xE0000000u) return;
    for (int i = size - 1; i >= 0; i--) { g_l1[(ea + (u32)i) & L1_MASK] = (u8)value; value >>= 8; }
}
int nfls2_hle(CPUState* cpu, u32 target);

#define RAM_SIZE 0x01800000u
#define DONE 0x81700000u               /* return address: back here */
#define WORK 0x01000000u               /* the area compared: inputs, outputs, stack */
#define WORK_SIZE 0x00700000u
static u8 g_ram_a[RAM_SIZE], g_ram_b[RAM_SIZE];
static CPUState A, B;

static double now(void) {
    struct timespec t;
    timespec_get(&t, TIME_UTC);
    return (double)t.tv_sec + t.tv_nsec * 1e-9;
}

static u32 be(const u8* p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }

static int load_dol(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    u8* d = (u8*)malloc((size_t)n);
    if (!d || fread(d, 1, (size_t)n, f) != (size_t)n) { fclose(f); return 0; }
    fclose(f);
    for (int i = 0; i < 18; i++) {
        u32 off = be(d + i * 4), a = be(d + 72 + i * 4), sz = be(d + 144 + i * 4);
        if (sz && (a & 0x01FFFFFFu) + sz <= RAM_SIZE && off + sz <= (u32)n) memcpy(g_ram_a + (a & 0x01FFFFFFu), d + off, sz);
    }
    free(d);
    return 1;
}

static void run_game(CPUState* c, u32 fn) {
    c->lr = DONE;
    c->pc = fn;
    c->downcount = 0;
    if (!dolrecomp_call(c, fn)) { printf("no game code at %08X\n", fn); exit(2); }
}

static unsigned long long g_rs = 88172645463325252ull;
static u32 rnd(void) { g_rs ^= g_rs << 13; g_rs ^= g_rs >> 7; g_rs ^= g_rs << 17; return (u32)(g_rs >> 11); }
static u32 rnd_float(int plain) {
    u32 s = rnd() & 0x80000000u, m = rnd() & 0x7FFFFFu;
    if (plain) return s | ((115u + rnd() % 20u) << 23) | m;
    switch (rnd() % 50) {
    case 0: return 0x7FC00000u | s;                       /* NaN */
    case 1: return s | m;                                 /* denormal */
    case 2: return 0;
    case 3: return 0x7F800000u | s;                       /* infinity */
    case 4: case 5: return s | ((100u + rnd() % 60u) << 23) | m;   /* huge */
    case 6: case 7: return s | ((1u + rnd() % 20u) << 23) | m;     /* tiny */
    case 8: case 9: case 10: return s | ((120u + rnd() % 10u) << 23) | (m & 0x7F0000u);   /* short: ties */
    case 11: case 12: return s | ((86u + rnd() % 5u) << 23) | (rnd() % 2 ? m : 0);      /* around the native  */
    case 13: case 14: return s | ((186u + rnd() % 5u) << 23) | (rnd() % 2 ? m : 0);     /* versions' limits   */
    default: return s | ((110u + rnd() % 30u) << 23) | m;
    }
}
static void put(u8* ram, u32 a, u32 v) {
    u8* p = ram + (a & 0x01FFFFFFu);
    p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v;
}

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: nfls2_hle_mtx_test <main.dol> [runs]\n"); return 2; }
    cpu_init(&A);
    A.ram = g_ram_a;
    A.ram_size = RAM_SIZE;
    A.msr = 0x2032;
    A.hid2 = PPC_HID2_LSQE | PPC_HID2_PSE;
    A.external_read = l1_read;
    A.external_write = l1_write;
    if (!load_dol(argv[1])) { printf("can't read %s\n", argv[1]); return 2; }
    memcpy(g_ram_b, g_ram_a, RAM_SIZE);
    static const u32 fns[3] = {NFLS2_HLE_MTX_CONCAT, NFLS2_HLE_MTX_MULTVEC, NFLS2_HLE_MTX_MULTVEC_SR};
    static const char* const names[3] = {"PSMTXConcat", "PSMTXMultVec", "PSMTXMultVecSR"};
    long native[3] = {0}, declined[3] = {0}, bad = 0;
    int runs = argc > 2 ? atoi(argv[2]) : 300000;
    for (int it = 0; it < runs; it++) {
        int j = it % 3, plain = rnd() % 2;
        for (u32 a = 0x81000000u; a < 0x81000100u; a += 4) put(g_ram_a, a, rnd_float(plain));
        for (u32 o = 0; o < 0x100u; o += 4) {     /* the same in the locked cache */
            u32 v = be(g_ram_a + 0x01000000u + o);
            g_l1_a[0x1000u + o] = (u8)(v >> 24); g_l1_a[0x1001u + o] = (u8)(v >> 16);
            g_l1_a[0x1002u + o] = (u8)(v >> 8); g_l1_a[0x1003u + o] = (u8)v;
        }
        for (int i = 0; i < 32; i++) {
            A.fpr[i] = (double)(int)(rnd() % 1000) / 7.0;
            A.ps1[i] = (double)(int)(rnd() % 1000) / 3.0;
        }
        A.gpr[1] = 0x81600000u; A.gpr[3] = 0x81000000u; A.gpr[4] = 0x81000040u; A.gpr[5] = 0x81000080u;
        switch (rnd() % 10) {                 /* outputs over inputs, misaligned, ... */
        case 0: A.gpr[5] = A.gpr[3]; break;
        case 1: A.gpr[5] = A.gpr[4]; break;
        case 2: A.gpr[5] = 0x81000008u; break;
        case 3: A.gpr[4] = 0x81000004u; break;
        case 4: A.gpr[5] = 0x81000082u; break;
        case 5: A.gpr[3] = 0xE0001000u; A.gpr[4] = 0xE0001040u; A.gpr[5] = 0xE0001080u; break;   /* locked cache */
        case 6: A.gpr[3] = 0xE0001000u; A.gpr[5] = 0xE0001000u; break;
        case 7: A.gpr[4] = 0xC1000040u; A.gpr[5] = 0xC1000000u; break;   /* uncached mirror, over a */
        case 8: A.gpr[5] = 0xE003FFF8u; break;                             /* wrapping the cache's end */
        }
        A.gpr[6] = 0x1234;
        A.msr = 0x2032;                       /* (an exception in the last run changes it) */
        A.fpscr = rnd() % 20 == 0 ? 1u : 0u;
        A.reserve_valid = 0;
        A.exception = 0;
        B = A;
        B.ram = g_ram_b;
        memcpy(g_ram_b + WORK, g_ram_a + WORK, WORK_SIZE);
        memcpy(g_l1_b, g_l1_a, sizeof g_l1_a);
        g_l1 = g_l1_a;
        run_game(&A, fns[j]);
        g_l1 = g_l1_b;
        B.lr = DONE;
        B.pc = fns[j];
        B.downcount = 0;
        if (!nfls2_hle(&B, fns[j])) { declined[j]++; continue; }
        native[j]++;
        const char* diff = NULL;
        if (A.pc != B.pc || A.downcount != B.downcount) diff = "pc / cycles";
        for (int i = 0; i < 32; i++) {
            if (memcmp(&A.fpr[i], &B.fpr[i], 8) || memcmp(&A.ps1[i], &B.ps1[i], 8)) diff = "float register";
            if (A.gpr[i] != B.gpr[i]) diff = "integer register";
        }
        if (A.fpscr != B.fpscr || A.cr != B.cr || A.lr != B.lr || A.ctr != B.ctr) diff = "special register";
        if (memcmp(g_ram_a + WORK, g_ram_b + WORK, WORK_SIZE)) diff = "memory";
        if (memcmp(g_l1_a, g_l1_b, sizeof g_l1_a)) diff = "locked cache";
        if (diff) {
            if (bad < 10) printf("%s, run %d: %s differs\n", names[j], it, diff);
            bad++;
        }
    }
    for (int j = 0; j < 3; j++) printf("%-15s %7ld done natively, %7ld left to the game code\n", names[j], native[j], declined[j]);
    printf("differences: %ld\n", bad);

    for (u32 a = 0x81000000u; a < 0x81000100u; a += 4) put(g_ram_a, a, ((118u + (a >> 2 & 7u)) << 23) | 0x123456u);
    const int n = 2000000;
    g_l1 = g_l1_a;
    A.msr = 0x2032;
    A.fpscr = 0;
    A.exception = 0;
    for (int j = 0; j < 3; j++) {
        double t = now();
        for (int k = 0; k < n; k++) {
            A.gpr[1] = 0x81600000u; A.gpr[3] = 0x81000000u; A.gpr[4] = 0x81000040u; A.gpr[5] = 0x81000080u;
            run_game(&A, fns[j]);
        }
        double tg = now() - t;
        t = now();
        for (int k = 0; k < n; k++) {
            A.gpr[1] = 0x81600000u; A.gpr[3] = 0x81000000u; A.gpr[4] = 0x81000040u; A.gpr[5] = 0x81000080u;
            A.lr = DONE;
            if (!nfls2_hle(&A, fns[j])) { printf("%s: declined in the timing run\n", names[j]); break; }
        }
        double tn = now() - t;
        printf("%-15s game code %6.1f ns, native %6.1f ns a call\n", names[j], tg * 1e9 / n, tn * 1e9 / n);
    }
    return bad != 0;
}
