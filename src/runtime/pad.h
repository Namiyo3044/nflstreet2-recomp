#ifndef NFLS2_PAD_H
#define NFLS2_PAD_H

#include <stdbool.h>
#include <stdint.h>

/* Controllers in ports 1-4. Input comes from the live window (keyboard and
 * gamepads, see pad_set_live) and, for port 1, from a script given on the
 * command line, e.g. --press 5.0:UP --press 5.5:A, which holds the named
 * buttons for a moment at that many seconds of game time. Both are combined.
 */
#define PAD_PORTS 4

/* Live controller state, in GameCube terms. */
typedef struct {
    uint16_t buttons;          /* PAD_* bits below */
    uint8_t stick_x, stick_y;  /* main stick, 0x80 = centre, up/right = larger */
    uint8_t cstick_x, cstick_y;
    uint8_t trigger_l, trigger_r;
} PadLive;

enum {
    PAD_LEFT = 1u << 0, PAD_RIGHT = 1u << 1, PAD_DOWN = 1u << 2, PAD_UP = 1u << 3,
    PAD_Z = 1u << 4, PAD_R = 1u << 5, PAD_L = 1u << 6,
    PAD_A = 1u << 8, PAD_B = 1u << 9, PAD_X = 1u << 10, PAD_Y = 1u << 11, PAD_START = 1u << 12,
};

/* Sets a port's live state (called by the window once per video field). */
void pad_set_live(int port, const PadLive* live);

/* A port's live state as the window last set it (netplay sends it). */
void pad_get_live(int port, PadLive* out);

/* Whether a controller is plugged into the port (at the start only port 1). */
void pad_set_connected(int port, bool connected);
bool pad_connected(int port);

/* Parses "SECONDS:BUTTON[+BUTTON...][@HOLD]" (e.g. "5.5:A", "12:START",
   "8:DOWN+B", "9:A@0.2"). HOLD is how long to hold, in seconds (default one
   video field). Returns false if the text isn't understood. */
bool pad_add_press(const char* spec);

/* The 8-byte standard-controller poll response for the current game time:
   buttons, main stick, C-stick, analog triggers. */
void pad_poll_response(int port, double game_seconds, uint8_t out[8]);

/* While on, port 1 taps START a few times a second (skips the intro movies). */
void pad_set_skip_taps(bool on);

/* Button names accepted by --press, for the usage text. */
const char* pad_button_names(void);

#endif
