#ifndef NFLS2_SHA1_H
#define NFLS2_SHA1_H

#include <stddef.h>
#include <stdint.h>

/* Writes the lowercase hex SHA-1 of data into out (40 chars + NUL). */
void sha1_hex(const void* data, size_t len, char out[41]);

#endif
