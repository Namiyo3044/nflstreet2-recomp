#include "apt/aptutil.h"
#include "apt/aptfile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SIG 0x09876543u

static uint32_t u32at(const uint8_t* b, uint32_t n, uint32_t o) {
    return o + 4 <= n ? (uint32_t)b[o] | (uint32_t)b[o + 1] << 8 | (uint32_t)b[o + 2] << 16 | (uint32_t)b[o + 3] << 24 : 0u;
}
static float f32at(const uint8_t* b, uint32_t n, uint32_t o) {
    uint32_t v = u32at(b, n, o); float f; memcpy(&f, &v, 4); return f;
}
#define U32(o) u32at(f->b, f->n, (o))
#define I32(o) ((int32_t)u32at(f->b, f->n, (o)))
#define F32(o) f32at(f->b, f->n, (o))
#define U8(o) ((o) < f->n ? f->b[(o)] : 0)

const char* apt_cstr(AptFile* f, uint32_t off) {
    if (!off || off >= f->n) return NULL;
    if (!memchr(f->b + off, 0, f->n - off)) return NULL;
    return (const char*)f->b + off;
}

static void parse_frames(AptFile* f, uint32_t o, int* nframes, AptFrame** frames) {
    uint32_t n = U32(o), lo = U32(o + 4);
    if (n > 100000) n = 0;
    *nframes = (int)n;
    *frames = (AptFrame*)calloc(n ? n : 1, sizeof(AptFrame));
    for (uint32_t i = 0; i < n; i++) {
        uint32_t fo = lo + i * 8, m = U32(fo), il = U32(fo + 4);
        if (m > 10000) m = 0;
        AptFrame* fr = &(*frames)[i];
        fr->items = (AptItem*)calloc(m ? m : 1, sizeof(AptItem));
        for (uint32_t k = 0; k < m; k++) {
            uint32_t p = U32(il + k * 4);
            if (!p) continue;
            uint32_t t = U32(p);
            AptItem* it = &fr->items[fr->n];
            it->kind = (int)t;
            if (t == IT_ACTION) it->code = U32(p + 4);
            else if (t == IT_LABEL) it->label = apt_cstr(f, U32(p + 4));
            else if (t == IT_REMOVE) it->arg = I32(p + 4);
            else if (t == IT_INIT) { it->arg = I32(p + 4); it->code = U32(p + 8); }
            else if (t == IT_BG) { }
            else if (t == IT_PLACE) {
                AptPlace* pl = (AptPlace*)calloc(1, sizeof *pl);
                uint32_t fl = U32(p + 4);
                pl->flags = fl; pl->depth = I32(p + 8); pl->ch = (fl & 2) ? I32(p + 12) : -1;
                for (int j = 0; j < 6; j++) pl->m[j] = F32(p + 16 + j * 4);
                for (int j = 0; j < 4; j++) { pl->tint[j] = U8(p + 40 + j); pl->add[j] = U8(p + 44 + j); }
                pl->ratio = F32(p + 48);
                pl->name = (fl & 32) ? apt_cstr(f, U32(p + 52)) : NULL;
                pl->clipdepth = (fl & 64) ? I32(p + 56) : 0;
                if (fl & 128) {
                    uint32_t po = U32(p + 60);
                    if (po) {
                        uint32_t ne = U32(po), eo = U32(po + 4);
                        if (ne > 64) ne = 0;
                        pl->events = (AptEvent*)calloc(ne ? ne : 1, sizeof(AptEvent));
                        for (uint32_t e = 0; e < ne; e++) {
                            uint32_t q = eo + e * 16;
                            pl->events[e].flags = (uint32_t)U8(q) | (uint32_t)U8(q + 1) << 8 | (uint32_t)U8(q + 2) << 16;
                            pl->events[e].key = U8(q + 3);
                            pl->events[e].code = U32(q + 8);
                        }
                        pl->nevents = (int)ne;
                    }
                }
                it->place = pl;
            } else continue;
            fr->n++;
        }
    }
}

