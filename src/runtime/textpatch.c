#include "textpatch.h"
#include "dol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_RULES 256
#define MAX_TEXT 1024

typedef struct {
    char old[MAX_TEXT], rep[MAX_TEXT];
    int line;
    int applied, too_long;
    int fit_room;                   /* longest room seen when it didn't fit */
} Rule;

static Rule g_rules[MAX_RULES];
static int g_count;

/* Reads one side of a rule: trims spaces, removes surrounding quotes, turns
   \n into a line break. */
static void parse_text(char* dst, const char* src, size_t n) {
    while (n && (*src == ' ' || *src == '\t')) { src++; n--; }
    while (n && (src[n - 1] == ' ' || src[n - 1] == '\t' || src[n - 1] == '\r' || src[n - 1] == '\n')) n--;
    if (n >= 2 && src[0] == '"' && src[n - 1] == '"') { src++; n -= 2; }
    size_t o = 0;
    for (size_t i = 0; i < n && o < MAX_TEXT - 1; i++) {
        if (src[i] == '\\' && i + 1 < n && src[i + 1] == 'n') { dst[o++] = '\n'; i++; }
        else dst[o++] = src[i];
    }
    dst[o] = 0;
}

static bool load_rules(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return false;
    char line[2 * MAX_TEXT + 16];
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        n++;
        const char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\r' || *p == '\n' || !*p) continue;
        const char* arrow = strstr(p, "=>");
        if (!arrow) {
            printf("text: %s line %d has no \"=>\", skipped\n", path, n);
            continue;
        }
        if (g_count == MAX_RULES) break;
        Rule* r = &g_rules[g_count];
        memset(r, 0, sizeof *r);
        parse_text(r->old, p, (size_t)(arrow - p));
        parse_text(r->rep, arrow + 2, strlen(arrow + 2));
        r->line = n;
        if (!r->old[0]) continue;
        g_count++;
    }
    fclose(f);
    return true;
}

/* '|' and line breaks look the same in the log, so a rule written with one
   also matches the other. */
static void swap_breaks(char* dst, const char* src) {
    for (; *src; src++) *dst++ = *src == '|' ? '\n' : *src == '\n' ? '|' : *src;
    *dst = 0;
}

/* Replaces every `old` in `s` with `rep` into out; returns the count. */
static int replace_all(char* out, size_t cap, const char* s, const char* old, const char* rep) {
    size_t lo = strlen(old), lr = strlen(rep), o = 0;
    int count = 0;
    while (*s) {
        if (!strncmp(s, old, lo)) {
            if (o + lr >= cap) return -1;
            memcpy(out + o, rep, lr);
            o += lr;
            s += lo;
            count++;
        } else {
            if (o + 1 >= cap) return -1;
            out[o++] = *s++;
        }
    }
    out[o] = 0;
    return count;
}

/* One string in the DOL at ram[start..], `len` characters. */
static void patch_string(CPUState* cpu, u32 start, u32 len, u32 section_end) {
    /* Room: the string plus the zero padding after it, up to the next
       4-byte boundary (where the next string or value starts). */
    u32 room_end = start + len + 1;
    while (room_end < section_end && (room_end & 3) && cpu->ram[room_end] == 0) room_end++;
    u32 room = room_end - start - 1;

    char cur[MAX_TEXT * 2], tmp[MAX_TEXT * 2], old2[MAX_TEXT], rep2[MAX_TEXT];
    if (len >= sizeof cur) return;
    memcpy(cur, cpu->ram + start, len);
    cur[len] = 0;
    bool changed = false;
    bool done_old[MAX_RULES] = {false};

    for (int i = 0; i < g_count; i++) {
        Rule* r = &g_rules[i];
        if (done_old[i]) continue;          /* an earlier choice for this text already fit */
        for (int v = 0; v < 2; v++) {
            const char* o = r->old;
            const char* p = r->rep;
            if (v) {
                if (!strpbrk(r->old, "|\n")) break;
                swap_breaks(old2, r->old);
                swap_breaks(rep2, r->rep);
                o = old2;
                p = rep2;
            }
            if (!strstr(cur, o)) continue;
            int n = replace_all(tmp, sizeof tmp, cur, o, p);
            if (n <= 0) continue;
            if (strlen(tmp) > room) {
                r->too_long++;
                if (room > (u32)r->fit_room) r->fit_room = (int)room;
                break;
            }
            strcpy(cur, tmp);
            changed = true;
            r->applied++;
            /* later lines with the same old text are fallbacks: skip them */
            for (int j = i + 1; j < g_count; j++)
                if (!strcmp(g_rules[j].old, r->old)) done_old[j] = true;
            break;
        }
    }
    if (changed) {
        size_t n = strlen(cur);
        memcpy(cpu->ram + start, cur, n);
        memset(cpu->ram + start + n, 0, room + 1 - n);
    }
}

void textpatch_apply(CPUState* cpu, const char* path) {
    if (!path || !load_rules(path)) {
        if (path) printf("text: no %s, game text unchanged\n", path);
        return;
    }
    if (!g_count) {
        printf("text: %s has no replacements\n", path);
        return;
    }
    for (int s = 0; s < 11; s++) {
        u32 addr, len;
        if (!dol_data_section(s, &addr, &len)) continue;
        u32 base = addr - 0x80000000u, end = base + len;
        if (end > cpu->ram_size) continue;
        u32 i = base;
        while (i < end) {
            u32 j = i;
            while (j < end && cpu->ram[j]) j++;
            if (j > i && j < end) patch_string(cpu, i, j - i, end);
            i = j + 1;
        }
    }
    int used = 0;
    for (int i = 0; i < g_count; i++) {
        Rule* r = &g_rules[i];
        if (r->applied) {
            used++;
            printf("text: line %d: changed %d place%s\n", r->line, r->applied, r->applied == 1 ? "" : "s");
        }
        if (r->too_long) {
            bool next = false;
            for (int j = i + 1; j < g_count; j++)
                if (!strcmp(g_rules[j].old, r->old)) next = true;
            printf("text: line %d: too long in %d place%s (room there: %d characters for the whole message)%s\n",
                   r->line, r->too_long, r->too_long == 1 ? "" : "s", r->fit_room,
                   next ? ", the next line for the same text was tried there" : ", left unchanged there");
        }
        if (!r->applied && !r->too_long) {
            /* a fallback line is fine to go unused if its first choice fit */
            bool fallback = false;
            for (int j = 0; j < i; j++)
                if (!strcmp(g_rules[j].old, r->old) && g_rules[j].applied) fallback = true;
            if (!fallback) printf("text: line %d: text not found in the game\n", r->line);
        }
    }
    printf("text: %d of %d replacements from %s used\n", used, g_count, path);
}
