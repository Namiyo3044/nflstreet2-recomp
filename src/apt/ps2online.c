#include "apt/aptutil.h"
#include "apt/ps2online.h"
#include "apt/aptdraw.h"
#include "apt/avm.h"
#include "apt/terf.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- where movies are: by pack and number */
static const char* const k_dats[] = {"LGENERAL", "SGENERAL", "FONLINE", "FONTS", "SONGENRL", "SONLOGIN", "SONMENU",
                                     "SONLOBBY", "SONSETTG", "SONSTAT", "SONPREGM", "SONEAMSG", NULL};
static const char* const k_required[] = {"LGENERAL", "SGENERAL", "FONLINE", "FONTS", "SONGENRL", "SONLOGIN", "SONMENU", NULL};

static const struct { const char* name; const char* dat; int idx; } k_shell[] = {
    {"Foregrnd", "SONGENRL", 1}, {"OLBKG", "SONGENRL", 2}, {"eaTrax", "LGENERAL", 5},
    {"FONMAIN", "FONLINE", 4}, {"FLOGIN", "FONLINE", 2}, {"FLOBBY", "FONLINE", 1}, {"FMYCARER", "FONLINE", 3},
    {"FPREGAME", "FONLINE", 5}, {"FPREMAIN", "FONLINE", 6}, {"FTOURNEY", "FONLINE", 7},
    {"SelectUser", "SGENERAL", 5}, {"SelectLoadedProfile", "SGENERAL", 4},
};
/* in each pack the movies are in the alphabetical order of their names */
static const char* const k_sonlogin[] = {"EmptyScreen", "PreMenuAnim", "TCLGN002", "TCLGN003", "TCLGN004", "TCLGN005", "TCLGN007",
                                         "TCLGN008", "TCLGN009", "TCLGN011", "TCLGN012", "TCLGN042", NULL};
static const char* const k_sonmenu[] = {"TCMNU001", "TCPRM001", "TCPRM003", "TCPRM004", NULL};
/* screens found in the other packs by their own scripts' names */
static const struct { const char* name; const char* dat; int idx; } k_screens[] = {
    {"TCGEN043", "SONGENRL", 4}, {"TCGEN044", "SONGENRL", 5}, {"TCPLN001", "SONGENRL", 6}, {"TCPLN002", "SONGENRL", 7},
    {"TCCHT001", "SONLOBBY", 3}, {"TCCHT006", "SONLOBBY", 4}, {"TCCHT010", "SONLOBBY", 5}, {"TCSET007", "SONSETTG", 0},
};
static const struct { const char* name; int idx; } k_libs[] = {{"eaStd", 4}, {"eaExt", 0}, {"eaSku", 3}, {"eaOnline", 2}, {"eaGame", 1}};

typedef struct { char dat[16]; int idx; AptFile* f; } Loaded;

static struct {
    bool open;
    char dir[512];
    VM* vm;
    TerfPack* packs[16];
    Loaded files[96]; int nfiles;
    const char* lib_names[5]; AptFile* libs[5];
    int ticks;
    char flow[48];                     /* the last flow the screens asked to change to (once) */
    void (*log)(const char* msg);
    /* the message being sent (Msg.Start .. Msg.End) */
    char msg_id[96];
    Val msg_args[8]; int msg_nargs;
} g;

static int dat_index(const char* dat) {
    for (int i = 0; k_dats[i]; i++) if (!apt_strcasecmp(k_dats[i], dat)) return i;
    return -1;
}
static TerfPack* pack(const char* dat) {
    int i = dat_index(dat);
    if (i < 0) return NULL;
    if (!g.packs[i]) {
        char path[640];
        snprintf(path, sizeof path, "%s/%s.DAT", g.dir, k_dats[i]);
        g.packs[i] = terf_load(path);
    }
    return g.packs[i];
}
static AptFile* movie(const char* dat, int idx) {
    for (int i = 0; i < g.nfiles; i++) if (!strcmp(g.files[i].dat, dat) && g.files[i].idx == idx) return g.files[i].f;
    TerfPack* p = pack(dat);
    if (!p || idx < 0 || idx >= p->count || g.nfiles >= 96) return NULL;
    char name[48];
    snprintf(name, sizeof name, "%s/%03d", dat, idx);
    AptFile* f = apt_open(&p->entries[idx], name);
    if (!f) return NULL;
    Loaded* l = &g.files[g.nfiles++];
    snprintf(l->dat, sizeof l->dat, "%s", dat); l->idx = idx; l->f = f;
    return f;
}

