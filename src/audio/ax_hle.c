#include "runtime/prof.h"
#include "audio/ax_hle.h"
#include "runtime/mmio.h"
#include "runtime/interrupts.h"

#include <stdio.h>
#include <string.h>

/* AXPB field offsets (bytes), from the SDK's AXVPB.h (decomp include/dolphin/ax). */
#define PB_NEXT_HI      0x00
#define PB_SRC_SELECT   0x08   /* 0 polyphase (4-tap), 1 linear, 2 none */
#define PB_COEF_SELECT  0x0A
#define PB_MIXER_CTRL   0x0C
#define PB_STATE        0x0E
#define PB_TYPE         0x10   /* 1 = stream: don't reload ADPCM state at the loop */
#define PB_MIX          0x12   /* vL, dL, vR, dR, auxAL, ..., vS, dS, ... (u16 each) */
#define PB_VE_VOL       0x64   /* volume envelope: current volume, delta per sample */
#define PB_VE_DELTA     0x66
#define PB_ADDR_LOOP    0x6E   /* loopFlag */
#define PB_ADDR_FORMAT  0x70
#define PB_ADDR_LOOPA   0x72   /* loopAddressHi, Lo */
#define PB_ADDR_END     0x76   /* endAddressHi, Lo */
#define PB_ADDR_CUR     0x7A   /* currentAddressHi, Lo */
#define PB_ADPCM_COEF   0x7E   /* a[8][2], s16 */
#define PB_ADPCM_PS     0xA0   /* adpcm.pred_scale */
#define PB_ADPCM_YN1    0xA2
#define PB_ADPCM_YN2    0xA4
#define PB_SRC_RATIO    0xA6   /* src.ratioHi, Lo (16.16 fixed point) */
#define PB_SRC_FRAC     0xAA   /* src.currentAddressFrac */
#define PB_SRC_LAST     0xAC   /* src.last_samples[4] */
#define PB_LOOP_PS      0xB4   /* adpcmLoop.loop_pred_scale */
#define PB_LOOP_YN1     0xB6
#define PB_LOOP_YN2     0xB8
#define PB_LPF_ON       0xBA   /* low-pass filter: on, yn1, a0, b0 */
#define PB_LPF_YN1      0xBC
#define PB_LPF_A0       0xBE
#define PB_LPF_B0       0xC0
#define PB_SIZE         0xC2

#define AX_STATE_RUN  1
#define FMT_ADPCM     0x00
#define FMT_PCM16     0x0A
#define FMT_PCM8      0x19

#define SAMPLES_PER_FRAME 160   /* 5 ms at 32 kHz */

static unsigned long long g_lists, g_voice_frames, g_voices_ended, g_frames_output;
/* Voice statistics: frames mixed per format, streams, silent voices, aux sends. */
static unsigned long long g_fmt_frames[3], g_stream_frames, g_silent_frames, g_aux_frames;
static u32 g_max_voices, g_last_output_addr, g_end_lines;

/* The frame being mixed: main left / right, 32-bit accumulators. */
static s32 g_left[SAMPLES_PER_FRAME], g_right[SAMPLES_PER_FRAME];

/* Effect buses A and B: left, right, surround, filled by the voices' aux
   sends. The game's aux callbacks get these, process them (reverb, or the
   game's own software mixer adding its voices) and hand back a buffer the
   DSP adds to the main mix. */
static s32 g_aux[2][3][SAMPLES_PER_FRAME];
static unsigned long long g_aux_returns[2], g_aux_nonzero[2];

static u8* ram_ptr(CPUState* cpu, u32 addr, u32 size) {
    u32 p = addr & 0x01FFFFFFu;
    return (p + size <= cpu->ram_size) ? cpu->ram + p : NULL;
}

