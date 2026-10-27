#pragma once
/* Apt movies (EA's Flash-like menus, as on PS2): characters, timelines with
   their scripts, constants, and the movie's shapes and images. */
#include <stdbool.h>
#include <stdint.h>
#include "apt/terf.h"

enum { CH_NONE, CH_SHAPE, CH_TEXT, CH_FONT, CH_BUTTON, CH_SPRITE, CH_IMAGE, CH_MOVIE };
enum { IT_ACTION = 1, IT_LABEL, IT_PLACE, IT_REMOVE, IT_BG, IT_INIT = 8 };

typedef struct { uint32_t flags; uint8_t key; uint32_t code; } AptEvent;

typedef struct {
    uint32_t flags; int depth; int ch;          /* ch -1: none */
    float m[6]; uint8_t tint[4], add[4]; float ratio;
    const char* name; int clipdepth;
    int nevents; AptEvent* events;
} AptPlace;

typedef struct {
    int kind;
    uint32_t code;                              /* action; init: code (sprite in arg) */
    int arg;                                    /* remove: depth; init: sprite */
    const char* label;
    AptPlace* place;
} AptItem;

typedef struct { int n; AptItem* items; } AptFrame;

typedef struct AptChar {
    int type;
    int nframes; AptFrame* frames;              /* sprite, movie */
    /* shape */ float bounds[4]; uint32_t geom;
    /* text */ float rect[4]; int font, align; uint8_t color[4]; float height; int multiline, wordwrap;
    const char* content; const char* var;
    /* font */ const char* fontname;
    /* image */ uint32_t tex;
    /* movie */ int nchars; struct AptChar** chars; int w, h, ms;
    int nimports; struct AptImport* imports;
    int nexports; struct AptExport* exports;
    int nlabels; const char** label_names; int* label_frames;
} AptChar;

typedef struct AptImport { const char* movie; const char* name; int ch; } AptImport;
typedef struct AptExport { const char* name; int ch; } AptExport;

/* geometry record: style 2 solid, 0/1 textured, 3 lines */
typedef struct { uint32_t count, style; float param; uint32_t color, image; float mtx[6]; int npts; float* pts; } AptGeomRec;
typedef struct { int n; AptGeomRec* recs; } AptGeom;
typedef struct { int w, h; uint32_t* px; uint32_t* pm; } AptImage;   /* 0xAARRGGBB; pm: premultiplied */

/* one decoded instruction */
typedef struct {
    uint8_t op;
    uint32_t next;
    int i;                                      /* ints, targets, register, sizes */
    float f;
    const char* s; const char* s2;
    int n; const uint32_t* list;                /* PushData / ConstantPool: constant numbers (raw, LE) */
    /* functions */
    const char* fname; int nargs; int nregs; int fflags; const int* argregs; const char** argnames; int body;
} AptIns;

enum { KC_UNDEF, KC_STR, KC_REG, KC_BOOL, KC_NUM };
typedef struct { int type; const char* s; double n; } AptConst;

typedef struct AptFile {
    char name[48];                              /* "SONMENU/000" */
    const uint8_t* b; uint32_t n;               /* the movie data */
    const uint8_t* c; uint32_t cn;              /* its constants file */
    int nconsts; AptConst* consts;
    AptChar* movie;
    TerfEntry* entry;                           /* the pack entry holding it (resources) */
    int nres; uint32_t* res_cid; uint32_t* res_kind; int* res_entry;
    AptGeom** geoms; AptImage** images;         /* decoded, by resource number */
    /* decoded instructions: open addressing by position */
    uint32_t ins_cap, ins_used; uint32_t* ins_pos; AptIns* ins;
} AptFile;

AptFile* apt_open(TerfEntry* e, const char* name);
const AptIns* apt_ins(AptFile* f, uint32_t pos);
AptGeom* apt_geom(AptFile* f, uint32_t cid);
AptImage* apt_image(AptFile* f, uint32_t cid);
const char* apt_cstr(AptFile* f, uint32_t off);
int apt_label(AptChar* c, const char* label);   /* frame of a label (-1) */
