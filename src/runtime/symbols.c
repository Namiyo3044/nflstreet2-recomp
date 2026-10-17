#include "symbols.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t start;
    uint32_t size;
    char* name;
} Symbol;

static Symbol* g_syms;
static size_t g_count;

static int cmp(const void* a, const void* b) {
    uint32_t x = ((const Symbol*)a)->start, y = ((const Symbol*)b)->start;
    return x < y ? -1 : x > y;
}

bool symbols_load(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f)
        return false;

    size_t cap = 0;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        unsigned int start, size;
        char name[400];
        if (sscanf(line, "%x %x %399s", &start, &size, name) != 3)
            continue;
        if (g_count == cap) {
            cap = cap ? cap * 2 : 16384;
            g_syms = (Symbol*)realloc(g_syms, cap * sizeof(Symbol));
            if (!g_syms) {
                fclose(f);
                return false;
            }
        }
        g_syms[g_count].start = start;
        g_syms[g_count].size = size;
        g_syms[g_count].name = (char*)malloc(strlen(name) + 1);
        strcpy(g_syms[g_count].name, name);
        g_count++;
    }
    fclose(f);
    qsort(g_syms, g_count, sizeof(Symbol), cmp);
    return g_count > 0;
}

static const Symbol* find(uint32_t addr) {
    size_t lo = 0, hi = g_count;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (g_syms[mid].start <= addr)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo == 0)
        return NULL;
    const Symbol* s = &g_syms[lo - 1];
    return (addr < s->start + s->size) ? s : NULL;
}

const char* symbols_lookup(uint32_t addr, uint32_t* offset) {
    const Symbol* s = find(addr);
    if (offset)
        *offset = s ? addr - s->start : 0;
    return s ? s->name : "?";
}

uint32_t symbols_function_start(uint32_t addr) {
    const Symbol* s = find(addr);
    return s ? s->start : 0;
}