static AptFile* resolve_movie(VM* vm, const char* url) {
    (void)vm;
    const char* b = url;
    for (const char* q = url; *q; q++) if (*q == '/' || *q == '\\') b = q + 1;
    char base[96];
    snprintf(base, sizeof base, "%s", b);
    size_t n = strlen(base);
    if (n > 4 && !apt_strcasecmp(base + n - 4, ".swf")) base[n - 4] = 0;
    for (size_t i = 0; i < sizeof k_shell / sizeof k_shell[0]; i++)
        if (!strcmp(k_shell[i].name, base)) return movie(k_shell[i].dat, k_shell[i].idx);
    if (!strncmp(base, "GAL_", 4)) {                 /* GAL_<PACK>_<NNN>: a screen by its place */
        char dat[16]; const char* us = strrchr(base + 4, '_');
        if (us && us - (base + 4) < 15) {
            memcpy(dat, base + 4, (size_t)(us - (base + 4))); dat[us - (base + 4)] = 0;
            return movie(dat, atoi(us + 1));
        }
    }
    for (int i = 0; k_sonlogin[i]; i++) if (!strcmp(k_sonlogin[i], base)) return movie("SONLOGIN", i);
    for (int i = 0; k_sonmenu[i]; i++) if (!strcmp(k_sonmenu[i], base)) return movie("SONMENU", i);
    for (size_t i = 0; i < sizeof k_screens / sizeof k_screens[0]; i++)
        if (!strcmp(k_screens[i].name, base)) return movie(k_screens[i].dat, k_screens[i].idx);
    return NULL;
}

/* ---- the engine's answers (the real ones came from EA's servers and the memory card):
   enough for the screens to show their layout */
static bool ends_with_ci(const char* s, const char* suf) {
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && !apt_strcasecmp(s + a - b, suf);
}
static int arg_int(VM* vm) { return g.msg_nargs ? v_int(vm, g.msg_args[0]) : 0; }

