#include "apt/aptutil.h"
#include "apt/avm.h"
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct Gc { struct Gc* gc_next; uint8_t gc_mark, gc_kind, pinned, pad; } Gc;
enum { GK_STR, GK_OBJ };

/* ================================================================ logging */
void vm_logf(VM* vm, const char* fmt, ...) {
    if (!vm->log || vm->logs > 4000) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    vm->logs++;
    vm->log(buf);
}

/* ================================================================ strings */
static uint32_t lower_hash(const char* s, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ (uint8_t)tolower((unsigned char)s[i])) * 16777619u;
    return h;
}

static void gc_link(VM* vm, Gc* g, uint8_t kind, size_t bytes) {
    g->gc_next = vm->gc_all; g->gc_mark = 0; g->gc_kind = kind; g->pinned = 0;
    vm->gc_all = g;
    vm->gc_bytes += bytes;
}

Str* str_new(VM* vm, const char* s, size_t n) {
    Str* r = (Str*)malloc(sizeof(Str) + n + 1);
    memcpy(r->s, s, n);
    r->s[n] = 0;
    r->len = (uint32_t)n;
    r->hash = lower_hash(s, n);
    gc_link(vm, (Gc*)r, GK_STR, sizeof(Str) + n + 1);
    return r;
}

static bool str_ieq(const Str* a, const Str* b) {
    if (a == b) return true;
    if (a->len != b->len || a->hash != b->hash) return false;
    for (uint32_t i = 0; i < a->len; i++)
        if (tolower((unsigned char)a->s[i]) != tolower((unsigned char)b->s[i])) return false;
    return true;
}
static bool str_is(const Str* a, const char* b) {
    size_t n = strlen(b);
    if (a->len != n) return false;
    for (size_t i = 0; i < n; i++) if (tolower((unsigned char)a->s[i]) != tolower((unsigned char)b[i])) return false;
    return true;
}

/* constant strings (instructions, constants files, built-in names): made once, kept */
typedef struct { const char* key; Str* s; } CStrSlot;
static CStrSlot* g_cs; static size_t g_cs_cap, g_cs_used;
static VM* g_cs_vm;
static Str* cstr(VM* vm, const char* s) {
    if (!s) s = "";
    if (g_cs_vm != vm) { free(g_cs); g_cs = NULL; g_cs_cap = g_cs_used = 0; g_cs_vm = vm; }
    if (g_cs_used * 2 >= g_cs_cap) {
        size_t oc = g_cs_cap, nc = oc ? oc * 2 : 4096;
        CStrSlot* o = g_cs;
        g_cs = (CStrSlot*)calloc(nc, sizeof(CStrSlot)); g_cs_cap = nc;
        for (size_t i = 0; i < oc; i++) if (o[i].key) {
            size_t h = ((uintptr_t)o[i].key * 2654435761u) & (nc - 1);
            while (g_cs[h].key) h = (h + 1) & (nc - 1);
            g_cs[h] = o[i];
        }
        free(o);
    }
    size_t h = ((uintptr_t)s * 2654435761u) & (g_cs_cap - 1);
    while (g_cs[h].key) {
        if (g_cs[h].key == s) return g_cs[h].s;
        h = (h + 1) & (g_cs_cap - 1);
    }
    Str* r = str_new(vm, s, strlen(s));
    r->pinned = 1;
    g_cs[h].key = s; g_cs[h].s = r; g_cs_used++;
    return r;
}
/* a built-in name: by its text (literal strings in this file are unique pointers per use site,
   so they go through the same table) */
#define S(lit) cstr(vm, lit)

/* ================================================================= values */
Val v_undef(void) { Val v; v.t = V_UNDEF; v.d = 0; return v; }
Val v_null(void) { Val v; v.t = V_NULL; v.d = 0; return v; }
Val v_bool(bool b) { Val v; v.t = V_BOOL; v.b = b; return v; }
Val v_num(double d) { Val v; v.t = V_NUM; v.d = d; return v; }
Val v_obj(Obj* o) { Val v; if (!o) return v_undef(); v.t = V_OBJ; v.o = o; return v; }
static Val v_s(Str* s) { Val v; v.t = V_STR; v.s = s; return v; }
Val v_strn(VM* vm, const char* s, size_t n) { return v_s(str_new(vm, s, n)); }
Val v_str(VM* vm, const char* s) { return v_strn(vm, s, strlen(s)); }

static void num_text(double n, char* out, size_t cap) {
    if (n != n) { snprintf(out, cap, "NaN"); return; }
    if (isinf(n)) { snprintf(out, cap, n > 0 ? "Infinity" : "-Infinity"); return; }
    if (n == floor(n) && fabs(n) < 1e15) { snprintf(out, cap, "%.0f", n); if (!strcmp(out, "-0")) snprintf(out, cap, "0"); return; }
    snprintf(out, cap, "%.15g", n);
}

static Val obj_get_s_impl(VM* vm, Obj* o, Str* key, Obj* self);
static Val call_fn(VM* vm, Obj* fn, Val self, int argc, Val* argv);
static Str* target_path(VM* vm, Obj* o);
static NativeFn g_obj_tostring, g_obj_valueof;

static Val to_prim(VM* vm, Val v) {
    if (v.t != V_OBJ) return v;
    Obj* o = v.o;
    if (o->kind == OK_ARRAY || o->kind == OK_CLIP || o->kind == OK_TEXT) return v_s(v_str_of(vm, v));
    Val vo = obj_get_s(vm, o, S("valueOf"));
    if (vo.t == V_OBJ && vo.o->kind == OK_FUNC && !(vo.o->fn->native == g_obj_valueof)) {
        Val r = call_fn(vm, vo.o, v, 0, NULL);
        if (r.t != V_OBJ) return r;
    }
    return v_s(v_str_of(vm, v));
}

double v_num_of(VM* vm, Val v) {
    switch (v.t) {
    case V_UNDEF: return NAN;
    case V_NULL: return 0;
    case V_BOOL: return v.b ? 1 : 0;
    case V_NUM: return v.d;
    case V_STR: {
        const char* s = v.s->s;
        while (isspace((unsigned char)*s)) s++;
        if (!*s) return 0;
        char* end;
        double d;
        if ((s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))) d = (double)strtoll(s + 2, &end, 16);
        else d = strtod(s, &end);
        while (isspace((unsigned char)*end)) end++;
        return *end ? NAN : d;
    }
    case V_OBJ: {
        Val vo = obj_get_s(vm, v.o, S("valueOf"));
        if (vo.t == V_OBJ && vo.o->kind == OK_FUNC && vo.o->fn->native != g_obj_valueof) {
            Val r = call_fn(vm, vo.o, v, 0, NULL);
            if (r.t != V_OBJ) return v_num_of(vm, r);
        }
        return NAN;
    }
    default: return NAN;
    }
}

int v_int(VM* vm, Val v) {
    double d = v_num_of(vm, v);
    if (d != d || isinf(d)) return 0;
    if (d > 2147483647.0) return 2147483647;
    if (d < -2147483648.0) return (-2147483647 - 1);
    return (int)d;
}
static int32_t v_i32(VM* vm, Val v) {
    double d = v_num_of(vm, v);
    if (d != d || isinf(d)) return 0;
    double m = fmod(trunc(d), 4294967296.0);
    if (m < 0) m += 4294967296.0;
    return (int32_t)(uint32_t)m;
}

Str* v_str_of(VM* vm, Val v) {
    char buf[64];
    switch (v.t) {
    case V_UNDEF: return S("undefined");
    case V_NULL: return S("null");
    case V_BOOL: return v.b ? S("true") : S("false");
    case V_NUM: num_text(v.d, buf, sizeof buf); return str_new(vm, buf, strlen(buf));
    case V_STR: return v.s;
    case V_OBJ: {
        Obj* o = v.o;
        if (o->kind == OK_CLIP) return target_path(vm, o);
        if (o->kind == OK_ARRAY) {
            size_t cap = 64, n = 0; char* out = (char*)malloc(cap);
            for (int i = 0; i < o->arr->n; i++) {
                Val x = o->arr->items[i];
                Str* s = (x.t == V_UNDEF || x.t == V_NULL) ? S("") : v_str_of(vm, x);
                if (n + s->len + 2 > cap) { cap = (n + s->len + 2) * 2; out = (char*)realloc(out, cap); }
                if (i) out[n++] = ',';
                memcpy(out + n, s->s, s->len); n += s->len;
            }
            Str* r = str_new(vm, out, n); free(out); return r;
        }
        Val ts = obj_get_s(vm, o, S("toString"));
        if (ts.t == V_OBJ && ts.o->kind == OK_FUNC && ts.o->fn->native != g_obj_tostring) {
            Val r = call_fn(vm, ts.o, v, 0, NULL);
            if (r.t != V_OBJ) return v_str_of(vm, r);
        }
        if (o->kind == OK_FUNC) return S("[type Function]");
        return S("[object Object]");
    }
    default: return S("");
    }
}
const char* v_cstr(VM* vm, Val v) { return v_str_of(vm, v)->s; }

bool v_truthy(Val v) {
    switch (v.t) {
    case V_BOOL: return v.b != 0;
    case V_NUM: return v.d == v.d && v.d != 0;
    case V_STR: return v.s->len != 0;
    case V_OBJ: return true;
    default: return false;
    }
}

static bool v_eq2(VM* vm, Val a, Val b) {
    if (a.t == V_BOOL) a = v_num(a.b ? 1 : 0);
    if (b.t == V_BOOL) b = v_num(b.b ? 1 : 0);
    bool an = a.t == V_UNDEF || a.t == V_NULL, bn = b.t == V_UNDEF || b.t == V_NULL;
    if (an || bn) return an && bn;
    if (a.t == V_NUM && b.t == V_NUM) return a.d == b.d;
    if (a.t == V_STR && b.t == V_STR) return str_ieq(a.s, b.s) && !strcmp(a.s->s, b.s->s);
    if (a.t == V_OBJ && b.t == V_OBJ) return a.o == b.o;
    if (a.t == V_OBJ) a = b.t == V_NUM ? v_num(v_num_of(vm, a)) : v_s(v_str_of(vm, a));
    if (b.t == V_OBJ) b = a.t == V_NUM ? v_num(v_num_of(vm, b)) : v_s(v_str_of(vm, b));
    if (a.t == V_STR && b.t == V_NUM) return v_num_of(vm, a) == b.d;
    if (b.t == V_STR && a.t == V_NUM) return v_num_of(vm, b) == a.d;
    if (a.t == V_STR && b.t == V_STR) return !strcmp(a.s->s, b.s->s);
    return false;
}
static bool v_strict(Val a, Val b) {
    if (a.t != b.t) return false;
    switch (a.t) {
    case V_UNDEF: case V_NULL: return true;
    case V_BOOL: return (a.b != 0) == (b.b != 0);
    case V_NUM: return a.d == b.d;
    case V_STR: return a.s == b.s || !strcmp(a.s->s, b.s->s);
    case V_OBJ: return a.o == b.o;
    default: return false;
    }
}
/* a < b: true, false, or undefined (NaN) */
static Val v_less(VM* vm, Val a, Val b) {
    a = to_prim(vm, a); b = to_prim(vm, b);
    if (a.t == V_STR && b.t == V_STR) return v_bool(strcmp(a.s->s, b.s->s) < 0);
    double x = v_num_of(vm, a), y = v_num_of(vm, b);
    if (x != x || y != y) return v_undef();
    return v_bool(x < y);
}

static const char* type_of(Val v) {
    switch (v.t) {
    case V_UNDEF: return "undefined";
    case V_NULL: return "null";
    case V_BOOL: return "boolean";
    case V_NUM: return "number";
    case V_STR: return "string";
    case V_OBJ: return v.o->kind == OK_CLIP ? "movieclip" : v.o->kind == OK_FUNC ? "function" : "object";
    default: return "undefined";
    }
}

/* ================================================================ objects */
Obj* obj_new(VM* vm, Obj* proto) {
    Obj* o = (Obj*)calloc(1, sizeof *o);
    o->proto = proto;
    gc_link(vm, (Gc*)o, GK_OBJ, sizeof *o + 64);
    return o;
}

static int prop_find(Obj* o, Str* key) {
    if (o->index) {
        int m = o->capindex - 1;
        for (int h = (int)(key->hash & (uint32_t)m);; h = (h + 1) & m) {
            int i = o->index[h];
            if (!i) return -1;
            if (str_ieq(o->props[i - 1].key, key)) return i - 1;
        }
    }
    for (int i = 0; i < o->nprops; i++) if (str_ieq(o->props[i].key, key)) return i;
    return -1;
}
static void index_rebuild(Obj* o) {
    free(o->index);
    o->capindex = 16;
    while (o->capindex < o->nprops * 3) o->capindex *= 2;
    o->index = (int*)calloc((size_t)o->capindex, sizeof(int));
    int m = o->capindex - 1;
    for (int i = 0; i < o->nprops; i++) {
        int h = (int)(o->props[i].key->hash & (uint32_t)m);
        while (o->index[h]) h = (h + 1) & m;
        o->index[h] = i + 1;
    }
}
static void prop_put(Obj* o, Str* key, Val v) {
    int i = prop_find(o, key);
    if (i >= 0) { o->props[i].v = v; return; }
    if (o->nprops == o->capprops) {
        o->capprops = o->capprops ? o->capprops * 2 : 8;
        o->props = (Prop*)realloc(o->props, (size_t)o->capprops * sizeof(Prop));
    }
    o->props[o->nprops].key = key; o->props[o->nprops].v = v; o->nprops++;
    if (o->nprops > 8 && (!o->index || o->nprops * 2 > o->capindex)) index_rebuild(o);
    else if (o->index) {
        int m = o->capindex - 1, h = (int)(key->hash & (uint32_t)m);
        while (o->index[h]) h = (h + 1) & m;
        o->index[h] = o->nprops;
    }
}
static bool prop_del(Obj* o, Str* key) {
    int i = prop_find(o, key);
    if (i < 0) return false;
    memmove(o->props + i, o->props + i + 1, (size_t)(o->nprops - i - 1) * sizeof(Prop));
    o->nprops--;
    if (o->index) index_rebuild(o);
    return true;
}
static bool own_has(Obj* o, Str* key) { return prop_find(o, key) >= 0; }

static GetSet* gs_find(Obj* o, Str* key) {
    for (int i = 0; i < o->ngs; i++) if (str_ieq(o->gs[i].key, key)) return &o->gs[i];
    return NULL;
}

Obj* arr_new(VM* vm) {
    Obj* a = obj_new(vm, vm->array_proto);
    a->kind = OK_ARRAY;
    a->arr = (ArrExt*)calloc(1, sizeof(ArrExt));
    return a;
}
void arr_push(VM* vm, Obj* a, Val v) {
    (void)vm;
    ArrExt* x = a->arr;
    if (x->n == x->cap) { x->cap = x->cap ? x->cap * 2 : 8; x->items = (Val*)realloc(x->items, (size_t)x->cap * sizeof(Val)); }
    x->items[x->n++] = v;
}
static void arr_setlen(Obj* a, int n) {
    ArrExt* x = a->arr;
    if (n < 0) n = 0;
    if (n > 1000000) n = 1000000;
    if (n > x->cap) { x->cap = n; x->items = (Val*)realloc(x->items, (size_t)x->cap * sizeof(Val)); }
    for (int i = x->n; i < n; i++) x->items[i] = v_undef();
    x->n = n;
}
static int str_index(Str* s) {         /* "12" -> 12, else -1 */
    if (!s->len || s->len > 9) return -1;
    int n = 0;
    for (uint32_t i = 0; i < s->len; i++) { if (!isdigit((unsigned char)s->s[i])) return -1; n = n * 10 + (s->s[i] - '0'); }
    return n;
}

Obj* native_fn(VM* vm, NativeFn fn, void* data, const char* name) {
    Obj* f = obj_new(vm, vm->function_proto);
    f->kind = OK_FUNC;
    f->fn = (FuncExt*)calloc(1, sizeof(FuncExt));
    f->fn->native = fn; f->fn->data = data;
    f->fn->name = name ? cstr(vm, name) : NULL;
    return f;
}
void obj_set_native(VM* vm, Obj* o, const char* key, NativeFn fn, void* data) {
    prop_put(o, cstr(vm, key), v_obj(native_fn(vm, fn, data, key)));
}

static Val catchall_fn(VM* vm, Val self, int argc, Val* argv, void* data) {
    (void)self;
    char line[256]; size_t n = 0;
    for (int i = 0; i < argc && i < 4 && n < 180; i++)
        n += (size_t)snprintf(line + n, sizeof line - n, "%s%.40s", i ? ", " : "", argv[i].t == V_OBJ ? "[obj]" : v_cstr(vm, argv[i]));
    line[n] = 0;
    vm_logf(vm, "engine call %s(%s)", (const char*)data, line);
    return v_undef();
}
Obj* catchall_new(VM* vm, const char* label) {
    Obj* o = obj_new(vm, vm->object_proto);
    o->catchall = 1; o->label = label;
    return o;
}

/* ============================================================ display */
static DispExt* disp_new(AptFile* f, AptChar* ch, int chid) {
    DispExt* d = (DispExt*)calloc(1, sizeof(DispExt));
    d->m[0] = d->m[3] = 1; d->visible = true; d->alpha = 100;
    for (int i = 0; i < 4; i++) d->cx[i] = 255;
    d->file = f; d->ch = ch; d->chid = chid; d->chid_place = -1;
    d->xs = d->ys = 100;
    return d;
}
static void disp_set_matrix(DispExt* d, const float* m) {
    memcpy(d->m, m, sizeof d->m);
    d->xs = (float)(hypot(m[0], m[1]) * 100.0); d->ys = (float)(hypot(m[2], m[3]) * 100.0);
    d->rot = (float)(atan2(m[1], m[0]) * 180.0 / M_PI);
}
static void disp_rebuild(DispExt* d) {
    double r = d->rot * M_PI / 180.0, sx = d->xs / 100.0, sy = d->ys / 100.0;
    d->m[0] = (float)(cos(r) * sx); d->m[1] = (float)(sin(r) * sx);
    d->m[2] = (float)(-sin(r) * sy); d->m[3] = (float)(cos(r) * sy);
}
static bool is_disp(Obj* o) { return o && (o->kind == OK_CLIP || o->kind == OK_TEXT || o->kind == OK_SHAPE); }

static Obj* disp_root(Obj* o) {
    while (o->d->parent && !o->d->is_root) o = o->d->parent;
    return o;
}

static Str* target_path(VM* vm, Obj* o) {
    char buf[512]; int n = 0;
    Obj* chain[32]; int k = 0;
    for (Obj* x = o; x && k < 32; x = x->d->parent) { chain[k++] = x; if (!x->d->parent) break; }
    for (int i = k - 1; i >= 0; i--) {
        Obj* x = chain[i];
        const char* nm = x->d->name ? x->d->name->s : "";
        if (!x->d->parent) n += snprintf(buf + n, sizeof buf - (size_t)n, "%s", nm[0] ? nm : "_level0");
        else n += snprintf(buf + n, sizeof buf - (size_t)n, ".%s", nm);
        if (n >= (int)sizeof buf - 1) break;
    }
    return str_new(vm, buf, strlen(buf));
}

static Obj* child_named(Obj* clip, Str* name) {
    DispExt* d = clip->d;
    for (int i = 0; i < d->nkids; i++) {
        Obj* c = d->kids[i].o;
        if (c->d->name && str_ieq(c->d->name, name)) return c;
    }
    return NULL;
}
static Obj* child_at(Obj* clip, int depth) {
    DispExt* d = clip->d;
    for (int i = 0; i < d->nkids; i++) if (d->kids[i].depth == depth) return d->kids[i].o;
    return NULL;
}

