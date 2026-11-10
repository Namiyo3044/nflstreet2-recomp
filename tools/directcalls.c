/* directcalls: rewrites DolRecomp's generated chunks so that a call into
 * another chunk calls it directly, instead of returning to the port's
 * dispatcher and coming back through it.
 *
 * DolRecomp splits the game's code into chunks, one C function each
 * (func_<start>, entered through a switch on ctx->pc). A `bl` within a chunk
 * is already a plain goto, but a `bl` to another chunk is emitted as
 *
 *         ctx->lr = <return address>;
 *         ctx->pc = <target>;
 *         return;
 *
 * so every such call, and its return, goes out to the dispatcher (which
 * looks the address up, checks the port's hooks, polls the hardware) and back
 * in through the chunk's entry switch. This tool turns it into
 *
 *         ctx->lr = <return address>;
 *         ctx->pc = <target>;
 *         if (NFLS2_DIRECT_OK(<target>) && nfls2_direct_enter()) {
 *             func_<target chunk>(ctx);
 *             nfls2_direct_leave();
 *             if (ctx->pc == <return address> && !ctx->exception &&
 *                 ctx->downcount > -(s64)DOLRECOMP_C_LOOP_CYCLE_BUDGET)
 *                 goto label_<return address>;
 *         }
 *         return;
 *
 * The callee runs exactly as the dispatcher would have run it (same entry,
 * same state, all of it in ctx), and the caller only carries on inline when
 * the callee came back to the instruction after the call with nothing
 * pending; anything else (a thread switch, an exception, the cycle budget
 * used up so the hardware gets polled) returns to the dispatcher with
 * ctx->pc already right, as before. NFLS2_DIRECT_OK lets the port keep the
 * calls it hooks (and every call, while a debug trace is on) going through
 * the dispatcher.
 *
 * A direct call to one of the functions in k_hle (below) first asks the
 * port to do it natively (src/runtime/hle_mtx.c); only if that declines does
 * the call go on as above.
 *
 * Usage: directcalls <output dir> <chunk.c>...
 * Each chunk is written to the output directory under its own name (only if
 * it changed, so an unchanged chunk isn't rebuilt), with its
 * `#include "../generated.h"` made `#include "generated.h"`.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>

typedef struct {
    const char* path;
    char* text;
    size_t len;
    uint32_t start;                    /* func_<start> */
    uint32_t* cases;                   /* its entry switch's addresses, sorted */
    size_t ncases;
} Chunk;

static Chunk* g_chunks;
static size_t g_nchunks, g_nvalid;   /* (the valid ones, with a main function, come first once sorted) */

static char* read_all(const char* path, size_t* len) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* d = (char*)malloc((size_t)n + 1);
    if (!d || fread(d, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(d); return NULL; }
    fclose(f);
    /* (Windows line endings, from a DolRecomp run on Windows, dropped: the
       patterns below are matched line by line) */
    size_t o = 0;
    for (long i = 0; i < n; i++)
        if (d[i] != '\r') d[o++] = d[i];
    d[o] = 0;
    *len = o;
    return d;
}

static int cmp_u32(const void* a, const void* b) {
    uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b;
    return x < y ? -1 : x > y;
}

static int cmp_chunk(const void* a, const void* b) {
    uint32_t x = ((const Chunk*)a)->start, y = ((const Chunk*)b)->start;
    return x < y ? -1 : x > y;
}

/* The chunk's main function and the cases of its entry switch. */
static int scan_chunk(Chunk* c) {
    const char* p = strstr(c->text, "\nvoid func_");
    if (!p) return 0;
    p += 11;
    c->start = (uint32_t)strtoul(p, NULL, 16);
    const char* sw = strstr(p, "switch (ctx->pc) {");
    if (!sw) return 0;
    const char* end = strstr(sw, "default: return;");
    if (!end) return 0;
    size_t cap = 1024;
    c->cases = (uint32_t*)malloc(cap * sizeof(uint32_t));
    for (const char* q = sw; (q = strstr(q, "case 0x")) && q < end; q += 7) {
        if (c->ncases == cap) { cap *= 2; c->cases = (uint32_t*)realloc(c->cases, cap * sizeof(uint32_t)); }
        c->cases[c->ncases++] = (uint32_t)strtoul(q + 7, NULL, 16);
    }
    qsort(c->cases, c->ncases, sizeof(uint32_t), cmp_u32);
    return 1;
}

static const Chunk* chunk_for(uint32_t addr) {
    size_t lo = 0, hi = g_nvalid;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (g_chunks[mid].start <= addr) lo = mid + 1;
        else hi = mid;
    }
    return lo ? &g_chunks[lo - 1] : NULL;
}

