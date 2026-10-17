#include "mmio.h"
#include "gx/fifo.h"
#include "runtime/disc.h"

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
static u32 g_last_read_addr, g_last_read_value;

/* ARAM: the GameCube's 16 MB auxiliary RAM, reached only by DMA. */
#define ARAM_SIZE (16u * 1024u * 1024u)
static u8* g_aram;

typedef struct {
    u32 base, size;
    const char* name;
    unsigned long long reads, writes;
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
    u32 ar = reg_get(DSP + 0x24, 4) & 0x03FFFFFFu;
    u32 cnt = reg_get(DSP + 0x28, 4);
    bool to_ram = (cnt & 0x80000000u) != 0;
    u32 len = cnt & 0x03FFFFE0u;
    if (ar + len > ARAM_SIZE) {
        /* Beyond the internal 16 MB is where expansion ARAM would sit. There
           is none, so the transfer does nothing; ARInit's size probe relies on
           this to conclude that no expansion is fitted. */
    } else if (mm + len > cpu->ram_size) {
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
    case 0x16: /* ARAM refresh/ready: bit 0 set means ARAM is usable */
        *out = reg16(off) | 1u;
        return true;
    default:
        return false;
    }
}

/* The DSP's side of the mail protocol. There is no DSP program running, so
   this answers the way the DSP ROM and a freshly started task would:
   - after reset the first boot runs the OS's tiny init program, which reports
     0x80544348; every later boot is the ROM, which reports 0x8071FEED;
   - __DSP_boot_task then sends the task's load parameters, ending with
     0x80F3D001 and the entry point; the task starts and reports 0xDCD10000
     ("task initialised") with a DSP interrupt, which runs the task's init
     callback in the game (for AX, the audio library). */
static bool g_dsp_booted_once;
static u32 g_dsp_cpu_mail_hi;
static u32 g_dsp_prev_mail;
static unsigned g_dsp_mail_log;

static void dsp_send_to_cpu(u32 mail, bool interrupt) {
    g_dsp_mail = mail;
    g_dsp_mail_valid = true;
    if (interrupt)
        g_dsp_csr |= DSP_CSR_DSPINT;
    if (g_dsp_mail_log < 64) {
        g_dsp_mail_log++;
        printf("  dsp mail -> cpu 0x%08X%s\n", mail, interrupt ? " (interrupt)" : "");
    }
}

static u32 g_dsp_ucode_addr, g_dsp_ucode_len;

/* Identifies the DSP program being started, so audio can later be handled
   by a native implementation of that exact program (AX version). */
static void dsp_log_ucode(CPUState* cpu) {
    u32 phys = g_dsp_ucode_addr & 0x01FFFFFFu;
    if (!g_dsp_ucode_len || phys >= cpu->ram_size || g_dsp_ucode_len > cpu->ram_size - phys)
        return;
    u64 h = 0xCBF29CE484222325ull;               /* FNV-1a */
    for (u32 i = 0; i < g_dsp_ucode_len; i++)
        h = (h ^ cpu->ram[phys + i]) * 0x100000001B3ull;
    printf("  dsp: starting program from 0x%08X, %u bytes, fnv64 %016llX\n",
           g_dsp_ucode_addr, g_dsp_ucode_len, (unsigned long long)h);
}

static void dsp_receive_mail(CPUState* cpu, u32 mail) {
    if (g_dsp_mail_log < 64) {
        g_dsp_mail_log++;
        printf("  dsp mail <- cpu 0x%08X\n", mail);
    }
    if (g_dsp_prev_mail == 0x80F3A001u)
        g_dsp_ucode_addr = mail;
    else if (g_dsp_prev_mail == 0x80F3A002u)
        g_dsp_ucode_len = mail;
    if (g_dsp_prev_mail == 0x80F3D001u) {
        dsp_log_ucode(cpu);
        /* That was the task's entry point: the task is now running. */
        dsp_send_to_cpu(0xDCD10000u, true);
    }
    g_dsp_prev_mail = mail;
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
            dsp_send_to_cpu(g_dsp_booted_once ? 0x8071FEEDu : 0x80544348u, false);
            g_dsp_booted_once = true;
        }
        return true;
    }
    /* CPU -> DSP mailbox: the mail is complete once the low half is written. */
    if (r == 0x00 && size == 4) {
        reg_set(off, value, 4);
        dsp_receive_mail(cpu, value);
        return true;
    }
    if (r == 0x00 && size == 2) {
        reg_set(off, value, 2);
        g_dsp_cpu_mail_hi = value & 0xFFFFu;
        return true;
    }
    if (r == 0x02 && size == 2) {
        reg_set(off, value, 2);
        dsp_receive_mail(cpu, (g_dsp_cpu_mail_hi << 16) | (value & 0xFFFFu));
        return true;
    }
    reg_set(off, value, size);
    if ((r == 0x28 && size == 4) || (r == 0x2A && size == 2))
        aram_dma(cpu);
    return true;
}

