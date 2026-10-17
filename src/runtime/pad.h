#ifndef NFLS2_PAD_H
#define NFLS2_PAD_H

#include <stdbool.h>
#include <stdint.h>

/* Controller in port 1. For now the input comes from a script given on the
 * command line, e.g. --press 5.0:UP --press 5.5:A, which holds the named
 * buttons for a moment at that many seconds of game time. A live window will
 * feed real keyboard/controller input through the same state later.
 */

/* Parses "SECONDS:BUTTON[+BUTTON...][@HOLD]" (e.g. "5.5:A", "12:START",
   "8:DOWN+B", "9:A@0.2"). HOLD is how long to hold, in seconds (default one
   video field). Returns false if the text isn't understood. */
bool pad_add_press(const char* spec);

/* The 8-byte standard-controller poll response for the current game time:
   buttons, main stick, C-stick, analog triggers. */
void pad_poll_response(double game_seconds, uint8_t out[8]);

/* Button names accepted by --press, for the usage text. */
const char* pad_button_names(void);

#endif
