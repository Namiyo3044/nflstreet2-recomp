#include "runtime/netplay.h"
#include "platform/discord.h"
#include "platform/menu.h"
#include "platform/settings.h"
#include "runtime/uicolors.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "third_party/stb_truetype.h"
#include "third_party/font_anton.h"

typedef uint32_t u32;
typedef uint8_t u8;

/* --------------------------------------------------------------- state --- */
static bool g_open;
static int g_sel;
static unsigned g_changes;
static bool g_dirty = true;
static int g_px_owner;                 /* the picture holds: 0 the menu, 1 the netplay badge */
static MenuInfo g_info;
/* The controls page: rows are the GameCube inputs, then "reset" and "back";
   column 0 is the keyboard, 1 the controller. */
static bool g_controls;
static int g_crow, g_ccol, g_capture;
#define CROW_KBPLAYER GCI_COUNT
#define CROW_NINTENDO (GCI_COUNT + 1)
#define CROW_RESET (GCI_COUNT + 2)
#define CROW_BACK (GCI_COUNT + 3)
#define CROWS (GCI_COUNT + 4)
static char g_players[256];
static bool g_shape_locked;
static float g_aspect = 4.0f / 3.0f;   /* the window's shape now (for window sizes) */
static char g_gpu_name[48];            /* the graphics card's name ("": not known) */

/* Rows left off the page for now: Menu Colours (unfinished; ui-colors.txt
   still works). */
static bool item_hidden(int i) { return i == MI_COLORS; }

void menu_set_gpu_name(const char* name) {
    snprintf(g_gpu_name, sizeof g_gpu_name, "%s", name ? name : "");
    g_dirty = true;
}

void menu_set_aspect(float aspect) {
    if (aspect == g_aspect) return;
    g_aspect = aspect;
    g_dirty = true;
}

void menu_set_shape_locked(bool locked) {
    if (g_shape_locked == locked) return;
    g_shape_locked = locked;
    if (locked && g_sel == MI_SHAPE) g_sel = MI_DISPLAY;   /* off the item that can't change */
    g_dirty = true;
}
/* The menu colours page: rows are the colours, then "reset" and "back";
   g_kedit while changing red / green / blue (g_kch) of the selected one. */
static bool g_colors_page, g_kedit;
static int g_krow, g_kch;
/* The quit page: 0 = YES, 1 = NO. */
static bool g_quit_page, g_quit_yes;
static int g_qsel;
/* The Online page (the main menu's Online): host or join a netplay session.
   Hosting and joining go on while the menu is open (menu_tick); once both
   players are connected and their settings match, both games restart into
   the session (menu_take_restart). */
enum { OP_MAIN, OP_JOIN, OP_HOSTING, OP_CONNECTING, OP_FAILED, OP_READY, OP_SESSION, OP_ENDED };
static bool g_online;
static int g_opage, g_osel;
static char g_oaddr[64];
static char g_restart[160];
static bool g_restart_pending;
#define NET_PORT 7120

void menu_set_players_text(const char* text) {
    if (strcmp(g_players, text) != 0) {
        snprintf(g_players, sizeof(g_players), "%s", text);
        g_dirty = true;
    }
}
static const char* (*g_key_name)(int);
static const char* (*g_pad_name)(int);

void menu_set_binding_names(const char* (*key_name)(int), const char* (*pad_name)(int)) {
    g_key_name = key_name;
    g_pad_name = pad_name;
}

int menu_capturing(void) { return g_open && g_controls ? g_capture : 0; }

void menu_bind(int code) {
    if (!g_capture) return;
    if (code) {
        int* b = g_capture == 1 ? g_settings.key_bind : g_settings.pad_bind;
        b[g_crow] = code;
        g_changes |= 1u << MI_CONTROLS;
    }
    g_capture = 0;
    g_dirty = true;
}

static bool item_enabled(int i) {
    switch (i) {
    case MI_SHAPE: return !g_shape_locked;
    case MI_WINSIZE: return !g_settings.fullscreen;
    case MI_RES: return g_settings.renderer == 0;
    case MI_TEXTURES: return g_info.textures_found && g_settings.renderer == 0;
    default: return true;
    }
}

void menu_set_info(const MenuInfo* info) { g_info = *info; }

void menu_open(const MenuInfo* info) {
    g_info = *info;
    g_open = true;
    g_quit_page = false;
    g_online = false;
    g_sel = g_shape_locked ? MI_DISPLAY : 0;
    g_controls = false;
    g_colors_page = false;
    g_capture = 0;
    g_dirty = true;
}

void menu_open_quit(void) {
    g_open = true;
    g_online = false;
    g_quit_page = true;
    g_qsel = 1;                        /* NO to start with */
    g_controls = false;
    g_colors_page = false;
    g_capture = 0;
    g_dirty = true;
}

void menu_open_online(void) {
    g_open = true;
    g_online = true;
    g_quit_page = false;
    g_controls = false;
    g_colors_page = false;
    g_capture = 0;
    g_opage = netplay_active() ? OP_SESSION : OP_MAIN;
    g_osel = 0;
    if (g_settings.net_delay < 1) g_settings.net_delay = 3;
    snprintf(g_oaddr, sizeof g_oaddr, "%s", g_settings.net_address);
    g_dirty = true;
}

/* The session ended from the other side: a notice while this game restarts. */
void menu_open_online_ended(void) {
    menu_open_online();
    g_opage = OP_ENDED;
}

bool menu_text_wanted(void) { return g_open && g_online && g_opage == OP_JOIN; }

