#include "mmio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MMIO_BASE 0xCC000000u
#define MMIO_SIZE 0x10000u

/* Plain register file: whatever was last written reads back. */
static u8 g_regs[MMIO_SIZE];
static u8 g_logged_read[MMIO_SIZE / 2];
static u8 g_logged_write[MMIO_SIZE / 2];
static bool g_log = true;

/* ARAM: the GameCube's 16 MB auxiliary RAM, reached only by DMA. */
#define ARAM_SIZE (16u * 1024u * 1024u)
static u8* g_aram;

typedef struct {
    u32 base, size;
    const char* name;
    u32 reads, writes;
} Device;

static Device g_devices[] = {
    {0xCC000000u, 0x1000, "CP", 0, 0},   /* command processor */
    {0xCC001000u, 0x1000, "PE", 0, 0},   /* pixel engine */
    {0xCC002000u, 0x1000, "VI", 0, 0},   /* video interface */
    {0xCC003000u, 0x1000, "PI", 0, 0},   /* processor interface / interrupts */
    {0xCC004000u, 0x1000, "MI", 0, 0},   /* memory interface */
    {0xCC005000u, 0x1000, "DSP", 0, 0},  /* DSP + ARAM DMA + audio DMA */
    {0xCC006000u, 0x0400, "DI", 0, 0},   /* disc interface */
    {0xCC006400u, 0x0400, "SI", 0, 0},   /* serial (controllers) */
    {0xCC006800u, 0x0400, "EXI", 0, 0},  /* memory cards, SRAM, RTC */
    {0xCC006C00u, 0x0400, "AI", 0, 0},   /* audio streaming */
    {0xCC008000u, 0x8000, "WGPIPE", 0, 0}, /* graphics FIFO write-gather pipe */
};

static Device* device_for(u32 addr) {
    for (size_t i = 0; i < sizeof(g_devices) / sizeof(g_devices[0]); i++)
        if (addr >= g_devices[i].base && addr < g_devices[i].base + g_devices[i].size)
            return &g_devices[i];
    return NULL;
}

static u32 reg_get(u32 off, u8 size) {
    u32 v = 0;
    for (u8 i = 0; i < size; i++)
        v = (v << 8) | g_regs[(off + i) & (MMIO_SIZE - 1)];
    return v;
}

static void reg_set(u32 off, u32 value, u8 size) {
    for (u8 i = 0; i < size; i++)
        g_regs[(off + i) & (MMIO_SIZE - 1)] = (u8)(value >> (8 * (size - 1 - i)));
}

static u16 reg16(u32 off) { return (u16)reg_get(off, 2); }
static void set16(u32 off, u16 v) { reg_set(off, v, 2); }

/* ---------------------------------------------------------------- DSP --- */
/* Only what the OS needs to bring the DSP up: reset/halt handshake, the boot
   mail it sends back, and instant ARAM DMA. Audio itself comes much later. */
#define DSP 0x5000u
#define DSP_CSR_RES     0x0001u
#define DSP_CSR_HALT    0x0004u
#define DSP_CSR_AIDINT  0x0008u
#define DSP_CSR_ARINT   0x0020u
#define DSP_CSR_DSPINT  0x0080u
#define DSP_CSR_W1C     (DSP_CSR_AIDINT | DSP_CSR_ARINT | DSP_CSR_DSPINT)

static u32 g_dsp_mail;
static bool g_dsp_mail_valid;
static u16 g_dsp_csr = DSP_CSR_HALT;

static void aram_dma(CPUState* cpu) {
    u32 mm = reg_get(DSP + 0x20, 4) & 0x01FFFFFFu;
    u32 ar = reg_get(DSP + 0x24, 4) & (ARAM_SIZE - 1);
    u32 cnt = reg_get(DSP + 0x28, 4);
    bool to_ram = (cnt & 0x80000000u) != 0;
    u32 len = cnt & 0x03FFFFE0u;
    if (mm + len > cpu->ram_size || ar + len > ARAM_SIZE) {
        fprintf(stderr, "mmio: ARAM DMA out of range (mm 0x%08X ar 0x%08X len 0x%X)\n", mm, ar, len);
    } else if (to_ram) {
        memcpy(cpu->ram + mm, g_aram + ar, len);
    } else {
        memcpy(g_aram + ar, cpu->ram + mm, len);
    }
    g_dsp_csr |= DSP_CSR_ARINT;
}

static bool dsp_read(u32 off, u8 size, u32* out) {
    u32 r = off - DSP;
    if (r == 0x04 && size == 4) {
        *out = (g_dsp_mail_valid ? 0x80000000u : 0u) | (g_dsp_mail & 0x7FFFFFFFu);
        g_dsp_mail_valid = false;
        return true;
    }
    switch (r) {
    case 0x00: /* CPU->DSP mail high: the DSP takes mail instantly */
        *out = reg16(off) & 0x7FFFu;
        return true;
    case 0x04: /* DSP->CPU mail high */
        *out = (g_dsp_mail_valid ? 0x8000u : 0u) | ((g_dsp_mail >> 16) & 0x7FFFu);
        return true;
    case 0x06: /* DSP->CPU mail low: reading it consumes the mail */
        *out = g_dsp_mail & 0xFFFFu;
        g_dsp_mail_valid = false;
        return true;
    case 0x0A: /* CSR: reset completes instantly, no DMA ever in progress */
        *out = g_dsp_csr & ~(DSP_CSR_RES | 0x0200u | 0x0400u);
        return true;
    default:
        return false;
    }
}

