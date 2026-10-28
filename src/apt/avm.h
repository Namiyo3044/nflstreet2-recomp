#pragma once
/* A small ActionScript 1 player (Flash 6 style, with EA's Apt opcodes) for the
   PS2 version's online menus: runs the movies' scripts so their screens build
   themselves, keeping a display tree the renderer draws (aptdraw.h).
   Property and variable names are case-insensitive (Flash 6 and Apt). Memory
   is garbage collected between frames (vm_gc), never while a script runs. */
#include <stdbool.h>
#include <stdint.h>
#include "apt/aptfile.h"

typedef struct VM VM;
typedef struct Obj Obj;

typedef struct Str {
    struct Gc* gc_next; uint8_t gc_mark, gc_kind, pinned, pad;
    uint32_t len, hash;                /* hash of the lower-case text */
    char s[];
} Str;

enum { V_UNDEF, V_NULL, V_BOOL, V_NUM, V_STR, V_OBJ, V_REG };   /* V_REG: a register (constants only) */
typedef struct { uint8_t t; union { double d; int b; Str* s; Obj* o; int r; }; } Val;

typedef Val (*NativeFn)(VM* vm, Val self, int argc, Val* argv, void* data);

enum { OK_PLAIN, OK_FUNC, OK_ARRAY, OK_CLIP, OK_TEXT, OK_SHAPE };

typedef struct { Str* key; Val v; } Prop;
typedef struct { Str* key; Obj* get; Obj* set; } GetSet;

typedef struct {
    NativeFn native; void* data;
    AptFile* file; uint32_t start, end;
    Val* pool; int npool;
    Obj** scope; int nscope;
    int nargs; const char** argnames; const int* argregs; int nregs, flags; bool v2;
    Str* name;
} FuncExt;

typedef struct { Val* items; int n, cap; } ArrExt;

typedef struct { int depth; Obj* o; } Child;

typedef struct {
    Obj* parent; int depth; Str* name;
    float m[6]; bool visible; float alpha;
    int cx[4], cxadd[4];               /* colour transform: multiply (0-255 = 0-1), add */
    AptFile* file; AptChar* ch; int chid, chid_place;
    bool dynamic, removed, is_root;
    float xs, ys, rot;
    int clipdepth;
    /* clip */
    int frame; bool playing; Child* kids; int nkids, capkids;
    AptEvent* events; AptFile* events_file; int nevents;
    /* text */
    Str* text; uint32_t color; Str* var; float rect[4]; float height; int align, font, multiline, wordwrap, autosize; bool html;
} DispExt;

struct Obj {
    struct Gc* gc_next; uint8_t gc_mark, gc_kind, pinned, kind;
    uint8_t catchall;                  /* the engine's objects: any missing function is a logged no-op */
    Obj* proto;
    Prop* props; int nprops, capprops; int* index; int capindex;
    GetSet* gs; int ngs;
    union { FuncExt* fn; ArrExt* arr; DispExt* d; };
    const char* label;                 /* debugging name for catch-all objects */
};

typedef struct {
    int id; Obj* fn; Str* method; Val self; double ms, next; Val* args; int nargs;
} Interval;

typedef struct { char url[256]; int level; Obj* clip; } LoadReq;

/* how the host answers things the scripts ask the engine */
typedef struct {
    AptFile* (*resolve_movie)(VM* vm, const char* url);   /* loadMovie: a URL to a movie */
    AptFile* (*resolve_lib)(VM* vm, const char* name);    /* imports: "eaStd" etc. */
    void (*level_loaded)(VM* vm, const char* url, int level, Obj* clip);
    void (*fscommand)(VM* vm, const char* cmd, const char* arg);
    /* before every script function runs (its name may be NULL): the host can change its arguments */
    void (*on_call)(VM* vm, Obj* fn, Val self, int argc, Val* argv);
    void* user;
} VMHost;

#define VM_LEVELS 32