void menu_text(const char* t) {
    if (!menu_text_wanted()) return;
    size_t n = strlen(g_oaddr);
    for (; *t && n + 1 < sizeof g_oaddr; t++) {
        char c = *t;
        if ((c >= '0' && c <= '9') || c == '.' || c == ':' || c == '-' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
            g_oaddr[n++] = c;
    }
    g_oaddr[n] = 0;
    g_dirty = true;
}

void menu_text_back(void) {
    size_t n = strlen(g_oaddr);
    if (n) g_oaddr[n - 1] = 0;
    g_dirty = true;
}

static void online_restart(const char* args) {
    snprintf(g_restart, sizeof g_restart, "%s", args);
    g_restart_pending = true;
}

bool menu_take_restart(char* args, size_t cap) {
    if (!g_restart_pending) return false;
    g_restart_pending = false;
    snprintf(args, cap, "%s", g_restart);
    return true;
}

/* --------------------------------------------- Delete User ID row --- */
/* Save/Load's Delete User ID row (uismenu.c): the User IDs on the card in slot A, picked with
   left / right; A asks again (the value and the help line say so), and A
   once more deletes that User ID's file. The game then restarts at once, so
   its card library (which keeps the old directory in memory) writes nothing
   back. The GameCube deleted saves from its own memory card manager; the
   game has no screen for it. */
/* (runtime/exi.h; declared here to keep the CPU's types out of this file) */
int exi_card_list(int slot, char files[][33], int max);
bool exi_card_delete(int slot, const char* name);
#define CARD_PREFIX "GN7E-69UserID"   /* the game's User ID files: this, then the name */
#define CARD_MAX 32
static char g_card_file[CARD_MAX][33];
static int g_card_n;                   /* User IDs on the card */
static int g_card_sel;                 /* the one picked */
static bool g_card_armed;              /* asking "delete?" */
static int g_card_done_fields;         /* deleted: fields left before the restart (0: none) */
static char g_card_help[160];

static void card_scan(void) {
    char files[64][33];
    int n = exi_card_list(0, files, 64);
    size_t pl = strlen(CARD_PREFIX);
    g_card_n = 0;
    for (int i = 0; i < n && g_card_n < CARD_MAX; i++)
        if (strncmp(files[i], CARD_PREFIX, pl) == 0 && files[i][pl])
            snprintf(g_card_file[g_card_n++], sizeof g_card_file[0], "%s", files[i]);
    if (g_card_sel >= g_card_n) g_card_sel = 0;
}

static const char* card_name(int i) { return g_card_file[i] + strlen(CARD_PREFIX); }

static int card_count(void) { card_scan(); return g_card_n ? g_card_n : 1; }
static int card_get(void) { return g_card_n ? g_card_sel : 0; }
static void card_set(int v) {
    if (v != g_card_sel) g_card_armed = false;
    g_card_sel = v < 0 || v >= g_card_n ? 0 : v;
}
static bool card_enabled(void) { return g_card_n > 0 && !netplay_active() && !g_card_done_fields; }

static const char* card_value(int v) {
    if (!g_card_n) return "None";
    if (g_card_done_fields) return "Deleted";
    if (g_card_armed && v == g_card_sel) return "Delete?";
    return card_name(v < 0 || v >= g_card_n ? 0 : v);
}

static const char* card_help(void) {
    if (g_card_done_fields) return "Deleted. The game is restarting...";
    if (netplay_active()) return "Not during an online game.";
    if (!g_card_n) return "No User IDs on the Memory Card in Slot A.";
    if (g_card_armed) {
        snprintf(g_card_help, sizeof g_card_help, "Press A again to delete %s. Its saved data will be lost and the game restarts.",
                 card_name(g_card_sel));
        return g_card_help;
    }
    return "Pick a User ID with left / right, then press A to delete it.";
}

int menu_card_row_count(int row) { (void)row; return card_count(); }
int menu_card_row_get(int row) { (void)row; return card_get(); }
void menu_card_row_set(int row, int v) { (void)row; card_set(v); }
const char* menu_card_row_name(int row, int v) { (void)row; return card_value(v); }
const char* menu_card_row_help(int row) { (void)row; return card_help(); }
bool menu_card_row_enabled(int row) { (void)row; return card_enabled(); }

void menu_card_focus(bool on) {
    /* The cursor gone from the row for a moment (a few fields, not a blip
       while it animates): the question is dropped. */
    static int away;
    away = on ? 0 : away + 1;
    if (away >= 6) g_card_armed = false;
}

void menu_card_press(void) {
    if (!card_enabled()) return;
    if (!g_card_armed) { g_card_armed = true; return; }
    g_card_armed = false;
    if (!exi_card_delete(0, g_card_file[g_card_sel])) {
        printf("delete user id: couldn't delete %s\n", g_card_file[g_card_sel]);
        return;
    }
    g_card_done_fields = 2;            /* at once (see above) */
}

bool menu_card_tick(char* args, size_t cap) {
    if (g_card_done_fields > 0 && --g_card_done_fields == 0) {
        g_card_done_fields = 1;        /* (stays as it is while the game closes) */
        snprintf(args, cap, "%s", "");
        return true;
    }
    return false;
}

/* ------------------------------------- the game's own Online screen --- */
/* The main menu's Online opens the Audio Settings screen in video mode with
   these rows (like Recomp Settings): Host a Game and Join a Game switch
   with left / right (or A), the address is typed on the keyboard while Join
   a Game has the cursor, and the help line says how it's going. */
enum { OR_HOST, OR_JOIN, OR_DELAY, OR_ADDR };
const char* const k_online_label[MENU_ONLINE_ROWS] = {"Host a Game", "Join a Game", "Input Delay", "This PC's Address"};
enum { OS_IDLE, OS_HOSTING, OS_JOINING, OS_FAILED, OS_READY };
static int g_ostate;
static int g_ofocus = -1;              /* the row with the cursor (-1: screen not open) */
static int g_oready_fields;            /* READY: fields shown before the restart */
static char g_oaddrs[160];             /* this PC's addresses ("a   b") */
static int g_onaddr;
static char g_ovalue[96];
static bool g_ohosted;                 /* READY: this side hosted */
static int g_oguard;                   /* fields after a start / stop when another is ignored (A may also
                                          step the row's value in the game: one press, one switch) */

static void online_addresses(void) {
    static bool done;
    if (done) return;
    done = true;
    g_onaddr = netplay_local_addresses(g_oaddrs, sizeof g_oaddrs);
}

static void online_cancel(void) {
    if (g_ostate == OS_HOSTING || g_ostate == OS_JOINING || g_ostate == OS_FAILED) netplay_lobby_cancel();
    g_ostate = OS_IDLE;
}

static void online_host(bool on) {
    if (g_ostate == OS_READY) return;
    if (!on) { if (g_ostate == OS_HOSTING) online_cancel(); return; }
    online_cancel();
    g_ostate = netplay_lobby_host(NET_PORT, g_settings.net_delay) ? OS_HOSTING : OS_FAILED;
    printf("online: hosting %s\n", g_ostate == OS_HOSTING ? "(waiting for a player)" : "failed");
}

static void online_join(bool on) {
    if (g_ostate == OS_READY) return;
    if (!on) { if (g_ostate == OS_JOINING) online_cancel(); return; }
    if (!g_settings.net_address[0]) return;   /* nothing typed yet */
    online_cancel();
    g_changes |= 1u << MI_COUNT;       /* (the address is saved) */
    g_ostate = netplay_lobby_join(g_settings.net_address, g_settings.net_delay) ? OS_JOINING : OS_FAILED;
    printf("online: joining %s %s\n", g_settings.net_address, g_ostate == OS_JOINING ? "" : "failed");
}

int menu_online_row_count(int row) {
    switch (row) {
    case OR_DELAY: return 12;
    case OR_ADDR: return g_onaddr > 1 ? g_onaddr : 1;
    default: return 2;
    }
}

int menu_online_row_get(int row) {
    switch (row) {
    case OR_HOST: return g_ostate == OS_HOSTING || (g_ostate == OS_READY && g_ohosted) ? 1 : 0;
    case OR_JOIN: return g_ostate == OS_JOINING || (g_ostate == OS_READY && !g_ohosted) ? 1 : 0;
    case OR_DELAY: return (g_settings.net_delay < 1 ? 3 : g_settings.net_delay > 12 ? 12 : g_settings.net_delay) - 1;
    default: return 0;
    }
}

void menu_online_row_set(int row, int v) {
    switch (row) {
    case OR_HOST: if (!g_oguard) { g_oguard = 12; online_host(v != 0); } break;
    case OR_JOIN: if (!g_oguard) { g_oguard = 12; online_join(v != 0); } break;
    case OR_DELAY:
        g_settings.net_delay = v < 0 ? 1 : v > 11 ? 12 : v + 1;
        g_changes |= 1u << MI_COUNT;
        break;
    default: break;
    }
}

const char* menu_online_row_name(int row, int v) {
    switch (row) {
    case OR_HOST: return g_ostate == OS_READY && v ? "Player Found!" : v ? "Searching..." : "Press A";
    case OR_JOIN:
        if (v) snprintf(g_ovalue, sizeof g_ovalue, "%s", g_ostate == OS_READY ? "Player Found!" : "Connecting...");
        else if (g_settings.net_address[0])
            snprintf(g_ovalue, sizeof g_ovalue, "%s%s", g_settings.net_address, g_ofocus == OR_JOIN ? "_" : "");
        else snprintf(g_ovalue, sizeof g_ovalue, "%s", g_ofocus == OR_JOIN ? "Type It_" : "No Address");
        return g_ovalue;
    case OR_DELAY: {
        static char d[32];
        snprintf(d, sizeof d, "%d (%d ms)", v + 1, ((v + 1) * 1001 + 30) / 60);
        return d;
    }
    default: {
        online_addresses();
        if (!g_onaddr) return "Not Found";
        static char one[40];
        const char* p = g_oaddrs;
        for (int i = 0; i < v && p; i++) { p = strstr(p, "   "); if (p) p += 3; }
        if (!p) p = g_oaddrs;
        size_t n = strcspn(p, " ");
        if (n >= sizeof one) n = sizeof one - 1;
        memcpy(one, p, n);
        one[n] = 0;
        return one;
    }
    }
}

const char* menu_online_row_help(int row) {
    static char msg[200];
    if (g_ostate == OS_READY) return "Player found! Both games are restarting. Please wait...";
    if (g_ostate == OS_FAILED) {
        const char* m = NULL;
        int d;
        netplay_lobby_poll(&m, &d);
        snprintf(msg, sizeof msg, "%s", m && m[0] ? m : "Something went wrong.");
        return msg;
    }
    if (g_ostate == OS_HOSTING) return "Searching for a game... (waiting for a player to join)";
    if (g_ostate == OS_JOINING) return "Connecting... Please wait.";
    switch (row) {
    case OR_HOST: return "Host a match for a player on another PC to join.";
    case OR_JOIN: return "Type the host's address, then press Enter or A.";
    case OR_DELAY: return "Higher hides more lag. The host's delay is used.";
    default: return g_onaddr > 1 ? "Give one to the player joining (left / right: more)."
                                 : "Give this to the player joining you.";
    }
}

bool menu_online_row_enabled(int row) {
    if (g_ostate == OS_READY) return row == OR_HOST || row == OR_JOIN ? true : false;
    if (row == OR_HOST) return g_ostate != OS_JOINING;
    if (row == OR_JOIN) return g_ostate != OS_HOSTING;
    if (row == OR_DELAY) return g_ostate == OS_IDLE || g_ostate == OS_FAILED;
    return true;
}

void menu_online_focus(int row) {
    if (row != g_ofocus && row >= 0 && g_ostate == OS_FAILED) online_cancel();   /* moved on from the message */
    if (row < 0 && g_ofocus >= 0 && g_ostate != OS_READY) online_cancel();      /* screen left: stop */
    g_ofocus = row;
}

void menu_online_press(int row) {
    if (g_ostate == OS_READY || g_oguard) return;
    if (row == OR_HOST || row == OR_JOIN || g_ostate == OS_FAILED) g_oguard = 12;
    if (g_ostate == OS_FAILED) { online_cancel(); return; }
    if (row == OR_HOST) online_host(g_ostate != OS_HOSTING);
    else if (row == OR_JOIN) online_join(g_ostate != OS_JOINING);
}

bool menu_online_typing(void) {
    return g_ofocus == OR_JOIN && (g_ostate == OS_IDLE || g_ostate == OS_FAILED) && !g_open;
}

void menu_online_text(const char* t) {
    if (!menu_online_typing()) return;
    if (g_ostate == OS_FAILED) online_cancel();
    char* a = g_settings.net_address;
    size_t n = strlen(a), cap = sizeof g_settings.net_address;
    for (; *t && n + 1 < cap; t++) {
        char c = *t;
        if ((c >= '0' && c <= '9') || c == '.' || c == ':' || c == '-' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
            a[n++] = c;
    }
    a[n] = 0;
    g_changes |= 1u << MI_COUNT;
}

void menu_online_text_back(bool all) {
    if (!menu_online_typing()) return;
    if (g_ostate == OS_FAILED) online_cancel();
    size_t n = strlen(g_settings.net_address);
    if (all) g_settings.net_address[0] = 0;
    else if (n) g_settings.net_address[n - 1] = 0;
    g_changes |= 1u << MI_COUNT;
}

/* Every video field: the connection going on. True once the game should
   restart into the session (args: its options). */
bool menu_online_tick(char* args, size_t cap) {
    if (g_oguard > 0) g_oguard--;
    if (g_ostate == OS_READY) {
        if (g_oready_fields > 0 && --g_oready_fields == 0) {
            snprintf(args, cap, "%s", g_restart);
            return true;
        }
        return false;
    }
    if (g_ostate != OS_HOSTING && g_ostate != OS_JOINING) return false;
    const char* msg;
    int delay;
    int st = netplay_lobby_poll(&msg, &delay);
    if (st == NETPLAY_LOBBY_FAILED) {
        printf("online: %s\n", msg ? msg : "failed");
        g_ostate = OS_FAILED;
    } else if (st == NETPLAY_LOBBY_READY) {
        if (g_ostate == OS_HOSTING) snprintf(g_restart, sizeof g_restart, "--net-host %d --net-delay %d", NET_PORT, delay);
        else snprintf(g_restart, sizeof g_restart, "--net-join %s --net-delay %d", g_settings.net_address, delay);
        printf("online: player found, restarting with %s\n", g_restart);
        g_ohosted = g_ostate == OS_HOSTING;
        g_ostate = OS_READY;
        g_oready_fields = 45;          /* "player found" on screen a moment */
    }
    return false;
}

/* Each pass of the menu loop: hosting / joining going on. */
void menu_tick(void) {
    if (!g_open || !g_online || (g_opage != OP_HOSTING && g_opage != OP_CONNECTING)) return;
    const char* msg;
    int delay;
    int st = netplay_lobby_poll(&msg, &delay);
    if (st == NETPLAY_LOBBY_FAILED) {
        g_opage = OP_FAILED;
        g_dirty = true;
    } else if (st == NETPLAY_LOBBY_READY) {
        char a[160];
        if (g_opage == OP_HOSTING) snprintf(a, sizeof a, "--net-host %d --net-delay %d", NET_PORT, delay);
        else snprintf(a, sizeof a, "--net-join %s --net-delay %d", g_oaddr, delay);
        g_opage = OP_READY;
        g_dirty = true;
        online_restart(a);
    }
}

static const char k_addr_chars[] = "0123456789.";

static void online_action(int a) {
    switch (g_opage) {
    case OP_MAIN:
        if (a == MENU_UP) g_osel = (g_osel + 3) % 4;
        else if (a == MENU_DOWN) g_osel = (g_osel + 1) % 4;
        else if ((a == MENU_LEFT || a == MENU_RIGHT) && g_osel == 2) {
            int d = g_settings.net_delay + (a == MENU_LEFT ? -1 : 1);
            g_settings.net_delay = d < 1 ? 1 : d > 12 ? 12 : d;
            g_changes |= 1u << MI_COUNT;
        } else if (a == MENU_OK) {
            if (g_osel == 0) {
                if (netplay_lobby_host(NET_PORT, g_settings.net_delay)) g_opage = OP_HOSTING;
                else g_opage = OP_FAILED;
            } else if (g_osel == 1) {
                g_opage = OP_JOIN;
            } else if (g_osel == 3) {
                g_open = false;
            }
        } else if (a == MENU_BACK) {
            g_open = false;
        }
        break;
    case OP_JOIN: {
        size_t n = strlen(g_oaddr);
        if (a == MENU_OK && n) {
            snprintf(g_settings.net_address, sizeof g_settings.net_address, "%s", g_oaddr);
            g_changes |= 1u << MI_COUNT;
            if (netplay_lobby_join(g_oaddr, g_settings.net_delay)) g_opage = OP_CONNECTING;
            else g_opage = OP_FAILED;
        } else if (a == MENU_BACK) {
            g_opage = OP_MAIN;
        } else if (a == MENU_UP || a == MENU_DOWN) {   /* controller: step the last character */
            if (!n) { g_oaddr[0] = '0'; g_oaddr[1] = 0; n = 1; }
            const char* c = strchr(k_addr_chars, g_oaddr[n - 1]);
            int i = c ? (int)(c - k_addr_chars) : 0, m = (int)strlen(k_addr_chars);
            i = (i + (a == MENU_UP ? 1 : m - 1)) % m;
            g_oaddr[n - 1] = k_addr_chars[i];
        } else if (a == MENU_RIGHT && n + 1 < sizeof g_oaddr) {
            g_oaddr[n] = '0';
            g_oaddr[n + 1] = 0;
        } else if (a == MENU_LEFT && n) {
            g_oaddr[n - 1] = 0;
        }
        break;
    }
    case OP_HOSTING:
    case OP_CONNECTING:
        if (a == MENU_BACK) { netplay_lobby_cancel(); g_opage = OP_MAIN; }
        break;
    case OP_FAILED:
        if (a == MENU_OK || a == MENU_BACK) { netplay_lobby_cancel(); g_opage = OP_MAIN; }
        break;
    case OP_READY:
        break;
    case OP_SESSION:
        if (a == MENU_UP || a == MENU_DOWN) g_osel ^= 1;
        else if (a == MENU_OK && g_osel == 0) { online_restart(""); g_opage = OP_READY; }
        else if (a == MENU_OK || a == MENU_BACK) g_open = false;
        break;
    }
    g_dirty = true;
}

bool menu_take_quit(void) {
    bool q = g_quit_yes;
    g_quit_yes = false;
    return q;
}

bool menu_is_open(void) { return g_open; }
/* The colours page, and the Online page in a netplay session (the other
   player's game can't wait for this one): the game runs on behind it. */
bool menu_live(void) { return g_open && (g_colors_page || (g_online && netplay_active())); }

unsigned menu_take_changes(void) {
    unsigned c = g_changes;
    g_changes = 0;
    return c;
}

static void change(int item, int dir) {
    Settings* s = &g_settings;
    switch (item) {
    case MI_SHAPE: s->screen_shape = (s->screen_shape - 1 + 4 + dir) % 4 + 1; break;   /* 4:3, 16:9, 21:9, 32:9 */
    case MI_DISPLAY: s->fullscreen = !s->fullscreen; break;
    case MI_WINSIZE: s->window_size = (s->window_size - 1 + SETTINGS_WINDOW_SIZES + dir) % SETTINGS_WINDOW_SIZES + 1; break;
    case MI_RENDERER: s->renderer = !s->renderer; break;
    case MI_RES: s->resolution = (s->resolution - 1 + 8 + dir) % 8 + 1; break;
    case MI_TEXTURES: s->custom_textures = !s->custom_textures; break;
    default: return;
    }
    g_changes |= 1u << item;
}

static bool needs_restart(int i);

/* The game's own Video Settings screen (uismenu.c): its five rows are the
   first five items, with values as numbers from 0. */
static const int k_game_item[MENU_GAME_ROWS] = {MI_SHAPE, MI_DISPLAY, MI_WINSIZE, MI_RES, MI_RENDERER};

int menu_game_row_count(int row) {
    switch (k_game_item[row]) {
    case MI_SHAPE: return 4;
    case MI_DISPLAY: case MI_RENDERER: return 2;
    case MI_WINSIZE: return SETTINGS_WINDOW_SIZES;
    default: return 8;
    }
}

int menu_game_row_get(int row) {
    const Settings* s = &g_settings;
    switch (k_game_item[row]) {
    case MI_SHAPE: return s->screen_shape >= 1 && s->screen_shape <= 4 ? s->screen_shape - 1 : 1;
    case MI_DISPLAY: return s->fullscreen ? 1 : 0;
    case MI_WINSIZE: return s->window_size - 1;
    case MI_RENDERER: return s->renderer ? 1 : 0;
    default: return s->resolution - 1;
    }
}

void menu_game_row_set(int row, int v) {
    Settings* s = &g_settings;
    int item = k_game_item[row];
    switch (item) {
    case MI_SHAPE: if (g_shape_locked) return; s->screen_shape = v + 1; break;
    case MI_DISPLAY: s->fullscreen = v != 0; break;
    case MI_WINSIZE: if (s->fullscreen) return; s->window_size = v + 1; break;
    case MI_RENDERER: s->renderer = v != 0; break;
    default: if (s->renderer) return; s->resolution = v + 1; break;
    }
    g_changes |= 1u << item;
    g_dirty = true;
}

bool menu_game_row_enabled(int row) {
    switch (k_game_item[row]) {
    case MI_SHAPE: return !g_shape_locked;
    case MI_WINSIZE: return !g_settings.fullscreen;
    case MI_RES: return g_settings.renderer == 0;   /* the graphics card's */
    default: return true;
    }
}

const char* menu_game_row_name(int row, int v) {
    static char buf[48];
    switch (k_game_item[row]) {
    case MI_SHAPE: return v == 3 ? "32:9" : v == 2 ? "21:9" : v ? "16:9" : "4:3";
    case MI_DISPLAY: return v ? "Full Screen" : "Window";
    case MI_WINSIZE: {
        int h = settings_window_height(v + 1);
        snprintf(buf, sizeof buf, "%d x %d", (int)((float)h * g_aspect + 0.5f), h);
        return buf;
    }
    case MI_RENDERER: return v ? "Software (slow)" : "OpenGL";
    default: snprintf(buf, sizeof buf, "%dx", v + 1); return buf;
    }
}

const char* menu_game_row_help(int row) {
    /* Short: the help line has room for about 45 letters. */
    static const char k_restart[] = "Takes effect the next time the game starts.";
    switch (k_game_item[row]) {
    case MI_SHAPE: return "Change the screen aspect ratio.";
    case MI_DISPLAY: return "Play in a window or in full screen.";
    case MI_WINSIZE: return "Change the size of the window.";
    case MI_RENDERER: return needs_restart(MI_RENDERER) ? k_restart : "Change how the game is drawn.";
    default: return "Change how sharply the game is drawn.";
    }
}

/* The game's Recomp Settings screen (Options): the port's own additions. */
extern int g_rain_style, g_screen_filters, g_specular;
enum { RR_RAIN, RR_FILTERS, RR_TEXTURES, RR_SPECULAR, RR_LABELS, RR_INTROS, RR_DISCORD };
const char* const k_recomp_label[MENU_RECOMP_ROWS] = {"The Alley Rain", "Screen Filters", "Custom Textures",
                                                      "Specular Highlights", "Nintendo Buttons",
                                                      "Skip Intro Videos", "Discord Status"};

int menu_recomp_row_count(int row) { return row == RR_SPECULAR || row == RR_RAIN ? 3 : 2; }

/* The rain row shows Off, GameCube, PS2/Xbox; the setting keeps 0 GameCube,
   1 PS2/Xbox (its old meanings) and 2 off. */
static const int k_rain_row[3] = {2, 0, 1};   /* row value -> setting */

int menu_recomp_row_get(int row) {
    switch (row) {
    case RR_RAIN: return g_settings.ps2_rain == 2 ? 0 : g_settings.ps2_rain == 1 ? 2 : 1;
    case RR_FILTERS: return g_settings.screen_filters ? 1 : 0;
    case RR_TEXTURES: return g_settings.custom_textures ? 1 : 0;
    case RR_INTROS: return g_settings.skip_intros ? 1 : 0;
    case RR_DISCORD: return g_settings.no_discord ? 0 : 1;
    case RR_SPECULAR: return g_settings.specular;
    default: return g_settings.nintendo_labels ? 0 : 1;
    }
}

/* Netplay: settings that change the game stay as they are (the other
   player's game has the same; changing one here would part them). */
static bool net_locked(int row) {
    return netplay_active() && row == RR_RAIN;   /* (the intros only matter at start-up) */
}

void menu_recomp_row_set(int row, int v) {
    if (net_locked(row)) return;
    switch (row) {
    case RR_RAIN:
        g_settings.ps2_rain = k_rain_row[v < 0 ? 0 : v > 2 ? 2 : v];
        g_rain_style = g_settings.ps2_rain;
        g_changes |= 1u << MI_COUNT;
        break;
    case RR_FILTERS:
        g_settings.screen_filters = v != 0;
        g_screen_filters = g_settings.screen_filters;
        g_changes |= 1u << MI_COUNT;
        break;
    case RR_TEXTURES:
        if (!g_info.textures_found || g_settings.renderer != 0) return;
        g_settings.custom_textures = v != 0;
        g_changes |= 1u << MI_TEXTURES;
        break;
    case RR_INTROS: g_settings.skip_intros = v != 0; g_changes |= 1u << MI_COUNT; break;
    case RR_DISCORD:
        g_settings.no_discord = v == 0;
        discord_enable(v != 0);
        g_changes |= 1u << MI_COUNT;
        break;
    case RR_SPECULAR:
        if (g_settings.renderer != 0) return;
        g_settings.specular = v < 0 ? 0 : v > 2 ? 2 : v;
        g_specular = g_settings.specular;
        g_changes |= 1u << MI_COUNT;
        break;
    default: g_settings.nintendo_labels = v == 0; g_changes |= 1u << MI_COUNT; break;
    }
    g_dirty = true;
}

bool menu_recomp_row_enabled(int row) {
    if (net_locked(row)) return false;
    if (row == RR_TEXTURES) return g_info.textures_found && g_settings.renderer == 0;
    if (row == RR_SPECULAR) return g_settings.renderer == 0;
    return true;
}

const char* menu_recomp_row_name(int row, int v) {
    switch (row) {
    case RR_RAIN: return v == 2 ? "PS2/Xbox" : v == 1 ? "GameCube" : "Off";
    case RR_FILTERS: return v ? "On" : "Off (Unstable)";
    case RR_INTROS: case RR_DISCORD: return v ? "On" : "Off";
    case RR_SPECULAR: return v == 2 ? "Accurate" : v == 1 ? "Subtle" : "Off";
    case RR_TEXTURES: return !g_info.textures_found ? "None Found" : v ? "On" : "Off";
    default: return v ? "By Position" : "By Label";
    }
}

const char* menu_recomp_row_help(int row) {
    switch (row) {
    case RR_RAIN: return "PS2/Xbox: more rain, with streaks and splashes.";
    case RR_FILTERS: return "Colour tint in matches. Off is unstable: it may flash.";
    case RR_TEXTURES: return "Use the replacement textures folder.";
    case RR_INTROS: return "Hides and skips the intro videos. Applies next start.";
    case RR_DISCORD: return "Shows what you're playing on your Discord profile.";
    case RR_SPECULAR: return "Shiny highlights on players. Accurate: as on PS2.";
    default: return "For Nintendo pads: A/B/X/Y by label or position.";
    }
}

static void controls_action(int a) {
    if (g_capture) return;             /* the window sends the key / button instead */
    switch (a) {
    case MENU_UP: g_crow = (g_crow + CROWS - 1) % CROWS; break;
    case MENU_DOWN: g_crow = (g_crow + 1) % CROWS; break;
    case MENU_LEFT: case MENU_RIGHT:
        if (g_crow == CROW_KBPLAYER) {
            g_settings.keyboard_player = (g_settings.keyboard_player + (a == MENU_LEFT ? 3 : 1)) % 4;
            g_changes |= 1u << MI_CONTROLS;
        } else if (g_crow == CROW_NINTENDO) {
            g_settings.nintendo_labels = !g_settings.nintendo_labels;
            g_changes |= 1u << MI_CONTROLS;
        } else {
            g_ccol ^= 1;
        }
        break;
    case MENU_OK:
        if (g_crow == CROW_BACK) g_controls = false;
        else if (g_crow == CROW_KBPLAYER) {
            g_settings.keyboard_player = (g_settings.keyboard_player + 1) % 4;
            g_changes |= 1u << MI_CONTROLS;
        } else if (g_crow == CROW_NINTENDO) {
            g_settings.nintendo_labels = !g_settings.nintendo_labels;
            g_changes |= 1u << MI_CONTROLS;
        }
        else if (g_crow == CROW_RESET) {
            for (int i = 0; i < GCI_COUNT; i++) g_settings.key_bind[i] = g_settings.pad_bind[i] = 0;
            g_changes |= 1u << MI_CONTROLS;
        } else {
            g_capture = g_ccol + 1;
        }
        break;
    case MENU_BACK: g_controls = false; break;
    }
    g_dirty = true;
}

static void colors_leave(void) {
    g_colors_page = false;
    g_kedit = false;
    uicolors_save();
}

static void colors_action(int a) {
    int n = uicolors_slot_count(), rows = n + 2;   /* + reset, back */
    if (g_kedit) {                     /* red / green / blue of row g_krow */
        u32 c = uicolors_slot_get(g_krow);
        int sh = 16 - 8 * g_kch, v = (int)((c >> sh) & 255);
        switch (a) {
        case MENU_UP: g_kch = (g_kch + 2) % 3; break;
        case MENU_DOWN: g_kch = (g_kch + 1) % 3; break;
        case MENU_LEFT: case MENU_RIGHT:
            v += a == MENU_LEFT ? -5 : 5;
            v = v < 0 ? 0 : v > 255 ? 255 : v;
            uicolors_slot_set(g_krow, (c & ~(0xFFu << sh)) | (u32)v << sh);
            break;
        case MENU_OK: case MENU_BACK: g_kedit = false; break;
        }
        g_dirty = true;
        return;
    }
    switch (a) {
    case MENU_UP: g_krow = (g_krow + rows - 1) % rows; break;
    case MENU_DOWN: g_krow = (g_krow + 1) % rows; break;
    case MENU_LEFT: case MENU_RIGHT:
        if (g_krow < n) {              /* the next / previous ready-made colour */
            int np = uicolors_preset_count(), p = uicolors_slot_preset(g_krow);
            if (p < 0) p = a == MENU_LEFT ? 1 : np - 1;   /* from a custom colour */
            u32 cur = uicolors_slot_get(g_krow);
            for (int k = 0; k < np; k++) {   /* skip ones that look the same (red on a red shape) */
                p = (p + (a == MENU_LEFT ? np - 1 : 1)) % np;
                if (uicolors_preset_color(g_krow, p) != cur) break;
            }
            uicolors_slot_set(g_krow, uicolors_preset_color(g_krow, p));
        }
        break;
    case MENU_OK:
        if (g_krow < n) { g_kedit = true; g_kch = 0; }
        else if (g_krow == n) uicolors_reset_all();
        else colors_leave();
        break;
    case MENU_BACK: colors_leave(); break;
    }
    g_dirty = true;
}

void menu_action(int a) {
    if (!g_open) return;
    if (g_online) {
        online_action(a);
        return;
    }
    if (g_quit_page) {
        switch (a) {
        case MENU_UP: case MENU_DOWN: case MENU_LEFT: case MENU_RIGHT: g_qsel ^= 1; break;
        case MENU_OK:
            if (g_qsel == 0) g_quit_yes = true;
            g_open = g_quit_page = false;
            break;
        case MENU_BACK: g_open = g_quit_page = false; break;
        }
        g_dirty = true;
        return;
    }
    if (g_controls) {
        controls_action(a);
        return;
    }
    if (g_colors_page) {
        colors_action(a);
        return;
    }
    switch (a) {
    case MENU_UP:
    case MENU_DOWN: {
        int d = a == MENU_UP ? -1 : 1;
        for (int n = 0; n < MI_COUNT; n++) {
            g_sel = (g_sel + d + MI_COUNT) % MI_COUNT;
            if (item_enabled(g_sel) && !item_hidden(g_sel)) break;
        }
        break;
    }
    case MENU_LEFT:
    case MENU_RIGHT:
        if (g_sel == MI_CONTROLS || g_sel == MI_COLORS) { if (a == MENU_RIGHT) menu_action(MENU_OK); return; }
        if (item_enabled(g_sel)) change(g_sel, a == MENU_LEFT ? -1 : 1);
        break;
    case MENU_OK:
        if (g_sel == MI_CLOSE) g_open = false;
        else if (g_sel == MI_CONTROLS) { g_controls = true; g_crow = 0; g_ccol = 0; g_capture = 0; }
        else if (g_sel == MI_COLORS) { g_colors_page = true; g_krow = 0; g_kedit = false; }
        else if (item_enabled(g_sel)) change(g_sel, 1);
        break;
    case MENU_BACK:
        g_open = false;
        break;
    }
    g_dirty = true;
}

/* ------------------------------------------------------------- drawing --- */
static u32* g_px;
static int g_w, g_h;
static stbtt_fontinfo g_font;
static bool g_font_ok;

typedef struct { int size, cp, w, h, xoff, yoff; float adv; u8* bmp; } Glyph;
#define GLYPHS 1024
static Glyph g_glyphs[GLYPHS];
static int g_nglyphs;

static const Glyph* glyph(int size, int cp) {
    for (int i = 0; i < g_nglyphs; i++)
        if (g_glyphs[i].size == size && g_glyphs[i].cp == cp) return &g_glyphs[i];
    if (g_nglyphs == GLYPHS) {          /* full (many window sizes): start again */
        for (int i = 0; i < g_nglyphs; i++) stbtt_FreeBitmap(g_glyphs[i].bmp, NULL);
        g_nglyphs = 0;
    }
    Glyph* g = &g_glyphs[g_nglyphs++];
    float sc = stbtt_ScaleForPixelHeight(&g_font, (float)size);
    int adv, lsb;
    stbtt_GetCodepointHMetrics(&g_font, cp, &adv, &lsb);
    g->size = size;
    g->cp = cp;
    g->adv = (float)adv * sc;
    g->bmp = stbtt_GetCodepointBitmap(&g_font, 0, sc, cp, &g->w, &g->h, &g->xoff, &g->yoff);
    return g;
}

/* Premultiplied "over" of colour rgb at alpha a (0-255). */
static inline void blend(int x, int y, u32 rgb, int a) {
    if (a <= 0 || x < 0 || y < 0 || x >= g_w || y >= g_h) return;
    if (a > 255) a = 255;
    u32* d = &g_px[(size_t)y * g_w + x];
    u32 dv = *d;
    int inv = 255 - a;
    int r = (int)((rgb >> 16) & 255) * a / 255 + (int)((dv >> 16) & 255) * inv / 255;
    int g = (int)((rgb >> 8) & 255) * a / 255 + (int)((dv >> 8) & 255) * inv / 255;
    int b = (int)(rgb & 255) * a / 255 + (int)(dv & 255) * inv / 255;
    int al = a + (int)(dv >> 24) * inv / 255;
    *d = ((u32)al << 24) | ((u32)r << 16) | ((u32)g << 8) | (u32)b;
}

static void fill_rect(int x0, int y0, int x1, int y1, u32 rgb, int a) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > g_w) x1 = g_w;
    if (y1 > g_h) y1 = g_h;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) blend(x, y, rgb, a);
}