static void collect_labels(AptChar* c) {
    int n = 0;
    for (int i = 0; i < c->nframes; i++)
        for (int k = 0; k < c->frames[i].n; k++) if (c->frames[i].items[k].kind == IT_LABEL && c->frames[i].items[k].label) n++;
    c->label_names = (const char**)calloc(n ? n : 1, sizeof(char*));
    c->label_frames = (int*)calloc(n ? n : 1, sizeof(int));
    for (int i = 0; i < c->nframes; i++)
        for (int k = 0; k < c->frames[i].n; k++)
            if (c->frames[i].items[k].kind == IT_LABEL && c->frames[i].items[k].label) {
                c->label_names[c->nlabels] = c->frames[i].items[k].label;
                c->label_frames[c->nlabels++] = i;
            }
}

int apt_label(AptChar* c, const char* label) {
    for (int i = 0; i < c->nlabels; i++) if (!strcmp(c->label_names[i], label)) return c->label_frames[i];
    for (int i = 0; i < c->nlabels; i++) if (!apt_strcasecmp(c->label_names[i], label)) return c->label_frames[i];
    return -1;
}

static AptChar* parse_char(AptFile* f, uint32_t p, int depth) {
    if (!p || U32(p + 4) != SIG || depth > 4) return NULL;
    uint32_t t = U32(p), q = p + 8;
    AptChar* c = (AptChar*)calloc(1, sizeof *c);
    switch (t) {
    case 9: {
        c->type = CH_MOVIE;
        parse_frames(f, q, &c->nframes, &c->frames);
        uint32_t nch = U32(q + 12), co = U32(q + 16);
        if (nch > 100000) nch = 0;
        c->w = (int)U32(q + 20); c->h = (int)U32(q + 24); c->ms = (int)U32(q + 28);
        uint32_t ni = U32(q + 32), io = U32(q + 36), ne = U32(q + 40), eo = U32(q + 44);
        if (ni > 10000) ni = 0;
        if (ne > 10000) ne = 0;
        c->nchars = (int)nch;
        c->chars = (AptChar**)calloc(nch ? nch : 1, sizeof(AptChar*));
        for (uint32_t i = 1; i < nch; i++) c->chars[i] = parse_char(f, U32(co + i * 4), depth + 1);
        c->nimports = (int)ni;
        c->imports = (AptImport*)calloc(ni ? ni : 1, sizeof(AptImport));
        for (uint32_t i = 0; i < ni; i++) {
            c->imports[i].movie = apt_cstr(f, U32(io + i * 16));
            c->imports[i].name = apt_cstr(f, U32(io + i * 16 + 4));
            c->imports[i].ch = (int)U32(io + i * 16 + 8);
        }
        c->nexports = (int)ne;
        c->exports = (AptExport*)calloc(ne ? ne : 1, sizeof(AptExport));
        for (uint32_t i = 0; i < ne; i++) {
            c->exports[i].name = apt_cstr(f, U32(eo + i * 8));
            c->exports[i].ch = (int)U32(eo + i * 8 + 4);
        }
        break;
    }
    case 1: c->type = CH_SHAPE; for (int i = 0; i < 4; i++) c->bounds[i] = F32(q + i * 4); c->geom = U32(q + 16); break;
    case 2:
        c->type = CH_TEXT;
        for (int i = 0; i < 4; i++) c->rect[i] = F32(q + i * 4);
        c->font = (int)U32(q + 16); c->align = (int)U32(q + 20);
        for (int i = 0; i < 4; i++) c->color[i] = U8(q + 24 + i);
        c->height = F32(q + 28); c->multiline = (int)U32(q + 36); c->wordwrap = (int)U32(q + 40);
        c->content = apt_cstr(f, U32(q + 44)); c->var = apt_cstr(f, U32(q + 48));
        break;
    case 3: c->type = CH_FONT; c->fontname = apt_cstr(f, U32(q)); break;
    case 4: c->type = CH_BUTTON; break;
    case 5: c->type = CH_SPRITE; parse_frames(f, q, &c->nframes, &c->frames); break;
    case 7: c->type = CH_IMAGE; c->tex = U32(q); break;
    default: c->type = CH_NONE; break;
    }
    if (c->type == CH_MOVIE || c->type == CH_SPRITE) collect_labels(c);
    return c;
}

