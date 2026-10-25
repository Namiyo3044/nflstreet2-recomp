#ifndef NFLS2_MMIO_H
#define NFLS2_MMIO_H

#include "cpu/cpu.h"

/* Hardware registers at 0xCC000000-0xCC00FFFF (VI, PI, DSP, DI, SI, EXI, AI,
   and the graphics write-gather pipe). Each device is modelled only as far as
   the game needs; everything else is a plain register file that remembers
   writes, and the first access to each register is logged. */
void mmio_install(CPUState* cpu);

void mmio_report(void);

/* Raw register value at offset `off` from 0xCC000000 (size 1, 2 or 4). */
u32 mmio_reg(u32 off, u8 size);

/* Interrupt plumbing used by interrupts.c. */
u32 mmio_pi_cause(void);         /* PI interrupt cause register, from device state */
u32 mmio_pi_mask(void);          /* PI interrupt mask register */
void mmio_vi_raise(u32 which);   /* assert VI display interrupt DI0-DI3 */

/* Pixel-engine events from the graphics command stream. */
void mmio_pe_finish(void);
void mmio_pe_token(u16 token, bool interrupt);

/* Frame pacing (see mmio.c): a display copy was made; a new video field began. */
void mmio_pe_display_copy(void);
void mmio_pe_field(void);
void mmio_set_frame_pacing(bool on);

/* The CPU wrote WPAR: reset the write-gather pipe. */
void mmio_wpar_written(void);

/* The disc file (and offset in it) that last filled this RAM offset, or NULL. */
const char* mmio_ram_source(u32 ram_offset, u64* file_offset);

/* The virtual memory window's contents (0x7E000000-0x7FFFFFFF). */
const u8* mmio_vmem(u32* size);

/* Timebase of the next timed hardware event (audio DMA), or ~0 if none. */
u64 mmio_next_event(void);

/* Locked-cache DMA (SPR DMAL trigger): bytes between main RAM and the locked
   cache at 0xE0000000; load = RAM to cache. */
void mmio_locked_cache_dma(CPUState* cpu, u32 mem_addr, u32 cache_addr, u32 bytes, bool load);

/* Controller polling, once per video field. */
void mmio_si_field(void);

/* Audio DMA timing: raises the AI DMA interrupt as output buffers finish. */
void mmio_ai_dma_tick(CPUState* cpu);
/* Reports a disc transfer finished once its time has come. */
void mmio_di_tick(CPUState* cpu);
/* Reports an ARAM transfer finished once its time has come. */
void mmio_dma_tick(CPUState* cpu);

/* Where finished audio buffers go: `frames` stereo samples, 16-bit
   big-endian, right channel first, at `rate` Hz. NULL (default) = nowhere. */
typedef void (*MmioAudioSink)(const u8* samples, u32 frames, u32 rate);
void mmio_set_audio_sink(MmioAudioSink sink);

/* Audio RAM (ARAM), for the audio mixer. */
const u8* mmio_aram(u32* size);

#endif