/* ----------------------------------------------------------------- PE --- */
/* Pixel engine interrupts: "draw done" (finish) and tokens, raised by the
   graphics command stream (gx/fifo.c). */
#define PE_INTRCTRL 0x100Au
#define PE_TOKEN    0x100Eu
#define PE_TOKEN_EN  0x1u
#define PE_FINISH_EN 0x2u
#define PE_TOKEN_INT 0x4u
#define PE_FINISH_INT 0x8u

static u16 g_pe_ctrl;

void mmio_pe_finish(void) {
    g_pe_ctrl |= PE_FINISH_INT;
}

void mmio_pe_token(u16 token, bool interrupt) {
    set16(PE_TOKEN, token);
    if (interrupt)
        g_pe_ctrl |= PE_TOKEN_INT;
}

/* ----------------------------------------------------------------- DI --- */
/* Disc interface: commands complete instantly, reading from the virtual disc
   built from the extracted disc folder (runtime/disc.c). */
#define DI 0x6000u
#define DI_SR     (DI + 0x00)   /* status: masks and interrupt flags */
#define DI_CVR    (DI + 0x04)   /* cover */
#define DI_CMD0   (DI + 0x08)
#define DI_CMD1   (DI + 0x0C)
#define DI_CMD2   (DI + 0x10)
#define DI_MAR    (DI + 0x14)   /* DMA address in main RAM */
#define DI_LENGTH (DI + 0x18)
#define DI_CR     (DI + 0x1C)   /* control: start, DMA, read/write */
#define DI_IMMBUF (DI + 0x20)
#define DI_SR_DEINTMASK 0x02u
#define DI_SR_DEINT     0x04u
#define DI_SR_TCINTMASK 0x08u
#define DI_SR_TCINT     0x10u
#define DI_SR_BRKINTMASK 0x20u
#define DI_SR_BRKINT    0x40u

static u32 g_di_sr, g_di_cvr;
static unsigned g_di_logged;

static bool di_interrupt_pending(void) {
    return ((g_di_sr & DI_SR_TCINT) && (g_di_sr & DI_SR_TCINTMASK)) ||
           ((g_di_sr & DI_SR_DEINT) && (g_di_sr & DI_SR_DEINTMASK)) ||
           ((g_di_sr & DI_SR_BRKINT) && (g_di_sr & DI_SR_BRKINTMASK)) ||
           ((g_di_cvr & 0x4u) && (g_di_cvr & 0x2u));
}