static void mul6(const float* a, const float* b, float* out) {
    float r[6];
    r[0] = a[0] * b[0] + a[2] * b[1]; r[1] = a[1] * b[0] + a[3] * b[1];
    r[2] = a[0] * b[2] + a[2] * b[3]; r[3] = a[1] * b[2] + a[3] * b[3];
    r[4] = a[0] * b[4] + a[2] * b[5] + a[4]; r[5] = a[1] * b[4] + a[3] * b[5] + a[5];
    memcpy(out, r, sizeof r);
}
static void inv6(const float* m, float* out) {
    double det = (double)m[0] * m[3] - (double)m[1] * m[2];
    if (fabs(det) < 1e-12) { float id[6] = {1, 0, 0, 1, 0, 0}; memcpy(out, id, sizeof id); return; }
    double a = m[3] / det, b = -m[1] / det, c = -m[2] / det, d = m[0] / det;
    out[0] = (float)a; out[1] = (float)b; out[2] = (float)c; out[3] = (float)d;
    out[4] = (float)(-(a * m[4] + c * m[5])); out[5] = (float)(-(b * m[4] + d * m[5]));
}
bool vm_world(Obj* o, float out[6]) {
    float m[6] = {1, 0, 0, 1, 0, 0};
    Obj* chain[64]; int k = 0;
    for (Obj* x = o; x && k < 64; x = x->d->parent) chain[k++] = x;
    for (int i = k - 1; i >= 0; i--) mul6(m, chain[i]->d->m, m);
    memcpy(out, m, sizeof m);
    return true;
}

extern void aptdraw_local_bounds(VM* vm, Obj* o, float out[4], bool* any);   /* aptdraw.c */
extern void aptdraw_text_size(VM* vm, Obj* o, float* w, float* h);
void vm_bounds(VM* vm, Obj* o, Obj* space, float out[4], bool* any) {
    float b[4]; bool have = false;
    aptdraw_local_bounds(vm, o, b, &have);
    *any = have;
    if (!have) { out[0] = out[1] = out[2] = out[3] = 0; return; }
    if (!space || space == o) { memcpy(out, b, sizeof b); return; }
    float ws[6], wo[6], is[6], m[6];
    vm_world(space, ws); vm_world(o, wo); inv6(ws, is); mul6(is, wo, m);
    float xs[4] = {b[0], b[2], b[0], b[2]}, ys[4] = {b[1], b[1], b[3], b[3]};
    out[0] = out[1] = 1e30f; out[2] = out[3] = -1e30f;
    for (int i = 0; i < 4; i++) {
        float x = m[0] * xs[i] + m[2] * ys[i] + m[4], y = m[1] * xs[i] + m[3] * ys[i] + m[5];
        if (x < out[0]) out[0] = x;
        if (y < out[1]) out[1] = y;
        if (x > out[2]) out[2] = x;
        if (y > out[3]) out[3] = y;
    }
}

static Val norm_num(double d) { return v_num(d); }

/* display properties (_x etc.); false if not one */
static bool disp_get(VM* vm, Obj* o, Str* key, Val* out) {
    if (!key->len || key->s[0] != '_') return false;
    DispExt* d = o->d;
    const char* k = key->s;
#define IS(x) str_is(key, x)
    if (IS("_x")) { *out = norm_num(d->m[4]); return true; }
    if (IS("_y")) { *out = norm_num(d->m[5]); return true; }
    if (IS("_xscale")) { *out = norm_num(d->xs); return true; }
    if (IS("_yscale")) { *out = norm_num(d->ys); return true; }
    if (IS("_alpha")) { *out = norm_num(d->alpha); return true; }
    if (IS("_visible")) { *out = v_bool(d->visible); return true; }
    if (IS("_rotation")) { *out = norm_num(d->rot); return true; }
    if (IS("_name")) { *out = d->name ? v_s(d->name) : v_s(S("")); return true; }
    if (IS("_parent")) { *out = d->parent ? v_obj(d->parent) : v_undef(); return true; }
    if (IS("_root")) { *out = v_obj(disp_root(o)); return true; }
    if (IS("_global")) { *out = v_obj(vm->glob); return true; }
    if (IS("_target")) { *out = v_s(target_path(vm, o)); return true; }
    if (IS("_currentframe")) { *out = v_num(o->kind == OK_CLIP ? d->frame + 1 : 1); return true; }
    if (IS("_totalframes") || IS("_framesloaded")) { *out = v_num(o->kind == OK_CLIP && d->ch ? (d->ch->nframes ? d->ch->nframes : 1) : 1); return true; }
    if (IS("_width") || IS("_height")) {
        float b[4]; bool any;
        vm_bounds(vm, o, d->parent, b, &any);
        *out = v_num(any ? (IS("_width") ? b[2] - b[0] : b[3] - b[1]) : 0);
        return true;
    }
    if (IS("_xmouse") || IS("_ymouse")) { *out = v_num(0); return true; }
    if (!strncmp(k, "_level", 6) && isdigit((unsigned char)k[6])) {
        int lv = atoi(k + 6);
        *out = lv >= 0 && lv < VM_LEVELS && vm->levels[lv] ? v_obj(vm->levels[lv]) : v_undef();
        return true;
    }
#undef IS
    return false;
}
static bool disp_set(VM* vm, Obj* o, Str* key, Val v) {
    if (!key->len || key->s[0] != '_') return false;
    DispExt* d = o->d;
#define IS(x) str_is(key, x)
    bool num = IS("_x") || IS("_y") || IS("_xscale") || IS("_yscale") || IS("_rotation") || IS("_alpha") || IS("_width") || IS("_height");
    double n = num ? v_num_of(vm, v) : 0;
    if (num && (n != n || isinf(n))) return true;          /* NaN: ignored */
    if (IS("_x")) { d->m[4] = (float)n; return true; }
    if (IS("_y")) { d->m[5] = (float)n; return true; }
    if (IS("_xscale")) { d->xs = (float)n; disp_rebuild(d); return true; }
    if (IS("_yscale")) { d->ys = (float)n; disp_rebuild(d); return true; }
    if (IS("_rotation")) { d->rot = (float)n; disp_rebuild(d); return true; }
    if (IS("_alpha")) { d->alpha = (float)n; return true; }
    if (IS("_visible")) { d->visible = v_truthy(v); return true; }
    if (IS("_name")) { d->name = v_str_of(vm, v); return true; }
    if (IS("_width") || IS("_height")) {
        if (o->kind == OK_TEXT) {
            if (IS("_width")) d->rect[2] = d->rect[0] + (float)n; else d->rect[3] = d->rect[1] + (float)n;
            return true;
        }
        float b[4]; bool any;
        vm_bounds(vm, o, d->parent, b, &any);
        if (any) {
            double cur = IS("_width") ? b[2] - b[0] : b[3] - b[1];
            if (cur > 0) { if (IS("_width")) d->xs = (float)(d->xs * n / cur); else d->ys = (float)(d->ys * n / cur); disp_rebuild(d); }
        }
        return true;
    }
    if (IS("_currentframe") || IS("_totalframes") || IS("_framesloaded") || IS("_target") || IS("_parent") || IS("_root")) return true;
#undef IS
    return false;
}

static Str* strip_html(VM* vm, Str* s);

static bool text_get(VM* vm, Obj* o, Str* key, Val* out) {
    DispExt* d = o->d;
    if (str_is(key, "text") || str_is(key, "htmlText")) { *out = v_s(d->text ? d->text : S("")); return true; }
    if (str_is(key, "textColor")) { *out = v_num(d->color); return true; }
    if (str_is(key, "textWidth") || str_is(key, "textHeight")) {
        float w, h;
        aptdraw_text_size(vm, o, &w, &h);
        *out = v_num(str_is(key, "textWidth") ? w : h);
        return true;
    }
    if (str_is(key, "length")) { *out = v_num(d->text ? d->text->len : 0); return true; }
    if (str_is(key, "variable")) { *out = d->var ? v_s(d->var) : v_null(); return true; }
    if (str_is(key, "_width")) {
        if (d->autosize) { float w, h; aptdraw_text_size(vm, o, &w, &h); *out = v_num((w + 4) * d->xs / 100.0); return true; }
        *out = v_num((d->rect[2] - d->rect[0]) * d->xs / 100.0);
        return true;
    }
    if (str_is(key, "_height")) { *out = v_num((d->rect[3] - d->rect[1]) * d->ys / 100.0); return true; }
    return disp_get(vm, o, key, out);
}
static bool text_set(VM* vm, Obj* o, Str* key, Val v) {
    DispExt* d = o->d;
    if (str_is(key, "text") || str_is(key, "htmlText")) {
        Str* t = v_str_of(vm, v);
        if (str_is(key, "htmlText") || d->html) t = strip_html(vm, t);
        d->text = t;
        return true;
    }
    if (str_is(key, "textColor")) { d->color = (uint32_t)v_int(vm, v); return true; }
    if (str_is(key, "html")) { d->html = v_truthy(v); return true; }
    if (str_is(key, "variable")) { d->var = v_str_of(vm, v); return true; }
    if (str_is(key, "autoSize")) {           /* (kept as a property too) */
        d->autosize = v.t == V_BOOL ? v.b : v.t == V_STR ? !str_is(v.s, "none") && v.s->len : 0;
        return false;
    }
    return disp_set(vm, o, key, v);
}

/* ------------------------------------------------------------ get / set */
static Val obj_get_s_impl(VM* vm, Obj* o, Str* key, Obj* self) {
    Obj* x = o;
    for (int n = 0; x && n < 64; x = x->proto, n++) {
        if (x->ngs) {
            GetSet* g = gs_find(x, key);
            if (g) return g->get ? call_fn(vm, g->get, v_obj(self), 0, NULL) : v_undef();
        }
        int i = prop_find(x, key);
        if (i >= 0) return x->props[i].v;
    }
    if (str_is(key, "__proto__")) return o->proto ? v_obj(o->proto) : v_undef();
    if (o->catchall && key->len) {
        Obj* f = native_fn(vm, catchall_fn, NULL, NULL);
        f->fn->name = key;
        char* nm = (char*)malloc(key->len + 1);           /* kept: one per engine function used */
        memcpy(nm, key->s, key->len + 1);
        f->fn->data = nm;
        prop_put(o, key, v_obj(f));
        return v_obj(f);
    }
    return v_undef();
}

Val obj_get_s(VM* vm, Obj* o, Str* key) {
    if (!o) return v_undef();
    switch (o->kind) {
    case OK_ARRAY: {
        if (str_is(key, "length")) return v_num(o->arr->n);
        int i = str_index(key);
        if (i >= 0) return i < o->arr->n ? o->arr->items[i] : v_undef();
        break;
    }
    case OK_FUNC:
        if (str_is(key, "prototype") && !own_has(o, key)) {
            Obj* p = obj_new(vm, vm->object_proto);
            prop_put(p, S("constructor"), v_obj(o));
            prop_put(o, S("prototype"), v_obj(p));
        }
        break;
    case OK_CLIP: {
        Val r;
        if (disp_get(vm, o, key, &r)) return r;
        int i = prop_find(o, key);
        if (i >= 0) return o->props[i].v;
        Obj* c = child_named(o, key);
        if (c) return v_obj(c);
        break;
    }
    case OK_TEXT: {
        Val r;
        if (text_get(vm, o, key, &r)) return r;
        break;
    }
    case OK_SHAPE: {
        Val r;
        if (disp_get(vm, o, key, &r)) return r;
        break;
    }
    default: break;
    }
    return obj_get_s_impl(vm, o, key, o);
}
Val obj_get(VM* vm, Obj* o, const char* key) { return obj_get_s(vm, o, str_new(vm, key, strlen(key))); }

static bool obj_has(VM* vm, Obj* o, Str* key) {
    (void)vm;
    if (o->kind == OK_CLIP && child_named(o, key)) return true;
    if (o->catchall) return true;
    for (Obj* x = o; x; x = x->proto) {
        if (own_has(x, key)) return true;
        if (x->ngs && gs_find(x, key)) return true;
    }
    return false;
}

void obj_set_s(VM* vm, Obj* o, Str* key, Val v) {
    if (!o) return;
    switch (o->kind) {
    case OK_ARRAY: {
        if (str_is(key, "length")) { arr_setlen(o, v_int(vm, v)); return; }
        int i = str_index(key);
        if (i >= 0) { if (i >= o->arr->n) arr_setlen(o, i + 1); o->arr->items[i] = v; return; }
        break;
    }
    case OK_CLIP: case OK_SHAPE:
        if (disp_set(vm, o, key, v)) return;
        break;
    case OK_TEXT:
        if (text_set(vm, o, key, v)) return;
        break;
    default: break;
    }
    if (str_is(key, "__proto__")) { o->proto = v.t == V_OBJ ? v.o : NULL; return; }
    for (Obj* x = o; x; x = x->proto) {
        if (x->ngs) {
            GetSet* g = gs_find(x, key);
            if (g) { if (g->set) call_fn(vm, g->set, v_obj(o), 1, &v); return; }
        }
    }
    prop_put(o, key, v);
}
void obj_set(VM* vm, Obj* o, const char* key, Val v) { obj_set_s(vm, o, str_new(vm, key, strlen(key)), v); }

/* names to enumerate (for..in): own and inherited, children of clips */
static int obj_keys(VM* vm, Obj* o, Str** out, int cap) {
    int n = 0;
    if (o->kind == OK_ARRAY) {
        for (int i = 0; i < o->arr->n && n < cap; i++) { char b[16]; snprintf(b, sizeof b, "%d", i); out[n++] = str_new(vm, b, strlen(b)); }
    }
    for (Obj* x = o; x && n < cap; x = x->proto) {
        for (int i = 0; i < x->nprops && n < cap; i++) {
            Str* k = x->props[i].key;
            if (k->len >= 2 && k->s[0] == '_' && k->s[1] == '_') continue;
            bool dup = false;
            for (int j = 0; j < n; j++) if (str_ieq(out[j], k)) { dup = true; break; }
            if (!dup) out[n++] = k;
        }
        if (x == vm->object_proto || x == vm->mc_proto || x == vm->array_proto || x == vm->function_proto) break;
    }
    if (o->kind == OK_CLIP)
        for (int i = 0; i < o->d->nkids && n < cap; i++) if (o->d->kids[i].o->d->name) out[n++] = o->d->kids[i].o->d->name;
    return n;
}

