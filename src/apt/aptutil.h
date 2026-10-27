#pragma once
/* Small string helpers (the same on every system). */
#include <ctype.h>
#include <stddef.h>
#include <string.h>

static inline int apt_strncasecmp(const char* a, const char* b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        int x = tolower((unsigned char)a[i]), y = tolower((unsigned char)b[i]);
        if (x != y) return x - y;
        if (!x) return 0;
    }
    return 0;
}
static inline int apt_strcasecmp(const char* a, const char* b) { return apt_strncasecmp(a, b, (size_t)-1); }
/* strtok_r */
static inline char* apt_strtok(char* s, const char* delim, char** save) {
    char* p = s ? s : *save;
    if (!p) return NULL;
    p += strspn(p, delim);
    if (!*p) { *save = p; return NULL; }
    char* e = p + strcspn(p, delim);
    if (*e) { *e = 0; *save = e + 1; } else *save = e;
    return p;
}