static void di_execute(CPUState* cpu) {
    u32 cmd0 = reg_get(DI_CMD0, 4);
    u32 cmd = cmd0 >> 24;
    u32 cr = reg_get(DI_CR, 4);
    bool dma = (cr & 2u) != 0;
    u32 mar = reg_get(DI_MAR, 4) & 0x01FFFFE0u;
    u32 len = reg_get(DI_LENGTH, 4);

    switch (cmd) {
    case 0xA8: {   /* read: offset in 4-byte units, length in bytes */
        u64 offset = (u64)reg_get(DI_CMD1, 4) << 2;
        u32 n = reg_get(DI_CMD2, 4);
        if (g_di_logged < 64) {
            g_di_logged++;
            printf("  disc read 0x%09llX +0x%X -> 0x%08X\n", (unsigned long long)offset, n,
                   0x80000000u | mar);
        }
        if (dma) {
            if (mar + n <= cpu->ram_size)
                disc_read(offset, cpu->ram + mar, n);
            set16(DI_LENGTH, 0);
            reg_set(DI_LENGTH, 0, 4);
        } else {
            u8 word[4];
            disc_read(offset, word, 4);
            reg_set(DI_IMMBUF, ((u32)word[0] << 24) | ((u32)word[1] << 16) | ((u32)word[2] << 8) | word[3], 4);
        }
        break;
    }
    case 0x12: {   /* inquiry: drive information */
        if (dma && mar + 0x20 <= cpu->ram_size) {
            memset(cpu->ram + mar, 0, 0x20);
            cpu->ram[mar + 3] = 0x02;              /* revision */
            cpu->ram[mar + 4] = 0x20; cpu->ram[mar + 5] = 0x02;
            cpu->ram[mar + 6] = 0x04; cpu->ram[mar + 7] = 0x02;
            reg_set(DI_LENGTH, 0, 4);
        }
        break;
    }
    case 0xE0:     /* request error: no error */
        reg_set(DI_IMMBUF, 0, 4);
        break;
    case 0xAB:     /* seek */
    case 0xE1:     /* audio stream play */
    case 0xE2:     /* audio stream status */
    case 0xE3:     /* stop motor */
    case 0xE4:     /* audio buffer config */
        reg_set(DI_IMMBUF, 0, 4);
        break;
    default:
        if (g_di_logged < 64) {
            g_di_logged++;
            printf("  disc: unhandled command 0x%08X\n", cmd0);
        }
        reg_set(DI_IMMBUF, 0, 4);
        break;
    }
    (void)len;
    reg_set(DI_CR, cr & ~1u, 4);      /* transfer finished */
    g_di_sr |= DI_SR_TCINT;
}

static bool di_read(u32 off, u8 size, u32* out) {
    if (off == DI_SR && size == 4) { *out = g_di_sr; return true; }
    if (off == DI_CVR && size == 4) { *out = g_di_cvr; return true; }  /* cover closed */
    return false;
}

static bool di_write(CPUState* cpu, u32 off, u32 value, u8 size) {
    if (off == DI_SR && size == 4) {
        u32 masks = value & (DI_SR_DEINTMASK | DI_SR_TCINTMASK | DI_SR_BRKINTMASK);
        u32 flags = g_di_sr & (DI_SR_DEINT | DI_SR_TCINT | DI_SR_BRKINT);
        flags &= ~(value & (DI_SR_DEINT | DI_SR_TCINT | DI_SR_BRKINT));   /* write 1 to clear */
        g_di_sr = masks | flags;
        return true;
    }
    if (off == DI_CVR && size == 4) {
        u32 flags = (g_di_cvr & 0x4u) & ~(value & 0x4u);
        g_di_cvr = (value & 0x2u) | flags;
        return true;
    }
    if (off == DI_CR && size == 4) {
        reg_set(off, value, 4);
        if (value & 1u)
            di_execute(cpu);
        return true;
    }
    return false;
}

/* ----------------------------------------------------------------- AI --- */
/* Audio streaming interface. Only the sample counter matters for now: while
   streaming is on it advances at 32 or 48 kHz of game time, which AIInit
   times to work out the hardware's sample-rate behaviour. */
#define AI 0x6C00u
#define AI_PSTAT   0x01u   /* playing */
#define AI_AFR     0x02u   /* 1 = 48 kHz, 0 = 32 kHz */
#define AI_AIINT   0x08u   /* interrupt status, write 1 to clear */
#define AI_SCRESET 0x20u   /* write 1 to reset the sample counter */
#define TIMEBASE_HZ 40500000ull

static u64 g_ai_base_tb;
static u32 g_ai_base_count;

static u32 ai_sample_count(CPUState* cpu) {
    u32 cr = reg_get(AI, 4);
    if (!(cr & AI_PSTAT))
        return g_ai_base_count;
    u64 rate = (cr & AI_AFR) ? 48000ull : 32000ull;
    return g_ai_base_count + (u32)((cpu->timebase - g_ai_base_tb) * rate / TIMEBASE_HZ);
}