/* one field of a reply; false: no sample for it */
static bool sample_field(VM* vm, const char* id, const char* field, Val* out) {
    char buf[64];
    struct { const char* id; const char* field; const char* s; double n; int kind; } t[] = {
        /* kind 0 string, 1 number, 2 bool */
        {"MsgLobbyLoginGetAccountDOB", "Month", NULL, 1, 1}, {"MsgLobbyLoginGetAccountDOB", "Day", NULL, 1, 1},
        {"MsgLobbyLoginGetAccountDOBRange", "nMinYear", NULL, 1905, 1},
        {"MsgChalGetIncomingInfo", "bPending", NULL, 1, 2}, {"MsgChalGetIncomingInfo", "nChalType", NULL, 0, 1},
        {"MsgChalGetIncomingInfo", "sUserName", "OPPONENT", 0, 0},
        {"GMRoomListGetCategoryDetail", "nSubRooms", NULL, 4, 1},
        {"GMRoomListGetRoomDetail", "nLocked", NULL, 0, 1}, {"GMRoomListGetRoomDetail", "nRoomPop", NULL, 12, 1},
        {"GMRoomListGetRoomDetail", "sCreator", "EA", 0, 0}, {"GMRoomListGetRoomDetail", "sDNF", "0%", 0, 0},
        {"GMRoomListGetRoomDetail", "sRank", "1", 0, 0},
        {"GMRoomUserListGetUser", "nColor", NULL, 0xFFFFFF, 1}, {"GMRoomUserListGetUser", "nIcon", NULL, 0, 1},
        {"GMRoomUserListGetUser", "nPingPct", NULL, 80, 1},
        {"GMLobbyRoomGetRoomInfo", "sRoomDesc", "Pick up a game with anyone in the room.", 0, 0},
        {"GMLobbyRoomGetRoomInfo", "sRoomHost", "EA", 0, 0}, {"GMLobbyRoomGetRoomInfo", "sRoomName", "NFL Teams", 0, 0},
        {"GMInfoModuleGetFindUserInfoResult", "sRecord", "10-2", 0, 0}, {"GMInfoModuleGetFindUserInfoResult", "sRank", "25", 0, 0},
        {"GMInfoModuleGetFindUserInfoResult", "sPlayerLevel", "5", 0, 0}, {"GMInfoModuleGetFindUserInfoResult", "sTeamLevel", "5", 0, 0},
        {"GMInfoModuleGetFindUserInfoResult", "sDNF", "0%", 0, 0},
        {"MsgGlobalQueryTeamData", "sShortName", "TEAM", 0, 0}, {"MsgGlobalQueryTeamData", "sTeamName", "Team", 0, 0},
        {"MsgGlobalQueryTeamData", "uLogoID", NULL, 0, 1},
        {"GMTeamSelectGetTeamStats", "offense", NULL, 80, 1}, {"GMTeamSelectGetTeamStats", "defense", NULL, 80, 1},
        {"GMVirtualListGetPosition", "nTotalSize", NULL, 8, 1}, {"GMVirtualListGetPosition", "nViewPosition", NULL, 0, 1},
        {"GMRoomChatGetLine", "sText", "", 0, 0},
    };
    if (!strcmp(id, "GMRoomListGetCategoryDetail") && !strcmp(field, "sTitle")) { snprintf(buf, sizeof buf, "Room %d", arg_int(vm) + 1); *out = v_str(vm, buf); return true; }
    if (!strcmp(id, "GMRoomListGetRoomDetail") && !strcmp(field, "sTitle")) { snprintf(buf, sizeof buf, "Room %d", arg_int(vm) + 1); *out = v_str(vm, buf); return true; }
    if (!strcmp(id, "GMRoomUserListGetUser") && !strcmp(field, "sName")) { snprintf(buf, sizeof buf, "PLAYER%d", arg_int(vm) + 1); *out = v_str(vm, buf); return true; }
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++)
        if (!strcmp(t[i].id, id) && !strcmp(t[i].field, field)) {
            *out = t[i].kind == 0 ? v_str(vm, t[i].s) : t[i].kind == 1 ? v_num(t[i].n) : v_bool(t[i].n != 0);
            return true;
        }
    return false;
}
/* a single answer (no named fields) */
static Val sample_single(VM* vm, const char* id) {
    static const struct { const char* id; const char* s; double n; int kind; } t[] = {
        {"MsgLobbyLoginGetAccountName", "STREETPLAYER", 0, 0}, {"MsgLobbyLoginGetAccountPassword", "********", 0, 0},
        {"MsgLobbyLoginGetAccountPasswordNew", "********", 0, 0}, {"MsgLobbyLoginGetAccountPasswordNewConfirm", "********", 0, 0},
        {"MsgLobbyLoginGetAccountEmail", "player@example.com", 0, 0}, {"MsgLobbyLoginGetAccountGender", NULL, 0, 1},
        {"MsgLobbyLoginGetDaysInMonthNumber", NULL, 31, 1}, {"GMLobbyGetCurrentPersona", "STREETPLAYER", 0, 0},
        {"MsgPeerConnectGetPeerPersonaName", "OPPONENT", 0, 0}, {"MsgVOIPGetPeerPersonaName", "OPPONENT", 0, 0},
        {"GMRoomListGetCategoryCount", NULL, 3, 1}, {"GMRoomListGetPopulation", NULL, 1024, 1},
        {"GMTeamSelectGetNumTeams", NULL, 4, 1}, {"GMTeamSelectGetTeamName", "Team", 0, 0}, {"MsgIsBroadband", NULL, 1, 2},
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++)
        if (!strcmp(t[i].id, id)) return t[i].kind == 0 ? v_str(vm, t[i].s) : t[i].kind == 1 ? v_num(t[i].n) : v_bool(t[i].n != 0);
    static const char* const suf[] = {"valuestr", "label", "name", "text", "persona", "email", "description", "defaulttext"};
    for (size_t i = 0; i < sizeof suf / sizeof suf[0]; i++) if (ends_with_ci(id, suf[i])) return v_str(vm, "");
    return v_bool(false);
}
static Val reply_default(VM* vm, const char* field) {
    const char* f = field[0] == '_' ? field + 1 : field;
    if (f[0] == 'b') return v_bool(false);
    if (f[0] == 's') return v_str(vm, "");
    if (strchr("niuefd", f[0]) && f[0]) return v_num(0);
    return v_undef();
}

