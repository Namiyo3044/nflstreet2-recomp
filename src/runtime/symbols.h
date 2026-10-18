#ifndef NFLS2_SYMBOLS_H
#define NFLS2_SYMBOLS_H

#include <stdbool.h>
#include <stdint.h>

/* Loads the "ADDRESS SIZE NAME" map written by tools/export_symbols.py. */
bool symbols_load(const char* path);

/* Name of the function containing addr ("?" if unknown); *offset gets addr - start. */
const char* symbols_lookup(uint32_t addr, uint32_t* offset);

/* Start address of the function containing addr, or 0. */
uint32_t symbols_function_start(uint32_t addr);

/* End address (exclusive) of the function containing addr, or 0. */
uint32_t symbols_function_end(uint32_t addr);

/* Start address of the function with this exact name, or 0. */
uint32_t symbols_address(const char* name);

/* Visits every symbol in address order. */
void symbols_each(void (*fn)(uint32_t start, const char* name, void* user), void* user);

#endif