static int has_case(const Chunk* c, uint32_t addr) {
    return bsearch(&addr, c->cases, c->ncases, sizeof(uint32_t), cmp_u32) != NULL;
}

/* Output buffer. */
static char* g_out;
static size_t g_out_len, g_out_cap;
static void put(const char* s, size_t n) {
    if (g_out_len + n + 1 > g_out_cap) {
        g_out_cap = (g_out_len + n + 1) * 2;
        g_out = (char*)realloc(g_out, g_out_cap);
    }
    memcpy(g_out + g_out_len, s, n);
    g_out_len += n;
}
static void puts_(const char* s) { put(s, strlen(s)); }

/* Matches `<spaces>ctx->lr = 0x<hex>u;\n<spaces>ctx->pc = 0x<hex>u;\n<spaces>return;\n`
   at p; fills the two addresses, the indent and the length. */
static size_t match_call(const char* p, uint32_t* ret, uint32_t* target, int* indent) {
    const char* s = p;
    int ind = 0;
    while (*s == ' ') s++, ind++;
    if (strncmp(s, "ctx->lr = 0x", 12)) return 0;
    char* e;
    *ret = (uint32_t)strtoul(s + 12, &e, 16);
    if (strncmp(e, "u;\n", 3)) return 0;
    s = e + 3;
    while (*s == ' ') s++;
    if (strncmp(s, "ctx->pc = 0x", 12)) return 0;
    *target = (uint32_t)strtoul(s + 12, &e, 16);
    if (strncmp(e, "u;\n", 3)) return 0;
    s = e + 3;
    while (*s == ' ') s++;
    if (strncmp(s, "return;\n", 8)) return 0;
    s += 8;
    *indent = ind;
    return (size_t)(s - p);
}

/* Matches `<spaces>ppc_fallback_instruction(ctx, 0x<raw>u, 0x<addr>u);\n<spaces>return;\n`
   for the instructions that never change the flow (mfspr, and the data cache
   operations, which do nothing on a PC), followed by the next instruction's
   label; fills the line's address. */
static size_t match_fallback(const char* p, uint32_t* addr, int* indent) {
    const char* s = p;
    int ind = 0;
    while (*s == ' ') s++, ind++;
    if (strncmp(s, "ppc_fallback_instruction(ctx, 0x", 32)) return 0;
    char* e;
    uint32_t raw = (uint32_t)strtoul(s + 32, &e, 16);
    if (strncmp(e, "u, 0x", 5)) return 0;
    *addr = (uint32_t)strtoul(e + 5, &e, 16);
    if (strncmp(e, "u);\n", 4)) return 0;
    s = e + 4;
    while (*s == ' ') s++;
    if (strncmp(s, "return;\n", 8)) return 0;
    s += 8;
    uint32_t op = raw >> 26, xo = (raw >> 1) & 0x3FFu;
    if (op != 31 || (xo != 339 && xo != 54 && xo != 86 && xo != 470)) return 0;
    const char* q = s;
    while (*q == '\n') q++;
    char want[32];
    snprintf(want, sizeof want, "label_%08X:", *addr + 4u);
    if (strncmp(q, want, strlen(want))) return 0;
    *indent = ind;
    return (size_t)(s - p);
}

/* Matches the call through a register (blrl / bctrl):
   `<spaces>ctx->lr = 0x<hex>u;\n<spaces>ctx->pc = target;\n<spaces>goto return_dispatch_<chunk>;\n`. */
static size_t match_indirect(const char* p, uint32_t* ret, int* indent) {
    const char* s = p;
    int ind = 0;
    while (*s == ' ') s++, ind++;
    if (strncmp(s, "ctx->lr = 0x", 12)) return 0;
    char* e;
    *ret = (uint32_t)strtoul(s + 12, &e, 16);
    if (strncmp(e, "u;\n", 3)) return 0;
    s = e + 3;
    while (*s == ' ') s++;
    if (strncmp(s, "ctx->pc = target;\n", 18)) return 0;
    s += 18;
    while (*s == ' ') s++;
    if (strncmp(s, "goto return_dispatch_", 21)) return 0;
    s = strchr(s, '\n');
    if (!s) return 0;
    *indent = ind;
    return (size_t)(s + 1 - p);
}

static size_t g_rewritten, g_kept, g_fallbacks, g_indirect, g_hle;

/* Functions the port does natively when called directly (src/runtime/hle_mtx.c,
   whose nfls2_hle() runs the whole function or declines; the call then goes
   on as usual): the direct calls to them try that first. */