AptFile* apt_open(TerfEntry* e, const char* name) {
    if (!e || !e->pack || e->pack->count < 2) return NULL;
    TerfPack* pk = e->pack;
    AptFile* f = (AptFile*)calloc(1, sizeof *f);
    snprintf(f->name, sizeof f->name, "%s", name);
    f->entry = e;
    f->b = pk->entries[0].data; f->n = pk->entries[0].size;
    f->c = pk->entries[1].data; f->cn = pk->entries[1].size;
    if (!f->b || !f->c || f->cn < 32 || memcmp(f->c, "Apt constant file", 17) != 0) { free(f); return NULL; }
    uint32_t entry = u32at(f->c, f->cn, 20), nc = u32at(f->c, f->cn, 24);
    if (nc > 200000) nc = 0;
    f->nconsts = (int)nc;
    f->consts = (AptConst*)calloc(nc ? nc : 1, sizeof(AptConst));
    for (uint32_t i = 0; i < nc; i++) {
        uint32_t t = u32at(f->c, f->cn, 32 + i * 8), v = u32at(f->c, f->cn, 36 + i * 8);
        AptConst* k = &f->consts[i];
        if (t == 1) { k->type = KC_STR; k->s = (v < f->cn && memchr(f->c + v, 0, f->cn - v)) ? (const char*)f->c + v : ""; }
        else if (t == 4) { k->type = KC_REG; k->n = v; }
        else if (t == 5) { k->type = KC_BOOL; k->n = v != 0; }
        else if (t == 6) { float x; memcpy(&x, &v, 4); k->type = KC_NUM; k->n = x; }
        else if (t == 7) { k->type = KC_NUM; k->n = (int32_t)v; }
        else k->type = KC_UNDEF;
    }
    /* the movie: its entry is in the constants file, into the movie data */
    f->movie = parse_char(f, entry, 0);
    if (!f->movie || f->movie->type != CH_MOVIE) { fprintf(stderr, "apt: %s has no movie\n", name); }
    if (pk->count >= 3 && pk->entries[2].data && pk->entries[2].size >= 4) {
        const uint8_t* ix = pk->entries[2].data; uint32_t in = pk->entries[2].size;
        uint32_t n = u32at(ix, in, 0);
        if (4 + n * 8 == in) {
            f->nres = (int)n;
            f->res_cid = (uint32_t*)calloc(n ? n : 1, 4); f->res_kind = (uint32_t*)calloc(n ? n : 1, 4);
            f->res_entry = (int*)calloc(n ? n : 1, sizeof(int));
            f->geoms = (AptGeom**)calloc(n ? n : 1, sizeof(void*)); f->images = (AptImage**)calloc(n ? n : 1, sizeof(void*));
            for (uint32_t i = 0; i < n; i++) {
                f->res_cid[i] = u32at(ix, in, 4 + i * 8); f->res_kind[i] = u32at(ix, in, 8 + i * 8);
                f->res_entry[i] = (int)(3 + i) < pk->count ? (int)(3 + i) : -1;
            }
        }
    }
    return f;
}

/* ------------------------------------------------------------ resources --- */
static int res_find(AptFile* f, uint32_t cid, uint32_t kind) {
    for (int i = 0; i < f->nres; i++) if (f->res_cid[i] == cid && f->res_kind[i] == kind) return i;
    return -1;
}

AptGeom* apt_geom(AptFile* f, uint32_t cid) {
    int r = res_find(f, cid, 0);
    if (r < 0 || f->res_entry[r] < 0) return NULL;
    if (f->geoms[r]) return f->geoms[r];
    TerfEntry* e = &f->entry->pack->entries[f->res_entry[r]];
    const uint8_t* b = e->data; uint32_t n = e->size;
    if (!b || n < 2) return NULL;
    AptGeom* g = (AptGeom*)calloc(1, sizeof *g);
    uint32_t p = 0;
    int nl = b[0] | b[1] << 8; p = 2;
    int cap = 16; g->recs = (AptGeomRec*)calloc((size_t)cap, sizeof(AptGeomRec));
    for (int l = 0; l < nl && p + 2 <= n; l++) {
        int nr = b[p] | b[p + 1] << 8; p += 2;
        for (int k = 0; k < nr && p + 44 <= n; k++) {
            AptGeomRec rec;
            rec.count = u32at(b, n, p); rec.style = u32at(b, n, p + 4); rec.param = f32at(b, n, p + 8);
            rec.color = u32at(b, n, p + 12); rec.image = u32at(b, n, p + 16); p += 20;
            for (int j = 0; j < 6; j++) rec.mtx[j] = f32at(b, n, p + j * 4);
            p += 24;
            uint32_t npts = rec.count * (rec.style == 3 ? 2u : 3u);
            if ((uint64_t)p + npts * 8 > n) break;
            rec.npts = (int)npts;
            rec.pts = (float*)malloc(npts * 8 + 8);
            memcpy(rec.pts, b + p, npts * 8);
            p += npts * 8;
            if (g->n == cap) { cap *= 2; g->recs = (AptGeomRec*)realloc(g->recs, (size_t)cap * sizeof(AptGeomRec)); }
            g->recs[g->n++] = rec;
        }
    }
    f->geoms[r] = g;
    return g;
}