static bool dsp_write(CPUState* cpu, u32 off, u32 value, u8 size) {
    u32 r = off - DSP;
    if (r == 0x0A && size == 2) {
        u16 v = (u16)value;
        bool was_halted = (g_dsp_csr & DSP_CSR_HALT) != 0;
        u16 keep = (u16)(v & ~DSP_CSR_W1C);
        u16 ints = (u16)(g_dsp_csr & DSP_CSR_W1C & ~(v & DSP_CSR_W1C));
        g_dsp_csr = (u16)(keep | ints);
        if (was_halted && !(g_dsp_csr & DSP_CSR_HALT)) {
            /* The DSP ROM boots and reports in; the OS checks for this mail. */
            g_dsp_mail = 0x80544348u;
            g_dsp_mail_valid = true;
        }
        return true;
    }
    reg_set(off, value, size);
    if ((r == 0x28 && size == 4) || (r == 0x2A && size == 2))
        aram_dma(cpu);
    return true;
}

/* ---------------------------------------------------------------- EXI --- */
/* Transfers complete the moment they start; no devices answer yet, so SRAM
   and the RTC read as zero and memory cards are absent. */
static bool exi_write(u32 off, u32 value, u8 size) {
    u32 r = (off - 0x6800u) % 0x14u;
    u32 ch = (off - 0x6800u) / 0x14u;
    if (ch < 3 && r == 0x0C && (value & 1u)) {
        reg_set(off, value & ~1u, size);              /* TSTART clears */
        u32 csr = 0x6800u + ch * 0x14u;
        reg_set(csr, reg_get(csr, 4) | 0x8u, 4);      /* TCINT: transfer complete */
        return true;
    }
    return false;
}

/* --------------------------------------------------------------- glue --- */
static void log_access(bool write, u32 addr, u32 value, u8 size) {
    u32 slot = ((addr - MMIO_BASE) & (MMIO_SIZE - 1)) >> 1;
    u8* seen = write ? g_logged_write : g_logged_read;
    if (!g_log || seen[slot])
        return;
    seen[slot] = 1;
    Device* d = device_for(addr);
    printf("  mmio %s %-6s 0x%08X = 0x%0*X\n", write ? "W" : "R", d ? d->name : "?", addr,
           size * 2, value);
}

static u64 external_read(CPUState* cpu, u32 ea, u8 size) {
    (void)cpu;
    if ((ea & 0xFFFF0000u) != MMIO_BASE) {
        fprintf(stderr, "warn: read%u from unmapped 0x%08X (pc 0x%08X)\n", size * 8, ea, cpu->pc);
        return 0;
    }
    u32 off = ea - MMIO_BASE;
    Device* d = device_for(ea);
    if (d)
        d->reads++;

    u32 value;
    if (!(off >= DSP && off < DSP + 0x200 && dsp_read(off, size, &value)))
        value = reg_get(off, size);
    if (!(d && strcmp(d->name, "WGPIPE") == 0))
        log_access(false, ea, value, size);
    return value;
}

static void external_write(CPUState* cpu, u32 ea, u64 value64, u8 size) {
    u32 value = (u32)value64;
    if ((ea & 0xFFFF0000u) != MMIO_BASE) {
        fprintf(stderr, "warn: write%u to unmapped 0x%08X (pc 0x%08X)\n", size * 8, ea, cpu->pc);
        return;
    }
    u32 off = ea - MMIO_BASE;
    Device* d = device_for(ea);
    if (d)
        d->writes++;

    if (d && strcmp(d->name, "WGPIPE") == 0)
        return; /* graphics commands: ignored until the GX layer exists */
    log_access(true, ea, value, size);

    if (off >= DSP && off < DSP + 0x200) {
        dsp_write(cpu, off, value, size);
        return;
    }
    if (off >= 0x6800u && off < 0x6800u + 3 * 0x14u && exi_write(off, value, size))
        return;
    reg_set(off, value, size);
}

void mmio_install(CPUState* cpu) {
    memset(g_regs, 0, sizeof(g_regs));
    if (!g_aram)
        g_aram = (u8*)calloc(1, ARAM_SIZE);
    set16(DSP + 0x0A, DSP_CSR_HALT);
    cpu->external_read = external_read;
    cpu->external_write = external_write;
}

void mmio_report(void) {
    printf("hardware register accesses:");
    for (size_t i = 0; i < sizeof(g_devices) / sizeof(g_devices[0]); i++)
        if (g_devices[i].reads || g_devices[i].writes)
            printf(" %s(r%u/w%u)", g_devices[i].name, g_devices[i].reads, g_devices[i].writes);
    printf("\n");
}
