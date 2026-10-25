#ifndef NFLS2_CREDITS_H
#define NFLS2_CREDITS_H

#include <stdint.h>

/* The port's credits, added at the start of the game's own credits roll. The
   roll reads its lines from Streamed.db each time it opens (the CDRL table:
   a header with the line count, then 72-byte lines and a check). The table
   can't hold more lines than the game's 411, so ours take the place of a
   few of its blank spacer lines, and the check is worked out again.
   Nothing on the disc changes. */

/* Called after each disc read with where it came from and where it went. */
void credits_on_disc_read(uint64_t disc_offset, uint8_t* dst, uint32_t n);

#endif
