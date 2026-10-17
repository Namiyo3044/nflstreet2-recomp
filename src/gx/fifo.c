#include "gx/fifo.h"
#include "gx/raster.h"
#include "gx/state.h"
#include "gx/texture.h"
#include "runtime/mmio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

u32 gx_cp[256];
u32 gx_bp[256] = {[0xFE] = 0x00FFFFFFu};  /* 0xFE: write mask, all bits by default */
u32 gx_xf_mem[0x800];
u32 gx_xf_reg[0x100];
s16 gx_tev_reg[4][4];
u8 gx_tev_konst[4][4];

/* Bytes received from the write-gather pipe that don't yet form a command. */
static u8* g_buf;
static size_t g_len, g_cap;

static struct {
    unsigned long long bytes, nops, cp, xf, indexed, bp, display_lists;
    unsigned long long draws, vertices, draw_done, tokens, copies_xfb, copies_tex;
    unsigned long long unknown;
} g_stats;
static unsigned g_unknown_logged;

/* ------------------------------------------------------- vertex sizes --- */
static u32 comp_size(u32 fmt) {
    switch (fmt) {
    case 0: case 1: return 1;   /* u8, s8 */
    case 2: case 3: return 2;   /* u16, s16 */
    default: return 4;          /* f32 */
    }
}

static u32 color_size(u32 fmt) {
    static const u32 sizes[8] = {2, 3, 4, 2, 3, 4, 4, 4}; /* 565, 888, 888x, 4444, 6666, 8888 */
    return sizes[fmt & 7];
}

static u32 index_size(u32 type) {
    return type == 2 ? 1u : type == 3 ? 2u : 0u;
}

/* Size in bytes of one vertex for vertex format (VAT) n, from the current
   vertex descriptor (which attributes are present, and how) and the VAT
   (component count and number format of each attribute). */
static u32 vertex_size(u32 vat) {
    u32 lo = gx_cp[0x50], hi = gx_cp[0x60];
    u32 a = gx_cp[0x70 + vat], b = gx_cp[0x80 + vat], c = gx_cp[0x90 + vat];
    u32 size = 0;

    for (u32 i = 0; i < 9; i++)          /* position/normal matrix index, 8 texture matrix indices */
        if ((lo >> i) & 1u)
            size += 1;

    u32 pos = (lo >> 9) & 3u;
    if (pos == 1)
        size += ((a & 1u) ? 3u : 2u) * comp_size((a >> 1) & 7u);
    else
        size += index_size(pos);

    u32 nrm = (lo >> 11) & 3u;
    if (nrm) {
        bool nbt = (a >> 9) & 1u;
        u32 fmt = (a >> 10) & 7u;
        bool index3 = (a >> 31) & 1u;
        if (nrm == 1)
            size += (nbt ? 9u : 3u) * comp_size(fmt);
        else
            size += (nbt && index3) ? 3u * index_size(nrm) : index_size(nrm);
    }

    u32 col0 = (lo >> 13) & 3u;
    size += col0 == 1 ? color_size((a >> 14) & 7u) : index_size(col0);
    u32 col1 = (lo >> 15) & 3u;
    size += col1 == 1 ? color_size((a >> 18) & 7u) : index_size(col1);

    /* Texture coordinate 0-7: (component-count bit, format) positions. */
    const u32 tc_cnt[8] = {(a >> 21) & 1u, b & 1u, (b >> 9) & 1u, (b >> 18) & 1u,
                           (b >> 27) & 1u, (c >> 5) & 1u, (c >> 14) & 1u, (c >> 23) & 1u};
    const u32 tc_fmt[8] = {(a >> 22) & 7u, (b >> 1) & 7u, (b >> 10) & 7u, (b >> 19) & 7u,
                           (b >> 28) & 7u, (c >> 6) & 7u, (c >> 15) & 7u, (c >> 24) & 7u};
    for (u32 i = 0; i < 8; i++) {
        u32 t = (hi >> (i * 2)) & 3u;
        if (t == 1)
            size += (tc_cnt[i] ? 2u : 1u) * comp_size(tc_fmt[i]);
        else
            size += index_size(t);
    }
    return size;
}

/* ------------------------------------------------------------ commands --- */
static u32 be32(const u8* p) {
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
}

