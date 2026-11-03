#include "pad.h"
#include "runtime/netsync.h"
#include "runtime/netplay.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* How long a scripted press is held: one video field, so the game sees it on
   exactly one controller update. Longer holds trigger menu auto-repeat. */
#define HOLD_SECONDS 0.017

/* Poll response layout (mode 3):
   byte 0: 0 0 0 START Y X B A      byte 1: 1 L R Z UP DOWN RIGHT LEFT
   bytes 2-3 main stick X/Y, 4-5 C-stick X/Y, 6-7 L/R analog. */
enum {
    BTN_A = 1u << 8, BTN_B = 1u << 9, BTN_X = 1u << 10, BTN_Y = 1u << 11, BTN_START = 1u << 12,
    BTN_LEFT = 1u << 0, BTN_RIGHT = 1u << 1, BTN_DOWN = 1u << 2, BTN_UP = 1u << 3,
    BTN_Z = 1u << 4, BTN_R = 1u << 5, BTN_L = 1u << 6,
    /* not real buttons: main-stick directions, pushed fully */
    STICK_LEFT = 1u << 16, STICK_RIGHT = 1u << 17, STICK_UP = 1u << 18, STICK_DOWN = 1u << 19,
};

static const struct { const char* name; uint32_t bits; } k_names[] = {
    {"A", BTN_A}, {"B", BTN_B}, {"X", BTN_X}, {"Y", BTN_Y}, {"START", BTN_START},
    {"UP", BTN_UP}, {"DOWN", BTN_DOWN}, {"LEFT", BTN_LEFT}, {"RIGHT", BTN_RIGHT},
    {"Z", BTN_Z}, {"L", BTN_L}, {"R", BTN_R},
    {"SUP", STICK_UP}, {"SDOWN", STICK_DOWN}, {"SLEFT", STICK_LEFT}, {"SRIGHT", STICK_RIGHT},
};

static PadLive g_lives[PAD_PORTS] = {{0, 0x80, 0x80, 0x80, 0x80, 0, 0}, {0, 0x80, 0x80, 0x80, 0x80, 0, 0},
                                      {0, 0x80, 0x80, 0x80, 0x80, 0, 0}, {0, 0x80, 0x80, 0x80, 0x80, 0, 0}};
static bool g_connected[PAD_PORTS] = {true, false, false, false};

void pad_set_live(int port, const PadLive* live) {
    if (port >= 0 && port < PAD_PORTS) g_lives[port] = *live;
}

void pad_set_connected(int port, bool connected) {
    if (port >= 0 && port < PAD_PORTS) g_connected[port] = connected;
}

bool pad_connected(int port) {
    bool net;
    if (netplay_connected(port, &net)) return net;   /* netplay: ports 1 and 2 */
    /* (what the game is told: recorded, or replaced in playback) */
    return netsync_connected(port, port >= 0 && port < PAD_PORTS && g_connected[port]);
}

static volatile bool g_rumble[PAD_PORTS];

void pad_set_rumble(int port, bool on) {
    if (port >= 0 && port < PAD_PORTS) g_rumble[port] = on;
}

bool pad_rumble(int port) { return port >= 0 && port < PAD_PORTS && g_rumble[port]; }

void pad_get_live(int port, PadLive* out) {
    *out = g_lives[port >= 0 && port < PAD_PORTS ? port : 0];
}

typedef struct { double at, hold; uint32_t bits; int state; } Press;
static Press g_presses[256];
static int g_count;

static bool same_name(const char* a, const char* b, size_t len) {
    for (size_t i = 0; i < len; i++) {
        char x = a[i], y = b[i];
        if (x >= 'a' && x <= 'z') x = (char)(x - 32);
        if (y >= 'a' && y <= 'z') y = (char)(y - 32);
        if (x != y)
            return false;
    }
    return true;
}

static uint32_t lookup(const char* name, size_t len) {
    for (size_t i = 0; i < sizeof(k_names) / sizeof(k_names[0]); i++)
        if (strlen(k_names[i].name) == len && same_name(k_names[i].name, name, len))
            return k_names[i].bits;
    return 0;
}

bool pad_add_press(const char* spec) {
    char* end = NULL;
    double at = strtod(spec, &end);
    if (!end || *end != ':' || at < 0 || g_count >= 256)
        return false;
    uint32_t bits = 0;
    const char* p = end + 1;
    double hold = HOLD_SECONDS;
    while (*p && *p != '@') {
        const char* q = p;
        while (*q && *q != '+' && *q != '@')
            q++;
        uint32_t b = lookup(p, (size_t)(q - p));
        if (!b)
            return false;
        bits |= b;
        p = *q == '+' ? q + 1 : q;
    }
    if (*p == '@') {
        hold = strtod(p + 1, &end);
        if (!end || *end || hold <= 0)
            return false;
    }
    if (!bits)
        return false;
    g_presses[g_count].at = at;
    g_presses[g_count].hold = hold;
    g_presses[g_count].bits = bits;
    g_presses[g_count].state = 0;
    g_count++;
    return true;
}

static bool g_skip_taps;
void pad_set_skip_taps(bool on) { g_skip_taps = on; }

void pad_poll_response(int port, double t, uint8_t out[8]) {
    uint32_t held = 0;
    /* START down for 0.1 s in every 0.25 s, so the game sees fresh presses. */
    if (g_skip_taps && port == 0 && t - 0.25 * (double)(long long)(t / 0.25) < 0.1)
        held |= PAD_START;
    PadLive g_live = g_lives[port >= 0 && port < PAD_PORTS ? port : 0];
    bool net = netplay_pad(port, &g_live);   /* netplay: both players' controllers, in step */
    for (int i = 0; i < g_count && port == 0 && !net; i++) {
        Press* pr = &g_presses[i];
        bool down = t >= pr->at && t < pr->at + pr->hold;
        if (down)
            held |= pr->bits;
        /* log each press once when the game first sees it and once when released */
        if (down && pr->state == 0) {
            pr->state = 1;
            printf("  pad: press %d down at %.3f s\n", i + 1, t);
        } else if (!down && pr->state == 1) {
            pr->state = 2;
            printf("  pad: press %d up at %.3f s\n", i + 1, t);
        }
    }

    uint16_t buttons = (uint16_t)((held & 0xFFFFu) | g_live.buttons);
    out[0] = (uint8_t)(buttons >> 8);
    out[1] = (uint8_t)(0x80u | (buttons & 0x7Fu));
    /* Scripted stick directions win over the live stick. */
    out[2] = held & STICK_LEFT ? 0x00 : held & STICK_RIGHT ? 0xFF : g_live.stick_x;
    out[3] = held & STICK_DOWN ? 0x00 : held & STICK_UP ? 0xFF : g_live.stick_y;
    out[4] = g_live.cstick_x;
    out[5] = g_live.cstick_y;
    /* analog triggers: scripted presses push them fully */
    out[6] = held & BTN_L ? 0xFF : g_live.trigger_l;
    out[7] = held & BTN_R ? 0xFF : g_live.trigger_r;
    netsync_poll(port, out);   /* recorded, or replaced in playback */
}

const char* pad_button_names(void) {
    return "A B X Y START UP DOWN LEFT RIGHT Z L R SUP SDOWN SLEFT SRIGHT";
}