/* A band slanted like the game's own banners: left edge leaning right by
   `skew` pixels from bottom to top, right edge likewise. */
static void fill_slant(int x0, int y0, int x1, int y1, int skew, u32 rgb, int a) {
    for (int y = y0; y < y1; y++) {
        float t = (float)(y1 - y) / (float)(y1 - y0);      /* 0 at the bottom, 1 at the top */
        float l = (float)x0 + t * (float)skew, r = (float)x1 + t * (float)skew;
        int xl = (int)floorf(l), xr = (int)ceilf(r);
        for (int x = xl; x < xr; x++) {
            float cov = 1.0f;
            if (x < l) cov = 1.0f - (l - (float)x);
            if (x + 1 > r) cov = r - (float)x;
            blend(x, y, rgb, (int)((float)a * (cov < 0 ? 0 : cov)));
        }
    }
}

/* A small solid triangle pointing left (dir -1) or right (1), centred at cx, cy. */
static void arrow(int cx, int cy, int size, int dir, u32 rgb, int a) {
    for (int x = 0; x <= size; x++) {          /* x = 0: the flat side, size: the tip */
        int half = size - x;
        int px = cx + dir * (x - size / 2);
        for (int y = -half; y <= half; y++) blend(px, cy + y, rgb, a);
    }
}

