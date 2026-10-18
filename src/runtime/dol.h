#ifndef NFLS2_DOL_H
#define NFLS2_DOL_H

#include "cpu/cpu.h"

#define NFLS2_EXPECTED_DOL_SHA1 "3561e946e9ff785b68692f87d2ba85d81fd2dcf4"

/* Loads <disc_dir>/sys/main.dol into guest RAM, verifies its SHA-1 unless
   skip_hash is set, zeroes BSS and returns the entry point (0 on failure). */
u32 dol_load(CPUState* cpu, const char* disc_dir, bool skip_hash);

/* Data section i (0-10) of the loaded DOL; false if that section is empty. */
bool dol_data_section(int i, u32* addr, u32* len);

/* Fills the low-memory boot globals that the IPL normally leaves for the game
   (disc ID, memory size, console type, clock speeds). */
bool boot_globals_init(CPUState* cpu, const char* disc_dir);

/* Copies the disc's file table to the top of RAM and points the OS at it,
   as the apploader does. Call after disc_open. */
bool boot_place_fst(CPUState* cpu);

#endif