static void write_bp(CPUState* cpu, u32 value) {
    u32 reg = value >> 24;
    u32 data = value & 0x00FFFFFFu;

    /* BP 0xFE is a write mask for the next BP write. */
    if (reg != 0xFE && gx_bp[0xFE] != 0x00FFFFFFu) {
        u32 mask = gx_bp[0xFE];
        data = (gx_bp[reg] & ~mask) | (data & mask);
        gx_bp[0xFE] = 0x00FFFFFFu;
    }
    gx_bp[reg] = data;
    g_stats.bp++;

    if (reg >= 0xE0 && reg <= 0xE7) {
        /* TEV colour registers: RA = red/alpha, BG = blue/green, 11 bits each. */
        u32 idx = (reg - 0xE0) / 2;
        bool ra = ((reg - 0xE0) & 1) == 0;
        bool konst = (data >> 23) & 1u;
        s32 lo = (s32)(data & 0x7FFu), hi = (s32)((data >> 12) & 0x7FFu);
        if (lo & 0x400) lo -= 0x800;
        if (hi & 0x400) hi -= 0x800;
        if (konst) {
            if (ra) { gx_tev_konst[idx][0] = (u8)lo; gx_tev_konst[idx][3] = (u8)hi; }
            else    { gx_tev_konst[idx][2] = (u8)lo; gx_tev_konst[idx][1] = (u8)hi; }
        } else {
            if (ra) { gx_tev_reg[idx][0] = (s16)lo; gx_tev_reg[idx][3] = (s16)hi; }
            else    { gx_tev_reg[idx][2] = (s16)lo; gx_tev_reg[idx][1] = (s16)hi; }
        }
    }

    switch (reg) {
    case 0x65: /* load a colour table (address in BP 0x64) into texture memory */
        gx_tex_load_tlut(cpu, gx_bp[0x64], data);
        break;
    case 0x45: /* PE_DONE: everything before this has been drawn */
        g_stats.draw_done++;
        mmio_pe_finish();
        break;
    case 0x47: /* PE_TOKEN */
        g_stats.tokens++;
        mmio_pe_token((u16)data, false);
        break;
    case 0x48: /* PE_TOKEN_INT */
        g_stats.tokens++;
        mmio_pe_token((u16)data, true);
        break;
    case 0x52: /* PE_COPY_EXECUTE: copy the frame (to the display buffer or a texture) */
        if (data & (1u << 14)) {
            g_stats.copies_xfb++;
            mmio_pe_display_copy();
        } else
            g_stats.copies_tex++;
        gx_raster_copy(data);
        break;
    default:
        break;
    }
}

static void log_unknown(const u8* p, size_t avail, bool in_dl) {
    g_stats.unknown++;
    if (g_unknown_logged >= 16)
        return;
    g_unknown_logged++;
    printf("  gx: unknown command 0x%02X%s, next bytes:", p[0], in_dl ? " in display list" : "");
    for (size_t i = 0; i < avail && i < 12; i++)
        printf(" %02X", p[i]);
    printf("\n");
}

/* LOAD_INDX_A-D: copy a matrix (or light) from attribute array 12-15 in RAM
   into transform memory. value = index << 16 | (count - 1) << 12 | address. */
static void load_indexed(CPUState* cpu, u32 array, u32 value) {
    u32 index = value >> 16;
    u32 count = ((value >> 12) & 0xFu) + 1u;
    u32 addr = value & 0xFFFu;
    u32 src = ((gx_cp[0xA0 + array] & 0x03FFFFFFu) + index * gx_cp[0xB0 + array]) & 0x01FFFFFFu;
    for (u32 i = 0; i < count; i++, addr++, src += 4) {
        if (src + 4 > cpu->ram_size || addr >= 0x800u)
            break;
        gx_xf_mem[addr] = be32(cpu->ram + src);
    }
}

static void run_display_list(CPUState* cpu, u32 addr, u32 size);

/* Decodes one command at p. Returns its length, or 0 if more bytes are
   needed to complete it. */