static void ai_write_control(CPUState* cpu, u32 value) {
    u32 count = ai_sample_count(cpu);   /* freeze the count at the old rate */
    u32 old = reg_get(AI, 4);
    if (value & AI_SCRESET)
        count = 0;
    u32 status = (old & AI_AIINT) & ~(value & AI_AIINT);
    reg_set(AI, (value & ~(AI_SCRESET | AI_AIINT)) | status, 4);
    g_ai_base_count = count;
    g_ai_base_tb = cpu->timebase;
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
    if (off == 0x3000u && size == 4)
        value = mmio_pi_cause();
    else if (off == AI + 0x08u && size == 4)
        value = ai_sample_count(cpu);
    else if (off >= DI && off < DI + 0x40 && di_read(off, size, &value))
        ;
    else if (off == 0x0000u && size == 2)
        value = 0x000Cu;                   /* CP status: FIFO read and command processing idle */
    else if (off == PE_INTRCTRL && size == 2)
        value = g_pe_ctrl;
    else if (!(off >= DSP && off < DSP + 0x200 && dsp_read(off, size, &value)))
        value = reg_get(off, size);
    g_last_read_addr = ea;
    g_last_read_value = value;
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

    if (d && strcmp(d->name, "WGPIPE") == 0) {
        gx_fifo_write(cpu, value, size);   /* graphics command stream */
        return;
    }
    log_access(true, ea, value, size);

    if (off >= DSP && off < DSP + 0x200) {
        dsp_write(cpu, off, value, size);
        return;
    }
    if (off >= 0x6800u && off < 0x6800u + 3 * 0x14u && exi_write(off, value, size))
        return;
    if (off == AI && size == 4) {
        ai_write_control(cpu, value);
        return;
    }
    if (off >= DI && off < DI + 0x40 && di_write(cpu, off, value, size))
        return;
    if (off == PE_INTRCTRL && size == 2) {
        /* Enable bits are stored; writing 1 to a status bit acknowledges it. */
        u16 v = (u16)value;
        g_pe_ctrl = (u16)((v & 0x3u) | (g_pe_ctrl & 0xCu & ~(v & 0xCu)));
        return;
    }
    reg_set(off, value, size);
}

/* ---------------------------------------------------------- interrupts --- */
/* PI interrupt cause (0xCC003000) is computed from the devices' own status
   bits rather than stored, as on hardware. */
#define PI_INT_DSP   0x00000040u
#define PI_INT_VI    0x00000100u
#define PI_INT_RSW   0x00010000u /* set = reset button NOT pressed */

static bool dsp_interrupt_pending(void) {
    u16 c = g_dsp_csr;
    return ((c & 0x0008u) && (c & 0x0010u)) ||  /* AI DMA */
           ((c & 0x0020u) && (c & 0x0040u)) ||  /* ARAM DMA */
           ((c & 0x0080u) && (c & 0x0100u));    /* DSP */
}

u32 mmio_pi_cause(void) {
    u32 cause = PI_INT_RSW;
    for (u32 i = 0; i < 4; i++) {
        u16 hi = reg16(0x2030u + i * 4u);
        if ((hi & 0x8000u) && (hi & 0x1000u))
            cause |= PI_INT_VI;
    }
    if (dsp_interrupt_pending())
        cause |= PI_INT_DSP;
    if (di_interrupt_pending())
        cause |= 0x00000004u;   /* DI */
    if ((g_pe_ctrl & PE_TOKEN_INT) && (g_pe_ctrl & PE_TOKEN_EN))
        cause |= 0x00000200u;   /* PE token */
    if ((g_pe_ctrl & PE_FINISH_INT) && (g_pe_ctrl & PE_FINISH_EN))
        cause |= 0x00000400u;   /* PE finish */
    return cause;
}

u32 mmio_pi_mask(void) {
    return reg_get(0x3004u, 4);
}

void mmio_vi_raise(u32 which) {
    u32 off = 0x2030u + (which & 3u) * 4u;
    u16 hi = reg16(off);
    if (hi & 0x1000u)              /* only if that display interrupt is enabled */
        set16(off, (u16)(hi | 0x8000u));
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
            printf(" %s(r%llu/w%llu)", g_devices[i].name, g_devices[i].reads, g_devices[i].writes);
    printf("\n");
    if (g_last_read_addr) {
        Device* d = device_for(g_last_read_addr);
        printf("last hardware register read: %s 0x%08X = 0x%X\n", d ? d->name : "?",
               g_last_read_addr, g_last_read_value);
    }
}