AptImage* apt_image(AptFile* f, uint32_t cid) {
    int r = res_find(f, cid, 1);
    if (r < 0 || f->res_entry[r] < 0) return NULL;
    if (f->images[r]) return f->images[r];
    TerfEntry* e = &f->entry->pack->entries[f->res_entry[r]];
    const uint8_t* b = e->data; uint32_t n = e->size;
    if (!b || n < 0x38 || memcmp(b, "MMAP", 4) != 0) return NULL;
    int w = b[0x28] | b[0x29] << 8, h = b[0x2A] | b[0x2B] << 8, fmt = b[0x2C] | b[0x2D] << 8;
    uint32_t poff = u32at(b, n, 0x34);
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return NULL;
    int ncol = fmt == 1 ? 256 : 16;
    uint32_t pixbytes = fmt == 1 ? (uint32_t)(w * h) : (uint32_t)(w * h / 2);
    uint32_t pal_off = poff + pixbytes + 0x10;
    if ((uint64_t)pal_off + (uint32_t)ncol * 4 > n) return NULL;
    uint32_t pal[256];
    for (int i = 0; i < ncol; i++) {
        int j = i;
        if (ncol == 256) {             /* PS2 CLUT order: entries 8-15 and 16-23 of every 32 swapped */
            if ((i & 0x18) == 0x08) j = i + 8;
            else if ((i & 0x18) == 0x10) j = i - 8;
        }
        const uint8_t* c = b + pal_off + j * 4;
        uint32_t a = c[3] * 2u; if (a > 255) a = 255;
        pal[i] = a << 24 | (uint32_t)c[0] << 16 | (uint32_t)c[1] << 8 | c[2];
    }
    AptImage* im = (AptImage*)calloc(1, sizeof *im);
    im->w = w; im->h = h; im->px = (uint32_t*)malloc((size_t)w * h * 4);
    for (int i = 0; i < w * h; i++) {
        int idx;
        if (fmt == 1) idx = b[poff + i];
        else { uint8_t v = b[poff + i / 2]; idx = (i & 1) ? v >> 4 : v & 15; }
        im->px[i] = pal[idx];
    }
    im->pm = (uint32_t*)malloc((size_t)w * h * 4);
    for (int i = 0; i < w * h; i++) {
        uint32_t c = im->px[i], a = c >> 24;
        im->pm[i] = a << 24 | ((c >> 16 & 255) * a / 255) << 16 | ((c >> 8 & 255) * a / 255) << 8 | (c & 255) * a / 255;
    }
    f->images[r] = im;
    return im;
}

/* --------------------------------------------------------- instructions --- */
static bool aligned_op(uint8_t op) {
    switch (op) {
    case 0x9B: case 0x8E: case 0x88: case 0x9D: case 0x99: case 0x96: case 0x83: case 0x8C: case 0x87: case 0x8B:
    case 0x81: case 0x9F: case 0x94: case 0xA1: case 0xB8: case 0xA4: case 0xA6: case 0xA5: case 0xA7: return true;
    default: return false;
    }
}