static size_t parse_one(CPUState* cpu, const u8* p, size_t avail, bool in_dl) {
    u8 op = p[0];

    if (op == 0x00) { g_stats.nops++; return 1; }
    if (op == 0x48 || op == 0x44) return 1;   /* invalidate vertex cache / unknown no-arg */

    if (op == 0x08) {                          /* LOAD_CP_REG: reg, value */
        if (avail < 6) return 0;
        gx_cp[p[1]] = be32(p + 2);
        g_stats.cp++;
        return 6;
    }
    if (op == 0x10) {                          /* LOAD_XF_REG: (count-1)<<16 | addr, values */
        if (avail < 5) return 0;
        u32 hdr = be32(p + 1);
        size_t n = ((hdr >> 16) & 0xFFFFu) + 1u;
        size_t len = 5 + n * 4;
        if (avail < len) return 0;
        u32 addr = hdr & 0xFFFFu;
        for (size_t i = 0; i < n; i++, addr++) {
            u32 v = be32(p + 5 + i * 4);
            if (addr < 0x800u)
                gx_xf_mem[addr] = v;
            else if (addr >= 0x1000u && addr < 0x1100u)
                gx_xf_reg[addr - 0x1000u] = v;
        }
        g_stats.xf++;
        return len;
    }
    if (op == 0x20 || op == 0x28 || op == 0x30 || op == 0x38) { /* LOAD_INDX_A-D */
        if (avail < 5) return 0;
        load_indexed(cpu, 12u + ((op >> 3) - 4u), be32(p + 1));
        g_stats.indexed++;
        return 5;
    }
    if (op == 0x40) {                          /* CALL_DL: address, size */
        if (avail < 9) return 0;
        if (!in_dl)
            run_display_list(cpu, be32(p + 1), be32(p + 5));
        return 9;
    }
    if (op == 0x61) {                          /* LOAD_BP_REG */
        if (avail < 5) return 0;
        write_bp(cpu, be32(p + 1));
        return 5;
    }
    if (op & 0x80) {                           /* draw: quads, triangles, strips, fans, lines, points */
        u8 prim = op & 0xF8;
        if (prim == 0x80 || prim == 0x88 || prim == 0x90 || prim == 0x98 ||
            prim == 0xA0 || prim == 0xA8 || prim == 0xB0 || prim == 0xB8) {
            if (avail < 3) return 0;
            u32 count = ((u32)p[1] << 8) | p[2];
            size_t len = 3 + (size_t)count * vertex_size(op & 7u);
            if (avail < len) return 0;
            g_stats.draws++;
            g_stats.vertices += count;
            gx_raster_draw(cpu, op, op & 7u, count, p + 3);
            return len;
        }
    }

    log_unknown(p, avail, in_dl);
    return 1;   /* skip a byte and try to resynchronise */
}

static void run_display_list(CPUState* cpu, u32 addr, u32 size) {
    u32 phys = addr & 0x01FFFFFFu;
    g_stats.display_lists++;
    if (phys >= cpu->ram_size || size > cpu->ram_size - phys) {
        printf("  gx: display list 0x%08X+0x%X is outside RAM\n", addr, size);
        return;
    }
    const u8* p = cpu->ram + phys;
    size_t off = 0;
    while (off < size) {
        size_t n = parse_one(cpu, p + off, size - off, true);
        if (n == 0)
            break;     /* truncated command at the end of the list */
        off += n;
    }
}

void gx_fifo_write(CPUState* cpu, u32 value, u8 size) {
    if (g_len + size > g_cap) {
        g_cap = g_cap ? g_cap * 2 : 65536;
        g_buf = (u8*)realloc(g_buf, g_cap);
        if (!g_buf) {
            fprintf(stderr, "gx: out of memory\n");
            exit(1);
        }
    }
    for (u8 i = 0; i < size; i++)
        g_buf[g_len++] = (u8)(value >> (8 * (size - 1 - i)));
    g_stats.bytes += size;

    size_t off = 0;
    while (off < g_len) {
        size_t n = parse_one(cpu, g_buf + off, g_len - off, false);
        if (n == 0)
            break;
        off += n;
    }
    if (off) {
        memmove(g_buf, g_buf + off, g_len - off);
        g_len -= off;
    }
}

void gx_fifo_report(void) {
    printf("gx: %llu bytes, %llu draws (%llu vertices), %llu display lists, "
           "%llu draw-done, %llu tokens, frame copies %llu (+%llu to textures), unknown %llu\n",
           g_stats.bytes, g_stats.draws, g_stats.vertices, g_stats.display_lists,
           g_stats.draw_done, g_stats.tokens, g_stats.copies_xfb, g_stats.copies_tex,
           g_stats.unknown);
}
