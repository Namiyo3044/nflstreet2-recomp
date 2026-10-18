#include "textscan.h"
#include "mmio.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define MAX_HITS 80

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

/* Does memory at p hold the phrase (any case), with characters `step` bytes
   apart (1 = plain bytes, 2 = 16-bit big-endian characters)? */
static bool match(const u8* p, const u8* end, const char* phrase, int step) {
    for (const char* c = phrase; *c; c++, p += step) {
        if (p + step > end) return false;
        if (step == 2 && p[0] != 0) return false;
        if (lower(p[step - 1]) != lower((u8)*c)) return false;
    }
    return true;
}

static u32 char_at(const u8* p, int step) { return step == 2 ? (u32)p[0] << 8 | p[1] : p[0]; }

/* The whole string around a hit: back to the previous NUL, forward to the next. */
static void show(const u8* base, u32 size, u32 off, u32 guest, int step, const char* region, int n) {
    u32 start = off, end = off;
    while (start >= (u32)step && off - start < 400 && char_at(base + start - step, step) != 0)
        start -= step;
    while (end + step <= size && char_at(base + end, step) != 0 && end - off < 400) end += step;
    char text[512];
    int t = 0;
    for (u32 i = start; i < end && t < (int)sizeof(text) - 1; i += step) {
        u32 c = char_at(base + i, step);
        text[t++] = c >= 0x20 && c < 0x7F ? (char)c : c == '\n' ? '|' : '.';
    }
    text[t] = 0;
    u32 room = (end - start) / step;
    /* bytes up to the next non-zero byte after the terminator: how far text could grow */
    u32 pad = end;
    while (pad + step <= size && char_at(base + pad, step) == 0 && pad - end < 64) pad += step;
    printf("text: #%d %s 0x%08X (%s) %u chars, %u spare:", n, region, guest + (start - off),
           step == 2 ? "16-bit" : "bytes", room, (pad - end) / step ? (pad - end) / step - 1 : 0);
    u64 foff = 0;
    const char* file = region[0] == 'R' ? mmio_ram_source(start, &foff) : NULL;
    if (file) printf(" from %s +0x%llX", file, (unsigned long long)foff);
    else printf(" (not straight from a disc read)");
    printf("\ntext:     \"%s\"\n", text);
}

static int scan(const u8* base, u32 size, u32 guest_base, const char* region, const char* phrase, int n) {
    size_t len = strlen(phrase);
    for (int step = 1; step <= 2; step++) {
        const u8* end = base + size;
        for (u32 off = 0; off + len * step <= size; off++) {
            if (lower(base[off + step - 1]) != lower((u8)phrase[0])) continue;
            if (!match(base + off, end, phrase, step)) continue;
            if (n < MAX_HITS) show(base, size, off, guest_base + off, step, region, n + 1);
            n++;
            off += (u32)(len * step) - 1;
        }
    }
    return n;
}

void textscan_run(CPUState* cpu, const char* phrase) {
    printf("text: searching memory for \"%s\"\n", phrase);
    int n = scan(cpu->ram, cpu->ram_size, 0x80000000u, "RAM", phrase, 0);
    u32 vsize;
    const u8* vmem = mmio_vmem(&vsize);
    if (vmem) n = scan(vmem, vsize, 0x7E000000u, "VMEM", phrase, n);
    if (n > MAX_HITS) printf("text: %d more not shown\n", n - MAX_HITS);
    printf("text: %d copies of \"%s\" found\n", n, phrase);
}