/* the message object whose Send is running: its constructor's name (GMComponentGetLabel...) */
static bool msg_name(VM* vm, char* out, size_t cap) {
    for (int back = 0; back < 3; back++) {
        Val t = vm_script_this(vm, back);
        if (t.t != V_OBJ) continue;
        Obj* ctor = NULL;
        Val c = obj_get(vm, t.o, "__constructor__");
        if (c.t == V_OBJ && c.o->kind == OK_FUNC) ctor = c.o;
        else if (t.o->proto) { Val k = obj_get(vm, t.o->proto, "constructor"); if (k.t == V_OBJ && k.o->kind == OK_FUNC) ctor = k.o; }
        if (ctor && ctor->fn->name) { snprintf(out, cap, "%s", ctor->fn->name->s); return true; }
    }
    return false;
}

static Val msg_start(VM* vm, Val self, int argc, Val* argv, void* data) {
    (void)self; (void)data;
    if (!msg_name(vm, g.msg_id, sizeof g.msg_id)) snprintf(g.msg_id, sizeof g.msg_id, "%s", argc ? v_cstr(vm, argv[argc - 1]) : "");
    g.msg_nargs = 0;
    return v_undef();
}
static Val msg_send(VM* vm, Val self, int argc, Val* argv, void* data) {
    (void)vm; (void)self; (void)data;
    g.msg_nargs = argc > 8 ? 8 : argc;
    for (int i = 0; i < g.msg_nargs; i++) g.msg_args[i] = argv[i];
    return v_bool(true);
}
static Val msg_end(VM* vm, Val self, int argc, Val* argv, void* data) {
    (void)self; (void)data;
    int nf = argc > 0 ? argc - 1 : 0;
    if (!nf) {
        Val r = sample_single(vm, g.msg_id);
        char a[160]; size_t n = 0; a[0] = 0;
        for (int i = 0; i < g.msg_nargs && i < 4 && n < 120; i++)
            n += (size_t)snprintf(a + n, sizeof a - n, "%s%.30s", i ? ", " : "", g.msg_args[i].t == V_OBJ ? "[obj]" : v_cstr(vm, g.msg_args[i]));
        vm_logf(vm, "MSG %s(%s) -> %s", g.msg_id, a, v_cstr(vm, r));
        if (!strcmp(g.msg_id, "MsgFlowScriptChangeFlow") && g.msg_nargs) {
            snprintf(g.flow, sizeof g.flow, "%s", v_cstr(vm, g.msg_args[0]));
            vm_logf(vm, "the screens change to flow %s", g.flow);
        }
        return r;
    }
    Obj* o = obj_new(vm, vm->object_proto);
    for (int i = nf - 1; i >= 0; i--) {
        const char* f = v_cstr(vm, argv[i]);
        char field[64]; snprintf(field, sizeof field, "%s", f);
        Val v;
        if (!sample_field(vm, g.msg_id, field, &v)) v = reply_default(vm, field);
        obj_set(vm, o, field, v);
    }
    vm_logf(vm, "MSG %s (%d fields)", g.msg_id, nf);
    return v_obj(o);
}

static void install_msg(VM* vm) {
    Val m = obj_get(vm, vm->glob, "Msg");
    if (m.t != V_OBJ) { vm_logf(vm, "no Msg object"); return; }
    obj_set_native(vm, m.o, "Start", msg_start, NULL);
    obj_set_native(vm, m.o, "Send", msg_send, NULL);
    obj_set_native(vm, m.o, "End", msg_end, NULL);
}

/* as after signing in: the online foreground's connected state */
static void go_online(VM* vm) {
    Obj* fg = vm->levels[12];
    if (!fg) return;
    Val t = v_bool(true);
    vm_call_method(vm, fg, "SetOnlineStatus", 1, &t);
    vm_call_method(vm, fg, "SetConnectionStatus", 1, &t);
}

const char* ps2o_missing_file(const char* dir) {
    static char name[32];
    for (int i = 0; k_required[i]; i++) {
        char path[640];
        snprintf(path, sizeof path, "%s/%s.DAT", dir, k_required[i]);
        FILE* f = fopen(path, "rb");
        if (!f) { snprintf(name, sizeof name, "%s.DAT", k_required[i]); return name; }
        fclose(f);
    }
    return NULL;
}
/* What the port leaves out of EA's screens (there are no EA servers to make
   them work): of the online main menu only Quick Game stays (its list: one
   row per item, a row of nothing but its "invalid" marker leaves it out);
   the EA Messenger hint goes from the button bars. */