static u32 rd16(const u8* p) { return ((u32)p[0] << 8) | p[1]; }
static s16 rds16(const u8* p) { return (s16)rd16(p); }
static u32 rd32h(const u8* p) { return (rd16(p) << 16) | rd16(p + 2); }   /* hi/lo u16 pair */
static void wr16(u8* p, u32 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
static void wr32h(u8* p, u32 v) { wr16(p, v >> 16); wr16(p + 2, v & 0xFFFFu); }

static s16 clamp16(s32 v) { return (s16)(v < -32768 ? -32768 : v > 32767 ? 32767 : v); }

/* ------------------------------------------------------- sample source --- */
/* A voice reads its samples from audio RAM through the DSP's accelerator.
   Addresses count samples of the format: nibbles for ADPCM (each 8-byte
   frame starts with a predictor/scale byte, then 14 nibbles), bytes for
   PCM8, 16-bit words for PCM16. */
typedef struct {
    const u8* aram;
    u32 aram_size;
    u32 fmt;
    u32 cur, end, loop;
    bool looping, stream, stopped;
    u32 ps;                            /* ADPCM predictor/scale */
    s32 yn1, yn2;
    s16 coef[16];
    u32 loop_ps;
    s32 loop_yn1, loop_yn2;
} Source;

static u8 aram_byte(const Source* s, u32 a) {
    return a < s->aram_size ? s->aram[a] : 0;
}

static s32 next_sample(Source* s) {
    if (s->stopped)
        return 0;
    s32 out;
    switch (s->fmt) {
    case FMT_PCM16:
        out = (s16)(((u32)aram_byte(s, s->cur * 2) << 8) | aram_byte(s, s->cur * 2 + 1));
        break;
    case FMT_PCM8:
        out = (s32)(s8)aram_byte(s, s->cur) << 8;
        break;
    default: {                         /* ADPCM */
        if ((s->cur & 15u) == 0) {     /* frame header: new predictor and scale */
            s->ps = aram_byte(s, s->cur >> 1);
            s->cur += 2;
        }
        u8 b = aram_byte(s, s->cur >> 1);
        s32 nib = (s->cur & 1u) ? (b & 15) : (b >> 4);
        if (nib >= 8) nib -= 16;
        u32 idx = (s->ps >> 4) & 7u;
        s32 scale = 1 << (s->ps & 15u);
        s32 v = ((nib * scale) << 11) + 1024 + s->coef[idx * 2] * s->yn1 + s->coef[idx * 2 + 1] * s->yn2;
        out = clamp16(v >> 11);
        s->yn2 = s->yn1;
        s->yn1 = out;
        break;
    }
    }

    /* The end address is the last sample played; then loop or stop. */
    if (s->cur == s->end || (s->fmt == FMT_ADPCM && (s->cur & ~15u) > (s->end | 15u))) {
        if (s->looping) {
            s->cur = s->loop;
            if (s->fmt == FMT_ADPCM && !s->stream) {
                s->ps = s->loop_ps;
                s->yn1 = s->loop_yn1;
                s->yn2 = s->loop_yn2;
            }
        } else {
            s->stopped = true;
        }
    } else {
        s->cur++;
    }
    return out;
}

/* ---------------------------------------------------------------- voice --- */
static void mix_bus(u8* pb, u32 vol_off, const s32* in, s32* bus) {
    u32 vol = rd16(pb + vol_off);
    s32 delta = rds16(pb + vol_off + 2);
    if (vol == 0 && delta == 0)
        return;
    s32 v = (s32)vol;
    for (u32 i = 0; i < SAMPLES_PER_FRAME; i++) {
        bus[i] += (s32)(((s64)in[i] * v) >> 15);
        v += delta;
        if (v < 0) v = 0;
        if (v > 0xFFFF) v = 0xFFFF;
    }
    wr16(pb + vol_off, (u32)v);
}

static void process_voice(u8* pb) {
    if (rd16(pb + PB_STATE) != AX_STATE_RUN)
        return;
    g_voice_frames++;

    Source s;
    memset(&s, 0, sizeof(s));
    s.aram = mmio_aram(&s.aram_size);
    s.fmt = rd16(pb + PB_ADDR_FORMAT);
    s.looping = rd16(pb + PB_ADDR_LOOP) != 0;
    s.stream = rd16(pb + PB_TYPE) == 1;
    s.cur = rd32h(pb + PB_ADDR_CUR);
    s.end = rd32h(pb + PB_ADDR_END);
    s.loop = rd32h(pb + PB_ADDR_LOOPA);
    for (int i = 0; i < 16; i++) s.coef[i] = rds16(pb + PB_ADPCM_COEF + i * 2);
    s.ps = rd16(pb + PB_ADPCM_PS);
    s.yn1 = rds16(pb + PB_ADPCM_YN1);
    s.yn2 = rds16(pb + PB_ADPCM_YN2);
    s.loop_ps = rd16(pb + PB_LOOP_PS);
    s.loop_yn1 = rds16(pb + PB_LOOP_YN1);
    s.loop_yn2 = rds16(pb + PB_LOOP_YN2);

    /* Resampling to 32 kHz: the position advances by the 16.16 ratio per
       output sample; h[] holds the last four source samples (h[3] newest).
       Polyphase is approximated by a 4-tap cubic, linear blends h[1]-h[2],
       and "none" plays at the source rate. */
    u32 src = rd16(pb + PB_SRC_SELECT);
    u32 ratio = src == 2 ? 0x10000u : rd32h(pb + PB_SRC_RATIO);
    if (ratio > 0x40000u) ratio = 0x40000u;          /* sanity: at most 4x */
    u32 pos = rd16(pb + PB_SRC_FRAC);
    s32 h[4];
    for (int i = 0; i < 4; i++) h[i] = rds16(pb + PB_SRC_LAST + i * 2);

    s32 buf[SAMPLES_PER_FRAME];
    for (u32 i = 0; i < SAMPLES_PER_FRAME; i++) {
        float t = (float)(pos & 0xFFFFu) / 65536.0f;
        s32 v;
        if (src == 0) {
            float p0 = (float)h[0], p1 = (float)h[1], p2 = (float)h[2], p3 = (float)h[3];
            float a = -0.5f * p0 + 1.5f * p1 - 1.5f * p2 + 0.5f * p3;
            float b = p0 - 2.5f * p1 + 2.0f * p2 - 0.5f * p3;
            float c = -0.5f * p0 + 0.5f * p2;
            v = (s32)(((a * t + b) * t + c) * t + p1);
        } else if (src == 1) {
            v = h[1] + (s32)((float)(h[2] - h[1]) * t);
        } else {
            v = h[1];
        }
        buf[i] = clamp16(v);
        pos += ratio;
        while (pos >= 0x10000u) {
            pos -= 0x10000u;
            h[0] = h[1]; h[1] = h[2]; h[2] = h[3];
            h[3] = next_sample(&s);
        }
    }

    /* Volume envelope: 1.15 fixed point, delta added every sample. */
    s32 vol = (s32)rd16(pb + PB_VE_VOL), vdelta = rds16(pb + PB_VE_DELTA);
    for (u32 i = 0; i < SAMPLES_PER_FRAME; i++) {
        buf[i] = (s32)(((s64)buf[i] * vol) >> 15);
        vol += vdelta;
        if (vol < 0) vol = 0;
        if (vol > 0xFFFF) vol = 0xFFFF;
    }
    wr16(pb + PB_VE_VOL, (u32)vol);

    /* One-pole low-pass filter. */
    if (rd16(pb + PB_LPF_ON)) {
        s32 y = rds16(pb + PB_LPF_YN1), a0 = (s32)rd16(pb + PB_LPF_A0), b0 = (s32)rd16(pb + PB_LPF_B0);
        for (u32 i = 0; i < SAMPLES_PER_FRAME; i++) {
            y = clamp16((a0 * buf[i] + b0 * y) >> 15);
            buf[i] = y;
        }
        wr16(pb + PB_LPF_YN1, (u32)(u16)y);
    }

    /* Main mix: left and right (surround is folded into both at half level). */
    if (s.fmt == FMT_PCM16) g_fmt_frames[1]++;
    else if (s.fmt == FMT_PCM8) g_fmt_frames[2]++;
    else g_fmt_frames[0]++;
    if (s.stream) g_stream_frames++;
    if (!rd16(pb + PB_MIX) && !rd16(pb + PB_MIX + 4) && !rd16(pb + PB_MIX + 0x1C)) g_silent_frames++;
    if (rd16(pb + PB_MIX + 0x08) | rd16(pb + PB_MIX + 0x0C) | rd16(pb + PB_MIX + 0x10) | rd16(pb + PB_MIX + 0x14))
        g_aux_frames++;
    mix_bus(pb, PB_MIX + 0x08, buf, g_aux[0][0]);
    mix_bus(pb, PB_MIX + 0x0C, buf, g_aux[0][1]);
    mix_bus(pb, PB_MIX + 0x20, buf, g_aux[0][2]);
    mix_bus(pb, PB_MIX + 0x10, buf, g_aux[1][0]);
    mix_bus(pb, PB_MIX + 0x14, buf, g_aux[1][1]);
    mix_bus(pb, PB_MIX + 0x18, buf, g_aux[1][2]);
    mix_bus(pb, PB_MIX + 0x00, buf, g_left);
    mix_bus(pb, PB_MIX + 0x04, buf, g_right);
    if (rd16(pb + PB_MIX + 0x1C)) {
        s32 half[SAMPLES_PER_FRAME];
        for (u32 i = 0; i < SAMPLES_PER_FRAME; i++) half[i] = buf[i] / 2;
        s32 tmp[SAMPLES_PER_FRAME] = {0};
        mix_bus(pb, PB_MIX + 0x1C, half, tmp);
        for (u32 i = 0; i < SAMPLES_PER_FRAME; i++) { g_left[i] += tmp[i]; g_right[i] += tmp[i]; }
    }

    /* Write the voice state back, as the DSP does. */
    wr16(pb + PB_SRC_FRAC, pos & 0xFFFFu);
    for (int i = 0; i < 4; i++) wr16(pb + PB_SRC_LAST + i * 2, (u32)(u16)clamp16(h[i]));
    wr32h(pb + PB_ADDR_CUR, s.cur);
    wr16(pb + PB_ADPCM_PS, s.ps);
    wr16(pb + PB_ADPCM_YN1, (u32)(u16)s.yn1);
    wr16(pb + PB_ADPCM_YN2, (u32)(u16)s.yn2);
    if (s.stopped) {
        wr16(pb + PB_STATE, 0);
        g_voices_ended++;
        if (s.fmt == FMT_PCM16 && g_end_lines < 40) {
            printf("  ax: %7.2f s  PCM16 voice ended (loop flag %u, cur 0x%08X end 0x%08X loop 0x%08X)\n",
                   interrupts_game_seconds(), s.looping, s.cur, s.end, s.loop);
            g_end_lines++;
        }
    }
    if (s.looping && !s.stopped && s.cur > s.end + 2 && g_end_lines < 40) {
        printf("  ax: %7.2f s  voice ran past its end (fmt 0x%X cur 0x%08X end 0x%08X loop 0x%08X)\n",
               interrupts_game_seconds(), s.fmt, s.cur, s.end, s.loop);
        g_end_lines++;
    }
}

static void process_pb_list(CPUState* cpu, u32 addr) {
    u32 count = 0;
    while (addr && count < 128) {          /* AX has at most 64 voices */
        u8* pb = ram_ptr(cpu, addr, PB_SIZE);
        if (!pb)
            break;
        process_voice(pb);
        addr = rd32h(pb + PB_NEXT_HI);
        count++;
    }
    if (count > g_max_voices)
        g_max_voices = count;
}

static s32 rd32(const u8* p) { return (s32)(((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]); }
static void wr32(u8* p, s32 v) { u32 u = (u32)v; p[0] = (u8)(u >> 24); p[1] = (u8)(u >> 16); p[2] = (u8)(u >> 8); p[3] = (u8)u; }

/* Aux bus exchange: the bus's buffers (planes channels of 160 32-bit samples)
   go to the CPU at up_addr, and the CPU's processed buffers from down_addr
   (the previous frame's, as the SDK rotates three buffers) are added to the
   main mix. A third plane (surround) is folded into left and right. */
static void aux_exchange(CPUState* cpu, int bus, u32 up_addr, u32 down_addr, int planes) {
    if (up_addr) {
        u8* up = ram_ptr(cpu, up_addr, planes * SAMPLES_PER_FRAME * 4);
        if (up)
            for (int c = 0; c < planes; c++)
                for (u32 i = 0; i < SAMPLES_PER_FRAME; i++)
                    wr32(up + (c * SAMPLES_PER_FRAME + i) * 4, g_aux[bus][c][i]);
    }
    if (down_addr) {
        u8* down = ram_ptr(cpu, down_addr, planes * SAMPLES_PER_FRAME * 4);
        if (down) {
            bool nonzero = false;
            for (u32 i = 0; i < SAMPLES_PER_FRAME; i++) {
                s32 l = rd32(down + i * 4), r = rd32(down + (SAMPLES_PER_FRAME + i) * 4);
                s32 sur = planes > 2 ? rd32(down + (2 * SAMPLES_PER_FRAME + i) * 4) / 2 : 0;
                g_left[i] += l + sur;
                g_right[i] += r + sur;
                nonzero |= (l | r | sur) != 0;
            }
            g_aux_returns[bus]++;
            if (nonzero) g_aux_nonzero[bus]++;
        }
    }
}

/* Writes the mixed frame as big-endian 16-bit stereo, right then left (the
   order the audio interface reads), to the buffer the audio DMA plays. */
static void output_frame(CPUState* cpu, u32 lr_addr) {
    u8* out = ram_ptr(cpu, lr_addr, SAMPLES_PER_FRAME * 4);
    if (!out)
        return;
    for (u32 i = 0; i < SAMPLES_PER_FRAME; i++) {
        wr16(out + i * 4, (u32)(u16)clamp16(g_right[i]));
        wr16(out + i * 4 + 2, (u32)(u16)clamp16(g_left[i]));
    }
    g_last_output_addr = lr_addr;
    g_frames_output++;
}

static void command_list(CPUState* cpu, u32 address) {
    g_lists++;
    memset(g_left, 0, sizeof(g_left));
    memset(g_right, 0, sizeof(g_right));
    memset(g_aux, 0, sizeof(g_aux));
    u32 pb_list = 0;
    bool mixed = false;
    for (u32 n = 0; n < 192; n++) {        /* a list is at most 384 u16 words */
        u8* p = ram_ptr(cpu, address, 2);
        if (!p)
            return;
        u32 op = rd16(p);
        switch (op) {
        case 0x00:                          /* studio (main mix) buffer */
        case 0x07:                          /* surround buffer */
        case 0x11:                          /* Dolby Pro Logic II buffer */
            address += 2 + 4;
            break;
        case 0x02: {                        /* voice list */
            u8* a = ram_ptr(cpu, address + 2, 4);
            if (a)
                pb_list = rd32h(a);
            address += 2 + 4;
            break;
        }
        case 0x03:                          /* process voices */
            if (pb_list && !mixed) {
                process_pb_list(cpu, pb_list);
                mixed = true;
            }
            address += 2;
            break;
        case 0x04:                          /* aux A: send L/R/S, return added to the mix */
        case 0x05:                          /* aux B */
        case 0x10: {                        /* aux B (Dolby Pro Logic II mode): left and right only */
            u8* a = ram_ptr(cpu, address + 2, 8);
            if (pb_list && !mixed) {
                process_pb_list(cpu, pb_list);
                mixed = true;
            }
            if (a)
                aux_exchange(cpu, op == 0x04 ? 0 : 1, rd32h(a), rd32h(a + 4), op == 0x10 ? 2 : 3);
            address += 2 + 8;
            break;
        }
        case 0x12:                          /* compressor */
            address += 2 + 8;
            break;
        case 0x0E: {                        /* output: surround address, then left/right address */
            u8* a = ram_ptr(cpu, address + 2, 8);
            if (pb_list && !mixed) {
                process_pb_list(cpu, pb_list);
                mixed = true;
            }
            if (a)
                output_frame(cpu, rd32h(a + 4));
            address += 2 + 8;
            break;
        }
        case 0x13: {                        /* aux A (DPL2): input, input 2, out L, R, Ls, Rs */
            u8* a = ram_ptr(cpu, address + 2, 24);
            if (pb_list && !mixed) {
                process_pb_list(cpu, pb_list);
                mixed = true;
            }
            if (a) {
                aux_exchange(cpu, 0, rd32h(a), 0, 3);
                /* out L and out R are consecutive planes; add them to the mix */
                aux_exchange(cpu, 0, 0, rd32h(a + 8), 2);
            }
            address += 2 + 24;
            break;
        }
        case 0x0F:                          /* end of list */
            if (pb_list && !mixed)
                process_pb_list(cpu, pb_list);
            return;
        default:
            return;                         /* unknown: stop rather than misparse */
        }
    }
}

void ax_hle_command_list(CPUState* cpu, u32 address) {
    int prev = prof_enter(PROF_AUDIO);
    command_list(cpu, address);
    prof_leave(prev);
}

void ax_hle_report(void) {
    printf("ax aux: bus A %llu returns (%llu with sound), bus B %llu returns (%llu with sound)\n",
           g_aux_returns[0], g_aux_nonzero[0], g_aux_returns[1], g_aux_nonzero[1]);
    printf("ax voices: frames mixed ADPCM %llu, PCM16 %llu, PCM8 %llu; streaming %llu; no main volume %llu; "
           "aux send %llu\n", g_fmt_frames[0], g_fmt_frames[1], g_fmt_frames[2], g_stream_frames, g_silent_frames,
           g_aux_frames);
    printf("ax: %llu command lists, %llu voice-frames mixed, %llu sounds ended, up to %u voices listed, "
           "%llu frames output (last to 0x%08X)\n",
           g_lists, g_voice_frames, g_voices_ended, g_max_voices, g_frames_output, g_last_output_addr);
}