struct VM {
    VMHost host;
    Obj *glob, *object_proto, *function_proto, *array_proto, *string_proto, *number_proto, *boolean_proto;
    Obj *mc_proto, *tf_proto, *shape_proto;
    Obj* levels[VM_LEVELS];
    Interval* intervals; int nintervals, capintervals, next_interval;
    LoadReq* loads; int nloads, caploads;
    Str** class_names; Obj** class_ctors; int nclasses, capclasses;
    double time_ms;
    int depth;                          /* call depth */
    struct Gc* gc_all; size_t gc_bytes, gc_last;
    int next_id;
    bool flash_mode;                    /* System.capabilities.os set: the scripts' own preview data */
    int logs;
    void (*log)(const char* msg);
    /* the libraries movies import from ("eaStd" etc.), set by the host */
    int nlibs; const char** lib_names; AptFile** libs;
    /* the running script functions' this (newest last; natives don't count) */
    Val this_stack[128]; int nthis;
    Obj* focus;
    bool trace;                         /* log every script call */
    /* keys: the code / character of the press being sent, Key.addListener's listeners */
    int key_code, key_ascii; bool key_down[512];
    Obj** key_listeners; int nkey_listeners, capkey_listeners;
};

/* life */
VM* vm_new(const VMHost* host, bool flash_mode);
void vm_free(VM* vm);
void vm_gc(VM* vm);                     /* between frames */
void vm_tick(VM* vm, double dt_ms);     /* intervals, pending loads, then every level's timelines */
Obj* vm_load_level(VM* vm, AptFile* f, int level, const char* url);
void vm_run_init_actions(VM* vm, AptFile* f, Obj* target);

/* values */
Val v_undef(void); Val v_null(void); Val v_bool(bool b); Val v_num(double d); Val v_obj(Obj* o);
Val v_str(VM* vm, const char* s); Val v_strn(VM* vm, const char* s, size_t n);
Str* str_new(VM* vm, const char* s, size_t n);
bool v_truthy(Val v); double v_num_of(VM* vm, Val v); Str* v_str_of(VM* vm, Val v); int v_int(VM* vm, Val v);
const char* v_cstr(VM* vm, Val v);      /* a C string (valid until the next GC) */

/* objects */
Obj* obj_new(VM* vm, Obj* proto);
Val obj_get(VM* vm, Obj* o, const char* key);
Val obj_get_s(VM* vm, Obj* o, Str* key);
void obj_set(VM* vm, Obj* o, const char* key, Val v);
void obj_set_s(VM* vm, Obj* o, Str* key, Val v);
Obj* native_fn(VM* vm, NativeFn fn, void* data, const char* name);
void obj_set_native(VM* vm, Obj* o, const char* key, NativeFn fn, void* data);
Obj* catchall_new(VM* vm, const char* label);
Obj* arr_new(VM* vm);
void arr_push(VM* vm, Obj* a, Val v);

/* calls */
Val vm_call(VM* vm, Obj* fn, Val self, int argc, Val* argv);
Val vm_call_method(VM* vm, Obj* o, const char* name, int argc, Val* argv);
Obj* vm_resolve_path(VM* vm, const char* path, Obj* base);

/* display tree (for the renderer) */
bool vm_world(Obj* o, float out[6]);    /* o's matrix to the stage */
void vm_bounds(VM* vm, Obj* o, Obj* space, float out[4], bool* any);
void vm_logf(VM* vm, const char* fmt, ...);
/* A button pressed / let go (Key codes: 37-40 the pad's directions, 300 Select,
   301 Start, 302 X, 303 Circle, 304 Triangle, 305 Square, 306-311 L1 R1 L2 R2 L3 R3):
   onKeyDown / onKeyUp for Key's listeners and the highlighted clips. */
void vm_key(VM* vm, int code, int ascii, bool down);
/* a character id in a movie, following imports into the libraries */
AptChar* vm_resolve_char(VM* vm, AptFile* f, int cid, AptFile** of, int* ocid);
/* for the host's boot sequence */
Obj* vm_new_movie_clip(VM* vm, AptFile* f);
void vm_goto_frame(VM* vm, Obj* mc, int fi, bool run_actions);
void vm_add_child(VM* vm, Obj* parent, Obj* child, int depth, const char* name, bool dynamic);
/* a text field's variable: looked up as a script on that clip would */
Val vm_lookup_var(VM* vm, Obj* clip, Str* name);
/* this of the script function `back` calls up (0: the newest), undefined if none */
Val vm_script_this(VM* vm, int back);
/* in the Apt order of a clip's children (by depth) */
static inline int vm_kid_count(Obj* o) { return o->kind == OK_CLIP ? o->d->nkids : 0; }
static inline Obj* vm_kid(Obj* o, int i) { return o->d->kids[i].o; }
