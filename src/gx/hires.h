#ifndef NFLS2_GX_HIRES_H
#define NFLS2_GX_HIRES_H

#include "cpu/cpu.h"

/* Custom (replacement) textures, in Dolphin's format.
 *
 * Files are PNGs named the way Dolphin names them:
 *   tex1_<w>x<h>[_m]_<texture hash>[_<palette hash>]_<format>.png
 * with optional _mip1, _mip2 ... files for the smaller levels, and $ in
 * place of the palette hash to match any palette. The hashes are XXH64 of
 * the texture's bytes and of the part of its palette it uses, so packs made
 * for Dolphin (found in Load/Textures/GN7E69) work as they are. Subfolders
 * are searched too.
 *
 * Images are loaded in the background the first time their texture is
 * used; the game's own texture is drawn until then. Only the GPU renderer
 * uses them.
 */

#define GX_HIRES_MAX_LEVELS 14

typedef struct {
    u32 w, h, levels;                  /* levels: 1 = no mip files (the GPU makes its own) */
    u32 lw[GX_HIRES_MAX_LEVELS], lh[GX_HIRES_MAX_LEVELS], off[GX_HIRES_MAX_LEVELS];
    const u32* texels;                 /* 0xAARRGGBB, all levels one after another */
} GxHires;

typedef struct HiresEntry HiresEntry;

/* Scans dir (and its subfolders) for replacement files. */
void hires_init(const char* dir);

/* Saves each texture the game uses as a PNG with its Dolphin name. */
void hires_set_dump_dir(const char* dir);

bool hires_active(void);

/* Looks up the replacement for a texture, starting its load if needed.
   data / size: the texture's level 0 bytes; tlut: its palette in TMEM
   (paletted formats, else NULL). mipmaps: the sampler uses mip levels.
   decoded: level 0 as 0xAARRGGBB (for dumping; may be NULL). */
HiresEntry* hires_find(u32 fmt, u32 w, u32 h, bool mipmaps, const u8* data, u32 size, const u8* tlut,
                       const u32* decoded);

/* The image, once loaded (NULL until then, or if it failed). */
const GxHires* hires_ready(HiresEntry* e);

/* Dolphin's XXH64 (seed 0); exposed for tests. */
u64 hires_xxh64(const void* data, size_t len);

/* The Dolphin file name (without .png) for a texture; exposed for tests. */
void hires_name(char* out, size_t cap, u32 fmt, u32 w, u32 h, bool mipmaps, const u8* data, u32 size,
                const u8* tlut);

void hires_report(void);

#endif