static float text_width(const char* s, int size) {
    float sc = stbtt_ScaleForPixelHeight(&g_font, (float)size), w = 0;
    for (const char* p = s; *p; p++) {
        w += glyph(size, (unsigned char)*p)->adv;
        if (p[1]) w += (float)stbtt_GetCodepointKernAdvance(&g_font, (unsigned char)p[0], (unsigned char)p[1]) * sc;
    }
    return w;
}

/* Text with its baseline at y; align 0 left, 1 right (x is the right edge), 2 centred. */
static void text(const char* s, float x, int y, int size, u32 rgb, int a, int align) {
    if (!g_font_ok) return;
    if (align) x -= text_width(s, size) / (align == 1 ? 1.0f : 2.0f);
    float sc = stbtt_ScaleForPixelHeight(&g_font, (float)size);
    for (const char* p = s; *p; p++) {
        const Glyph* g = glyph(size, (unsigned char)*p);
        int gx = (int)floorf(x + 0.5f) + g->xoff, gy = y + g->yoff;
        for (int j = 0; j < g->h; j++)
            for (int i = 0; i < g->w; i++) {
                int c = g->bmp[j * g->w + i];
                if (c) blend(gx + i, gy + j, rgb, c * a / 255);
            }
        x += g->adv;
        if (p[1]) x += (float)stbtt_GetCodepointKernAdvance(&g_font, (unsigned char)p[0], (unsigned char)p[1]) * sc;
    }
}