static AptIns* ins_slot(AptFile* f, uint32_t pos, bool* found) {
    if (f->ins_used * 2 >= f->ins_cap) {
        uint32_t oc = f->ins_cap, nc = oc ? oc * 2 : 1024;
        uint32_t* op = f->ins_pos; AptIns* oi = f->ins;
        f->ins_pos = (uint32_t*)malloc(nc * 4); f->ins = (AptIns*)calloc(nc, sizeof(AptIns));
        memset(f->ins_pos, 0xFF, nc * 4);
        f->ins_cap = nc; f->ins_used = 0;
        for (uint32_t i = 0; i < oc; i++) if (op[i] != 0xFFFFFFFFu) {
            uint32_t h = (op[i] * 2654435761u) & (nc - 1);
            while (f->ins_pos[h] != 0xFFFFFFFFu) h = (h + 1) & (nc - 1);
            f->ins_pos[h] = op[i]; f->ins[h] = oi[i]; f->ins_used++;
        }
        free(op); free(oi);
    }
    uint32_t h = (pos * 2654435761u) & (f->ins_cap - 1);
    while (f->ins_pos[h] != 0xFFFFFFFFu) {
        if (f->ins_pos[h] == pos) { *found = true; return &f->ins[h]; }
        h = (h + 1) & (f->ins_cap - 1);
    }
    *found = false;
    f->ins_pos[h] = pos; f->ins_used++;
    return &f->ins[h];
}

const AptIns* apt_ins(AptFile* f, uint32_t pos) {
    if (pos >= f->n) return NULL;
    bool found;
    AptIns* in = ins_slot(f, pos, &found);
    if (found) return in;
    memset(in, 0, sizeof *in);
    uint32_t p = pos;
    uint8_t op = U8(p); p++;
    if (aligned_op(op)) p = (p + 3) & ~3u;
    in->op = op;
    switch (op) {
    case 0x81: case 0x87: case 0x9F: case 0x94: case 0xB7: in->i = I32(p); p += 4; break;
    case 0x83: in->s = apt_cstr(f, U32(p)); in->s2 = apt_cstr(f, U32(p + 4)); p += 8; break;
    case 0x88: case 0x96: {
        uint32_t n = U32(p), o = U32(p + 4); p += 8;
        if (n > 65536 || (uint64_t)o + n * 4 > f->n) n = 0;
        in->n = (int)n; in->list = (const uint32_t*)(f->b + o);
        break;
    }
    case 0x8C: case 0x8B: case 0xA1: case 0xA4: case 0xA5: case 0xA6: case 0xA7: in->s = apt_cstr(f, U32(p)); p += 4; break;
    case 0x8E: {
        in->fname = apt_cstr(f, U32(p)); uint32_t n = U32(p + 4);
        in->nregs = U8(p + 8); in->fflags = (int)(U8(p + 9) | U8(p + 10) << 8 | U8(p + 11) << 16);
        uint32_t po = U32(p + 12);
        if (n > 64) n = 0;
        in->nargs = (int)n;
        int* regs = (int*)calloc(n ? n : 1, sizeof(int)); const char** names = (const char**)calloc(n ? n : 1, sizeof(char*));
        for (uint32_t k = 0; k < n; k++) { regs[k] = I32(po + 8 * k); names[k] = apt_cstr(f, U32(po + 8 * k + 4)); }
        in->argregs = regs; in->argnames = names;
        in->body = I32(p + 16); p += 28;
        break;
    }
    case 0x9B: {
        in->fname = apt_cstr(f, U32(p)); uint32_t n = U32(p + 4), po = U32(p + 8);
        if (n > 64) n = 0;
        in->nargs = (int)n;
        const char** names = (const char**)calloc(n ? n : 1, sizeof(char*));
        for (uint32_t k = 0; k < n; k++) names[k] = apt_cstr(f, U32(po + 4 * k));
        in->argnames = names;
        in->body = I32(p + 12); p += 24;
        break;
    }
    case 0x99: case 0x9D: case 0xB8: { int32_t off = I32(p); p += 4; in->i = (int)(p + off); break; }
    case 0xA2: case 0xAE: case 0xAF: case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB9: in->i = U8(p); p += 1; break;
    case 0xB5: in->i = (int8_t)U8(p); p += 1; break;
    case 0xA3: in->i = U8(p) | U8(p + 1) << 8; p += 2; break;
    case 0xB6: in->i = (int16_t)(U8(p) | U8(p + 1) << 8); p += 2; break;
    case 0xB4: in->f = F32(p); p += 4; break;
    default: break;
    }
    in->next = p;
    return in;
}
