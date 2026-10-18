#include "platform/settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Settings g_settings = {0, 4, 0, 0, 2, 1};
static char g_path[1024];

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

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
            s->screen_shape = strcmp(val, "4:3") == 0 ? 1 : strcmp(val, "16:9") == 0 ? 2 : 0;
        else if (strcmp(key, "fullscreen") == 0) s->fullscreen = atoi(val) != 0;
        else if (strcmp(key, "window_size") == 0) s->window_size = clampi(atoi(val), 1, 4);
        else if (strcmp(key, "custom_textures") == 0) s->custom_textures = atoi(val) != 0;
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
    static const char* const k_shape[] = {"auto", "4:3", "16:9"};
    const Settings* s = &g_settings;
    fprintf(f, "# NFL Street 2 port settings. Changed from the video menu in the game window (F1).\n");
    fprintf(f, "renderer=%s\n", s->renderer ? "software" : "gpu");
    fprintf(f, "resolution=%d\n", s->resolution);
    fprintf(f, "screen_shape=%s\n", k_shape[clampi(s->screen_shape, 0, 2)]);
    fprintf(f, "fullscreen=%d\n", s->fullscreen);
    fprintf(f, "window_size=%d\n", s->window_size);
    fprintf(f, "custom_textures=%d\n", s->custom_textures);
    fclose(f);
}