/* Word-wrapped text inside width `maxw`; returns the number of lines. */
static int text_wrapped(const char* s, float x, int y, float maxw, int size, int line_h, u32 rgb, int a) {
    char line[540] = "", word[128];
    int lines = 0;
    const char* p = s;
    while (*p) {
        int n = 0;
        while (*p == ' ') p++;
        while (*p && *p != ' ' && n < 127) word[n++] = *p++;
        word[n] = 0;
        if (!n) break;
        char trial[700];
        snprintf(trial, sizeof(trial), "%s%s%s", line, line[0] ? " " : "", word);
        if (line[0] && text_width(trial, size) > maxw) {
            text(line, x, y + lines * line_h, size, rgb, a, 0);
            lines++;
            snprintf(line, sizeof(line), "%s", word);
        } else {
            snprintf(line, sizeof(line), "%s", trial);
        }
    }
    if (line[0]) {
        text(line, x, y + lines * line_h, size, rgb, a, 0);
        lines++;
    }
    return lines;
}

static const char* item_label(int i) {
    static const char* const k[MI_COUNT] = {"ASPECT RATIO", "DISPLAY", "WINDOW RESOLUTION", "INTERNAL RESOLUTION",
                                            "RENDERER", "CUSTOM TEXTURES", "MENU COLOURS",
                                            "CONTROLS", "BACK TO THE GAME"};
    return k[i];
}

static void item_value(int i, char* out, size_t cap) {
    const Settings* s = &g_settings;
    switch (i) {
    case MI_SHAPE: snprintf(out, cap, "%s", s->screen_shape == 1 ? "4:3" : s->screen_shape == 3 ? "21:9 (TEST)" : s->screen_shape == 4 ? "32:9 (TEST)" : "16:9"); break;
    case MI_DISPLAY: snprintf(out, cap, "%s", s->fullscreen ? "FULL SCREEN" : "WINDOW"); break;
    case MI_WINSIZE: {
        int h = settings_window_height(s->window_size);
        snprintf(out, cap, "%d X %d", (int)((float)h * g_aspect + 0.5f), h);
        break;
    }
    case MI_RENDERER:
        snprintf(out, cap, "%s", s->renderer ? "SOFTWARE (SLOW)" : "OPENGL");
        break;
    case MI_RES:
        snprintf(out, cap, "%dX", s->resolution);
        break;
    case MI_TEXTURES:
        snprintf(out, cap, "%s", !g_info.textures_found ? "NONE FOUND" : s->custom_textures ? "ON" : "OFF");
        break;
    case MI_COLORS: {
        int changed = 0;
        for (int k = 0; k < uicolors_slot_count(); k++) changed += uicolors_slot_get(k) != uicolors_slot_original(k);
        if (changed) snprintf(out, cap, "%d CHANGED", changed);
        else snprintf(out, cap, "THE GAME'S OWN");
        break;
    }
    case MI_CONTROLS: snprintf(out, cap, "KEYBOARD AND CONTROLLER"); break;
    default: out[0] = 0; break;
    }
}

static const char* item_help(int i) {
    switch (i) {
    case MI_SHAPE: return "The picture's shape: 4:3 or 16:9 (F9 also switches; the game's own screen option is set "
                          "to match). Can't be changed during a match (the match lays out its scoreboard when it "
                          "starts); change it in the game's menus.";
    case MI_DISPLAY: return "Play in a window or on the whole screen (F11 also switches).";
    case MI_WINSIZE: return "How big the window is when not playing full screen.";
    case MI_RENDERER: return "OpenGL draws on the graphics card: faster and sharper than the software renderer. Takes effect "
                             "the next time the game starts.";
    case MI_RES: return "How sharply the graphics card draws the game, as a multiple of the GameCube's own picture. "
                        "Higher needs a faster card.";
    case MI_TEXTURES: return g_info.textures_found
                                 ? "Replacement textures from the custom texture folder (Dolphin's format, "
                                   ".png or .dds). Turn off and on again (or press F5 twice) to load files added or changed since."
                                 : "No custom texture folder was found. Put Dolphin-format texture packs in "
                                   "C:\\nfls2\\textures to use them.";
    case MI_COLORS: return "Change the colours of the game's menu bars and music ticker (shapes custom textures can't "
                           "reach). The game carries on behind the page so you can see each change.";
    case MI_CONTROLS: return "Choose which keys and controller buttons work each GameCube button and stick.";
    default: return "Close this menu and carry on playing.";
    }
}

static bool needs_restart(int i) {
    const Settings* s = &g_settings;
    if (i == MI_RENDERER) return (s->renderer == 0) != g_info.gpu_running;
    if (i == MI_RES) return false;     /* changed straight away (the graphics card's) */
    return false;
}

static void draw_controls(int w, int h, float u);
static void draw_colors(int w, int h, float u);
static void draw_online(int w, int h, float u);

#define COL_ORANGE 0xFF7A00u
#define COL_YELLOW 0xFFD21Fu
#define COL_WHITE 0xFFFFFFu
#define COL_GREY 0xB4B4BEu
#define COL_DIM 0x5A5A64u
#define COL_PANEL 0x0E0E12u