static Str* strip_html(VM* vm, Str* s) {
    char* out = (char*)malloc(s->len + 1); size_t n = 0;
    for (uint32_t i = 0; i < s->len;) {
        if (s->s[i] == '<') {
            const char* e = strchr(s->s + i, '>');
            if (!e) break;
            if (!apt_strncasecmp(s->s + i, "<br", 3) || !apt_strncasecmp(s->s + i, "</p", 3)) out[n++] = '\n';
            i = (uint32_t)(e - s->s) + 1;
            continue;
        }
        if (s->s[i] == '&') {
            static const struct { const char* e; char c; } k[] = {{"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}};
            bool done = false;
            for (int j = 0; j < 5; j++) {
                size_t l = strlen(k[j].e);
                if (!strncmp(s->s + i, k[j].e, l)) { out[n++] = k[j].c; i += (uint32_t)l; done = true; break; }
            }
            if (done) continue;
        }
        out[n++] = s->s[i++];
    }
    while (n && out[n - 1] == '\n') n--;
    Str* r = str_new(vm, out, n);
    free(out);
    return r;
}

/* ============================================================ execution */
typedef struct Ctx {
    AptFile* file;
    Val* pool; int npool;
    Val* regs; int nregs;
    Obj** scope; int nscope, capscope;
    Val self;
    Obj* target;
    Val* st; int sp, capst;
    Obj* act;
    Obj* func;
    bool returned; Val ret;
} Ctx;

static void push(Ctx* c, Val v) {
    if (c->sp == c->capst) { c->capst = c->capst ? c->capst * 2 : 32; c->st = (Val*)realloc(c->st, (size_t)c->capst * sizeof(Val)); }
    c->st[c->sp++] = v;
}
static Val pop(Ctx* c) { return c->sp ? c->st[--c->sp] : v_undef(); }
static void scope_push(Ctx* c, Obj* o) {
    if (c->nscope == c->capscope) { c->capscope = c->capscope ? c->capscope * 2 : 8; c->scope = (Obj**)realloc(c->scope, (size_t)c->capscope * sizeof(Obj*)); }
    c->scope[c->nscope++] = o;
}
static Val reg_get(Ctx* c, int r) { return r >= 0 && r < c->nregs ? c->regs[r] : v_undef(); }
static void ctx_free(Ctx* c) { free(c->st); free(c->scope); free(c->regs); free(c->pool); }

static Val const_val(VM* vm, AptFile* f, uint32_t idx) {
    if (idx >= (uint32_t)f->nconsts) return v_undef();
    AptConst* k = &f->consts[idx];
    switch (k->type) {
    case KC_STR: return v_s(cstr(vm, k->s));
    case KC_REG: { Val v; v.t = V_REG; v.r = (int)k->n; return v; }
    case KC_BOOL: return v_bool(k->n != 0);
    case KC_NUM: return v_num(k->n);
    default: return v_undef();
    }
}

static Obj* root_of(VM* vm, Obj* t) { return t && is_disp(t) ? disp_root(t) : vm->levels[0]; }

static void execute(VM* vm, Ctx* c, uint32_t pc, uint32_t end);
static Val call_fn(VM* vm, Obj* fn, Val self, int argc, Val* argv);
static void remove_child(VM* vm, Obj* parent, int depth);
static void goto_clip(VM* vm, Obj* mc, Val frame, bool play);
static Obj* attach(VM* vm, Obj* parent, Str* link, Str* name, int depth, Obj* init);
static void geturl(VM* vm, const char* url, const char* target, Obj* base);

/* ---- paths and variables */
Obj* vm_resolve_path(VM* vm, const char* path, Obj* base) {
    if (!path || !*path) return base;
    char buf[256];
    snprintf(buf, sizeof buf, "%s", path);
    Obj* o = base;
    char* p = buf;
    if (*p == '/') { o = vm->levels[0]; p++; }
    char* save = NULL;
    for (char* part = apt_strtok(p, "./", &save); part; part = apt_strtok(NULL, "./", &save)) {
        if (!strcmp(part, "_root")) o = root_of(vm, base);
        else if (!strncmp(part, "_level", 6) && isdigit((unsigned char)part[6])) { int lv = atoi(part + 6); o = lv < VM_LEVELS ? vm->levels[lv] : NULL; }
        else if (!strcmp(part, "_parent") || !strcmp(part, "..")) o = o && is_disp(o) ? o->d->parent : NULL;
        else if (!strcmp(part, "this")) {}
        else if (!strcmp(part, "_global")) o = vm->glob;
        else {
            if (!o) return NULL;
            Val v = obj_get_s(vm, o, str_new(vm, part, strlen(part)));
            if (v.t != V_OBJ && o == base) v = obj_get_s(vm, vm->glob, str_new(vm, part, strlen(part)));
            o = v.t == V_OBJ ? v.o : NULL;
        }
        if (!o) return NULL;
    }
    return o;
}

static Val lookup(VM* vm, Ctx* c, Str* name) {
    const char* s = name->s;
    if (strchr(s, ':')) {
        const char* colon = strrchr(s, ':');
        char path[256]; size_t n = (size_t)(colon - s); if (n >= sizeof path) n = sizeof path - 1;
        memcpy(path, s, n); path[n] = 0;
        Obj* o = vm_resolve_path(vm, path, c->target);
        return o ? obj_get_s(vm, o, str_new(vm, colon + 1, strlen(colon + 1))) : v_undef();
    }
    if (strchr(s, '.') || (s[0] != '/' && strchr(s, '/'))) {
        char buf[256]; snprintf(buf, sizeof buf, "%s", s);
        char* save = NULL; char* part = apt_strtok(buf, "./", &save);
        Val v = part ? lookup(vm, c, str_new(vm, part, strlen(part))) : v_undef();
        for (part = apt_strtok(NULL, "./", &save); part; part = apt_strtok(NULL, "./", &save))
            v = v.t == V_OBJ ? obj_get_s(vm, v.o, str_new(vm, part, strlen(part))) : v_undef();
        return v;
    }
    if (str_is(name, "this")) return c->self;
    if (str_is(name, "_root")) return v_obj(root_of(vm, c->target));
    if (!apt_strncasecmp(s, "_level", 6) && isdigit((unsigned char)s[6])) { int lv = atoi(s + 6); return lv < VM_LEVELS && vm->levels[lv] ? v_obj(vm->levels[lv]) : v_undef(); }
    if (str_is(name, "_global")) return v_obj(vm->glob);
    if (str_is(name, "_parent")) return c->target && is_disp(c->target) && c->target->d->parent ? v_obj(c->target->d->parent) : v_undef();
    for (int i = c->nscope - 1; i >= 0; i--) {
        Obj* sc = c->scope[i];
        if (sc->kind == OK_CLIP) {
            int k = prop_find(sc, name);
            if (k >= 0) return sc->props[k].v;
            Val r;
            if (s[0] == '_' && disp_get(vm, sc, name, &r)) return r;
            Obj* ch = child_named(sc, name);
            if (ch) return v_obj(ch);
            for (Obj* x = sc->proto; x; x = x->proto) if (own_has(x, name) || (x->ngs && gs_find(x, name))) return obj_get_s_impl(vm, sc, name, sc);
        } else if (obj_has(vm, sc, name)) return obj_get_s(vm, sc, name);
    }
    if (!strncmp(s, "GM_", 3)) return v_s(name);       /* the engine's message numbers: by name */
    if (isupper((unsigned char)s[0])) vm_logf(vm, "undefined variable %s", s);
    return v_undef();
}

static void name_fn(Val v, Str* name) {
    if (v.t == V_OBJ && v.o->kind == OK_FUNC && !v.o->fn->name) v.o->fn->name = name;
}

static void setvar(VM* vm, Ctx* c, Str* name, Val v) {
    name_fn(v, name);
    const char* s = name->s;
    if (strchr(s, ':')) {
        const char* colon = strrchr(s, ':');
        char path[256]; size_t n = (size_t)(colon - s); if (n >= sizeof path) n = sizeof path - 1;
        memcpy(path, s, n); path[n] = 0;
        Obj* o = vm_resolve_path(vm, path, c->target);
        if (o) obj_set_s(vm, o, str_new(vm, colon + 1, strlen(colon + 1)), v);
        return;
    }
    if (strchr(s, '.')) {
        char buf[256]; snprintf(buf, sizeof buf, "%s", s);
        char* last = strrchr(buf, '.'); *last = 0;
        Val o = lookup(vm, c, str_new(vm, buf, strlen(buf)));
        if (o.t == V_OBJ) obj_set_s(vm, o.o, str_new(vm, last + 1, strlen(last + 1)), v);
        return;
    }
    for (int i = c->nscope - 1; i >= 0; i--) {
        Obj* sc = c->scope[i];
        if (sc == vm->glob || sc->kind == OK_CLIP) break;
        if (own_has(sc, name)) { prop_put(sc, name, v); return; }
    }
    for (int i = c->nscope - 1; i >= 0; i--) {
        Obj* sc = c->scope[i];
        if (sc == c->act || sc == vm->glob) continue;
        if (sc->kind == OK_CLIP) break;
        if (obj_has(vm, sc, name)) { obj_set_s(vm, sc, name, v); return; }
    }
    if (c->target) obj_set_s(vm, c->target, name, v);
}

static void define_local(VM* vm, Ctx* c, Str* name, Val v) {
    name_fn(v, name);
    if (c->act) prop_put(c->act, name, v);
    else if (c->target) obj_set_s(vm, c->target, name, v);
}

static Val get_member(VM* vm, Val o, Val name) {
    switch (o.t) {
    case V_STR: {
        Str* n = v_str_of(vm, name);
        if (str_is(n, "length")) return v_num(o.s->len);
        return obj_get_s(vm, vm->string_proto, n);
    }
    case V_BOOL: return obj_get_s(vm, vm->boolean_proto, v_str_of(vm, name));
    case V_NUM: return obj_get_s(vm, vm->number_proto, v_str_of(vm, name));
    case V_OBJ:
        if (o.o->kind == OK_ARRAY && name.t == V_NUM && name.d >= 0 && name.d == floor(name.d)) {
            int i = (int)name.d;
            return i < o.o->arr->n ? o.o->arr->items[i] : v_undef();
        }
        return obj_get_s(vm, o.o, v_str_of(vm, name));
    default: return v_undef();
    }
}
static void set_member(VM* vm, Val o, Val name, Val v) {
    if (o.t != V_OBJ) return;
    Str* n = v_str_of(vm, name);
    name_fn(v, n);
    if (o.o->kind == OK_ARRAY && name.t == V_NUM && name.d >= 0 && name.d == floor(name.d)) {
        int i = (int)name.d;
        if (i >= o.o->arr->n) arr_setlen(o.o, i + 1);
        o.o->arr->items[i] = v;
        return;
    }
    obj_set_s(vm, o.o, n, v);
}

static const char* describe(VM* vm, Val v) {
    if (v.t == V_OBJ && is_disp(v.o)) return target_path(vm, v.o)->s;
    if (v.t == V_OBJ) return v.o->kind == OK_FUNC ? "function" : "object";
    return v_cstr(vm, v);
}

static Val call_method(VM* vm, Ctx* c, Val o, Val name, int argc, Val* argv) {
    (void)c;
    if (name.t == V_UNDEF || name.t == V_NULL || (name.t == V_STR && name.s->len == 0))
        return o.t == V_OBJ && o.o->kind == OK_FUNC ? call_fn(vm, o.o, v_undef(), argc, argv) : v_undef();
    Val fn = get_member(vm, o, name);
    if (fn.t != V_OBJ || fn.o->kind != OK_FUNC) {
        if (o.t == V_OBJ) vm_logf(vm, "no method %s on %s", v_cstr(vm, name), describe(vm, o));
        else vm_logf(vm, "call %s on %s", v_cstr(vm, name), v_cstr(vm, o));
        return v_undef();
    }
    return call_fn(vm, fn.o, o, argc, argv);
}

static Val call_named(VM* vm, Ctx* c, Str* name, int argc, Val* argv) {
    Val fn = lookup(vm, c, name);
    if (fn.t != V_OBJ || fn.o->kind != OK_FUNC) { vm_logf(vm, "no function %s", name->s); return v_undef(); }
    return call_fn(vm, fn.o, c->target ? v_obj(c->target) : v_undef(), argc, argv);
}

static Val construct_new(VM* vm, Val ctor, int argc, Val* argv);

/* ---- calling */
static Val call_fn(VM* vm, Obj* fn, Val self, int argc, Val* argv) {
    if (!fn || fn->kind != OK_FUNC) return v_undef();
    FuncExt* f = fn->fn;
    if (f->native) return f->native(vm, self, argc, argv, f->data);
    if (!f->file) return v_undef();
    if (vm->depth > 120) { vm_logf(vm, "call depth exceeded"); return v_undef(); }
    if (vm->host.on_call) vm->host.on_call(vm, fn, self, argc, argv);
    if (vm->trace) vm_logf(vm, "%*scall %s this=%s", vm->depth, "", f->name ? f->name->s : "?", describe(vm, self));
    Ctx c;
    memset(&c, 0, sizeof c);
    c.file = f->file; c.func = fn;
    c.npool = f->npool;
    c.pool = (Val*)malloc((size_t)(f->npool + 1) * sizeof(Val));
    if (f->npool) memcpy(c.pool, f->pool, (size_t)f->npool * sizeof(Val));
    Obj* act = obj_new(vm, NULL);
    c.act = act;
    int nregs = f->v2 ? f->nregs + 1 : 4;
    if (nregs < 4) nregs = 4;
    c.nregs = nregs;
    c.regs = (Val*)calloc((size_t)nregs, sizeof(Val));
    Obj* args = arr_new(vm);
    for (int i = 0; i < argc; i++) arr_push(vm, args, argv[i]);
    prop_put(args, S("callee"), v_obj(fn));
    if (f->v2) {
        int r = 1, fl = f->flags;
        Obj* tgt = self.t == V_OBJ ? self.o : NULL;
        if ((fl & 1) && r < nregs) c.regs[r++] = self;
        if ((fl & 4) && r < nregs) c.regs[r++] = v_obj(args);
        if ((fl & 0x10) && r < nregs) c.regs[r++] = v_undef();
        if ((fl & 0x40) && r < nregs) c.regs[r++] = v_obj(vm->levels[0]);
        if ((fl & 0x80) && r < nregs) c.regs[r++] = tgt && is_disp(tgt) && tgt->d->parent ? v_obj(tgt->d->parent) : v_undef();
        if ((fl & 0x100) && r < nregs) c.regs[r++] = v_obj(vm->glob);
        for (int i = 0; i < f->nargs; i++) {
            Val v = i < argc ? argv[i] : v_undef();
            int reg = f->argregs ? f->argregs[i] : 0;
            if (reg > 0 && reg < nregs) c.regs[reg] = v;
            else if (f->argnames[i]) prop_put(act, cstr(vm, f->argnames[i]), v);
        }
    } else {
        for (int i = 0; i < f->nargs; i++)
            if (f->argnames[i]) prop_put(act, cstr(vm, f->argnames[i]), i < argc ? argv[i] : v_undef());
    }
    prop_put(act, S("arguments"), v_obj(args));
    prop_put(act, S("this"), self);
    for (int i = 0; i < f->nscope; i++) scope_push(&c, f->scope[i]);
    scope_push(&c, act);
    c.self = self;
    Obj* t = f->nscope ? f->scope[f->nscope - 1] : NULL;
    c.target = t && t->kind == OK_CLIP ? t : vm->levels[0];
    vm->depth++;
    if (vm->nthis < 128) vm->this_stack[vm->nthis] = self;
    vm->nthis++;
    execute(vm, &c, f->start, f->end);
    vm->nthis--;
    vm->depth--;
    Val r = c.returned ? c.ret : v_undef();
    ctx_free(&c);
    return r;
}

Val vm_call(VM* vm, Obj* fn, Val self, int argc, Val* argv) { return call_fn(vm, fn, self, argc, argv); }
Val vm_call_method(VM* vm, Obj* o, const char* name, int argc, Val* argv) {
    Val fn = obj_get(vm, o, name);
    if (fn.t != V_OBJ || fn.o->kind != OK_FUNC) return v_undef();
    return call_fn(vm, fn.o, v_obj(o), argc, argv);
}

static void run_code(VM* vm, AptFile* f, uint32_t addr, Obj* target) {
    if (vm->depth > 120) return;
    Ctx c;
    memset(&c, 0, sizeof c);
    c.file = f; c.nregs = 4; c.regs = (Val*)calloc(4, sizeof(Val));
    scope_push(&c, vm->glob);
    if (target) scope_push(&c, target);
    c.self = v_obj(target); c.target = target;
    vm->depth++;
    execute(vm, &c, addr, 0xFFFFFFFFu);
    vm->depth--;
    ctx_free(&c);
}

static Obj* make_func(VM* vm, Ctx* c, const AptIns* in, bool v2) {
    Obj* fn = obj_new(vm, vm->function_proto);
    fn->kind = OK_FUNC;
    FuncExt* f = (FuncExt*)calloc(1, sizeof(FuncExt));
    fn->fn = f;
    f->file = c->file;
    f->npool = c->npool;
    f->pool = (Val*)malloc((size_t)(c->npool + 1) * sizeof(Val));
    if (c->npool) memcpy(f->pool, c->pool, (size_t)c->npool * sizeof(Val));
    f->nscope = c->nscope;
    f->scope = (Obj**)malloc((size_t)(c->nscope + 1) * sizeof(Obj*));
    if (c->nscope) memcpy(f->scope, c->scope, (size_t)c->nscope * sizeof(Obj*));
    f->nargs = in->nargs; f->argnames = in->argnames; f->argregs = v2 ? in->argregs : NULL;
    f->nregs = in->nregs; f->flags = in->fflags; f->v2 = v2;
    f->start = in->next; f->end = in->next + (uint32_t)in->body;
    f->name = in->fname && in->fname[0] ? cstr(vm, in->fname) : NULL;
    return fn;
}

static bool instance_of(Val o, Val ctor, VM* vm) {
    if (o.t != V_OBJ || ctor.t != V_OBJ || ctor.o->kind != OK_FUNC) return false;
    Val p = obj_get_s(vm, ctor.o, S("prototype"));
    if (p.t != V_OBJ) return false;
    int n = 0;
    for (Obj* q = o.o->proto; q && n < 64; q = q->proto, n++) if (q == p.o) return true;
    return false;
}

static const char* const k_props[22] = {"_x", "_y", "_xscale", "_yscale", "_currentframe", "_totalframes", "_alpha", "_visible",
    "_width", "_height", "_rotation", "_target", "_framesloaded", "_name", "_droptarget", "_url", "_highquality", "_focusrect",
    "_soundbuftime", "_quality", "_xmouse", "_ymouse"};

static Obj* as_clip(VM* vm, Val t, Obj* base) {
    if (t.t == V_OBJ && is_disp(t.o)) return t.o;
    if (t.t == V_STR) return vm_resolve_path(vm, t.s->s, base);
    return NULL;
}

#define POP() pop(c)
#define PUSH(v) push(c, (v))
#define NARGS_MAX 64

static int pop_args(VM* vm, Ctx* c, Val* args) {
    int n = v_int(vm, POP());
    if (n < 0) n = 0;
    if (n > NARGS_MAX) n = NARGS_MAX;
    for (int i = 0; i < n; i++) args[i] = POP();
    return n;
}

static Val pool_at(Ctx* c, int i) {
    if (i < 0 || i >= c->npool) return v_undef();
    Val v = c->pool[i];
    return v.t == V_REG ? reg_get(c, v.r) : v;
}
static Str* pool_str(VM* vm, Ctx* c, int i) {
    if (i < 0 || i >= c->npool) return S("");
    return v_str_of(vm, c->pool[i].t == V_REG ? reg_get(c, c->pool[i].r) : c->pool[i]);
}

static void execute(VM* vm, Ctx* c, uint32_t pc, uint32_t end) {
    AptFile* file = c->file;
    long guard = 0;
    Val args[NARGS_MAX];
    while (!c->returned) {
        if (pc >= end) return;
        const AptIns* in = apt_ins(file, pc);
        if (!in) { vm_logf(vm, "bad code position %x in %s", pc, file->name); return; }
        if (++guard > 3000000) { vm_logf(vm, "runaway loop in %s at %x", file->name, pc); return; }
        uint8_t op = in->op;
        uint32_t nx = in->next;
        if (vm->trace && c->func && c->func->fn->name && getenv("APT_TRACE_FN") && !strcmp(getenv("APT_TRACE_FN"), c->func->fn->name->s))
            vm_logf(vm, "  @%x op %02x sp %d top %s", pc, op, c->sp, c->sp ? v_cstr(vm, c->st[c->sp - 1]) : "-");
        switch (op) {
        case 0x00: return;
        case 0xA2: case 0xA3: PUSH(pool_at(c, in->i)); break;
        case 0xAE: PUSH(lookup(vm, c, pool_str(vm, c, in->i))); break;
        case 0xAF: { Val o = POP(); PUSH(get_member(vm, o, v_s(pool_str(vm, c, in->i)))); break; }
        case 0x4F: { Val v = POP(), n = POP(), o = POP(); set_member(vm, o, n, v); break; }
        case 0x4E: { Val n = POP(), o = POP(); PUSH(get_member(vm, o, n)); break; }
        case 0x70: case 0x56: PUSH(c->self); break;
        case 0x71: case 0x58: PUSH(v_obj(vm->glob)); break;
        case 0xB5: case 0xB6: case 0xB7: PUSH(v_num(in->i)); break;
        case 0xB4: PUSH(v_num(in->f)); break;
        case 0x59: PUSH(v_num(0)); break;
        case 0x5A: PUSH(v_num(1)); break;
        case 0xA1: PUSH(v_s(cstr(vm, in->s))); break;
        case 0x73: PUSH(v_bool(true)); break;
        case 0x74: PUSH(v_bool(false)); break;
        case 0x75: PUSH(v_null()); break;
        case 0x76: PUSH(v_undef()); break;
        case 0xB2: case 0xB3: {
            Str* name = pool_str(vm, c, in->i);
            Val o = POP();
            int n = pop_args(vm, c, args);
            Val r = call_method(vm, c, o, v_s(name), n, args);
            if (op == 0xB3) setvar(vm, c, v_str_of(vm, POP()), r);   /* EA's "x = o.f()": the name is below */
            break;
        }
        case 0xB0: case 0xB1: {
            Str* name = pool_str(vm, c, in->i);
            int n = pop_args(vm, c, args);
            Val r = call_named(vm, c, name, n, args);
            if (op == 0xB1) setvar(vm, c, v_str_of(vm, POP()), r);   /* "x = f()" */
            break;
        }
        case 0x1D: { Val v = POP(), n = POP(); setvar(vm, c, v_str_of(vm, n), v); break; }
        case 0x1C: { Val n = POP(); PUSH(lookup(vm, c, v_str_of(vm, n))); break; }
        case 0x9D: if (v_truthy(POP())) { pc = (uint32_t)in->i; continue; } break;
        case 0xB8: if (!v_truthy(POP())) { pc = (uint32_t)in->i; continue; } break;
        case 0x99: pc = (uint32_t)in->i; continue;
        case 0x12: PUSH(v_bool(!v_truthy(POP()))); break;
        case 0x9B: case 0x8E: {
            Obj* fn = make_func(vm, c, in, op == 0x8E);
            if (fn->fn->name) define_local(vm, c, fn->fn->name, v_obj(fn));
            else PUSH(v_obj(fn));
            pc = fn->fn->end;
            continue;
        }
        case 0x49: { Val b = POP(), a = POP(); PUSH(v_bool(v_eq2(vm, a, b))); break; }
        case 0x3C: { Val v = POP(), n = POP(); define_local(vm, c, v_str_of(vm, n), v); break; }
        case 0x41: { Str* n = v_str_of(vm, POP()); if (c->act && !own_has(c->act, n)) prop_put(c->act, n, v_undef()); break; }
        case 0x72: { Str* n = v_str_of(vm, POP()); define_local(vm, c, n, v_num(0)); break; }
        case 0x52: case 0x5D: case 0x5E: {
            Val name = POP(), o = POP();
            int n = pop_args(vm, c, args);
            Val r = call_method(vm, c, o, name, n, args);
            if (op != 0x5D) PUSH(r);
            break;
        }
        case 0x3D: case 0x5B: case 0x5C: {
            Val name = POP();
            int n = pop_args(vm, c, args);
            Val r = name.t == V_OBJ && name.o->kind == OK_FUNC ? call_fn(vm, name.o, v_undef(), n, args)
                                                                : call_named(vm, c, v_str_of(vm, name), n, args);
            if (op != 0x5B) PUSH(r);
            break;
        }
        case 0x66: { Val b = POP(), a = POP(); PUSH(v_bool(v_strict(a, b))); break; }
        case 0x47: {
            Val b = POP(), a = POP();
            Val a2 = (a.t == V_OBJ && a.o->kind != OK_CLIP) ? to_prim(vm, a) : a;
            Val b2 = (b.t == V_OBJ && b.o->kind != OK_CLIP) ? to_prim(vm, b) : b;
            if (a2.t == V_STR || b2.t == V_STR || a2.t == V_OBJ || b2.t == V_OBJ) {
                Str* x = v_str_of(vm, a2); Str* y = v_str_of(vm, b2);
                char* t = (char*)malloc(x->len + y->len + 1);
                memcpy(t, x->s, x->len); memcpy(t + x->len, y->s, y->len);
                PUSH(v_s(str_new(vm, t, x->len + y->len)));
                free(t);
            } else PUSH(v_num(v_num_of(vm, a2) + v_num_of(vm, b2)));
            break;
        }
        case 0x0A: { Val b = POP(), a = POP(); PUSH(v_num(v_num_of(vm, a) + v_num_of(vm, b))); break; }
        case 0x0B: { Val b = POP(), a = POP(); PUSH(v_num(v_num_of(vm, a) - v_num_of(vm, b))); break; }
        case 0x0C: { Val b = POP(), a = POP(); PUSH(v_num(v_num_of(vm, a) * v_num_of(vm, b))); break; }
        case 0x0D: {
            Val b = POP(), a = POP(); double y = v_num_of(vm, b), x = v_num_of(vm, a);
            PUSH(v_num(y == 0 ? (x == 0 || x != x ? NAN : copysign(INFINITY, x)) : x / y));
            break;
        }
        case 0x3F: { Val b = POP(), a = POP(); double y = v_num_of(vm, b), x = v_num_of(vm, a); PUSH(v_num(y != 0 && y == y ? fmod(x, y) : NAN)); break; }
        case 0x48: { Val b = POP(), a = POP(); PUSH(v_less(vm, a, b)); break; }
        case 0x67: { Val b = POP(), a = POP(); PUSH(v_less(vm, b, a)); break; }
        case 0x0F: { Val b = POP(), a = POP(); PUSH(v_bool(v_num_of(vm, a) < v_num_of(vm, b))); break; }
        case 0x0E: { Val b = POP(), a = POP(); PUSH(v_bool(v_num_of(vm, a) == v_num_of(vm, b))); break; }
        case 0x13: { Val b = POP(), a = POP(); PUSH(v_bool(!strcmp(v_cstr(vm, a), v_cstr(vm, b)))); break; }
        case 0x29: { Val b = POP(), a = POP(); PUSH(v_bool(strcmp(v_cstr(vm, a), v_cstr(vm, b)) < 0)); break; }
        case 0x68: { Val b = POP(), a = POP(); PUSH(v_bool(strcmp(v_cstr(vm, a), v_cstr(vm, b)) > 0)); break; }
        case 0x21: {
            Val b = POP(), a = POP(); Str* x = v_str_of(vm, a); Str* y = v_str_of(vm, b);
            char* t = (char*)malloc(x->len + y->len + 1);
            memcpy(t, x->s, x->len); memcpy(t + x->len, y->s, y->len);
            PUSH(v_s(str_new(vm, t, x->len + y->len))); free(t);
            break;
        }
        case 0x14: case 0x31: PUSH(v_num(v_str_of(vm, POP())->len)); break;
        case 0x15: case 0x35: {
            int n = v_int(vm, POP()), i = v_int(vm, POP()); Str* t = v_str_of(vm, POP());
            int b = i - 1 < 0 ? 0 : i - 1;
            if (b > (int)t->len) b = (int)t->len;
            if (n < 0 || b + n > (int)t->len) n = (int)t->len - b;
            PUSH(v_s(str_new(vm, t->s + b, (size_t)n)));
            break;
        }
        case 0x10: { Val b = POP(), a = POP(); PUSH(v_bool(v_truthy(a) && v_truthy(b))); break; }
        case 0x11: { Val b = POP(), a = POP(); PUSH(v_bool(v_truthy(a) || v_truthy(b))); break; }
        case 0x17: POP(); break;
        case 0x4C: PUSH(c->sp ? c->st[c->sp - 1] : v_undef()); break;
        case 0x4D: if (c->sp >= 2) { Val t = c->st[c->sp - 1]; c->st[c->sp - 1] = c->st[c->sp - 2]; c->st[c->sp - 2] = t; } break;
        case 0x50: PUSH(v_num(v_num_of(vm, POP()) + 1)); break;
        case 0x51: PUSH(v_num(v_num_of(vm, POP()) - 1)); break;
        case 0x18: PUSH(v_num(v_int(vm, POP()))); break;
        case 0x4A: PUSH(v_num(v_num_of(vm, POP()))); break;
        case 0x4B: PUSH(v_s(v_str_of(vm, POP()))); break;
        case 0x44: PUSH(v_s(cstr(vm, type_of(POP())))); break;
        case 0x60: { Val b = POP(), a = POP(); PUSH(v_num(v_i32(vm, a) & v_i32(vm, b))); break; }
        case 0x61: { Val b = POP(), a = POP(); PUSH(v_num(v_i32(vm, a) | v_i32(vm, b))); break; }
        case 0x62: { Val b = POP(), a = POP(); PUSH(v_num(v_i32(vm, a) ^ v_i32(vm, b))); break; }
        case 0x63: { Val b = POP(), a = POP(); PUSH(v_num((int32_t)((uint32_t)v_i32(vm, a) << (v_int(vm, b) & 31)))); break; }
        case 0x64: { Val b = POP(), a = POP(); PUSH(v_num(v_i32(vm, a) >> (v_int(vm, b) & 31))); break; }
        case 0x65: { Val b = POP(), a = POP(); PUSH(v_num((double)((uint32_t)v_i32(vm, a) >> (v_int(vm, b) & 31)))); break; }
        case 0x3E: c->ret = POP(); c->returned = true; return;
        case 0x40: {
            Val name = POP();
            int n = pop_args(vm, c, args);
            Val ctor = lookup(vm, c, v_str_of(vm, name));
            if (ctor.t != V_OBJ) vm_logf(vm, "new %s: not found", v_cstr(vm, name));
            PUSH(construct_new(vm, ctor, n, args));
            break;
        }
        case 0x53: {
            Val name = POP(), o = POP();
            int n = pop_args(vm, c, args);
            Val ctor = (name.t == V_UNDEF || (name.t == V_STR && !name.s->len)) ? o : get_member(vm, o, name);
            if (ctor.t != V_OBJ) vm_logf(vm, "new method %s: not found", v_cstr(vm, name));
            PUSH(construct_new(vm, ctor, n, args));
            break;
        }
        case 0x43: {
            int n = v_int(vm, POP());
            Obj* o = obj_new(vm, vm->object_proto);
            for (int i = 0; i < n; i++) { Val v = POP(), k = POP(); Str* ks = v_str_of(vm, k); name_fn(v, ks); prop_put(o, ks, v); }
            PUSH(v_obj(o));
            break;
        }
        case 0x42: {
            int n = v_int(vm, POP());
            Obj* a = arr_new(vm);
            for (int i = 0; i < n; i++) arr_push(vm, a, POP());
            PUSH(v_obj(a));
            break;
        }
        case 0x88: {
            free(c->pool);
            c->npool = in->n;
            c->pool = (Val*)malloc((size_t)(in->n + 1) * sizeof(Val));
            for (int i = 0; i < in->n; i++) c->pool[i] = const_val(vm, file, in->list[i]);
            break;
        }
        case 0x96:
            for (int i = 0; i < in->n; i++) {
                Val v = const_val(vm, file, in->list[i]);
                PUSH(v.t == V_REG ? reg_get(c, v.r) : v);
            }
            break;
        case 0xB9: PUSH(reg_get(c, in->i)); break;
        case 0x87: {
            int r = in->i;
            if (r >= 0 && r < 256) {
                if (r >= c->nregs) { c->regs = (Val*)realloc(c->regs, (size_t)(r + 1) * sizeof(Val)); for (int i = c->nregs; i <= r; i++) c->regs[i] = v_undef(); c->nregs = r + 1; }
                c->regs[r] = c->sp ? c->st[c->sp - 1] : v_undef();
            }
            break;
        }
        case 0xA4: PUSH(lookup(vm, c, cstr(vm, in->s))); break;
        case 0xA6: { Val n = POP(); setvar(vm, c, v_str_of(vm, n), v_s(cstr(vm, in->s))); break; }   /* name popped; the string is the value */
        case 0xA5: { Val o = POP(); PUSH(get_member(vm, o, v_s(cstr(vm, in->s)))); break; }
        case 0xA7: { Val n = POP(), o = POP(); set_member(vm, o, n, v_s(cstr(vm, in->s))); break; }
        case 0x3A: { Val n = POP(), o = POP(); PUSH(v_bool(o.t == V_OBJ && prop_del(o.o, v_str_of(vm, n)))); break; }
        case 0x3B: {
            Str* n = v_str_of(vm, POP()); bool ok = false;
            for (int i = c->nscope - 1; i >= 0 && !ok; i--) ok = prop_del(c->scope[i], n);
            PUSH(v_bool(ok));
            break;
        }
        case 0x46: case 0x55: {
            Val o = POP();
            if (op == 0x46) o = lookup(vm, c, v_str_of(vm, o));
            PUSH(v_null());
            if (o.t == V_OBJ) {
                Str* keys[512];
                int n = obj_keys(vm, o.o, keys, 512);
                for (int i = n - 1; i >= 0; i--) PUSH(v_s(keys[i]));
            }
            break;
        }
        case 0x54: { Val ctor = POP(), o = POP(); PUSH(v_bool(instance_of(o, ctor, vm))); break; }
        case 0x69: {
            Val sup = POP(), sub = POP();
            if (sub.t == V_OBJ && sup.t == V_OBJ && sub.o->kind == OK_FUNC && sup.o->kind == OK_FUNC) {
                Val sp = obj_get_s(vm, sup.o, S("prototype"));
                Obj* p = obj_new(vm, sp.t == V_OBJ ? sp.o : vm->object_proto);
                prop_put(p, S("__constructor__"), sup);
                prop_put(sub.o, S("prototype"), v_obj(p));
            }
            break;
        }
        case 0x26: vm_logf(vm, "trace: %s", v_cstr(vm, POP())); break;
        case 0x34: PUSH(v_num(floor(vm->time_ms))); break;
        case 0x30: { int n = v_int(vm, POP()); PUSH(v_num(n <= 0 ? 0 : 37 % n)); break; }
        case 0x06: if (c->target && c->target->kind == OK_CLIP) c->target->d->playing = true; break;
        case 0x07: if (c->target && c->target->kind == OK_CLIP) c->target->d->playing = false; break;
        case 0x04: if (c->target && c->target->kind == OK_CLIP) goto_clip(vm, c->target, v_num(c->target->d->frame + 2), false); break;
        case 0x05: if (c->target && c->target->kind == OK_CLIP) goto_clip(vm, c->target, v_num(c->target->d->frame), false); break;
        case 0x81: if (c->target && c->target->kind == OK_CLIP) goto_clip(vm, c->target, v_num(in->i + 1), false); break;
        case 0x8C: if (c->target && c->target->kind == OK_CLIP) goto_clip(vm, c->target, v_s(cstr(vm, in->s)), false); break;
        case 0x9F: { Val f = POP(); if (c->target && c->target->kind == OK_CLIP) goto_clip(vm, c->target, f, (in->i & 1) != 0); break; }
        case 0x8B: {
            if (!in->s || !in->s[0]) { Obj* base = c->nscope > 1 ? c->scope[1] : vm->levels[0]; c->target = base; }
            else { Obj* t = vm_resolve_path(vm, in->s, c->nscope > 1 ? c->scope[1] : vm->levels[0]); if (t && t->kind == OK_CLIP) c->target = t; }
            break;
        }
        case 0x20: { Val t = POP(); Obj* o = as_clip(vm, t, c->target); if (o && o->kind == OK_CLIP) c->target = o; break; }
        case 0x22: {
            int i = v_int(vm, POP()); Val t = POP(); Obj* o = as_clip(vm, t, c->target);
            PUSH(o && i >= 0 && i < 22 ? obj_get_s(vm, o, cstr(vm, k_props[i])) : v_undef());
            break;
        }
        case 0x23: {
            Val v = POP(); int i = v_int(vm, POP()); Val t = POP(); Obj* o = as_clip(vm, t, c->target);
            if (o && i >= 0 && i < 22) obj_set_s(vm, o, cstr(vm, k_props[i]), v);
            break;
        }
        case 0x83: geturl(vm, in->s ? in->s : "", in->s2 ? in->s2 : "", c->target); break;
        case 0x9A: { Val t = POP(), u = POP(); geturl(vm, v_cstr(vm, u), v_cstr(vm, t), c->target); break; }
        case 0x94: {
            Val o = POP();
            uint32_t bodyend = nx + (uint32_t)in->i;
            if (o.t == V_OBJ) {
                scope_push(c, o.o);
                execute(vm, c, nx, bodyend);
                c->nscope--;
                if (c->returned) return;
            }
            pc = bodyend;
            continue;
        }
        case 0x24: {
            int d = v_int(vm, POP()); Val tgt = POP(), src = POP();
            Obj* o = as_clip(vm, src, c->target);
            if (o && o->kind == OK_CLIP) { Val a[2] = {tgt, v_num(d)}; vm_call_method(vm, o, "duplicateMovieClip", 2, a); }
            break;
        }
        case 0x25: { Val t = POP(); Obj* o = as_clip(vm, t, c->target); if (o && o->d->parent) remove_child(vm, o->d->parent, o->d->depth); break; }
        case 0x45: { Val t = POP(); PUSH(t.t == V_OBJ && is_disp(t.o) ? v_s(target_path(vm, t.o)) : v_undef()); break; }
        case 0x08: case 0x09: case 0x77: case 0x27: case 0x28: break;
        case 0x2B: { Val o = POP(), ctor = POP(); PUSH(instance_of(o, ctor, vm) ? o : v_null()); break; }
        case 0x2C: { int n = v_int(vm, POP()); for (int i = 0; i < n; i++) POP(); POP(); break; }
        case 0x32: case 0x36: { Str* t = v_str_of(vm, POP()); PUSH(v_num(t->len ? (uint8_t)t->s[0] : 0)); break; }
        case 0x33: case 0x37: { char ch = (char)(v_int(vm, POP()) & 0xFF); PUSH(v_s(str_new(vm, &ch, 1))); break; }
        case 0x9E: POP(); break;
        case 0x2A: vm_logf(vm, "throw %s", v_cstr(vm, POP())); return;
        default: vm_logf(vm, "unhandled opcode %02x at %x in %s", op, pc, file->name); break;
        }
        pc = nx;
    }
}

Val vm_script_this(VM* vm, int back) {
    int i = vm->nthis - 1 - back;
    return i >= 0 && i < 128 ? vm->this_stack[i] : v_undef();
}

Val vm_lookup_var(VM* vm, Obj* clip, Str* name) {
    Ctx c;
    memset(&c, 0, sizeof c);
    scope_push(&c, vm->glob);
    if (clip) scope_push(&c, clip);
    c.self = v_obj(clip); c.target = clip;
    Val r = lookup(vm, &c, name);
    ctx_free(&c);
    return r;
}

/* ============================================================ timelines */
static int clip_nframes(Obj* mc) { AptChar* ch = mc->d->ch; return ch && ch->nframes > 0 ? ch->nframes : 1; }
static AptFrame* clip_frame(Obj* mc, int fi) {
    AptChar* ch = mc->d->ch;
    return ch && fi >= 0 && fi < ch->nframes ? &ch->frames[fi] : NULL;
}

static Obj* new_clip(VM* vm, AptFile* f, AptChar* ch, int chid) {
    Obj* o = obj_new(vm, vm->mc_proto);
    o->kind = OK_CLIP;
    o->d = disp_new(f, ch, chid);
    o->d->frame = -1; o->d->playing = true;
    return o;
}

static Obj* new_text(VM* vm, AptFile* f, AptChar* ch, int chid) {
    Obj* o = obj_new(vm, vm->tf_proto);
    o->kind = OK_TEXT;
    DispExt* d = o->d = disp_new(f, ch, chid);
    d->rect[2] = 100; d->rect[3] = 20; d->height = 12; d->color = 0xFFFFFF;
    if (ch) {
        if (ch->content && ch->content[0]) d->text = strip_html(vm, cstr(vm, ch->content));
        d->color = (uint32_t)ch->color[0] << 16 | (uint32_t)ch->color[1] << 8 | ch->color[2];
        if (ch->var && ch->var[0]) d->var = cstr(vm, ch->var);
        memcpy(d->rect, ch->rect, sizeof d->rect);
        d->height = ch->height > 0 ? ch->height : 12;
        d->align = ch->align; d->font = ch->font; d->multiline = ch->multiline; d->wordwrap = ch->wordwrap;
    }
    if (!d->text) d->text = S("");
    return o;
}

static Obj* make_instance(VM* vm, AptFile* f, int cid, AptChar* ch) {
    if (ch->type == CH_SPRITE || ch->type == CH_MOVIE) return new_clip(vm, f, ch, cid);
    if (ch->type == CH_TEXT) return new_text(vm, f, ch, cid);
    Obj* o = obj_new(vm, vm->shape_proto);
    o->kind = OK_SHAPE;
    o->d = disp_new(f, ch, cid);
    return o;
}

static void add_child(VM* vm, Obj* parent, Obj* child, int depth, Str* name, bool dynamic) {
    if (child_at(parent, depth)) remove_child(vm, parent, depth);
    DispExt* p = parent->d;
    child->d->parent = parent; child->d->depth = depth;
    if (name && name->len) child->d->name = name;
    else if (!child->d->name) child->d->name = S("");
    child->d->dynamic = dynamic;
    if (p->nkids == p->capkids) { p->capkids = p->capkids ? p->capkids * 2 : 8; p->kids = (Child*)realloc(p->kids, (size_t)p->capkids * sizeof(Child)); }
    int i = p->nkids;
    while (i > 0 && p->kids[i - 1].depth > depth) { p->kids[i] = p->kids[i - 1]; i--; }
    p->kids[i].depth = depth; p->kids[i].o = child;
    p->nkids++;
}

static void run_events(VM* vm, Obj* mc, uint32_t mask) {
    DispExt* d = mc->d;
    for (int i = 0; i < d->nevents && !d->removed; i++)
        if (d->events[i].flags & mask) run_code(vm, d->events_file, d->events[i].code, mc);
}

static void remove_child(VM* vm, Obj* parent, int depth) {
    DispExt* p = parent->d;
    for (int i = 0; i < p->nkids; i++) {
        if (p->kids[i].depth != depth) continue;
        Obj* c = p->kids[i].o;
        memmove(&p->kids[i], &p->kids[i + 1], (size_t)(p->nkids - i - 1) * sizeof(Child));
        p->nkids--;
        c->d->removed = true;
        if (c->kind == OK_CLIP) {
            DispExt* d = c->d;
            for (int k = 0; k < d->nevents; k++)
                if (d->events[k].flags & 0x04) run_code(vm, d->events_file, d->events[k].code, c);
            Val fn = obj_get_s(vm, c, S("onUnload"));
            if (fn.t == V_OBJ && fn.o->kind == OK_FUNC) call_fn(vm, fn.o, v_obj(c), 0, NULL);
        }
        return;
    }
}

static AptFile* lib_named(VM* vm, const char* movie) {
    const char* b = movie;
    for (const char* q = movie; *q; q++) if (*q == '/' || *q == '\\') b = q + 1;
    for (int i = 0; i < vm->nlibs; i++) if (!strcmp(vm->lib_names[i], b)) return vm->libs[i];
    if (vm->host.resolve_lib) return vm->host.resolve_lib(vm, b);
    return NULL;
}

/* a character id in a file, following imports */
static AptChar* resolve_char(VM* vm, AptFile* f, int cid, AptFile** of, int* ocid, int depth) {
    AptChar* mv = f->movie;
    if (cid > 0 && cid < mv->nchars && mv->chars[cid]) { *of = f; *ocid = cid; return mv->chars[cid]; }
    for (int i = 0; i < mv->nimports; i++) {
        AptImport* im = &mv->imports[i];
        if (im->ch != cid) continue;
        AptFile* lib = lib_named(vm, im->movie);
        if (!lib) { vm_logf(vm, "missing lib %s for %s", im->movie, im->name); return NULL; }
        for (int e = 0; e < lib->movie->nexports; e++)
            if (!strcmp(lib->movie->exports[e].name, im->name)) {
                if (depth > 8) return NULL;
                return resolve_char(vm, lib, lib->movie->exports[e].ch, of, ocid, depth + 1);
            }
        vm_logf(vm, "lib %s has no %s", im->movie, im->name);
        return NULL;
    }
    return NULL;
}

/* an attachMovie linkage name */
static AptChar* linkage(VM* vm, AptFile* f, const char* name, AptFile** of, int* ocid) {
    AptChar* mv = f->movie;
    for (int e = 0; e < mv->nexports; e++)
        if (!strcmp(mv->exports[e].name, name)) {
            int c = mv->exports[e].ch;
            if (c > 0 && c < mv->nchars && mv->chars[c]) { *of = f; *ocid = c; return mv->chars[c]; }
        }
    for (int i = 0; i < mv->nimports; i++)
        if (!strcmp(mv->imports[i].name, name)) return resolve_char(vm, f, mv->imports[i].ch, of, ocid, 0);
    for (int l = 0; l < vm->nlibs; l++) {
        AptFile* lib = vm->libs[l];
        for (int e = 0; e < lib->movie->nexports; e++)
            if (!strcmp(lib->movie->exports[e].name, name)) {
                int c = lib->movie->exports[e].ch;
                if (c > 0 && c < lib->movie->nchars && lib->movie->chars[c]) { *of = lib; *ocid = c; return lib->movie->chars[c]; }
            }
    }
    return NULL;
}

static Obj* class_named(VM* vm, const char* name) {
    for (int i = 0; i < vm->nclasses; i++) if (!strcmp(vm->class_names[i]->s, name)) return vm->class_ctors[i];
    return NULL;
}
static Obj* class_for(VM* vm, AptFile* f, int cid) {
    for (int e = 0; e < f->movie->nexports; e++)
        if (f->movie->exports[e].ch == cid) {
            Obj* c = class_named(vm, f->movie->exports[e].name);
            if (c) return c;
        }
    return NULL;
}

static void goto_frame(VM* vm, Obj* mc, int fi, bool run_actions);
static void run_frame_actions(VM* vm, Obj* mc, int fi);

/* a new clip: its first frame placed, init object / clip events, class constructor, load */
static void construct(VM* vm, Obj* mc, Obj* init, const char* link, AptEvent* ev, int nev, AptFile* evfile) {
    DispExt* d = mc->d;
    d->events = ev; d->nevents = nev; d->events_file = evfile;
    Obj* cls = link ? class_named(vm, link) : NULL;
    if (!cls && d->chid > 0) cls = class_for(vm, d->file, d->chid);
    if (cls) {
        Val p = obj_get_s(vm, cls, S("prototype"));
        mc->proto = p.t == V_OBJ ? p.o : vm->mc_proto;
    }
    if (init) {
        Str* keys[256];
        int n = obj_keys(vm, init, keys, 256);
        for (int i = 0; i < n; i++) obj_set_s(vm, mc, keys[i], obj_get_s(vm, init, keys[i]));
    }
    run_events(vm, mc, 0x0200);                      /* initialize (component parameters) */
    goto_frame(vm, mc, 0, false);                    /* children constructed before this clip's constructor */
    if (cls) call_fn(vm, cls, v_obj(mc), 0, NULL);
    run_events(vm, mc, 0x040000);                    /* construct */
    run_frame_actions(vm, mc, 0);
    run_events(vm, mc, 0x01);                        /* load */
    Val fn = obj_get_s(vm, mc, S("onLoad"));
    if (fn.t == V_OBJ && fn.o->kind == OK_FUNC) call_fn(vm, fn.o, v_obj(mc), 0, NULL);
}

static Obj* attach(VM* vm, Obj* parent, Str* link, Str* name, int depth, Obj* init) {
    AptFile* f = NULL; int cid = 0;
    AptChar* ch = linkage(vm, parent->d->file, link->s, &f, &cid);
    if (!ch) { vm_logf(vm, "attachMovie: no linkage %s on %s", link->s, target_path(vm, parent)->s); return NULL; }
    Obj* inst = make_instance(vm, f, cid, ch);
    add_child(vm, parent, inst, depth, name, true);
    if (inst->kind == OK_CLIP) construct(vm, inst, init, link->s, NULL, 0, NULL);
    else if (init) {
        Str* keys[256];
        int n = obj_keys(vm, init, keys, 256);
        for (int i = 0; i < n; i++) obj_set_s(vm, inst, keys[i], obj_get_s(vm, init, keys[i]));
    }
    return inst;
}

static int frame_index(VM* vm, Obj* mc, Val f) {
    if (f.t == V_STR) {
        const char* s = f.s->s;
        bool digits = true, any = false;
        for (const char* q = s; *q; q++) { if (isdigit((unsigned char)*q)) any = true; else if (!isspace((unsigned char)*q)) digits = false; }
        if (!(digits && any)) {
            int fi = mc->d->ch ? apt_label(mc->d->ch, s) : -1;
            if (fi < 0) vm_logf(vm, "no label %s in %s", s, target_path(vm, mc)->s);
            return fi;
        }
    }
    int n = v_int(vm, f) - 1, nf = clip_nframes(mc);
    return n < 0 ? 0 : n >= nf ? nf - 1 : n;
}

static void goto_clip(VM* vm, Obj* mc, Val f, bool play) {
    int fi = frame_index(vm, mc, f);
    if (fi < 0) return;
    mc->d->playing = play;
    if (fi == mc->d->frame) return;
    goto_frame(vm, mc, fi, true);
}

/* the wanted state of a depth */
typedef struct {
    int depth, ch;
    bool hm, ht; float m[6]; uint8_t tint[4], add[4];
    Str* name; int clipdepth;
    AptEvent* ev; int nev;
} Want;

static void want_from(VM* vm, Want* w, const AptPlace* p) {
    memset(w, 0, sizeof *w);
    w->depth = p->depth; w->ch = p->ch;
    if (p->flags & 4) { w->hm = true; memcpy(w->m, p->m, sizeof w->m); }
    if (p->flags & 8) { w->ht = true; memcpy(w->tint, p->tint, 4); memcpy(w->add, p->add, 4); }
    w->name = p->name && p->name[0] ? cstr(vm, p->name) : NULL;
    w->clipdepth = p->clipdepth;
    w->ev = p->events; w->nev = p->nevents;
}

static void place_new(VM* vm, Obj* mc, const Want* w) {
    AptFile* f = NULL; int cid = 0;
    AptChar* ch = resolve_char(vm, mc->d->file, w->ch, &f, &cid, 0);
    if (!ch) { vm_logf(vm, "place: no character %d in %s", w->ch, mc->d->file->name); return; }
    Obj* inst = make_instance(vm, f, cid, ch);
    DispExt* d = inst->d;
    d->chid_place = w->ch;
    if (w->hm) disp_set_matrix(d, w->m);
    if (w->ht) for (int i = 0; i < 4; i++) { d->cx[i] = w->tint[i]; d->cxadd[i] = w->add[i]; }
    d->clipdepth = w->clipdepth;
    Str* name = w->name;
    if (!name) { char b[32]; snprintf(b, sizeof b, "instance%d", ++vm->next_id); name = str_new(vm, b, strlen(b)); }
    add_child(vm, mc, inst, w->depth, name, false);
    if (inst->kind == OK_CLIP) construct(vm, inst, NULL, NULL, w->ev, w->nev, mc->d->file);
}

static void place(VM* vm, Obj* mc, const AptPlace* p) {
    Obj* c = child_at(mc, p->depth);
    if ((p->flags & 1) && c && (p->ch < 0 || c->d->chid_place == p->ch)) {
        DispExt* d = c->d;
        if (p->flags & 4) disp_set_matrix(d, p->m);
        if (p->flags & 8) for (int i = 0; i < 4; i++) { d->cx[i] = p->tint[i]; d->cxadd[i] = p->add[i]; }
        if (p->name && p->name[0]) d->name = cstr(vm, p->name);
        return;
    }
    if (p->ch < 0) return;
    Want w;
    want_from(vm, &w, p);
    if ((p->flags & 1) && c && !c->d->dynamic) {
        /* the character at that depth replaced: its place, colour and name carry over */
        DispExt* d = c->d;
        if (!w.hm) { w.hm = true; memcpy(w.m, d->m, sizeof w.m); }
        if (!w.ht) { w.ht = true; for (int i = 0; i < 4; i++) { w.tint[i] = (uint8_t)d->cx[i]; w.add[i] = (uint8_t)d->cxadd[i]; } }
        if (!w.name && d->name && strncmp(d->name->s, "instance", 8)) w.name = d->name;
    }
    place_new(vm, mc, &w);
}

static void goto_frame(VM* vm, Obj* mc, int fi, bool run_actions) {
    DispExt* d = mc->d;
    int nf = clip_nframes(mc);
    if (fi >= nf) fi = nf - 1;
    if (fi < 0) fi = 0;
    if (fi < d->frame || d->frame < 0) {
        /* rebuild: the timeline's state at fi */
        Want* want = NULL; int nw = 0, cw = 0;
        for (int k = 0; k <= fi; k++) {
            AptFrame* fr = clip_frame(mc, k);
            if (!fr) continue;
            for (int j = 0; j < fr->n; j++) {
                AptItem* it = &fr->items[j];
                if (it->kind == IT_PLACE) {
                    const AptPlace* p = it->place;
                    int at = -1;
                    for (int q = 0; q < nw; q++) if (want[q].depth == p->depth) { at = q; break; }
                    if (p->ch >= 0 && !((p->flags & 1) && at >= 0 && want[at].ch == p->ch)) {
                        if (at < 0) {
                            if (nw == cw) { cw = cw ? cw * 2 : 16; want = (Want*)realloc(want, (size_t)cw * sizeof(Want)); }
                            at = nw;
                            while (at > 0 && want[at - 1].depth > p->depth) { want[at] = want[at - 1]; at--; }
                            nw++;
                        }
                        want_from(vm, &want[at], p);
                    } else if (at >= 0) {
                        Want* w = &want[at];
                        if (p->flags & 4) { w->hm = true; memcpy(w->m, p->m, sizeof w->m); }
                        if (p->flags & 8) { w->ht = true; memcpy(w->tint, p->tint, 4); memcpy(w->add, p->add, 4); }
                        if (p->name && p->name[0]) w->name = cstr(vm, p->name);
                        if (p->ch >= 0) w->ch = p->ch;
                    }
                } else if (it->kind == IT_REMOVE) {
                    for (int q = 0; q < nw; q++) if (want[q].depth == it->arg) { memmove(&want[q], &want[q + 1], (size_t)(nw - q - 1) * sizeof(Want)); nw--; break; }
                }
            }
        }
        for (int i = d->nkids - 1; i >= 0; i--) {
            if (i >= d->nkids) continue;
            Obj* c = d->kids[i].o;
            if (c->d->dynamic) continue;
            const Want* w = NULL;
            for (int q = 0; q < nw; q++) if (want[q].depth == d->kids[i].depth) { w = &want[q]; break; }
            if (!w || w->ch != c->d->chid_place) remove_child(vm, mc, d->kids[i].depth);
        }
        d->frame = fi;
        for (int q = 0; q < nw && !d->removed; q++) {
            Obj* c = child_at(mc, want[q].depth);
            if (c && !c->d->dynamic) { if (want[q].hm) disp_set_matrix(c->d, want[q].m); continue; }
            place_new(vm, mc, &want[q]);
        }
        free(want);
    } else {
        for (int k = d->frame + 1; k <= fi && !d->removed; k++) {
            d->frame = k;
            AptFrame* fr = clip_frame(mc, k);
            if (!fr) continue;
            for (int j = 0; j < fr->n; j++) {
                AptItem* it = &fr->items[j];
                if (it->kind == IT_PLACE) place(vm, mc, it->place);
                else if (it->kind == IT_REMOVE) {
                    Obj* c = child_at(mc, it->arg);
                    if (c && !c->d->dynamic) remove_child(vm, mc, it->arg);
                }
            }
        }
    }
    d->frame = fi;
    if (run_actions) run_frame_actions(vm, mc, fi);
}

static void run_frame_actions(VM* vm, Obj* mc, int fi) {
    AptFrame* fr = clip_frame(mc, fi);
    if (!fr) return;
    for (int j = 0; j < fr->n; j++)
        if (fr->items[j].kind == IT_ACTION) {
            run_code(vm, mc->d->file, fr->items[j].code, mc);
            if (mc->d->removed) return;
        }
}

void vm_run_init_actions(VM* vm, AptFile* f, Obj* target) {
    if (!f->movie || f->movie->nframes < 1) return;
    AptFrame* fr = &f->movie->frames[0];
    for (int j = 0; j < fr->n; j++)
        if (fr->items[j].kind == IT_INIT) run_code(vm, f, fr->items[j].code, target ? target : vm->levels[0]);
}

/* ============================================================ frame loop */
static void advance(VM* vm, Obj* mc) {
    DispExt* d = mc->d;
    if (d->removed) return;
    run_events(vm, mc, 0x02);                        /* enterFrame */
    if (obj_has(vm, mc, S("onEnterFrame"))) {
        Val fn = obj_get_s(vm, mc, S("onEnterFrame"));
        if (fn.t == V_OBJ && fn.o->kind == OK_FUNC) call_fn(vm, fn.o, v_obj(mc), 0, NULL);
    }
    if (d->removed) return;
    int nf = clip_nframes(mc);
    if (d->playing && nf > 1) {
        int f = d->frame + 1;
        if (f >= nf) f = 0;
        goto_frame(vm, mc, f, true);
    }
    int n = d->nkids;
    if (!n) return;
    int* depths = (int*)malloc((size_t)n * sizeof(int));
    for (int i = 0; i < n; i++) depths[i] = d->kids[i].depth;
    for (int i = 0; i < n; i++) {
        Obj* c = child_at(mc, depths[i]);
        if (c && c->kind == OK_CLIP) advance(vm, c);
    }
    free(depths);
}

static void do_loads(VM* vm);

void vm_tick(VM* vm, double dt_ms) {
    vm->time_ms += dt_ms;
    int n = vm->nintervals;
    if (n) {
        int* ids = (int*)malloc((size_t)n * sizeof(int));
        for (int i = 0; i < n; i++) ids[i] = vm->intervals[i].id;
        for (int k = 0; k < n; k++) {
            for (int fired = 0; fired < 4; fired++) {
                Interval* iv = NULL;
                for (int i = 0; i < vm->nintervals; i++) if (vm->intervals[i].id == ids[k]) { iv = &vm->intervals[i]; break; }
                if (!iv || vm->time_ms < iv->next) break;
                iv->next += iv->ms;
                Obj* fn = iv->fn; Val self = iv->self;
                if (!fn && iv->method && self.t == V_OBJ) {
                    Val f = obj_get_s(vm, self.o, iv->method);
                    fn = f.t == V_OBJ && f.o->kind == OK_FUNC ? f.o : NULL;
                }
                int na = iv->nargs;
                Val args[16];
                if (na > 16) na = 16;
                if (na) memcpy(args, iv->args, (size_t)na * sizeof(Val));
                if (fn) call_fn(vm, fn, self, na, args);
            }
        }
        free(ids);
    }
    do_loads(vm);
    for (int lv = 0; lv < VM_LEVELS; lv++) if (vm->levels[lv]) advance(vm, vm->levels[lv]);
}

/* ============================================================ loading */
static void queue_load(VM* vm, const char* url, int level, Obj* clip) {
    if (vm->nloads == vm->caploads) { vm->caploads = vm->caploads ? vm->caploads * 2 : 8; vm->loads = (LoadReq*)realloc(vm->loads, (size_t)vm->caploads * sizeof(LoadReq)); }
    LoadReq* r = &vm->loads[vm->nloads++];
    snprintf(r->url, sizeof r->url, "%s", url ? url : "");
    r->level = level; r->clip = clip;
}

static void geturl(VM* vm, const char* url, const char* target, Obj* base) {
    if (!strncmp(url, "FSCommand:", 10)) {
        vm_logf(vm, "fscommand %s %s", url + 10, target);
        if (vm->host.fscommand) vm->host.fscommand(vm, url + 10, target);
        return;
    }
    if (!strncmp(target, "_level", 6) && target[6]) {
        bool digits = true;
        for (const char* q = target + 6; *q; q++) if (!isdigit((unsigned char)*q)) digits = false;
        if (digits) { queue_load(vm, url, atoi(target + 6), NULL); return; }
    }
    if (target[0]) {
        Obj* t = vm_resolve_path(vm, target, base ? base : vm->levels[0]);
        if (t && t->kind == OK_CLIP) { queue_load(vm, url, -1, t); return; }
    }
    vm_logf(vm, "getURL %s %s", url, target);
}

Obj* vm_load_level(VM* vm, AptFile* f, int level, const char* url) {
    if (level < 0 || level >= VM_LEVELS) return NULL;
    Obj* old = vm->levels[level];
    if (old) old->d->removed = true;
    vm->levels[level] = NULL;
    if (!f) return NULL;
    Obj* mc = new_clip(vm, f, f->movie, 0);
    char b[16]; snprintf(b, sizeof b, "_level%d", level);
    mc->d->is_root = true; mc->d->name = str_new(vm, b, strlen(b)); mc->d->depth = level;
    vm->levels[level] = mc;
    vm_run_init_actions(vm, f, level == 0 ? mc : vm->levels[0]);
    construct(vm, mc, NULL, NULL, NULL, 0, NULL);
    if (vm->host.level_loaded) vm->host.level_loaded(vm, url ? url : "", level, mc);
    vm_logf(vm, "loaded %s into _level%d", url && url[0] ? url : f->name, level);
    return mc;
}

static void do_loads(VM* vm) {
    int n = vm->nloads;
    if (!n) return;
    LoadReq* q = (LoadReq*)malloc((size_t)n * sizeof(LoadReq));
    memcpy(q, vm->loads, (size_t)n * sizeof(LoadReq));
    vm->nloads = 0;
    for (int i = 0; i < n; i++) {
        AptFile* f = q[i].url[0] && vm->host.resolve_movie ? vm->host.resolve_movie(vm, q[i].url) : NULL;
        if (q[i].level >= 0) {
            if (q[i].url[0] && !f) vm_logf(vm, "load: no movie for %s (level %d)", q[i].url, q[i].level);
            vm_load_level(vm, f, q[i].level, q[i].url);
        } else if (q[i].clip) {
            Obj* clip = q[i].clip;
            if (!f) { vm_logf(vm, "load: no movie for %s", q[i].url); continue; }
            while (clip->d->nkids) remove_child(vm, clip, clip->d->kids[0].depth);
            clip->d->file = f; clip->d->ch = f->movie; clip->d->frame = -1;
            vm_run_init_actions(vm, f, vm->levels[0]);
            construct(vm, clip, NULL, NULL, NULL, 0, NULL);
            vm_logf(vm, "loaded %s into %s", q[i].url, target_path(vm, clip)->s);
        }
    }
    free(q);
}

Obj* vm_new_movie_clip(VM* vm, AptFile* f) { return new_clip(vm, f, f->movie, 0); }
void vm_goto_frame(VM* vm, Obj* mc, int fi, bool run_actions) { goto_frame(vm, mc, fi, run_actions); }
void vm_add_child(VM* vm, Obj* parent, Obj* child, int depth, const char* name, bool dynamic) {
    add_child(vm, parent, child, depth, name ? str_new(vm, name, strlen(name)) : NULL, dynamic);
}

/* ============================================================ builtins */
#define NATIVE(fn) static Val fn(VM* vm, Val self, int argc, Val* argv, void* data)
#define ARG(i) ((i) < argc ? argv[i] : v_undef())
#define UNUSED (void)vm; (void)self; (void)argc; (void)argv; (void)data
#define SELF_OBJ (self.t == V_OBJ ? self.o : NULL)

static Val construct_new(VM* vm, Val ctor, int argc, Val* argv) {
    if (ctor.t != V_OBJ || ctor.o->kind != OK_FUNC) return v_undef();
    Obj* c = ctor.o;
    Val arr = obj_get_s(vm, vm->glob, S("Array"));
    if (arr.t == V_OBJ && arr.o == c) {
        Obj* a = arr_new(vm);
        if (argc == 1 && argv[0].t == V_NUM) arr_setlen(a, v_int(vm, argv[0]));
        else for (int i = 0; i < argc; i++) arr_push(vm, a, argv[i]);
        return v_obj(a);
    }
    Val p = obj_get_s(vm, c, S("prototype"));
    Obj* o = obj_new(vm, p.t == V_OBJ ? p.o : vm->object_proto);
    prop_put(o, S("__constructor__"), ctor);
    Val r = call_fn(vm, c, v_obj(o), argc, argv);
    if (c->fn->native && r.t != V_UNDEF && r.t != V_NULL) return r;   /* String(), Number(), Object(x) */
    return v_obj(o);
}

/* -- Object, Function */
NATIVE(n_object) { UNUSED; if (argc && argv[0].t == V_OBJ) return argv[0]; return self.t == V_OBJ && self.o != vm->glob ? self : v_obj(obj_new(vm, vm->object_proto)); }
NATIVE(n_obj_tostring) { UNUSED; return v_s(S("[object Object]")); }
NATIVE(n_obj_valueof) { UNUSED; return self; }
NATIVE(n_has_own) { UNUSED; return v_bool(SELF_OBJ && argc && own_has(self.o, v_str_of(vm, argv[0]))); }
NATIVE(n_false) { UNUSED; return v_bool(false); }
NATIVE(n_true) { UNUSED; return v_bool(true); }
NATIVE(n_undef) { UNUSED; return v_undef(); }
NATIVE(n_hundred) { UNUSED; return v_num(100); }
NATIVE(n_add_property) {
    UNUSED;
    Obj* o = SELF_OBJ;
    if (argc < 3 || !o) return v_bool(false);
    Str* k = v_str_of(vm, argv[0]);
    GetSet* g = gs_find(o, k);
    if (!g) { o->gs = (GetSet*)realloc(o->gs, (size_t)(o->ngs + 1) * sizeof(GetSet)); g = &o->gs[o->ngs++]; g->key = k; }
    g->get = argv[1].t == V_OBJ && argv[1].o->kind == OK_FUNC ? argv[1].o : NULL;
    g->set = argv[2].t == V_OBJ && argv[2].o->kind == OK_FUNC ? argv[2].o : NULL;
    return v_bool(true);
}
NATIVE(n_register_class) {
    UNUSED;
    if (argc < 2) return v_bool(true);
    Str* name = v_str_of(vm, argv[0]);
    Obj* ctor = argv[1].t == V_OBJ && argv[1].o->kind == OK_FUNC ? argv[1].o : NULL;
    for (int i = 0; i < vm->nclasses; i++) if (!strcmp(vm->class_names[i]->s, name->s)) { vm->class_ctors[i] = ctor; return v_bool(true); }
    if (vm->nclasses == vm->capclasses) {
        vm->capclasses = vm->capclasses ? vm->capclasses * 2 : 32;
        vm->class_names = (Str**)realloc(vm->class_names, (size_t)vm->capclasses * sizeof(Str*));
        vm->class_ctors = (Obj**)realloc(vm->class_ctors, (size_t)vm->capclasses * sizeof(Obj*));
    }
    vm->class_names[vm->nclasses] = name; vm->class_ctors[vm->nclasses++] = ctor;
    return v_bool(true);
}
NATIVE(n_fn_call) {
    UNUSED;
    if (!SELF_OBJ || self.o->kind != OK_FUNC) return v_undef();
    Val t = argc && argv[0].t == V_OBJ ? argv[0] : v_undef();
    return call_fn(vm, self.o, t, argc > 1 ? argc - 1 : 0, argv + 1);
}
NATIVE(n_fn_apply) {
    UNUSED;
    if (!SELF_OBJ || self.o->kind != OK_FUNC) return v_undef();
    Val t = argc && argv[0].t == V_OBJ ? argv[0] : v_undef();
    if (argc > 1 && argv[1].t == V_OBJ && argv[1].o->kind == OK_ARRAY) {
        ArrExt* a = argv[1].o->arr;
        int n = a->n > 64 ? 64 : a->n;
        Val args[64];
        memcpy(args, a->items, (size_t)n * sizeof(Val));
        return call_fn(vm, self.o, t, n, args);
    }
    return call_fn(vm, self.o, t, 0, NULL);
}
NATIVE(n_asnative) { UNUSED; return v_obj(native_fn(vm, n_undef, NULL, "ASnative")); }
NATIVE(n_trace) { UNUSED; vm_logf(vm, "trace: %s", v_cstr(vm, ARG(0))); return v_undef(); }

/* -- Array */
static ArrExt* arr_of(Val v) { return v.t == V_OBJ && v.o->kind == OK_ARRAY ? v.o->arr : NULL; }
NATIVE(n_array) {
    UNUSED;
    Obj* a = arr_new(vm);
    if (argc == 1 && argv[0].t == V_NUM) arr_setlen(a, v_int(vm, argv[0]));
    else for (int i = 0; i < argc; i++) arr_push(vm, a, argv[i]);
    return v_obj(a);
}
NATIVE(n_a_push) { UNUSED; ArrExt* a = arr_of(self); if (!a) return v_undef(); for (int i = 0; i < argc; i++) arr_push(vm, self.o, argv[i]); return v_num(a->n); }
NATIVE(n_a_pop) { UNUSED; ArrExt* a = arr_of(self); if (!a || !a->n) return v_undef(); return a->items[--a->n]; }
NATIVE(n_a_shift) {
    UNUSED; ArrExt* a = arr_of(self); if (!a || !a->n) return v_undef();
    Val v = a->items[0]; memmove(a->items, a->items + 1, (size_t)(a->n - 1) * sizeof(Val)); a->n--; return v;
}
NATIVE(n_a_unshift) {
    UNUSED; ArrExt* a = arr_of(self); if (!a) return v_undef();
    int n = a->n; arr_setlen(self.o, n + argc);
    memmove(a->items + argc, a->items, (size_t)n * sizeof(Val));
    for (int i = 0; i < argc; i++) a->items[i] = argv[i];
    return v_num(a->n);
}
static Str* arr_join(VM* vm, ArrExt* a, const char* sep) {
    size_t cap = 64, n = 0, sl = strlen(sep);
    char* b = (char*)malloc(cap);
    for (int i = 0; a && i < a->n; i++) {
        const char* t = (a->items[i].t == V_UNDEF || a->items[i].t == V_NULL) ? "" : v_cstr(vm, a->items[i]);
        size_t tl = strlen(t);
        while (n + tl + sl + 1 > cap) { cap *= 2; b = (char*)realloc(b, cap); }
        if (i) { memcpy(b + n, sep, sl); n += sl; }
        memcpy(b + n, t, tl); n += tl;
    }
    Str* r = str_new(vm, b, n);
    free(b);
    return r;
}
NATIVE(n_a_join) { UNUSED; Val s = ARG(0); return v_s(arr_join(vm, arr_of(self), s.t == V_UNDEF ? "," : v_cstr(vm, s))); }
NATIVE(n_a_tostring) { UNUSED; return v_s(arr_join(vm, arr_of(self), ",")); }
NATIVE(n_a_slice) {
    UNUSED; ArrExt* a = arr_of(self); Obj* r = arr_new(vm); if (!a) return v_obj(r);
    int n = a->n, b = argc ? v_int(vm, argv[0]) : 0, e = argc > 1 && argv[1].t != V_UNDEF ? v_int(vm, argv[1]) : n;
    if (b < 0) b += n;
    if (e < 0) e += n;
    if (b < 0) b = 0;
    if (e > n) e = n;
    for (int i = b; i < e; i++) arr_push(vm, r, a->items[i]);
    return v_obj(r);
}
NATIVE(n_a_splice) {
    UNUSED; ArrExt* a = arr_of(self); if (!a) return v_undef();
    int n = a->n, b = argc ? v_int(vm, argv[0]) : 0;
    if (b < 0) b += n;
    if (b < 0) b = 0;
    if (b > n) b = n;
    int c = argc > 1 ? v_int(vm, argv[1]) : n - b;
    if (c < 0) c = 0;
    if (b + c > n) c = n - b;
    Obj* out = arr_new(vm);
    for (int i = 0; i < c; i++) arr_push(vm, out, a->items[b + i]);
    int add = argc > 2 ? argc - 2 : 0;
    int nn = n - c + add;
    Val* items = (Val*)malloc((size_t)(nn + 1) * sizeof(Val));
    memcpy(items, a->items, (size_t)b * sizeof(Val));
    for (int i = 0; i < add; i++) items[b + i] = argv[2 + i];
    memcpy(items + b + add, a->items + b + c, (size_t)(n - b - c) * sizeof(Val));
    arr_setlen(self.o, nn);
    memcpy(a->items, items, (size_t)nn * sizeof(Val));
    free(items);
    return v_obj(out);
}
NATIVE(n_a_concat) {
    UNUSED; Obj* r = arr_new(vm); ArrExt* a = arr_of(self);
    for (int i = 0; a && i < a->n; i++) arr_push(vm, r, a->items[i]);
    for (int k = 0; k < argc; k++) {
        ArrExt* b = arr_of(argv[k]);
        if (b) for (int i = 0; i < b->n; i++) arr_push(vm, r, b->items[i]);
        else arr_push(vm, r, argv[k]);
    }
    return v_obj(r);
}
NATIVE(n_a_reverse) {
    UNUSED; ArrExt* a = arr_of(self);
    if (a) for (int i = 0, j = a->n - 1; i < j; i++, j--) { Val t = a->items[i]; a->items[i] = a->items[j]; a->items[j] = t; }
    return self;
}
typedef struct { VM* vm; Obj* fn; int opts; Str* field; } SortCtx;
static int sort_cmp(SortCtx* s, Val x, Val y) {
    VM* vm = s->vm;
    if (s->field) {
        x = x.t == V_OBJ ? obj_get_s(vm, x.o, s->field) : v_undef();
        y = y.t == V_OBJ ? obj_get_s(vm, y.o, s->field) : v_undef();
    }
    int r;
    if (s->fn) { Val a[2] = {x, y}; r = v_int(vm, call_fn(vm, s->fn, v_undef(), 2, a)); }
    else if (s->opts & 16) { double a = v_num_of(vm, x), b = v_num_of(vm, y); r = a < b ? -1 : a > b ? 1 : 0; }
    else if (s->opts & 1) r = apt_strcasecmp(v_cstr(vm, x), v_cstr(vm, y));
    else r = strcmp(v_cstr(vm, x), v_cstr(vm, y));
    return (s->opts & 2) && !s->fn ? -r : r;
}
static void arr_sort(SortCtx* s, ArrExt* a) {
    for (int i = 1; i < a->n; i++) {                 /* insertion sort: stable, lists are short */
        Val v = a->items[i]; int j = i;
        while (j > 0 && sort_cmp(s, a->items[j - 1], v) > 0) { a->items[j] = a->items[j - 1]; j--; }
        a->items[j] = v;
    }
}
NATIVE(n_a_sort) {
    UNUSED; ArrExt* a = arr_of(self); if (!a) return self;
    SortCtx s = {vm, NULL, 0, NULL};
    if (argc && argv[0].t == V_OBJ && argv[0].o->kind == OK_FUNC) s.fn = argv[0].o;
    if (argc && argv[argc - 1].t == V_NUM) s.opts = v_int(vm, argv[argc - 1]);
    arr_sort(&s, a);
    return self;
}
NATIVE(n_a_sorton) {
    UNUSED; ArrExt* a = arr_of(self); if (!a || !argc) return self;
    ArrExt* fa = arr_of(argv[0]);
    SortCtx s = {vm, NULL, argc > 1 ? v_int(vm, argv[1]) : 0, fa ? (fa->n ? v_str_of(vm, fa->items[0]) : S("")) : v_str_of(vm, argv[0])};
    if (!(s.opts & 16)) s.opts &= ~1;
    arr_sort(&s, a);
    return self;
}

/* -- String */
static Str* self_str(VM* vm, Val self) { return v_str_of(vm, self); }
NATIVE(n_string) { UNUSED; return argc ? v_s(v_str_of(vm, argv[0])) : v_s(S("")); }
NATIVE(n_s_upper) {
    UNUSED; Str* t = self_str(vm, self); Str* r = str_new(vm, t->s, t->len);
    for (uint32_t i = 0; i < r->len; i++) r->s[i] = (char)toupper((unsigned char)r->s[i]);
    return v_s(r);
}
NATIVE(n_s_lower) {
    UNUSED; Str* t = self_str(vm, self); Str* r = str_new(vm, t->s, t->len);
    for (uint32_t i = 0; i < r->len; i++) r->s[i] = (char)tolower((unsigned char)r->s[i]);
    return v_s(r);
}
NATIVE(n_s_self) { UNUSED; return v_s(self_str(vm, self)); }
NATIVE(n_s_charat) { UNUSED; Str* t = self_str(vm, self); int i = v_int(vm, ARG(0)); return i >= 0 && i < (int)t->len ? v_s(str_new(vm, t->s + i, 1)) : v_s(S("")); }
NATIVE(n_s_charcode) { UNUSED; Str* t = self_str(vm, self); int i = v_int(vm, ARG(0)); return i >= 0 && i < (int)t->len ? v_num((uint8_t)t->s[i]) : v_num(NAN); }
NATIVE(n_s_indexof) {
    UNUSED; if (!argc) return v_num(-1);
    Str* t = self_str(vm, self); Str* n = v_str_of(vm, argv[0]);
    int st = argc > 1 ? v_int(vm, argv[1]) : 0;
    if (st < 0) st = 0;
    if (st > (int)t->len) return v_num(-1);
    const char* p = strstr(t->s + st, n->s);
    return v_num(p ? (double)(p - t->s) : -1);
}
NATIVE(n_s_lastindexof) {
    UNUSED; if (!argc) return v_num(-1);
    Str* t = self_str(vm, self); Str* n = v_str_of(vm, argv[0]);
    for (int i = (int)t->len - (int)n->len; i >= 0; i--) if (!memcmp(t->s + i, n->s, n->len)) return v_num(i);
    return v_num(-1);
}
NATIVE(n_s_substr) {
    UNUSED; Str* t = self_str(vm, self); int L = (int)t->len;
    int b = argc ? v_int(vm, argv[0]) : 0;
    if (b < 0) { b = L + b; if (b < 0) b = 0; }
    if (b > L) b = L;
    int n = argc > 1 && argv[1].t != V_UNDEF ? v_int(vm, argv[1]) : L;
    if (n < 0) n = 0;
    if (b + n > L) n = L - b;
    return v_s(str_new(vm, t->s + b, (size_t)n));
}
NATIVE(n_s_substring) {
    UNUSED; Str* t = self_str(vm, self); int L = (int)t->len;
    int b = argc ? v_int(vm, argv[0]) : 0, e = argc > 1 && argv[1].t != V_UNDEF ? v_int(vm, argv[1]) : L;
    if (b < 0) b = 0;
    if (e < 0) e = 0;
    if (b > e) { int x = b; b = e; e = x; }
    if (b > L) b = L;
    if (e > L) e = L;
    return v_s(str_new(vm, t->s + b, (size_t)(e - b)));
}
NATIVE(n_s_slice) {
    UNUSED; Str* t = self_str(vm, self); int L = (int)t->len;
    int b = argc ? v_int(vm, argv[0]) : 0, e = argc > 1 && argv[1].t != V_UNDEF ? v_int(vm, argv[1]) : L;
    if (b < 0) b += L;
    if (e < 0) e += L;
    if (b < 0) b = 0;
    if (e > L) e = L;
    if (b > e) b = e;
    return v_s(str_new(vm, t->s + b, (size_t)(e - b)));
}
NATIVE(n_s_split) {
    UNUSED; Str* t = self_str(vm, self); Obj* r = arr_new(vm);
    if (!argc || argv[0].t == V_UNDEF) { arr_push(vm, r, v_s(t)); return v_obj(r); }
    Str* d = v_str_of(vm, argv[0]);
    if (!d->len) { for (uint32_t i = 0; i < t->len; i++) arr_push(vm, r, v_s(str_new(vm, t->s + i, 1))); return v_obj(r); }
    const char* p = t->s;
    for (;;) {
        const char* q = strstr(p, d->s);
        if (!q) { arr_push(vm, r, v_s(str_new(vm, p, strlen(p)))); break; }
        arr_push(vm, r, v_s(str_new(vm, p, (size_t)(q - p))));
        p = q + d->len;
    }
    return v_obj(r);
}
NATIVE(n_s_concat) {
    UNUSED; Str* t = self_str(vm, self);
    size_t n = t->len; for (int i = 0; i < argc; i++) n += v_str_of(vm, argv[i])->len;
    char* b = (char*)malloc(n + 1); size_t k = t->len; memcpy(b, t->s, k);
    for (int i = 0; i < argc; i++) { Str* x = v_str_of(vm, argv[i]); memcpy(b + k, x->s, x->len); k += x->len; }
    Str* r = str_new(vm, b, n); free(b);
    return v_s(r);
}
NATIVE(n_s_fromcharcode) {
    UNUSED; char b[256]; int n = 0;
    for (int i = 0; i < argc && n < 255; i++) b[n++] = (char)(v_int(vm, argv[i]) & 0xFF);
    return v_s(str_new(vm, b, (size_t)n));
}

/* -- Number, Boolean, globals */
NATIVE(n_number) { UNUSED; return v_num(argc ? v_num_of(vm, argv[0]) : 0); }
NATIVE(n_boolean) { UNUSED; return v_bool(argc && v_truthy(argv[0])); }
NATIVE(n_num_tostring) {
    UNUSED; double d = v_num_of(vm, self);
    int r = argc ? v_int(vm, argv[0]) : 10;
    if (r == 10 || r < 2 || r > 36 || d != d) return v_s(v_str_of(vm, v_num(d)));
    long long n = (long long)d; bool neg = n < 0; if (neg) n = -n;
    char b[80]; int k = 79; b[k] = 0;
    do { b[--k] = "0123456789abcdefghijklmnopqrstuvwxyz"[n % r]; n /= r; } while (n && k > 1);
    if (neg) b[--k] = '-';
    return v_s(str_new(vm, b + k, strlen(b + k)));
}
NATIVE(n_parseint) {
    UNUSED; const char* t = v_cstr(vm, ARG(0)); int radix = argc > 1 ? v_int(vm, argv[1]) : 0;
    while (isspace((unsigned char)*t)) t++;
    int sgn = 1;
    if (*t == '+' || *t == '-') { if (*t == '-') sgn = -1; t++; }
    if (t[0] == '0' && (t[1] == 'x' || t[1] == 'X') && isxdigit((unsigned char)t[2])) return v_num(sgn * (double)strtoll(t + 2, NULL, 16));
    if (!isdigit((unsigned char)*t)) return v_num(NAN);
    if (radix < 2 || radix > 36) radix = 10;
    char* e; long long v = strtoll(t, &e, radix);
    if (e == t) return v_num(NAN);
    return v_num(sgn * (double)v);
}
NATIVE(n_parsefloat) {
    UNUSED; const char* t = v_cstr(vm, ARG(0)); char* e;
    double d = strtod(t, &e);
    return e == t ? v_num(NAN) : v_num(d);
}
NATIVE(n_isnan) { UNUSED; double d = argc ? v_num_of(vm, argv[0]) : NAN; return v_bool(d != d); }
NATIVE(n_isfinite) { UNUSED; double d = argc ? v_num_of(vm, argv[0]) : NAN; return v_bool(isfinite(d)); }
NATIVE(n_ident_str) { UNUSED; return v_s(v_str_of(vm, ARG(0))); }

/* -- Math */
typedef double (*Fn1)(double);
static double m_round(double x) { return floor(x + 0.5); }
static double m_sqrt(double x) { return x >= 0 ? sqrt(x) : NAN; }
static double m_log(double x) { return x > 0 ? log(x) : NAN; }
NATIVE(n_math1) { UNUSED; double x = argc ? v_num_of(vm, argv[0]) : NAN; return v_num(((Fn1)data)(x)); }
NATIVE(n_math_max) { UNUSED; double r = -INFINITY; for (int i = 0; i < argc; i++) { double x = v_num_of(vm, argv[i]); if (x != x) return v_num(NAN); if (x > r) r = x; } return v_num(r); }
NATIVE(n_math_min) { UNUSED; double r = INFINITY; for (int i = 0; i < argc; i++) { double x = v_num_of(vm, argv[i]); if (x != x) return v_num(NAN); if (x < r) r = x; } return v_num(r); }
NATIVE(n_math_pow) { UNUSED; return argc > 1 ? v_num(pow(v_num_of(vm, argv[0]), v_num_of(vm, argv[1]))) : v_num(NAN); }
NATIVE(n_math_atan2) { UNUSED; return argc > 1 ? v_num(atan2(v_num_of(vm, argv[0]), v_num_of(vm, argv[1]))) : v_num(NAN); }
NATIVE(n_math_random) { UNUSED; return v_num(0.37); }

/* -- Selection, Color, Date */
NATIVE(n_set_focus) { UNUSED; vm->focus = argc && argv[0].t == V_OBJ ? argv[0].o : NULL; return v_bool(true); }
NATIVE(n_get_focus) { UNUSED; return vm->focus && is_disp(vm->focus) ? v_s(target_path(vm, vm->focus)) : v_null(); }
NATIVE(n_color) { UNUSED; if (SELF_OBJ) prop_put(self.o, S("__target"), ARG(0)); return v_undef(); }
static Obj* color_target(VM* vm, Val self) {
    if (self.t != V_OBJ) return NULL;
    Val t = obj_get_s(vm, self.o, S("__target"));
    Obj* o = as_clip(vm, t, vm->levels[0]);
    return o && is_disp(o) ? o : NULL;
}
NATIVE(n_set_rgb) {
    UNUSED; Obj* t = color_target(vm, self);
    if (t && argc) { int c = v_int(vm, argv[0]); DispExt* d = t->d; d->cx[0] = d->cx[1] = d->cx[2] = 0; d->cxadd[0] = (c >> 16) & 255; d->cxadd[1] = (c >> 8) & 255; d->cxadd[2] = c & 255; d->cxadd[3] = 0; }
    return v_undef();
}
NATIVE(n_get_rgb) { UNUSED; Obj* t = color_target(vm, self); return v_num(t ? (t->d->cxadd[0] << 16 | t->d->cxadd[1] << 8 | t->d->cxadd[2]) : 0); }
static double tr_get(VM* vm, Obj* o, const char* k, double def) { Val v = obj_get(vm, o, k); return v.t == V_UNDEF ? def : v_num_of(vm, v); }
NATIVE(n_set_transform) {
    UNUSED; Obj* t = color_target(vm, self);
    if (t && argc && argv[0].t == V_OBJ) {
        Obj* o = argv[0].o; DispExt* d = t->d;
        static const char* const mk[4] = {"ra", "ga", "ba", "aa"}; static const char* const ak[4] = {"rb", "gb", "bb", "ab"};
        for (int i = 0; i < 4; i++) { d->cx[i] = (int)(tr_get(vm, o, mk[i], 100) * 2.55); d->cxadd[i] = (int)tr_get(vm, o, ak[i], 0); }
    }
    return v_undef();
}
NATIVE(n_get_transform) {
    UNUSED; Obj* t = color_target(vm, self); Obj* o = obj_new(vm, vm->object_proto);
    if (t) {
        static const char* const mk[4] = {"ra", "ga", "ba", "aa"}; static const char* const ak[4] = {"rb", "gb", "bb", "ab"};
        for (int i = 0; i < 4; i++) { prop_put(o, cstr(vm, mk[i]), v_num(t->d->cx[i] / 2.55)); prop_put(o, cstr(vm, ak[i]), v_num(t->d->cxadd[i])); }
    }
    return v_obj(o);
}
NATIVE(n_const_num) { UNUSED; return v_num((double)(intptr_t)data); }

/* -- intervals, loading */
NATIVE(n_set_interval) {
    UNUSED; if (!argc) return v_undef();
    Interval iv; memset(&iv, 0, sizeof iv);
    int a0;
    if (argv[0].t == V_OBJ && argv[0].o->kind == OK_FUNC) { iv.fn = argv[0].o; iv.self = v_undef(); iv.ms = argc > 1 ? v_num_of(vm, argv[1]) : 0; a0 = 2; }
    else if (argv[0].t == V_OBJ && argc > 2) { iv.self = argv[0]; iv.method = v_str_of(vm, argv[1]); iv.ms = v_num_of(vm, argv[2]); a0 = 3; }
    else return v_undef();
    if (!(iv.ms >= 1)) iv.ms = 1;
    iv.next = vm->time_ms + iv.ms;
    iv.nargs = argc > a0 ? argc - a0 : 0;
    if (iv.nargs > 16) iv.nargs = 16;
    if (iv.nargs) { iv.args = (Val*)malloc((size_t)iv.nargs * sizeof(Val)); memcpy(iv.args, argv + a0, (size_t)iv.nargs * sizeof(Val)); }
    iv.id = ++vm->next_interval;
    if (vm->nintervals == vm->capintervals) { vm->capintervals = vm->capintervals ? vm->capintervals * 2 : 16; vm->intervals = (Interval*)realloc(vm->intervals, (size_t)vm->capintervals * sizeof(Interval)); }
    vm->intervals[vm->nintervals++] = iv;
    return v_num(iv.id);
}
NATIVE(n_clear_interval) {
    UNUSED; if (!argc) return v_undef();
    int id = v_int(vm, argv[0]);
    for (int i = 0; i < vm->nintervals; i++)
        if (vm->intervals[i].id == id) {
            free(vm->intervals[i].args);
            memmove(&vm->intervals[i], &vm->intervals[i + 1], (size_t)(vm->nintervals - i - 1) * sizeof(Interval));
            vm->nintervals--;
            break;
        }
    return v_undef();
}
NATIVE(n_get_timer) { UNUSED; return v_num(floor(vm->time_ms)); }
NATIVE(n_fscommand) {
    UNUSED; const char* c = v_cstr(vm, ARG(0)); char cmd[128]; snprintf(cmd, sizeof cmd, "%s", c);
    const char* a = argc > 1 ? v_cstr(vm, argv[1]) : "";
    vm_logf(vm, "fscommand %s %s", cmd, a);
    if (vm->host.fscommand) vm->host.fscommand(vm, cmd, a);
    return v_undef();
}
NATIVE(n_get_version) { UNUSED; return v_s(S("PS2 7,0,0,0")); }
NATIVE(n_load_movie_num) { UNUSED; if (argc > 1) queue_load(vm, v_cstr(vm, argv[0]), v_int(vm, argv[1]), NULL); return v_undef(); }
NATIVE(n_unload_movie_num) { UNUSED; if (argc) queue_load(vm, "", v_int(vm, argv[0]), NULL); return v_undef(); }
NATIVE(n_load_movie) {
    UNUSED; if (argc < 2) return v_undef();
    char u[256]; snprintf(u, sizeof u, "%s", v_cstr(vm, argv[0]));
    geturl(vm, u, v_cstr(vm, argv[1]), vm->levels[0]);
    return v_undef();
}

/* -- MovieClip */
static Obj* clip_of(Val self) { return self.t == V_OBJ && self.o->kind == OK_CLIP ? self.o : NULL; }
NATIVE(n_mc_play) { UNUSED; Obj* c = clip_of(self); if (c) c->d->playing = true; return v_undef(); }
NATIVE(n_mc_stop) { UNUSED; Obj* c = clip_of(self); if (c) c->d->playing = false; return v_undef(); }
NATIVE(n_mc_gotostop) { UNUSED; Obj* c = clip_of(self); if (c && argc) goto_clip(vm, c, argv[0], false); return v_undef(); }
NATIVE(n_mc_gotoplay) { UNUSED; Obj* c = clip_of(self); if (c && argc) goto_clip(vm, c, argv[0], true); return v_undef(); }
NATIVE(n_mc_next) { UNUSED; Obj* c = clip_of(self); if (c) goto_clip(vm, c, v_num(c->d->frame + 2), false); return v_undef(); }
NATIVE(n_mc_prev) { UNUSED; Obj* c = clip_of(self); if (c) goto_clip(vm, c, v_num(c->d->frame), false); return v_undef(); }
NATIVE(n_mc_attach) {
    UNUSED; Obj* c = clip_of(self); if (!c || argc < 3) return v_undef();
    Obj* r = attach(vm, c, v_str_of(vm, argv[0]), v_str_of(vm, argv[1]), v_int(vm, argv[2]), argc > 3 && argv[3].t == V_OBJ ? argv[3].o : NULL);
    return v_obj(r);
}
NATIVE(n_mc_create_empty) {
    UNUSED; Obj* c = clip_of(self); if (!c || argc < 2) return v_undef();
    Obj* mc = new_clip(vm, c->d->file, NULL, 0);
    add_child(vm, c, mc, v_int(vm, argv[1]), v_str_of(vm, argv[0]), true);
    construct(vm, mc, NULL, NULL, NULL, 0, NULL);
    return v_obj(mc);
}
NATIVE(n_mc_create_text) {
    UNUSED; Obj* c = clip_of(self); if (!c || argc < 6) return v_undef();
    Obj* tf = new_text(vm, c->d->file, NULL, 0);
    tf->d->rect[0] = tf->d->rect[1] = 0;
    tf->d->rect[2] = (float)v_num_of(vm, argv[4]); tf->d->rect[3] = (float)v_num_of(vm, argv[5]);
    tf->d->m[4] = (float)v_num_of(vm, argv[2]); tf->d->m[5] = (float)v_num_of(vm, argv[3]);
    add_child(vm, c, tf, v_int(vm, argv[1]), v_str_of(vm, argv[0]), true);
    return v_obj(tf);
}
NATIVE(n_mc_duplicate) {
    UNUSED; Obj* c = clip_of(self); if (!c || argc < 2 || !c->d->parent) return v_undef();
    Obj* mc = new_clip(vm, c->d->file, c->d->ch, c->d->chid);
    disp_set_matrix(mc->d, c->d->m);
    add_child(vm, c->d->parent, mc, v_int(vm, argv[1]), v_str_of(vm, argv[0]), true);
    construct(vm, mc, argc > 2 && argv[2].t == V_OBJ ? argv[2].o : NULL, NULL, NULL, 0, NULL);
    return v_obj(mc);
}
NATIVE(n_remove_self) { UNUSED; Obj* o = SELF_OBJ; if (o && is_disp(o) && o->d->parent) remove_child(vm, o->d->parent, o->d->depth); return v_undef(); }
NATIVE(n_mc_swap) {
    UNUSED; Obj* t = SELF_OBJ; if (!t || !is_disp(t) || !t->d->parent || !argc) return v_undef();
    Obj* p = t->d->parent;
    int d2 = argv[0].t == V_OBJ && is_disp(argv[0].o) && argv[0].o->d->parent == p ? argv[0].o->d->depth : v_int(vm, argv[0]);
    int d1 = t->d->depth;
    if (d1 == d2) return v_undef();
    Obj* other = child_at(p, d2);
    DispExt* pd = p->d;
    /* take both out, put back swapped (no unload) */
    for (int i = 0; i < pd->nkids; i++) if (pd->kids[i].o == t || pd->kids[i].o == other) { memmove(&pd->kids[i], &pd->kids[i + 1], (size_t)(pd->nkids - i - 1) * sizeof(Child)); pd->nkids--; i--; }
    bool tdyn = true;
    Str* tn = t->d->name;
    add_child(vm, p, t, d2, tn, tdyn);
    if (other) { bool od = other->d->dynamic; add_child(vm, p, other, d1, other->d->name, od); }
    return v_undef();
}
NATIVE(n_get_depth) { UNUSED; Obj* o = SELF_OBJ; return o && is_disp(o) ? v_num(o->d->depth) : v_undef(); }
NATIVE(n_mc_next_depth) { UNUSED; Obj* c = clip_of(self); return v_num(c && c->d->nkids ? c->d->kids[c->d->nkids - 1].depth + 1 : 0); }
NATIVE(n_mc_at_depth) { UNUSED; Obj* c = clip_of(self); Obj* r = c && argc ? child_at(c, v_int(vm, argv[0])) : NULL; return v_obj(r); }
NATIVE(n_mc_bounds) {
    UNUSED; Obj* o = obj_new(vm, vm->object_proto); Obj* t = SELF_OBJ;
    float b[4] = {0, 0, 0, 0}; bool any = false;
    if (t && is_disp(t)) {
        Obj* sp = argc && argv[0].t == V_OBJ && is_disp(argv[0].o) ? argv[0].o : t;
        vm_bounds(vm, t, sp != t ? sp : NULL, b, &any);
        if (!any) b[0] = b[1] = b[2] = b[3] = 0;
    }
    prop_put(o, S("xMin"), v_num(b[0])); prop_put(o, S("yMin"), v_num(b[1]));
    prop_put(o, S("xMax"), v_num(b[2])); prop_put(o, S("yMax"), v_num(b[3]));
    return v_obj(o);
}
static void xform_point(VM* vm, Val self, Val pt, bool inverse) {
    Obj* t = SELF_OBJ;
    if (!t || !is_disp(t) || pt.t != V_OBJ) return;
    float m[6]; vm_world(t, m);
    if (inverse) inv6(m, m);
    double x = v_num_of(vm, obj_get(vm, pt.o, "x")), y = v_num_of(vm, obj_get(vm, pt.o, "y"));
    obj_set(vm, pt.o, "x", v_num(m[0] * x + m[2] * y + m[4]));
    obj_set(vm, pt.o, "y", v_num(m[1] * x + m[3] * y + m[5]));
}
NATIVE(n_mc_l2g) { UNUSED; xform_point(vm, self, ARG(0), false); return v_undef(); }
NATIVE(n_mc_g2l) { UNUSED; xform_point(vm, self, ARG(0), true); return v_undef(); }
NATIVE(n_mc_load_movie) { UNUSED; Obj* c = clip_of(self); if (c && argc) queue_load(vm, v_cstr(vm, argv[0]), -1, c); return v_undef(); }
NATIVE(n_mc_unload_movie) { UNUSED; Obj* c = clip_of(self); if (c) while (c->d->nkids) remove_child(vm, c, c->d->kids[0].depth); return v_undef(); }
NATIVE(n_mc_set_state) {
    UNUSED; Obj* c = clip_of(self); if (!c || !argc) return v_undef();
    prop_put(c, S("_sState"), argv[0]);
    Str* st = v_str_of(vm, argv[0]);
    if (c->d->ch && apt_label(c->d->ch, st->s) >= 0) goto_clip(vm, c, v_s(st), argc > 1 && v_truthy(argv[1]));
    return v_undef();
}
NATIVE(n_tf_format) { UNUSED; return v_obj(obj_new(vm, vm->object_proto)); }

/* -- Key */
NATIVE(n_key_code) { UNUSED; return v_num(vm->key_code); }
NATIVE(n_key_ascii) { UNUSED; return v_num(vm->key_ascii); }
NATIVE(n_key_isdown) { UNUSED; int c = v_int(vm, ARG(0)); return v_bool(c >= 0 && c < 512 && vm->key_down[c]); }
NATIVE(n_key_add) {
    UNUSED;
    if (!argc || argv[0].t != V_OBJ) return v_bool(false);
    for (int i = 0; i < vm->nkey_listeners; i++) if (vm->key_listeners[i] == argv[0].o) return v_bool(true);
    if (vm->nkey_listeners == vm->capkey_listeners) {
        vm->capkey_listeners = vm->capkey_listeners ? vm->capkey_listeners * 2 : 16;
        vm->key_listeners = (Obj**)realloc(vm->key_listeners, (size_t)vm->capkey_listeners * sizeof(Obj*));
    }
    vm->key_listeners[vm->nkey_listeners++] = argv[0].o;
    return v_bool(true);
}
NATIVE(n_key_remove) {
    UNUSED;
    for (int i = 0; argc && i < vm->nkey_listeners; i++)
        if (argv[0].t == V_OBJ && vm->key_listeners[i] == argv[0].o) {
            memmove(&vm->key_listeners[i], &vm->key_listeners[i + 1], (size_t)(vm->nkey_listeners - i - 1) * sizeof(Obj*));
            vm->nkey_listeners--;
            return v_bool(true);
        }
    return v_bool(false);
}
NATIVE(n_key_stick) {
    UNUSED; Obj* o = obj_new(vm, vm->object_proto);
    prop_put(o, S("fXAxisValue"), v_num(0)); prop_put(o, S("fYAxisValue"), v_num(0));
    return v_obj(o);
}

/* the clips a press goes to: those whose state is their HIGHLIGHTED one */
static void collect_highlighted(VM* vm, Obj* mc, Obj*** out, int* n, int* cap) {
    DispExt* d = mc->d;
    if (d->removed || !d->visible) return;
    int k = prop_find(mc, S("_sState"));
    if (k >= 0) {
        Val st = mc->props[k].v, hl = obj_get_s(vm, mc, S("HIGHLIGHTED"));
        if (hl.t != V_UNDEF && v_eq2(vm, st, hl)) {
            if (*n == *cap) { *cap = *cap ? *cap * 2 : 16; *out = (Obj**)realloc(*out, (size_t)*cap * sizeof(Obj*)); }
            (*out)[(*n)++] = mc;
        }
    }
    for (int i = 0; i < d->nkids; i++) if (d->kids[i].o->kind == OK_CLIP) collect_highlighted(vm, d->kids[i].o, out, n, cap);
}

void vm_key(VM* vm, int code, int ascii, bool down) {
    vm->key_code = code; vm->key_ascii = ascii;
    if (code >= 0 && code < 512) vm->key_down[code] = down;
    Obj** t = NULL; int n = 0, cap = 0;
    for (int i = 0; i < vm->nkey_listeners; i++) {
        if (n == cap) { cap = cap ? cap * 2 : 16; t = (Obj**)realloc(t, (size_t)cap * sizeof(Obj*)); }
        t[n++] = vm->key_listeners[i];
    }
    int nl = n;
    for (int lv = 0; lv < VM_LEVELS; lv++) if (vm->levels[lv]) collect_highlighted(vm, vm->levels[lv], &t, &n, &cap);
    Str* ev = down ? S("onKeyDown") : S("onKeyUp");
    for (int i = 0; i < n; i++) {
        Obj* o = t[i];
        if (i >= nl && o->d->removed) continue;
        Val fn = obj_get_s(vm, o, ev);
        if (fn.t == V_OBJ && fn.o->kind == OK_FUNC) call_fn(vm, fn.o, v_obj(o), 0, NULL);
    }
    free(t);
    vm->key_code = 0; vm->key_ascii = 0;
}

static Obj* add_ctor(VM* vm, const char* name, NativeFn fn, Obj* proto) {
    Obj* c = native_fn(vm, fn, NULL, name);
    if (!proto) proto = obj_new(vm, vm->object_proto);
    prop_put(c, S("prototype"), v_obj(proto));
    prop_put(proto, S("constructor"), v_obj(c));
    prop_put(vm->glob, cstr(vm, name), v_obj(c));
    return c;
}
static void set_num(VM* vm, Obj* o, const char* k, double v) { prop_put(o, cstr(vm, k), v_num(v)); }
static void natives(VM* vm, Obj* o, const char* const* names, NativeFn fn) { for (; *names; names++) obj_set_native(vm, o, *names, fn, NULL); }

static void setup_builtins(VM* vm) {
    Obj* g = vm->glob; Obj* P = vm->object_proto;
    Obj* O = add_ctor(vm, "Object", n_object, P);
    obj_set_native(vm, P, "toString", n_obj_tostring, NULL);
    obj_set_native(vm, P, "valueOf", n_obj_valueof, NULL);
    obj_set_native(vm, P, "hasOwnProperty", n_has_own, NULL);
    obj_set_native(vm, P, "addProperty", n_add_property, NULL);
    static const char* const pf[] = {"watch", "unwatch", "isPrototypeOf", NULL};
    natives(vm, P, pf, n_false);
    obj_set_native(vm, O, "registerClass", n_register_class, NULL);
    add_ctor(vm, "Function", n_undef, vm->function_proto);
    obj_set_native(vm, vm->function_proto, "call", n_fn_call, NULL);
    obj_set_native(vm, vm->function_proto, "apply", n_fn_apply, NULL);
    obj_set_native(vm, g, "ASSetPropFlags", n_undef, NULL);
    obj_set_native(vm, g, "ASnative", n_asnative, NULL);
    obj_set_native(vm, g, "trace", n_trace, NULL);
    /* Array */
    Obj* A = vm->array_proto;
    Obj* Ar = add_ctor(vm, "Array", n_array, A);
    obj_set_native(vm, A, "push", n_a_push, NULL); obj_set_native(vm, A, "pop", n_a_pop, NULL);
    obj_set_native(vm, A, "shift", n_a_shift, NULL); obj_set_native(vm, A, "unshift", n_a_unshift, NULL);
    obj_set_native(vm, A, "join", n_a_join, NULL); obj_set_native(vm, A, "toString", n_a_tostring, NULL);
    obj_set_native(vm, A, "slice", n_a_slice, NULL); obj_set_native(vm, A, "splice", n_a_splice, NULL);
    obj_set_native(vm, A, "concat", n_a_concat, NULL); obj_set_native(vm, A, "reverse", n_a_reverse, NULL);
    obj_set_native(vm, A, "sort", n_a_sort, NULL); obj_set_native(vm, A, "sortOn", n_a_sorton, NULL);
    set_num(vm, Ar, "CASEINSENSITIVE", 1); set_num(vm, Ar, "DESCENDING", 2); set_num(vm, Ar, "UNIQUESORT", 4);
    set_num(vm, Ar, "RETURNINDEXEDARRAY", 8); set_num(vm, Ar, "NUMERIC", 16);
    /* String */
    Obj* SP = vm->string_proto;
    Obj* St = add_ctor(vm, "String", n_string, SP);
    obj_set_native(vm, SP, "toUpperCase", n_s_upper, NULL); obj_set_native(vm, SP, "toLowerCase", n_s_lower, NULL);
    obj_set_native(vm, SP, "toString", n_s_self, NULL); obj_set_native(vm, SP, "valueOf", n_s_self, NULL);
    obj_set_native(vm, SP, "charAt", n_s_charat, NULL); obj_set_native(vm, SP, "charCodeAt", n_s_charcode, NULL);
    obj_set_native(vm, SP, "indexOf", n_s_indexof, NULL); obj_set_native(vm, SP, "lastIndexOf", n_s_lastindexof, NULL);
    obj_set_native(vm, SP, "substr", n_s_substr, NULL); obj_set_native(vm, SP, "substring", n_s_substring, NULL);
    obj_set_native(vm, SP, "slice", n_s_slice, NULL); obj_set_native(vm, SP, "split", n_s_split, NULL);
    obj_set_native(vm, SP, "concat", n_s_concat, NULL);
    obj_set_native(vm, St, "fromCharCode", n_s_fromcharcode, NULL);
    /* Number, Boolean */
    Obj* Nu = add_ctor(vm, "Number", n_number, vm->number_proto);
    obj_set_native(vm, vm->number_proto, "toString", n_num_tostring, NULL);
    set_num(vm, Nu, "MAX_VALUE", 1.7976931348623157e308); set_num(vm, Nu, "MIN_VALUE", 5e-324);
    set_num(vm, Nu, "NaN", NAN); set_num(vm, Nu, "POSITIVE_INFINITY", INFINITY); set_num(vm, Nu, "NEGATIVE_INFINITY", -INFINITY);
    add_ctor(vm, "Boolean", n_boolean, vm->boolean_proto);
    obj_set_native(vm, g, "parseInt", n_parseint, NULL); obj_set_native(vm, g, "parseFloat", n_parsefloat, NULL);
    obj_set_native(vm, g, "isNaN", n_isnan, NULL); obj_set_native(vm, g, "isFinite", n_isfinite, NULL);
    set_num(vm, g, "NaN", NAN); set_num(vm, g, "Infinity", INFINITY);
    obj_set_native(vm, g, "escape", n_ident_str, NULL); obj_set_native(vm, g, "unescape", n_ident_str, NULL);
    /* Math */
    Obj* M = obj_new(vm, P);
    static const struct { const char* n; Fn1 f; } m1[] = {{"abs", fabs}, {"floor", floor}, {"ceil", ceil}, {"sqrt", m_sqrt}, {"sin", sin},
        {"cos", cos}, {"tan", tan}, {"atan", atan}, {"asin", asin}, {"acos", acos}, {"exp", exp}, {"log", m_log}, {"round", m_round}};
    for (size_t i = 0; i < sizeof m1 / sizeof m1[0]; i++) obj_set_native(vm, M, m1[i].n, n_math1, (void*)m1[i].f);
    obj_set_native(vm, M, "max", n_math_max, NULL); obj_set_native(vm, M, "min", n_math_min, NULL);
    obj_set_native(vm, M, "pow", n_math_pow, NULL); obj_set_native(vm, M, "atan2", n_math_atan2, NULL);
    obj_set_native(vm, M, "random", n_math_random, NULL);
    set_num(vm, M, "PI", M_PI); set_num(vm, M, "E", 2.718281828459045); set_num(vm, M, "SQRT2", 1.4142135623730951);
    prop_put(g, S("Math"), v_obj(M));
    /* Key, Mouse, Stage, Selection, System */
    Obj* K = obj_new(vm, P);
    static const struct { const char* n; int v; } keys[] = {{"BACKSPACE", 8}, {"ENTER", 13}, {"SHIFT", 16}, {"CONTROL", 17}, {"ESCAPE", 27},
        {"SPACE", 32}, {"LEFT", 37}, {"UP", 38}, {"RIGHT", 39}, {"DOWN", 40}, {"TAB", 9}, {"DELETEKEY", 46}, {"HOME", 36}, {"END", 35},
        {"PGUP", 33}, {"PGDN", 34}, {"INSERT", 45}, {"CAPSLOCK", 20}};
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) set_num(vm, K, keys[i].n, keys[i].v);
    obj_set_native(vm, K, "isDown", n_key_isdown, NULL); obj_set_native(vm, K, "getCode", n_key_code, NULL);
    obj_set_native(vm, K, "getAscii", n_key_ascii, NULL);
    obj_set_native(vm, K, "addListener", n_key_add, NULL); obj_set_native(vm, K, "removeListener", n_key_remove, NULL);
    obj_set_native(vm, K, "getAnalogStickInfo", n_key_stick, NULL);
    static const char* const lst[] = {NULL};
    natives(vm, K, lst, n_true);
    prop_put(g, S("Key"), v_obj(K));
    Obj* Mo = obj_new(vm, P);
    static const char* const mo[] = {"addListener", "removeListener", "show", "hide", NULL};
    natives(vm, Mo, mo, n_true);
    prop_put(g, S("Mouse"), v_obj(Mo));
    Obj* Sg = obj_new(vm, P);
    set_num(vm, Sg, "width", 854); set_num(vm, Sg, "height", 480);
    prop_put(Sg, S("scaleMode"), v_s(S("noScale")));
    obj_set_native(vm, Sg, "addListener", n_true, NULL);
    prop_put(g, S("Stage"), v_obj(Sg));
    Obj* Se = obj_new(vm, P);
    obj_set_native(vm, Se, "setFocus", n_set_focus, NULL); obj_set_native(vm, Se, "getFocus", n_get_focus, NULL);
    static const char* const se[] = {"addListener", "removeListener", "setSelection", NULL};
    natives(vm, Se, se, n_true);
    prop_put(g, S("Selection"), v_obj(Se));
    Obj* Sy = obj_new(vm, P); Obj* Ca = obj_new(vm, P);
    prop_put(Ca, S("language"), v_s(S("en")));
    prop_put(Ca, S("os"), v_s(vm->flash_mode ? S("Windows") : S("")));
    prop_put(Sy, S("capabilities"), v_obj(Ca));
    prop_put(g, S("System"), v_obj(Sy));
    /* Sound, Color, Date */
    Obj* SoP = obj_new(vm, P);
    static const char* const so[] = {"attachSound", "start", "stop", "setVolume", "setPan", "loadSound", "setTransform", NULL};
    natives(vm, SoP, so, n_undef);
    obj_set_native(vm, SoP, "getVolume", n_hundred, NULL);
    add_ctor(vm, "Sound", n_undef, SoP);
    Obj* CoP = obj_new(vm, P);
    obj_set_native(vm, CoP, "setRGB", n_set_rgb, NULL); obj_set_native(vm, CoP, "getRGB", n_get_rgb, NULL);
    obj_set_native(vm, CoP, "setTransform", n_set_transform, NULL); obj_set_native(vm, CoP, "getTransform", n_get_transform, NULL);
    add_ctor(vm, "Color", n_color, CoP);
    Obj* DaP = obj_new(vm, P);
    static const struct { const char* n; double v; } da[] = {{"getFullYear", 2005}, {"getYear", 105}, {"getMonth", 0}, {"getDate", 11},
        {"getDay", 2}, {"getHours", 20}, {"getMinutes", 15}, {"getSeconds", 0}, {"getMilliseconds", 0}, {"getTime", 1105500000000.0}};
    for (size_t i = 0; i < sizeof da / sizeof da[0]; i++) obj_set_native(vm, DaP, da[i].n, n_const_num, (void*)(intptr_t)da[i].v);
    add_ctor(vm, "Date", n_undef, DaP);
    /* MovieClip, TextField */
    Obj* MP = vm->mc_proto;
    add_ctor(vm, "MovieClip", n_undef, MP);
    add_ctor(vm, "TextField", n_undef, vm->tf_proto);
    obj_set_native(vm, MP, "play", n_mc_play, NULL); obj_set_native(vm, MP, "stop", n_mc_stop, NULL);
    obj_set_native(vm, MP, "gotoAndStop", n_mc_gotostop, NULL); obj_set_native(vm, MP, "gotoAndPlay", n_mc_gotoplay, NULL);
    obj_set_native(vm, MP, "nextFrame", n_mc_next, NULL); obj_set_native(vm, MP, "prevFrame", n_mc_prev, NULL);
    obj_set_native(vm, MP, "attachMovie", n_mc_attach, NULL); obj_set_native(vm, MP, "createEmptyMovieClip", n_mc_create_empty, NULL);
    obj_set_native(vm, MP, "createTextField", n_mc_create_text, NULL); obj_set_native(vm, MP, "duplicateMovieClip", n_mc_duplicate, NULL);
    obj_set_native(vm, MP, "removeMovieClip", n_remove_self, NULL); obj_set_native(vm, MP, "swapDepths", n_mc_swap, NULL);
    obj_set_native(vm, MP, "getDepth", n_get_depth, NULL); obj_set_native(vm, MP, "getNextHighestDepth", n_mc_next_depth, NULL);
    obj_set_native(vm, MP, "getInstanceAtDepth", n_mc_at_depth, NULL); obj_set_native(vm, MP, "getBounds", n_mc_bounds, NULL);
    obj_set_native(vm, MP, "localToGlobal", n_mc_l2g, NULL); obj_set_native(vm, MP, "globalToLocal", n_mc_g2l, NULL);
    obj_set_native(vm, MP, "hitTest", n_false, NULL); obj_set_native(vm, MP, "setMask", n_undef, NULL);
    obj_set_native(vm, MP, "loadMovie", n_mc_load_movie, NULL); obj_set_native(vm, MP, "unloadMovie", n_mc_unload_movie, NULL);
    static const char* const mcn[] = {"startDrag", "stopDrag", "loadVariables", "beginFill", "endFill", "lineStyle", "moveTo", "lineTo",
        "clear", "curveTo", "beginGradientFill", "attachText", "AnimCB", NULL};
    natives(vm, MP, mcn, n_undef);
    obj_set_native(vm, MP, "getBytesLoaded", n_hundred, NULL); obj_set_native(vm, MP, "getBytesTotal", n_hundred, NULL);
    obj_set_native(vm, MP, "_setState", n_mc_set_state, NULL);
    Obj* TP = vm->tf_proto;
    static const char* const tfn[] = {"setTextFormat", "setNewTextFormat", "replaceSel", "addListener", "removeListener", NULL};
    natives(vm, TP, tfn, n_undef);
    obj_set_native(vm, TP, "removeTextField", n_remove_self, NULL);
    obj_set_native(vm, TP, "getTextFormat", n_tf_format, NULL);
    obj_set_native(vm, TP, "getDepth", n_get_depth, NULL);
    prop_put(g, S("MCPrototype"), v_obj(MP));        /* Apt: the movie clip prototype, by this name */
    prop_put(g, S("TFPrototype"), v_obj(TP));
    obj_set_native(vm, g, "setInterval", n_set_interval, NULL); obj_set_native(vm, g, "clearInterval", n_clear_interval, NULL);
    obj_set_native(vm, g, "getTimer", n_get_timer, NULL); obj_set_native(vm, g, "fscommand", n_fscommand, NULL);
    obj_set_native(vm, g, "updateAfterEvent", n_undef, NULL); obj_set_native(vm, g, "getVersion", n_get_version, NULL);
    obj_set_native(vm, g, "loadMovieNum", n_load_movie_num, NULL); obj_set_native(vm, g, "unloadMovieNum", n_unload_movie_num, NULL);
    obj_set_native(vm, g, "loadMovie", n_load_movie, NULL);
    add_ctor(vm, "XML", n_undef, NULL);
    add_ctor(vm, "LoadVars", n_undef, NULL);
}

/* ============================================================ life */
VM* vm_new(const VMHost* host, bool flash_mode) {
    VM* vm = (VM*)calloc(1, sizeof(VM));
    if (host) vm->host = *host;
    vm->flash_mode = flash_mode;
    vm->object_proto = obj_new(vm, NULL);
    vm->function_proto = obj_new(vm, vm->object_proto);
    vm->array_proto = obj_new(vm, vm->object_proto);
    vm->string_proto = obj_new(vm, vm->object_proto);
    vm->number_proto = obj_new(vm, vm->object_proto);
    vm->boolean_proto = obj_new(vm, vm->object_proto);
    vm->mc_proto = obj_new(vm, vm->object_proto);
    vm->tf_proto = obj_new(vm, vm->object_proto);
    vm->shape_proto = obj_new(vm, vm->object_proto);
    vm->glob = obj_new(vm, vm->object_proto);
    setup_builtins(vm);
    return vm;
}

/* ---- garbage collection: mark from the roots, free the rest */
typedef struct { Gc** a; int n, cap; } MarkStack;
static void mark(MarkStack* ms, void* p) {
    Gc* g = (Gc*)p;
    if (!g || g->gc_mark) return;
    g->gc_mark = 1;
    if (g->gc_kind == GK_STR) return;
    if (ms->n == ms->cap) { ms->cap = ms->cap ? ms->cap * 2 : 1024; ms->a = (Gc**)realloc(ms->a, (size_t)ms->cap * sizeof(Gc*)); }
    ms->a[ms->n++] = g;
}
static void mark_val(MarkStack* ms, Val v) {
    if (v.t == V_STR) mark(ms, v.s);
    else if (v.t == V_OBJ) mark(ms, v.o);
}
static void mark_obj_fields(MarkStack* ms, Obj* o) {
    mark(ms, o->proto);
    for (int i = 0; i < o->nprops; i++) { mark(ms, o->props[i].key); mark_val(ms, o->props[i].v); }
    for (int i = 0; i < o->ngs; i++) { mark(ms, o->gs[i].key); mark(ms, o->gs[i].get); mark(ms, o->gs[i].set); }
    switch (o->kind) {
    case OK_FUNC:
        if (o->fn) {
            for (int i = 0; i < o->fn->npool; i++) mark_val(ms, o->fn->pool[i]);
            for (int i = 0; i < o->fn->nscope; i++) mark(ms, o->fn->scope[i]);
            mark(ms, o->fn->name);
        }
        break;
    case OK_ARRAY:
        for (int i = 0; i < o->arr->n; i++) mark_val(ms, o->arr->items[i]);
        break;
    case OK_CLIP: case OK_TEXT: case OK_SHAPE: {
        DispExt* d = o->d;
        mark(ms, d->parent); mark(ms, d->name); mark(ms, d->text); mark(ms, d->var);
        for (int i = 0; i < d->nkids; i++) mark(ms, d->kids[i].o);
        break;
    }
    default: break;
    }
}
static void obj_free(Obj* o) {
    free(o->props); free(o->index); free(o->gs);
    switch (o->kind) {
    case OK_FUNC: if (o->fn) { free(o->fn->pool); free(o->fn->scope); free(o->fn); } break;
    case OK_ARRAY: if (o->arr) { free(o->arr->items); free(o->arr); } break;
    case OK_CLIP: case OK_TEXT: case OK_SHAPE: if (o->d) { free(o->d->kids); free(o->d); } break;
    default: break;
    }
    free(o);
}

void vm_gc(VM* vm) {
    MarkStack ms = {0};
    Obj* roots[] = {vm->glob, vm->object_proto, vm->function_proto, vm->array_proto, vm->string_proto, vm->number_proto,
        vm->boolean_proto, vm->mc_proto, vm->tf_proto, vm->shape_proto, vm->focus};
    for (size_t i = 0; i < sizeof roots / sizeof roots[0]; i++) mark(&ms, roots[i]);
    for (int i = 0; i < VM_LEVELS; i++) mark(&ms, vm->levels[i]);
    for (int i = 0; i < vm->nintervals; i++) {
        Interval* iv = &vm->intervals[i];
        mark(&ms, iv->fn); mark(&ms, iv->method); mark_val(&ms, iv->self);
        for (int k = 0; k < iv->nargs; k++) mark_val(&ms, iv->args[k]);
    }
    for (int i = 0; i < vm->nloads; i++) mark(&ms, vm->loads[i].clip);
    for (int i = 0; i < vm->nclasses; i++) { mark(&ms, vm->class_names[i]); mark(&ms, vm->class_ctors[i]); }
    for (int i = 0; i < vm->nthis && i < 128; i++) mark_val(&ms, vm->this_stack[i]);
    for (int i = 0; i < vm->nkey_listeners; i++) mark(&ms, vm->key_listeners[i]);
    for (Gc* g = vm->gc_all; g; g = g->gc_next) if (g->pinned) { g->gc_mark = 0; mark(&ms, g); }
    while (ms.n) mark_obj_fields(&ms, (Obj*)ms.a[--ms.n]);
    free(ms.a);
    Gc** link = &vm->gc_all;
    size_t live = 0;
    while (*link) {
        Gc* g = *link;
        if (g->gc_mark) { g->gc_mark = 0; link = &g->gc_next; live++; continue; }
        *link = g->gc_next;
        if (g->gc_kind == GK_STR) free(g);
        else obj_free((Obj*)g);
    }
    if (getenv("APT_GCLOG")) {
        size_t no = 0, ns = 0;
        for (Gc* q = vm->gc_all; q; q = q->gc_next) { if (q->gc_kind == GK_STR) ns++; else no++; }
        vm_logf(vm, "gc: %zu objects, %zu strings live", no, ns);
    }
    vm->gc_bytes = live * 64;
    vm->gc_last = vm->gc_bytes;
}

void vm_free(VM* vm) {
    if (!vm) return;
    Gc* g = vm->gc_all;
    while (g) { Gc* n = g->gc_next; if (g->gc_kind == GK_STR) free(g); else obj_free((Obj*)g); g = n; }
    for (int i = 0; i < vm->nintervals; i++) free(vm->intervals[i].args);
    free(vm->intervals); free(vm->loads); free(vm->key_listeners); free(vm->class_names); free(vm->class_ctors);
    if (g_cs_vm == vm) { free(g_cs); g_cs = NULL; g_cs_cap = g_cs_used = 0; g_cs_vm = NULL; }
    free(vm);
}

AptChar* vm_resolve_char(VM* vm, AptFile* f, int cid, AptFile** of, int* ocid) { return resolve_char(vm, f, cid, of, ocid, 0); }
