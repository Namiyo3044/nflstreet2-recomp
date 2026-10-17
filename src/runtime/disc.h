#ifndef NFLS2_DISC_H
#define NFLS2_DISC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A virtual GameCube disc built from an extracted disc folder (sys\ and
 * files\). The game reads the disc by byte offset; this maps every offset
 * back to the extracted file it came from, using the disc's own file table
 * (sys\fst.bin) and system area layout (sys\boot.bin).
 */
bool disc_open(const char* disc_dir);

/* Reads len bytes at disc offset into dst. Gaps between files read as zero.
   Returns false if any part fell outside the known disc contents. */
bool disc_read(uint64_t offset, void* dst, size_t len);

/* The file table, as the apploader would place it in RAM. */
const uint8_t* disc_fst(size_t* size);

void disc_report(void);

#endif