const uint32_t* menu_draw(int w, int h, bool* dirty) {
    if (!g_font_ok) {
        g_font_ok = stbtt_InitFont(&g_font, k_font_anton, stbtt_GetFontOffsetForIndex(k_font_anton, 0)) != 0;
        if (!g_font_ok) printf("menu: couldn't read the built-in font\n");
    }
    if (w != g_w || h != g_h || !g_px) {
        free(g_px);
        g_px = (u32*)malloc((size_t)w * (size_t)h * 4);
        g_w = w;
        g_h = h;
        g_dirty = true;
        if (!g_px) { g_w = g_h = 0; *dirty = false; return NULL; }
    }
    if (g_px_owner != 0) { g_px_owner = 0; g_dirty = true; }
    *dirty = g_dirty;
    if (!g_dirty) return g_px;
    g_dirty = false;
    memset(g_px, 0, (size_t)w * (size_t)h * 4);

    float u = (float)h / 1080.0f;              /* layout unit: 1 pixel at 1080 lines */
    if ((float)w / 1920.0f < u * 0.75f) u = (float)w / 1920.0f / 0.75f;
#define U(v) ((int)((v) * u + 0.5f))
    if (g_colors_page) {                               /* the game left bright, to see the colours */
        draw_colors(w, h, u);
        return g_px;
    }
    fill_rect(0, 0, w, h, 0, 150);                     /* the game, dimmed */
    if (g_online) {
        draw_online(w, h, u);
        return g_px;
    }
    if (g_quit_page) {
        int pw = U(760), ph = U(330);
        int px0 = (w - pw) / 2, py0 = (h - ph) / 2;
        fill_rect(px0, py0, px0 + pw, py0 + ph, COL_PANEL, 232);
        fill_rect(px0, py0 + ph - U(6), px0 + pw, py0 + ph, COL_ORANGE, 255);
        int by0 = py0 - U(36), by1 = py0 + U(78);
        fill_slant(px0 - U(24), by0, px0 + U(420), by1, U(28), COL_ORANGE, 255);
        fill_slant(px0 + U(430), by0 + U(18), px0 + U(460), by1, U(26), COL_YELLOW, 255);
        text("QUIT GAME?", (float)(px0 + U(28)), by1 - U(24), U(78), 0x111111u, 255, 0);
        text("Anything not saved will be lost.", (float)(w / 2), py0 + U(160), U(34), COL_GREY, 255, 2);
        static const char* const k_q[2] = {"YES", "NO"};
        for (int i = 0; i < 2; i++) {
            int cx = w / 2 + (i == 0 ? -U(150) : U(150));
            int y0 = py0 + U(205), y1 = y0 + U(76);
            bool sel = g_qsel == i;
            if (sel) {
                fill_rect(cx - U(120), y0, cx + U(120), y1, COL_ORANGE, 46);
                fill_rect(cx - U(120), y1 - U(6), cx + U(120), y1, COL_ORANGE, 255);
            }
            text(k_q[i], (float)cx, y0 + U(56), U(46), sel ? COL_YELLOW : COL_GREY, 255, 2);
        }
        text("LEFT / RIGHT: CHOOSE     A / ENTER: CONFIRM     B / ESC: BACK", (float)(w / 2), py0 + ph + U(50), U(26),
             COL_GREY, 230, 2);
        return g_px;
    }
    if (g_controls) {
        draw_controls(w, h, u);
        return g_px;
    }

    int pw = U(980), ph = U(950);
    int px0 = (w - pw) / 2, py0 = (h - ph) / 2;
    fill_rect(px0, py0, px0 + pw, py0 + ph, COL_PANEL, 232);
    fill_rect(px0, py0 + ph - U(6), px0 + pw, py0 + ph, COL_ORANGE, 255);   /* bottom edge */

    /* Title on a slanted orange banner breaking out of the panel. */
    int by0 = py0 - U(36), by1 = py0 + U(78);
    fill_slant(px0 - U(24), by0, px0 + U(520), by1, U(28), COL_ORANGE, 255);
    fill_slant(px0 + U(530), by0 + U(18), px0 + U(560), by1, U(26), COL_YELLOW, 255);
    text("VIDEO SETTINGS", (float)(px0 + U(28)), by1 - U(24), U(78), 0x111111u, 255, 0);

    /* Rows */
    int row_h = U(70), ry = py0 + U(130), lx = px0 + U(60), vx = px0 + pw - U(70);
    int slot = 0;
    for (int i = 0; i < MI_COUNT; i++) {
        if (item_hidden(i)) continue;
        int y0 = ry + slot++ * row_h + (i == MI_CLOSE ? U(18) : 0);
        bool sel = i == g_sel, en = item_enabled(i);
        if (sel) {
            fill_rect(px0 + U(24), y0, px0 + pw - U(24), y0 + row_h - U(8), COL_ORANGE, 46);
            fill_rect(px0 + U(24), y0, px0 + U(34), y0 + row_h - U(8), COL_ORANGE, 255);
        }
        int base = y0 + U(50);
        u32 lc = !en ? COL_DIM : sel ? COL_YELLOW : COL_GREY;
        text(item_label(i), (float)lx, base, U(40), lc, 255, 0);
        char v[64];
        item_value(i, v, sizeof(v));
        if (v[0]) {
            u32 vc = !en ? COL_DIM : sel ? COL_WHITE : 0xE6E6E6u;
            int vw = (int)text_width(v, U(40));
            text(v, (float)vx, base, U(40), vc, 255, 1);
            if (sel && en) {
                arrow(vx - vw - U(26), base - U(15), U(10), -1, COL_YELLOW, 255);
                arrow(vx + U(22), base - U(15), U(10), 1, COL_YELLOW, 255);
            }
            if (needs_restart(i))
                text("AFTER RESTART", (float)(vx - vw - U(56)), base, U(24), COL_ORANGE, 255, 1);
            else if (i == MI_SHAPE && g_shape_locked)
                text("CHANGE IT IN THE MENUS", (float)(vx - vw - U(30)), base, U(24), COL_ORANGE, 255, 1);
        }
    }

    /* What the selected item does. */
    int hy = ry + slot * row_h + U(48);
    fill_rect(px0 + U(60), hy - U(10), px0 + pw - U(60), hy - U(8), COL_DIM, 255);
    text_wrapped(item_help(g_sel), (float)(px0 + U(60)), hy + U(36), (float)(pw - U(120)), U(28), U(36), COL_GREY, 255);

    text("UP / DOWN: CHOOSE     LEFT / RIGHT: CHANGE     ESC, F1 OR B: CLOSE", (float)(w / 2), py0 + ph + U(50),
         U(26), COL_GREY, 230, 2);
#undef U
    return g_px;
}

/* ------------------------------------------------------ controls page --- */
static const char* const k_input_labels[GCI_COUNT] = {
    "A", "B", "X", "Y", "Z", "START", "L", "R",
    "D-PAD UP", "D-PAD DOWN", "D-PAD LEFT", "D-PAD RIGHT",
    "STICK UP", "STICK DOWN", "STICK LEFT", "STICK RIGHT",
    "C-STICK UP", "C-STICK DOWN", "C-STICK LEFT", "C-STICK RIGHT",
};

static void draw_controls(int w, int h, float u) {
#define U(v) ((int)((v) * u + 0.5f))
    int pw = U(1100), ph = U(1010);
    int px0 = (w - pw) / 2, py0 = (h - ph) / 2;
    fill_rect(px0, py0, px0 + pw, py0 + ph, COL_PANEL, 232);
    fill_rect(px0, py0 + ph - U(6), px0 + pw, py0 + ph, COL_ORANGE, 255);
    int by0 = py0 - U(30), by1 = py0 + U(70);
    fill_slant(px0 - U(24), by0, px0 + U(380), by1, U(24), COL_ORANGE, 255);
    fill_slant(px0 + U(390), by0 + U(16), px0 + U(416), by1, U(22), COL_YELLOW, 255);
    text("CONTROLS", (float)(px0 + U(28)), by1 - U(20), U(68), 0x111111u, 255, 0);

    int lx = px0 + U(60), c0 = px0 + U(560), c1 = px0 + U(880);
    int top = py0 + U(100), row_h = U(34), fs = U(25);
    text("KEYBOARD", (float)c0, top, U(26), COL_ORANGE, 255, 2);
    text("CONTROLLER", (float)c1, top, U(26), COL_ORANGE, 255, 2);
    int ry = top + U(14);
    for (int r = 0; r < CROWS; r++) {
        int y0 = ry + r * row_h + (r >= CROW_KBPLAYER ? U(12) : 0);
        int base = y0 + U(27);
        bool sel = r == g_crow;
        if (sel) fill_rect(px0 + U(24), y0, px0 + pw - U(24), y0 + row_h - U(4), COL_ORANGE, 30);
        if (r == CROW_KBPLAYER || r == CROW_NINTENDO) {
            text(r == CROW_KBPLAYER ? "KEYBOARD PLAYS AS" : "NINTENDO CONTROLLERS", (float)lx, base, fs,
                 sel ? COL_YELLOW : COL_GREY, 255, 0);
            char v[40];
            if (r == CROW_KBPLAYER) snprintf(v, sizeof(v), "PLAYER %d", g_settings.keyboard_player + 1);
            else snprintf(v, sizeof(v), "%s", g_settings.nintendo_labels ? "BY LABEL (A = A)" : "BY POSITION");
            text(v, (float)c0, base, fs, sel ? COL_WHITE : 0xD2D2D2u, 255, 2);
            if (sel) {
                int vw = (int)text_width(v, fs);
                arrow(c0 - vw / 2 - U(22), base - U(10), U(8), -1, COL_YELLOW, 255);
                arrow(c0 + vw / 2 + U(22), base - U(10), U(8), 1, COL_YELLOW, 255);
            }
            continue;
        }
        if (r >= CROW_RESET) {
            text(r == CROW_RESET ? "RESET ALL TO DEFAULTS" : "BACK", (float)lx, base, fs, sel ? COL_YELLOW : COL_GREY,
                 255, 0);
            if (sel) fill_rect(px0 + U(24), y0, px0 + U(32), y0 + row_h - U(4), COL_ORANGE, 255);
            continue;
        }
        text(k_input_labels[r], (float)lx, base, fs, sel ? COL_YELLOW : COL_GREY, 255, 0);
        for (int c = 0; c < 2; c++) {
            int cx = c ? c1 : c0;
            int code = c ? g_settings.pad_bind[r] : g_settings.key_bind[r];
            const char* name = code < 0 ? "-" : c ? (g_pad_name ? g_pad_name(code) : "?") : (g_key_name ? g_key_name(code) : "?");
            bool cell = sel && c == g_ccol;
            if (cell) {
                fill_rect(cx - U(150), y0 + U(2), cx + U(150), y0 + row_h - U(6), COL_ORANGE, g_capture ? 200 : 90);
                if (g_capture) name = c ? "PRESS A BUTTON" : "PRESS A KEY";
            }
            text(name, (float)cx, base, fs, cell ? COL_WHITE : 0xD2D2D2u, 255, 2);
        }
    }
    if (g_players[0])
        text(g_players, (float)(w / 2), py0 + ph - U(50), U(22), COL_ORANGE, 255, 2);
    const char* help = g_capture ? "ESC: CANCEL     DELETE: NO BINDING"
                                 : "UP / DOWN: CHOOSE     LEFT / RIGHT: KEYBOARD OR CONTROLLER     ENTER OR A: CHANGE     ESC OR B: BACK";
    text(help, (float)(w / 2), py0 + ph - U(24), U(22), COL_GREY, 230, 2);
#undef U
}