static const uint32_t k_hle[] = {
    0x8024AA18u,                       /* PSMTXConcat */
    0x8024AAE4u,                       /* PSMTXMultVec */
    0x8024AB38u,                       /* PSMTXMultVecSR */
};
static int is_hle(uint32_t target) {
    for (size_t i = 0; i < sizeof k_hle / sizeof k_hle[0]; i++)
        if (k_hle[i] == target) return 1;
    return 0;
}

static void rewrite(const Chunk* c) {
    g_out_len = 0;
    const char* t = c->text;
    const char* func = strstr(t, "\nvoid func_");      /* calls before this are in the loop helpers */
    const char* body_end = strstr(func ? func : t, "\nreturn_dispatch_");
    /* the labels of the main function, to know the return address has one */
    const char* p = t;
    while (*p) {
        const char* nl = strchr(p, '\n');
        size_t line_len = nl ? (size_t)(nl - p + 1) : strlen(p);
        if (!strncmp(p, "#include \"../generated.h\"", 25)) {
            puts_("#include \"generated.h\"\n");
            p += line_len;
            continue;
        }
        uint32_t ret, target;
        int indent;
        int in_body = func && p > func && (!body_end || p < body_end);
        uint32_t fa;
        size_t fm = in_body ? match_fallback(p, &fa, &indent) : 0;
        if (fm) {
            /* carries on to the next instruction unless the CPU core moved
               the flow (an exception) */
            char buf[512];
            char pad[64];
            int n = indent < 60 ? indent : 60;
            memset(pad, ' ', (size_t)n);
            pad[n] = 0;
            int k = (int)strcspn(p, "\n") + 1;
            put(p, (size_t)k);
            snprintf(buf, sizeof buf, "%sif (ctx->pc != 0x%08Xu || ctx->exception) return;\n", pad, fa + 4u);
            puts_(buf);
            g_fallbacks++;
            p += fm;
            continue;
        }
        size_t im = in_body ? match_indirect(p, &ret, &indent) : 0;
        if (im && has_case(c, ret)) {
            /* the target is only known when it runs: looked up the way the
               dispatcher would, a target in this chunk going through its own
               switch as before */
            char buf[1536];
            char pad[64];
            int n = indent < 60 ? indent : 60;
            memset(pad, ' ', (size_t)n);
            pad[n] = 0;
            snprintf(buf, sizeof buf,
                     "%sctx->lr = 0x%08Xu;\n"
                     "%sctx->pc = target;\n"
                     "%s{\n"
                     "%s    DolRecompFunction fn_ = dolrecomp_find_original(target);\n"
                     "%s    if (fn_ == func_%08X) goto return_dispatch_%08X;\n"
                     "%s    if (fn_ && NFLS2_DIRECT_OK(target) && nfls2_direct_enter()) {\n"
                     "%s        fn_(ctx);\n"
                     "%s        nfls2_direct_leave();\n"
                     "%s        if (ctx->pc == 0x%08Xu && !ctx->exception && ctx->downcount > -(s64)DOLRECOMP_C_LOOP_CYCLE_BUDGET)\n"
                     "%s            goto label_%08X;\n"
                     "%s    }\n"
                     "%s}\n"
                     "%sreturn;\n",
                     pad, ret, pad, pad, pad, pad, c->start, c->start, pad, pad, pad, pad, ret, pad, ret, pad, pad, pad);
            puts_(buf);
            g_indirect++;
            p += im;
            continue;
        }
        size_t m = in_body ? match_call(p, &ret, &target, &indent) : 0;
        if (m) {
            const Chunk* tc = chunk_for(target);
            /* (the return address is one of the caller's entry points, so its label exists) */
            if (tc && tc != c && has_case(tc, target) && has_case(c, ret)) {
                char buf[1024];
                char pad[64];
                int n = indent < 60 ? indent : 60;
                memset(pad, ' ', (size_t)n);
                pad[n] = 0;
                snprintf(buf, sizeof buf, "%sctx->lr = 0x%08Xu;\n%sctx->pc = 0x%08Xu;\n", pad, ret, pad, target);
                puts_(buf);
                if (is_hle(target)) {
                    /* (natively, if it can: ctx->pc is then the return address; declared
                       here rather than in fastmem.h, which every chunk includes, so only the
                       chunks with such a call are rebuilt) */
                    snprintf(buf, sizeof buf,
                             "%sint nfls2_hle(CPUState* cpu, u32 target);\n"
                             "%sif (NFLS2_DIRECT_OK(0x%08Xu) && nfls2_hle(ctx, 0x%08Xu)) {\n"
                             "%s    if (ctx->downcount > -(s64)DOLRECOMP_C_LOOP_CYCLE_BUDGET)\n"
                             "%s        goto label_%08X;\n"
                             "%s    return;\n"
                             "%s}\n",
                             pad, pad, target, target, pad, pad, ret, pad, pad);
                    puts_(buf);
                    g_hle++;
                }
                snprintf(buf, sizeof buf,
                         "%sif (NFLS2_DIRECT_OK(0x%08Xu) && nfls2_direct_enter()) {\n"
                         "%s    func_%08X(ctx);\n"
                         "%s    nfls2_direct_leave();\n"
                         "%s    if (ctx->pc == 0x%08Xu && !ctx->exception && ctx->downcount > -(s64)DOLRECOMP_C_LOOP_CYCLE_BUDGET)\n"
                         "%s        goto label_%08X;\n"
                         "%s}\n"
                         "%sreturn;\n",
                         pad, target, pad, tc->start, pad, pad, ret, pad, ret, pad, pad);
                puts_(buf);
                g_rewritten++;
                p += m;
                continue;
            }
            g_kept++;
        }
        put(p, line_len);
        p += line_len;
    }
}

