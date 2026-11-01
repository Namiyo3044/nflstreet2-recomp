#include "platform/settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Settings g_settings = {0, 4, 0, 0, 3, 1, {0}, {0}, 0, 1, 1, 1, 0, 0, 3, ""};

const char* const k_gc_input_names[GCI_COUNT] = {
    "a", "b", "x", "y", "z", "start", "l", "r",
    "dpad_up", "dpad_down", "dpad_left", "dpad_right",
    "stick_up", "stick_down", "stick_left", "stick_right",
    "cstick_up", "cstick_down", "cstick_left", "cstick_right",
};
static char g_path[1024];

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

int settings_window_height(int window_size) {
    static const int k_h[SETTINGS_WINDOW_SIZES] = {480, 720, 900, 1080, 1440, 2160};
    return k_h[clampi(window_size, 1, SETTINGS_WINDOW_SIZES) - 1];
}

bool settings_load(const char* path) {
    if (!path || !*path) return false;
    snprintf(g_path, sizeof(g_path), "%s", path);
    FILE* f = fopen(path, "r");
    if (!f) return false;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char* eq = strchr(line, '=');
        if (line[0] == '#' || !eq) continue;
        *eq = 0;
        char* key = line;
        char* val = eq + 1;
        val[strcspn(val, "\r\n")] = 0;
        Settings* s = &g_settings;
        if (strcmp(key, "renderer") == 0) s->renderer = strcmp(val, "software") == 0;
        else if (strcmp(key, "resolution") == 0) s->resolution = clampi(atoi(val), 1, 8);
        else if (strcmp(key, "screen_shape") == 0)
            s->screen_shape = strcmp(val, "4:3") == 0 ? 1 : strcmp(val, "16:9") == 0 ? 2 : strcmp(val, "21:9") == 0 ? 3 : strcmp(val, "32:9") == 0 ? 4 : 0;
        else if (strcmp(key, "fullscreen") == 0) s->fullscreen = atoi(val) != 0;
        else if (strcmp(key, "window_size") == 0) s->window_size = clampi(atoi(val), 1, SETTINGS_WINDOW_SIZES);
        else if (strcmp(key, "custom_textures") == 0) s->custom_textures = atoi(val) != 0;
        else if (strcmp(key, "keyboard_player") == 0) s->keyboard_player = clampi(atoi(val), 1, 4) - 1;
        else if (strcmp(key, "nintendo_labels") == 0) s->nintendo_labels = atoi(val) != 0;
        else if (strcmp(key, "ps2_rain") == 0) { int v = atoi(val); s->ps2_rain = v < 0 ? 0 : v > 2 ? 2 : v; }
        else if (strcmp(key, "screen_filters") == 0) s->screen_filters = atoi(val) != 0;
        else if (strcmp(key, "skip_intros") == 0) s->skip_intros = atoi(val) != 0;
        else if (strcmp(key, "specular") == 0) { int v = atoi(val); s->specular = v < 0 ? 0 : v > 2 ? 2 : v; }
        else if (strcmp(key, "net_delay") == 0) s->net_delay = clampi(atoi(val), 1, 12);
        else if (strcmp(key, "net_address") == 0) snprintf(s->net_address, sizeof(s->net_address), "%s", val);
        else if (strncmp(key, "key_", 4) == 0 || strncmp(key, "pad_", 4) == 0) {
            for (int i = 0; i < GCI_COUNT; i++)
                if (strcmp(key + 4, k_gc_input_names[i]) == 0) {
                    if (key[0] == 'k') s->key_bind[i] = atoi(val);
                    else s->pad_bind[i] = atoi(val);
                }
        }
    }
    fclose(f);
    return true;
}

void settings_save(void) {
    if (!g_path[0]) return;
    FILE* f = fopen(g_path, "w");
    if (!f) {
        printf("settings: couldn't write %s\n", g_path);
        return;
    }
    static const char* const k_shape[] = {"auto", "4:3", "16:9", "21:9", "32:9"};
    const Settings* s = &g_settings;
    fprintf(f, "# NFL Street 2 port settings. Changed from the video menu in the game window (F1).\n");
    fprintf(f, "renderer=%s\n", s->renderer ? "software" : "gpu");
    fprintf(f, "resolution=%d\n", s->resolution);
    fprintf(f, "screen_shape=%s\n", k_shape[clampi(s->screen_shape, 0, 4)]);
    fprintf(f, "fullscreen=%d\n", s->fullscreen);
    fprintf(f, "window_size=%d\n", s->window_size);
    fprintf(f, "custom_textures=%d\n", s->custom_textures);
    fprintf(f, "keyboard_player=%d\n", s->keyboard_player + 1);
    fprintf(f, "nintendo_labels=%d\n", s->nintendo_labels);
    fprintf(f, "ps2_rain=%d\n", s->ps2_rain);
    fprintf(f, "screen_filters=%d\n", s->screen_filters);
    fprintf(f, "skip_intros=%d\n", s->skip_intros);
    fprintf(f, "specular=%d\n", s->specular);
    fprintf(f, "net_delay=%d\n", s->net_delay);
    if (s->net_address[0]) fprintf(f, "net_address=%s\n", s->net_address);
    fprintf(f, "# Controls (set from the menu): key_ = keyboard key code, pad_ = controller button / stick code,\n"
               "# -1 = nothing, 0 = the default.\n");
    for (int i = 0; i < GCI_COUNT; i++) fprintf(f, "key_%s=%d\n", k_gc_input_names[i], s->key_bind[i]);
    for (int i = 0; i < GCI_COUNT; i++) fprintf(f, "pad_%s=%d\n", k_gc_input_names[i], s->pad_bind[i]);
    fclose(f);
}