/* ------------------------------------------------- menu colours page --- */
static void draw_colors(int w, int h, float u) {
#define U(v) ((int)((v) * u + 0.5f))
    int n = uicolors_slot_count(), rows = n + 2;
    int pw = U(820), row_h = U(60), top = U(120);
    int ph = top + rows * row_h + U(30) + U(250);
    int px0 = w - pw - U(40), py0 = (h - ph) / 2;     /* to the right: the game shows beside it */
    if (px0 < U(16)) px0 = U(16);
    fill_rect(px0, py0, px0 + pw, py0 + ph, COL_PANEL, 225);
    fill_rect(px0, py0 + ph - U(6), px0 + pw, py0 + ph, COL_ORANGE, 255);
    int by0 = py0 - U(30), by1 = py0 + U(70);
    fill_slant(px0 - U(24), by0, px0 + U(470), by1, U(24), COL_ORANGE, 255);
    fill_slant(px0 + U(480), by0 + U(16), px0 + U(506), by1, U(22), COL_YELLOW, 255);
    text("MENU COLOURS", (float)(px0 + U(28)), by1 - U(20), U(68), 0x111111u, 255, 0);

    int lx = px0 + U(50), sx = px0 + pw - U(90), vx = sx - U(40), fs = U(28);
    for (int r = 0; r < rows; r++) {
        int y0 = py0 + top + r * row_h + (r >= n ? U(14) : 0), base = y0 + U(40);
        bool sel = r == g_krow;
        if (sel) {
            fill_rect(px0 + U(20), y0, px0 + pw - U(20), y0 + row_h - U(6), COL_ORANGE, g_kedit ? 70 : 40);
            fill_rect(px0 + U(20), y0, px0 + U(28), y0 + row_h - U(6), COL_ORANGE, 255);
        }
        if (r >= n) {
            text(r == n ? "RESET ALL TO THE GAME'S OWN" : "BACK", (float)lx, base, fs, sel ? COL_YELLOW : COL_GREY,
                 255, 0);
            continue;
        }
        text(uicolors_slot_name(r), (float)lx, base, fs, sel ? COL_YELLOW : COL_GREY, 255, 0);
        u32 c = uicolors_slot_get(r);
        int p = uicolors_slot_preset(r);
        char v[48];
        if (p >= 0) snprintf(v, sizeof(v), "%s", uicolors_preset_name(p));
        else snprintf(v, sizeof(v), "CUSTOM  %06X", c);
        int vw = (int)text_width(v, fs);
        text(v, (float)vx, base, fs, sel ? COL_WHITE : 0xD2D2D2u, 255, 1);
        if (sel && !g_kedit) {
            arrow(vx - vw - U(22), base - U(11), U(8), -1, COL_YELLOW, 255);
            arrow(vx + U(16), base - U(11), U(8), 1, COL_YELLOW, 255);
        }
        /* The colour itself, beside the game's own (small, behind). */
        int sy0 = y0 + U(10), sy1 = y0 + row_h - U(16);
        fill_rect(sx + U(34), sy0 + U(10), sx + U(56), sy1, uicolors_slot_original(r), 255);
        fill_rect(sx, sy0, sx + U(40), sy1, 0xFFFFFFu, 255);
        fill_rect(sx + U(2), sy0 + U(2), sx + U(38), sy1 - U(2), c, 255);
    }

    /* Below: red / green / blue of the selected colour, or what it is. */
    int iy = py0 + top + rows * row_h + U(40);
    fill_rect(px0 + U(50), iy - U(10), px0 + pw - U(50), iy - U(8), COL_DIM, 255);
    if (g_krow < n) {
        u32 c = uicolors_slot_get(g_krow);
        static const char* const k_ch[3] = {"RED", "GREEN", "BLUE"};
        static const u32 k_chc[3] = {0xE04040u, 0x40C040u, 0x4080F0u};
        for (int k = 0; k < 3; k++) {
            int y = iy + U(20) + k * U(46), v = (int)((c >> (16 - 8 * k)) & 255);
            bool on = g_kedit && g_kch == k;
            text(k_ch[k], (float)lx, y + U(26), U(26), on ? COL_YELLOW : g_kedit ? COL_GREY : COL_DIM, 255, 0);
            int bx0 = lx + U(130), bx1 = px0 + pw - U(150);
            fill_rect(bx0, y + U(10), bx1, y + U(22), 0x303038u, 255);
            fill_rect(bx0, y + U(10), bx0 + (bx1 - bx0) * v / 255, y + U(22), k_chc[k], g_kedit ? 255 : 120);
            char num[8];
            snprintf(num, sizeof(num), "%d", v);
            text(num, (float)(px0 + pw - U(60)), y + U(26), U(26), on ? COL_WHITE : COL_GREY, 255, 1);
            if (on) {
                arrow(bx0 - U(20), y + U(16), U(7), -1, COL_YELLOW, 255);
                arrow(bx1 + U(20), y + U(16), U(7), 1, COL_YELLOW, 255);
            }
        }
        text_wrapped(uicolors_slot_help(g_krow), (float)lx, iy + U(190), (float)(pw - U(100)), U(24), U(30),
                     COL_GREY, 255);
    } else {
        text_wrapped(g_krow == n ? "Put every colour on this page back to the game's own."
                                 : "Back to the video settings. Colours are saved to ui-colors.txt.",
                     (float)lx, iy + U(40), (float)(pw - U(100)), U(24), U(30), COL_GREY, 255);
    }
    const char* help = g_kedit ? "UP / DOWN: RED, GREEN OR BLUE     LEFT / RIGHT: CHANGE     A OR B: DONE"
                               : "LEFT / RIGHT: COLOUR     A / ENTER: FINE-TUNE     B / ESC: BACK";
    text(help, (float)(px0 + pw / 2), py0 + ph + U(40), U(22), COL_WHITE, 255, 2);
#undef U
}

/* --------------------------------------------------------- Online page --- */
/* In the PS2 version's online colours: navy and teal on a dark panel. */
#define COL_NAVY 0x0A1E5Au
#define COL_TEAL 0x1C9AB4u
#define COL_TEAL_LIGHT 0x8FE3F0u

/* ------------------------------------------------- netplay session badge --- */
/* A small badge in the corner during a session: which player this is and how
   the connection is (four bars from the round trip). In the menus it's always
   there; in a match only when something needs saying: waiting for the other
   player, a weak connection, or the games out of sync. */
static u32 g_hud_key = 0xFFFFFFFFu;

const uint32_t* menu_hud(int w, int h, bool* dirty, const NetplayStatus* st, bool in_menus) {
    int bars = st->ping_ms < 0 ? 0 : st->ping_ms < 60 ? 4 : st->ping_ms < 120 ? 3 : st->ping_ms < 200 ? 2 : 1;
    bool waiting = st->waiting_ms > 400;
    bool show = in_menus || waiting || st->out_of_sync || (bars > 0 && bars <= 1);
    u32 key = (u32)show | (u32)bars << 1 | (u32)waiting << 4 | (u32)st->out_of_sync << 5 | (u32)st->player << 6 |
              (u32)in_menus << 8 | (u32)(st->recent_wait) << 9;
    if (!g_font_ok) {
        g_font_ok = stbtt_InitFont(&g_font, k_font_anton, stbtt_GetFontOffsetForIndex(k_font_anton, 0)) != 0;
    }
    if (w != g_w || h != g_h || !g_px) {
        free(g_px);
        g_px = (u32*)malloc((size_t)w * (size_t)h * 4);
        g_w = w; g_h = h; key ^= 0x80000000u;
        if (!g_px) { g_w = g_h = 0; *dirty = false; return NULL; }
    }
    if (g_px_owner != 1) { g_px_owner = 1; g_hud_key = ~key; }
    *dirty = key != g_hud_key;
    if (!*dirty) return show ? g_px : NULL;
    g_hud_key = key;
    memset(g_px, 0, (size_t)w * (size_t)h * 4);
    if (!show) { *dirty = true; return NULL; }
    float u = (float)h / 1080.0f;
#define U(v) ((int)((v) * u + 0.5f))
    int x0 = U(28), y0 = U(26), bw = U(waiting || st->out_of_sync ? 470 : 250), bh = U(58);
    fill_rect(x0, y0, x0 + bw, y0 + bh, COL_PANEL, 200);
    fill_rect(x0, y0 + bh - U(4), x0 + bw, y0 + bh, st->out_of_sync ? 0xD02020u : COL_TEAL, 255);
    char line[64];
    snprintf(line, sizeof line, "P%d", st->player);
    text(line, (float)(x0 + U(14)), y0 + U(44), U(40), COL_WHITE, 255, 0);
    text(st->player == 1 ? "HOST" : "JOINED", (float)(x0 + U(62)), y0 + U(40), U(26), COL_GREY, 255, 0);
    /* connection: four bars */
    int gx = x0 + U(160);
    for (int i = 0; i < 4; i++) {
        int bx = gx + i * U(18), top = y0 + U(42) - U(8) * (i + 1);
        u32 c = i < bars ? (bars >= 3 ? COL_TEAL_LIGHT : bars == 2 ? COL_YELLOW : COL_ORANGE) : COL_DIM;
        fill_rect(bx, top, bx + U(12), y0 + U(44), c, 255);
    }
    if (st->out_of_sync)
        text("OUT OF SYNC", (float)(x0 + U(250)), y0 + U(42), U(32), 0xFF5050u, 255, 0);
    else if (waiting)
        text("WAITING FOR OPPONENT...", (float)(x0 + U(250)), y0 + U(42), U(26), COL_YELLOW, 255, 0);
#undef U
    return g_px;
}