static void write_if_changed(const char* path) {
    size_t old_len;
    char* old = read_all(path, &old_len);
    if (old && old_len == g_out_len && !memcmp(old, g_out, g_out_len)) { free(old); return; }
    free(old);
    FILE* f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "directcalls: can't write %s\n", path); exit(1); }
    fwrite(g_out, 1, g_out_len, f);
    fclose(f);
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: directcalls <output dir> <chunk.c>...\n");
        return 1;
    }
    const char* out_dir = argv[1];
    /* The chunks: given one by one, or "@list" (a file of their paths, one a
       line), which keeps the command short whatever the paths are: 160-odd
       long paths with spaces broke the build command on Windows. */
    char** paths = argv + 2;
    size_t npaths = (size_t)(argc - 2);
    if (argc == 3 && argv[2][0] == '@') {
        size_t len = 0;
        char* list = read_all(argv[2] + 1, &len);
        if (!list) { fprintf(stderr, "directcalls: can't read the list %s\n", argv[2] + 1); return 1; }
        size_t cap = 256;
        paths = (char**)malloc(cap * sizeof(char*));
        npaths = 0;
        for (char* line = strtok(list, "\r\n"); line; line = strtok(NULL, "\r\n")) {
            if (!*line) continue;
            if (npaths == cap) { cap *= 2; paths = (char**)realloc(paths, cap * sizeof(char*)); }
            paths[npaths++] = line;
        }
        if (!npaths) { fprintf(stderr, "directcalls: the list %s is empty\n", argv[2] + 1); return 1; }
    }
    g_nchunks = npaths;
    g_chunks = (Chunk*)calloc(g_nchunks, sizeof(Chunk));
    for (size_t i = 0; i < g_nchunks; i++) {
        Chunk* c = &g_chunks[i];
        c->path = paths[i];
        c->text = read_all(c->path, &c->len);
        if (!c->text) { fprintf(stderr, "directcalls: can't read %s\n", c->path); return 1; }
        if (!scan_chunk(c)) c->start = 0xFFFFFFFFu;   /* (no main function found: left as it is, never a target) */
    }
    /* sorted by start for the lookups; the paths go with them */
    qsort(g_chunks, g_nchunks, sizeof(Chunk), cmp_chunk);
    g_nvalid = g_nchunks;
    while (g_nvalid && g_chunks[g_nvalid - 1].start == 0xFFFFFFFFu) g_nvalid--;
    for (size_t i = 0; i < g_nchunks; i++) {
        Chunk* c = &g_chunks[i];
        rewrite(c);
        const char* base = strrchr(c->path, '/');
        const char* base2 = strrchr(c->path, '\\');
        if (!base || (base2 && base2 > base)) base = base2;
        char path[4096];
        snprintf(path, sizeof path, "%s/%s", out_dir, base ? base + 1 : c->path);
        write_if_changed(path);
    }
    printf("directcalls: %zu chunks (%zu with a main function), %zu calls into other chunks made direct, %zu left to the dispatcher, "
           "%zu calls through a register looked up directly, %zu cache / special-register instructions carried on inline, "
           "%zu calls to functions done natively\n",
           g_nchunks, g_nvalid, g_rewritten, g_kept, g_indirect, g_fallbacks, g_hle);
    if (g_nvalid > 1 && !g_rewritten) {
        fprintf(stderr, "directcalls: no calls matched; the generated code's layout isn't the one this tool knows\n");
        return 1;
    }
    return 0;
}