static void on_call(VM* vm, Obj* fn, Val self, int argc, Val* argv) {
    Str* name = fn->fn->name;
    if (!name) return;
    if (!strcmp(name->s, "InitMenu") && argc >= 1 && argv[0].t == V_OBJ && argv[0].o->kind == OK_ARRAY) {
        ArrExt* rows = argv[0].o->arr;
        if (rows->n < 1 || rows->items[0].t != V_OBJ || rows->items[0].o->kind != OK_ARRAY) return;
        ArrExt* first = rows->items[0].o->arr;
        if (first->n < 1) return;
        Val invalid = first->items[first->n - 1];
        if (invalid.t != V_STR || !strstr(invalid.s->s, "INVALID")) return;
        for (int r = 1; r < rows->n; r++) {
            if (rows->items[r].t != V_OBJ || rows->items[r].o->kind != OK_ARRAY) continue;
            ArrExt* row = rows->items[r].o->arr;
            for (int i = 0; i < row->n; i++) row->items[i] = invalid;
        }
        /* and the second row's bar (a shape of the menu's below its first row) */
        if (self.t == V_OBJ && self.o->kind == OK_CLIP)
            for (int i = 0; i < self.o->d->nkids; i++) {
                Obj* k = self.o->d->kids[i].o;
                if (k->kind == OK_SHAPE && k->d->m[5] > 20) k->d->visible = false;
            }
        vm_logf(vm, "online main menu: only %s kept", first->items[0].t == V_STR ? first->items[0].s->s : "the first item");
    } else if (!strcmp(name->s, "SetHelp") && argc >= 1 && argv[0].t == V_OBJ) {
        for (int k = 0; k < 8; k++) {
            char key[16]; snprintf(key, sizeof key, "_oBtn%d", k);
            Val b = obj_get(vm, argv[0].o, key);
            if (b.t != V_OBJ) continue;
            Val d = obj_get(vm, b.o, "sDesc");
            if (d.t == V_STR && strstr(d.s->s, "Messenger")) {
                obj_set(vm, b.o, "sDesc", v_str(vm, ""));
                obj_set(vm, b.o, "sBtn1", v_str(vm, ""));
                obj_set(vm, b.o, "sBtn2", v_str(vm, ""));
            }
        }
    }
}

/* the foreground's EA Messenger button (top left) */
static void hide_messenger(void) {
    Obj* c = g.vm->levels[12] ? vm_resolve_path(g.vm, "Circle", g.vm->levels[12]) : NULL;
    if (c && c->kind == OK_CLIP) c->d->visible = false;
}

bool ps2o_files_present(const char* dir) { return ps2o_missing_file(dir) == NULL; }

bool ps2o_open(const char* dir, void (*log)(const char* msg)) {
    ps2o_close();
    if (strcmp(g.dir, dir)) {                       /* another folder: what was loaded is no use */
        memset(&g, 0, sizeof g);
        snprintf(g.dir, sizeof g.dir, "%s", dir);
    }
    g.ticks = 0;
    g.flow[0] = 0;
    g.log = log;
    if (!ps2o_files_present(dir)) return false;
    AptFile* shell = movie("SGENERAL", 3);
    for (int i = 0; i < 5; i++) { g.lib_names[i] = k_libs[i].name; g.libs[i] = movie("LGENERAL", k_libs[i].idx); }
    TerfPack* fonts = pack("FONTS");
    if (!shell || !fonts) { if (log) log("ps2 online: the shell or fonts didn't load"); return false; }
    for (int i = 0; i < 5; i++) if (!g.libs[i]) { if (log) log("ps2 online: a library didn't load"); return false; }
    aptdraw_set_fonts(fonts);
    VMHost host;
    memset(&host, 0, sizeof host);
    host.resolve_movie = resolve_movie;
    host.on_call = on_call;
    VM* vm = g.vm = vm_new(&host, true);
    vm->log = log;
    vm->nlibs = 5; vm->lib_names = g.lib_names; vm->libs = g.libs;
    obj_set(vm, vm->glob, "extern", v_obj(catchall_new(vm, "extern")));
    /* the shell: its init actions, then its first frame (it defines the component classes) */
    Obj* root = vm_new_movie_clip(vm, shell);
    root->d->is_root = true;
    root->d->name = NULL;
    vm->levels[0] = root;
    obj_set(vm, root, "_name", v_str(vm, "_level0"));
    vm_run_init_actions(vm, shell, root);
    vm_goto_frame(vm, root, 0, true);
    install_msg(vm);
    /* the libraries: their init actions (class registrations) once */
    for (int i = 0; i < 5; i++) {
        Obj* holder = vm_new_movie_clip(vm, g.libs[i]);
        char nm[32]; snprintf(nm, sizeof nm, "__lib_%s", g.lib_names[i]);
        holder->d->visible = false;
        vm_add_child(vm, root, holder, -100 - root->d->nkids, nm, true);
        vm_run_init_actions(vm, g.libs[i], root);
        vm_goto_frame(vm, holder, 0, true);
        holder->d->playing = false;
    }
    g.open = true;
    for (int i = 0; i < 40; i++) {
        vm_tick(vm, 1000.0 / 30);
        if (i == 2) go_online(vm);
    }
    hide_messenger();
    vm_gc(vm);                         /* (the online flow has put up its main menu by now) */
    return true;
}