static void draw_online(int w, int h, float u) {
#define U(v) ((int)((v) * u + 0.5f))
    int pw = U(1040), ph = U(700);
    int px0 = (w - pw) / 2, py0 = (h - ph) / 2;
    fill_rect(px0, py0, px0 + pw, py0 + ph, COL_PANEL, 236);
    fill_rect(px0, py0 + ph - U(6), px0 + pw, py0 + ph, COL_TEAL, 255);
    int by0 = py0 - U(36), by1 = py0 + U(78);
    fill_slant(px0 - U(24), by0, px0 + U(330), by1, U(28), COL_NAVY, 255);
    fill_slant(px0 + U(340), by0 + U(18), px0 + U(372), by1, U(26), COL_TEAL, 255);
    text("ONLINE", (float)(px0 + U(28)), by1 - U(24), U(78), COL_WHITE, 255, 0);
    int lx = px0 + U(60), cx = w / 2, ty = py0 + U(170);
    const char* hint = "";
    char line[256];
    switch (g_opage) {
    case OP_MAIN: {
        static const char* const k_rows[4] = {"HOST A GAME", "JOIN A GAME", "INPUT DELAY", "BACK"};
        static const char* const k_help[4] = {
            "Wait for another player to join you. You will be player 1, and your memory card is used.",
            "Join a game another player is hosting, by their address. You will be player 2.",
            "How many video fields ahead each controller is sent, to hide the connection's delay. 3 suits a home network; "
            "over the internet, about your ping in milliseconds divided by 16, plus 1. The host's is used.",
            "Back to the main menu."};
        int row_h = U(74), ry = py0 + U(120);
        for (int i = 0; i < 4; i++) {
            int y0 = ry + i * row_h + (i == 3 ? U(16) : 0);
            bool sel = i == g_osel;
            if (sel) {
                fill_rect(px0 + U(24), y0, px0 + pw - U(24), y0 + row_h - U(8), COL_TEAL, 52);
                fill_rect(px0 + U(24), y0, px0 + U(34), y0 + row_h - U(8), COL_TEAL, 255);
            }
            text(k_rows[i], (float)lx, y0 + U(52), U(42), sel ? COL_TEAL_LIGHT : COL_GREY, 255, 0);
            if (i == 2) {
                snprintf(line, sizeof line, "%d FIELDS (%d MS)", g_settings.net_delay, (g_settings.net_delay * 1001 + 30) / 60);
                int vx = px0 + pw - U(70), vw = (int)text_width(line, U(40));
                text(line, (float)vx, y0 + U(52), U(40), sel ? COL_WHITE : 0xE6E6E6u, 255, 1);
                if (sel) {
                    arrow(vx - vw - U(26), y0 + U(37), U(10), -1, COL_TEAL_LIGHT, 255);
                    arrow(vx + U(22), y0 + U(37), U(10), 1, COL_TEAL_LIGHT, 255);
                }
            }
        }
        int hy = ry + 4 * row_h + U(56);
        fill_rect(px0 + U(60), hy - U(10), px0 + pw - U(60), hy - U(8), COL_DIM, 255);
        text_wrapped(k_help[g_osel], (float)lx, hy + U(36), (float)(pw - U(120)), U(28), U(36), COL_GREY, 255);
        hint = "UP / DOWN: CHOOSE     LEFT / RIGHT: CHANGE     A / ENTER: SELECT     B / ESC: BACK";
        break;
    }
    case OP_JOIN: {
        text("THE HOST'S ADDRESS", (float)lx, ty - U(20), U(34), COL_GREY, 255, 0);
        int fy0 = ty, fy1 = ty + U(86);
        fill_rect(lx, fy0, px0 + pw - U(60), fy1, 0x000000u, 200);
        fill_rect(lx, fy1 - U(5), px0 + pw - U(60), fy1, COL_TEAL, 255);
        snprintf(line, sizeof line, "%s_", g_oaddr);
        text(line, (float)(lx + U(20)), fy1 - U(24), U(54), COL_WHITE, 255, 0);
        text_wrapped("Type it on the keyboard (for example 192.168.1.23) or paste it with CTRL+V. With a controller: "
                     "UP / DOWN changes the last character, RIGHT adds one, LEFT removes one. Add :PORT if the host "
                     "uses another port than 7120.",
                     (float)lx, fy1 + U(70), (float)(pw - U(120)), U(28), U(36), COL_GREY, 255);
        hint = "TYPE OR CTRL+V: ADDRESS     ENTER: CONNECT     ESC: BACK";
        break;
    }
    case OP_HOSTING: {
        text("WAITING FOR A PLAYER TO JOIN...", (float)cx, ty, U(48), COL_TEAL_LIGHT, 255, 2);
        char addrs[200];
        int n = netplay_local_addresses(addrs, sizeof addrs);
        text("GIVE THE OTHER PLAYER THIS PC'S ADDRESS:", (float)cx, ty + U(90), U(32), COL_GREY, 255, 2);
        text(n ? addrs : "(NOT FOUND: SEE IPCONFIG)", (float)cx, ty + U(160), U(54), COL_WHITE, 255, 2);
        text_wrapped("On the same home network that's all. Over the internet: use a virtual network both of you join "
                     "(such as Tailscale) and its address, or forward TCP port 7120 on your router and give your "
                     "public address. Windows may ask to allow the game through its firewall.",
                     (float)lx, ty + U(240), (float)(pw - U(120)), U(28), U(36), COL_GREY, 255);
        hint = "B / ESC: STOP HOSTING";
        break;
    }
    case OP_CONNECTING:
        snprintf(line, sizeof line, "CONNECTING TO %s...", g_oaddr);
        text(line, (float)cx, ty + U(60), U(46), COL_TEAL_LIGHT, 255, 2);
        hint = "B / ESC: CANCEL";
        break;
    case OP_FAILED: {
        const char* msg;
        int d;
        netplay_lobby_poll(&msg, &d);
        text("COULDN'T START THE SESSION", (float)cx, ty, U(46), COL_ORANGE, 255, 2);
        text_wrapped(msg && msg[0] ? msg : "SOMETHING WENT WRONG.", (float)lx, ty + U(80), (float)(pw - U(120)), U(32),
                     U(42), COL_WHITE, 255);
        hint = "A / B: BACK";
        break;
    }
    case OP_READY:
        text(netplay_active() ? "LEAVING THE SESSION..." : "PLAYER FOUND!", (float)cx, ty + U(40), U(58), COL_TEAL_LIGHT, 255, 2);
        text(netplay_active() ? "RESTARTING THE GAME ON ITS OWN" : "BOTH GAMES ARE RESTARTING INTO THE SESSION",
             (float)cx, ty + U(120), U(34), COL_GREY, 255, 2);
        break;
    case OP_ENDED:
        text("CONNECTION LOST", (float)cx, ty + U(40), U(58), COL_TEAL_LIGHT, 255, 2);
        text("The connection to your opponent has been lost.", (float)cx, ty + U(120), U(34), COL_GREY, 255, 2);
        text("Restarting the game...", (float)cx, ty + U(170), U(34), COL_GREY, 255, 2);
        break;
    case OP_SESSION: {
        text("YOU'RE IN AN ONLINE SESSION", (float)cx, ty - U(10), U(46), COL_TEAL_LIGHT, 255, 2);
        static const char* const k_rows[2] = {"LEAVE THE SESSION", "BACK"};
        int row_h = U(74), ry = ty + U(60);
        for (int i = 0; i < 2; i++) {
            int y0 = ry + i * row_h;
            bool sel = i == g_osel;
            if (sel) {
                fill_rect(px0 + U(24), y0, px0 + pw - U(24), y0 + row_h - U(8), COL_TEAL, 52);
                fill_rect(px0 + U(24), y0, px0 + U(34), y0 + row_h - U(8), COL_TEAL, 255);
            }
            text(k_rows[i], (float)lx, y0 + U(52), U(42), sel ? COL_TEAL_LIGHT : COL_GREY, 255, 0);
        }
        text_wrapped("Leaving ends the session: both games restart on their own.",
                     (float)lx, ry + 2 * row_h + U(50), (float)(pw - U(120)), U(28), U(36), COL_GREY, 255);
        hint = "UP / DOWN: CHOOSE     A / ENTER: SELECT     B / ESC: BACK";
        break;
    }
    }
    if (hint[0]) text(hint, (float)(w / 2), py0 + ph + U(50), U(26), COL_GREY, 230, 2);
#undef U
}
