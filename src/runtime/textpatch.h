#ifndef NFLS2_TEXTPATCH_H
#define NFLS2_TEXTPATCH_H
#include "cpu/cpu.h"

/* Rewrites the game's built-in text (from main.dol) using a list of
   "old => new" lines from a file, before the game starts. Each new text has
   to fit where the old one was stored; see text-replacements.txt. A missing
   file is fine (nothing changes). */
void textpatch_apply(CPUState* cpu, const char* path);

#endif