bool ps2o_is_open(void) { return g.open; }

void ps2o_tick(void) {
    if (!g.open) return;
    g.vm->trace = getenv("APT_TRACE_TICK") != NULL;
    if (g.vm->trace) g.vm->logs = 0;
    vm_tick(g.vm, 1000.0 / 30);
    g.vm->trace = false;
    hide_messenger();
    if (++g.ticks % 30 == 0) vm_gc(g.vm);
}

void ps2o_draw(uint32_t* px, int w, int h) {
    if (!g.open) { memset(px, 0, (size_t)w * h * 4); return; }
    /* the 854 x 480 stage fitted to the window's height, centred */
    float scale = h / 480.0f;
    if (854 * scale > w) scale = w / 854.0f;
    aptdraw_levels(g.vm, px, w, h, scale, (w - 854 * scale) * 0.5f, (h - 480 * scale) * 0.5f, 0xFF000000u);
}

void ps2o_button(int button, bool down) {
    static const int codes[] = {38, 40, 37, 39, 302, 303, 304, 305, 301, 300, 306, 307, 308, 309};
    if (!g.open || button < 0 || button >= (int)(sizeof codes / sizeof codes[0])) return;
    g.vm->trace = getenv("APT_TRACE") != NULL;
    if (g.vm->trace) g.vm->logs = 0;
    vm_key(g.vm, codes[button], 0, down);
    g.vm->trace = false;
}

bool ps2o_take_flow(char* out, size_t cap) {
    if (!g.flow[0]) return false;
    snprintf(out, cap, "%s", g.flow);
    g.flow[0] = 0;
    return true;
}

static void dump(VM* vm, Obj* o, int depth, int maxd) {
    DispExt* d = o->d;
    printf("%*s%s %s ch%d vis%d a%.0f x%.1f y%.1f s%.0f/%.0f f%d kids%d%s%s\n", depth * 2, "", d->name ? d->name->s : "?",
           o->kind == OK_CLIP ? "clip" : o->kind == OK_TEXT ? "text" : "shape", d->chid, d->visible, d->alpha, d->m[4], d->m[5],
           d->xs, d->ys, d->frame, d->nkids, o->kind == OK_TEXT && d->text ? " text=" : "", o->kind == OK_TEXT && d->text ? d->text->s : "");
    (void)vm;
    if (depth < maxd) for (int i = 0; i < d->nkids; i++) dump(vm, d->kids[i].o, depth + 1, maxd);
}
void ps2o_debug_tree(const char* path, int maxd) {
    if (!g.open) return;
    Obj* o = vm_resolve_path(g.vm, path, g.vm->levels[0]);
    if (o && (o->kind == OK_CLIP || o->kind == OK_TEXT || o->kind == OK_SHAPE)) dump(g.vm, o, 0, maxd);
    else printf("no %s\n", path);
}

void ps2o_debug_get(const char* path, const char* prop) {
    if (!g.open) return;
    Obj* o = vm_resolve_path(g.vm, path, g.vm->levels[0]);
    if (!o) { printf("no %s\n", path); return; }
    Val v = obj_get(g.vm, o, prop);
    printf("%s.%s = %s (type %d)\n", path, prop, v_cstr(g.vm, v), v.t);
}

void ps2o_close(void) {
    if (g.vm) vm_free(g.vm);
    g.vm = NULL;
    g.open = false;
    /* the packs and movies stay loaded for the next time (they're the bulk of the work) */
}
