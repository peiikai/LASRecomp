// Saves: export and import (runtime/saves.h). Runs in the data folder (the working directory).
#include "saves.h"

#include <SDL3/SDL.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <sys/utime.h>
#else
#include <unistd.h>
#include <utime.h>
#endif

#include "i18n_mark.h"
#include "platform.h"
#include "rt.h"
#include "zipfile.h"

#define MANIFEST "lasrecomp-saves.txt"
#define FORMAT 1                       // lasrecomp-saves.txt's first line: "LAS Recomp saves <FORMAT>"
#define BACKUPS "backups"
#define KEEP_BACKUPS 20
enum { SAVE_SIZE = 0x5EE0, SAVE_VERSION = 0xE };
enum { MAX_ENTRY = 64 << 20 };         // no file of a bundle is bigger (a seed.json is about 1 MB)

// ---- small file helpers
static bool is_dir(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}
static long long file_mtime(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode) ? (long long)st.st_mtime : 0;
}
static void mkdirs(const char *path) {
    char p[1024];
    snprintf(p, sizeof p, "%s", path);
    for (char *c = p + 1; *c; c++)
        if (*c == '/') { *c = 0; mkdir(p, 0755); *c = '/'; }
    mkdir(p, 0755);
}
// Written in full and flushed to the disk (a cut-off write never replaces anything: the caller renames it into place)
static bool write_file(const char *path, const void *data, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, n, f) == n && fflush(f) == 0;
#ifdef _WIN32
    ok = ok && _commit(_fileno(f)) == 0;
#else
    ok = ok && fsync(fileno(f)) == 0;
#endif
    ok &= fclose(f) == 0;
    if (!ok) remove(path);
    return ok;
}
static bool replace_file(const char *from, const char *to) {
#ifdef _WIN32
    return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return rename(from, to) == 0;
#endif
}
// A rename lasts only once its folder is on the disk too (as runtime/services/fs.c's sync_dir_of; Windows' renames
// are written through: MOVEFILE_WRITE_THROUGH)
static void sync_dir(const char *dir) {
#ifndef _WIN32
    int fd = open(dir, O_RDONLY);
    if (fd >= 0) { fsync(fd); close(fd); }
#else
    (void)dir;
#endif
}
static void sync_dir_of(const char *path) {
    char d[1100];
    snprintf(d, sizeof d, "%s", path);
    char *slash = strrchr(d, '/');
    sync_dir(slash ? (*slash = 0, d) : ".");
}
static void set_mtime(const char *path, long long t) {
    if (t <= 0) return;
#ifdef _WIN32
    struct _utimbuf u = {(time_t)t, (time_t)t};
    _utime(path, &u);
#else
    struct utimbuf u = {(time_t)t, (time_t)t};
    utime(path, &u);
#endif
}
static void say(char *err, size_t n, const char *msg) {
    if (err && n) snprintf(err, n, "%s", msg);
}

// ---- the save itself (docs/save-format.md)
static uint32_t la_crc(const uint8_t *p, size_t n) {  // (reflected CRC32 table, initial value 0xEDB88320, no final XOR)
    static uint32_t t[256];
    if (!t[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? (c >> 1) ^ 0xEDB88320u : c >> 1;
            t[i] = c;
        }
    uint32_t c = 0xEDB88320u;
    for (size_t i = 0; i < n; i++) c = t[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c;
}
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

const char *saves_check(const unsigned char *b, size_t n) {  // (the reasons are for the log)
    if (n < SAVE_SIZE) return "too short for a save";
    if (rd32(b) != SAVE_VERSION) return "not a save of this game";
    uint8_t h[0xE0];
    memcpy(h, b, sizeof h);
    memset(h + 4, 0, 4);  // (each CRC is computed with its own field zeroed)
    if (la_crc(h, sizeof h) != rd32(b + 4)) return "damaged (header checksum)";
    uint32_t want = rd32(b + 0x5EC4);
    uint8_t *body = malloc(SAVE_SIZE - 0xE0);
    if (!body) return "damaged (body checksum)";
    memcpy(body, b + 0xE0, SAVE_SIZE - 0xE0);
    memset(body + (0x5EC4 - 0xE0), 0, 4);
    bool ok = la_crc(body, SAVE_SIZE - 0xE0) == want;
    free(body);
    return ok ? NULL : "damaged (body checksum)";
}

static const char *const SLOT_NAMES[SAVES_SLOTS] = {LN("File 1"), LN("File 2"), LN("File 3"), LN("Autosave")};
const char *saves_slot_name(int slot) { return slot >= 0 && slot < SAVES_SLOTS ? SLOT_NAMES[slot] : ""; }

// ---- names in a bundle (and in the data folder: the same layout)
// A file or folder name that means the same on every system: no separators or characters Windows refuses (a ':'
// would write an NTFS stream of another file), no dot or space last (Windows drops them: "abc." is "abc"), no
// device name (CON, NUL, COM1, ... with or without an extension)
static bool name_ok(const char *s, size_t n) {
    if (!n || s[n - 1] == '.' || s[n - 1] == ' ') return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
            c == '>' || c == '|' || c == 0x7F)
            return false;
    }
    size_t b = 0;
    while (b < n && s[b] != '.') b++;
    static const char *const dev[] = {"CON", "PRN", "AUX", "NUL"};
    for (int i = 0; i < 4; i++)
        if (b == 3 && !SDL_strncasecmp(s, dev[i], 3)) return false;
    if (b == 4 && (!SDL_strncasecmp(s, "COM", 3) || !SDL_strncasecmp(s, "LPT", 3)) && s[3] >= '1' && s[3] <= '9')
        return false;
    return true;
}
// A seed's folder name as the app makes them; nothing that could leave rando/ (no separators, no "..", no dot
// first), and not an import's working folder (<seed>.import)
static bool seed_name_ok(const char *s, size_t n) {
    if (!n || n >= sizeof(((SavesGame *)0)->seed) || s[0] == '.' || !name_ok(s, n)) return false;
    return !(n >= 7 && !memcmp(s + n - 7, ".import", 7));
}
// The save folder of a game: "save/save_0" or "rando/<seed>/save/save_0"
static void save_dir(const char *seed, char *out, size_t n) {
    if (*seed) snprintf(out, n, "rando/%s/save/save_0", seed);
    else snprintf(out, n, "save/save_0");
}
// A file of slot N in a save folder: savedataN.bin, its sidecars (savedataN.bin.flagsx, ...), screenshotN.bin.
// -1: not one (a working copy, .pending, never is).
static int slot_of_file(const char *f) {
    size_t k = strlen(f);
    if (k > 8 && !strcmp(f + k - 8, ".pending")) return -1;
    if (k > 7 && !strcmp(f + k - 7, ".import")) return -1;
    if (!strncmp(f, "savedata", 8) && f[8] >= '0' && f[8] < '0' + SAVES_SLOTS && !strncmp(f + 9, ".bin", 4) &&
        (!f[13] || (f[13] == '.' && f[14] && name_ok(f, k))))
        return f[8] - '0';
    if (!strncmp(f, "screenshot", 10) && f[10] >= '0' && f[10] < '0' + SAVES_SLOTS && !strcmp(f + 11, ".bin"))
        return f[10] - '0';
    return -1;
}
static bool is_data_file(const char *f) { return !strncmp(f, "savedata", 8) && !strcmp(f + 9, ".bin"); }
// A file of a seed folder that goes along with its saves: a plain file at its top (not hidden, not a working copy),
// or exefs/*.pchtxt (the seed's patches). Never romfs/ (game files) or anything else in a folder.
static bool seed_file_ok(const char *rel) {
    if (!rel[0] || rel[0] == '.') return false;
    size_t k = strlen(rel);
    if ((k > 4 && !strcmp(rel + k - 4, ".tmp")) || (k > 8 && !strcmp(rel + k - 8, ".pending"))) return false;
    if (!strncmp(rel, "exefs/", 6)) {
        const char *f = rel + 6;
        size_t m = strlen(f);
        return m > 7 && f[0] != '.' && name_ok(f, m) && !strcmp(f + m - 7, ".pchtxt");
    }
    return name_ok(rel, k);
}

typedef enum { E_OTHER, E_MANIFEST, E_SLOT, E_SEED } EntryKind;
typedef struct { EntryKind kind; char seed[128]; int slot; const char *file; } Entry;  // file: points into the name
static Entry classify(const char *name) {
    Entry e = {E_OTHER, "", -1, NULL};
    if (!strcmp(name, MANIFEST)) { e.kind = E_MANIFEST; return e; }
    if (strstr(name, "..") || strchr(name, '\\') || name[0] == '/') return e;
    const char *rest = NULL;
    if (!strncmp(name, "save/save_0/", 12)) rest = name + 12;
    else if (!strncmp(name, "rando/", 6)) {
        const char *s = name + 6, *sl = strchr(s, '/');
        if (!sl || !seed_name_ok(s, (size_t)(sl - s))) return e;
        snprintf(e.seed, sizeof e.seed, "%.*s", (int)(sl - s), s);
        if (!strncmp(sl + 1, "save/save_0/", 12)) rest = sl + 13;
        else if (seed_file_ok(sl + 1)) { e.kind = E_SEED, e.file = sl + 1; return e; }
        else return e;
    } else return e;
    if (strchr(rest, '/')) return e;
    int s = slot_of_file(rest);
    if (s < 0) return e;
    e.kind = E_SLOT, e.slot = s, e.file = rest;
    return e;
}

// ---- the games in the data folder
typedef struct { char seed[128]; long long mtime[SAVES_SLOTS]; bool seed_files; } LocalGame;
typedef struct { int n; LocalGame *g; } Local;  // (as many as there are; local_free)
static LocalGame *local_add(Local *l) {
    LocalGame *g = realloc(l->g, (size_t)(l->n + 1) * sizeof *g);
    if (!g) return NULL;
    l->g = g;
    memset(&g[l->n], 0, sizeof *g);
    return &g[l->n++];
}
static void local_free(Local *l) { free(l->g), l->g = NULL, l->n = 0; }

// The slots of one game (0 for each missing)
static void local_slots(const char *seed, long long mt[SAVES_SLOTS]) {
    char d[400], p[600];
    save_dir(seed, d, sizeof d);
    for (int s = 0; s < SAVES_SLOTS; s++) {
        snprintf(p, sizeof p, "%s/savedata%d.bin", d, s);
        mt[s] = file_mtime(p);
    }
}
static bool has_seed_record(const char *seed) {
    char p[400];
    snprintf(p, sizeof p, "rando/%s/seed.json", seed);
    return file_mtime(p) != 0;
}
// Every game with at least one save: vanilla first, then the seeds. false: out of memory (l is freed).
static bool local_scan(Local *l) {
    memset(l, 0, sizeof *l);
    long long mt[SAVES_SLOTS];
    local_slots("", mt);
    LocalGame *g;
    if (mt[0] || mt[1] || mt[2] || mt[3]) {
        if (!(g = local_add(l))) return false;
        memcpy(g->mtime, mt, sizeof mt);
    }
    DIR *d = opendir("rando");
    if (!d) return true;
    struct dirent *e;
    bool ok = true;
    while (ok && (e = readdir(d))) {
        if (!seed_name_ok(e->d_name, strlen(e->d_name))) continue;
        char p[400];
        snprintf(p, sizeof p, "rando/%s", e->d_name);
        if (!is_dir(p)) continue;
        local_slots(e->d_name, mt);
        if (!(mt[0] || mt[1] || mt[2] || mt[3])) continue;
        if (!(g = local_add(l))) { ok = false; break; }
        snprintf(g->seed, sizeof g->seed, "%s", e->d_name);
        memcpy(g->mtime, mt, sizeof mt);
        g->seed_files = has_seed_record(e->d_name);
    }
    closedir(d);
    if (!ok) local_free(l);
    return ok;
}

bool saves_any(void) {
    Local l;
    bool any = local_scan(&l) && l.n > 0;
    local_free(&l);
    return any;
}

// ---- reading a bundle
// NULL: out of memory
static SavesGame *info_game(SavesInfo *info, const char *seed) {
    for (int i = 0; i < info->n; i++)
        if (!strcmp(info->g[i].seed, seed)) return &info->g[i];
    SavesGame *all = realloc(info->g, (size_t)(info->n + 1) * sizeof *all);
    if (!all) return NULL;
    info->g = all;
    SavesGame *g = &all[info->n++];
    memset(g, 0, sizeof *g);
    snprintf(g->seed, sizeof g->seed, "%s", seed);
    return g;
}
// The manifest's lines: "LAS Recomp saves 1", "app <v>", "game <v>", "slot <seed or -> <N> <mtime>"
static bool read_manifest(ZipReader *z, SavesInfo *info, char *err, size_t n) {
    int i = zip_find(z, MANIFEST);
    size_t sz = 0;
    char *t = i >= 0 ? zip_read(z, i, &sz) : NULL;
    if (!t) { say(err, n, LN("That file isn't a LAS Recomp saves file.")); return false; }
    int fmt = 0;
    if (sscanf(t, "LAS Recomp saves %d", &fmt) != 1) {
        free(t);
        say(err, n, LN("That file isn't a LAS Recomp saves file."));
        return false;
    }
    if (fmt > FORMAT) {
        free(t);
        say(err, n, LN("That saves file is from a newer LAS Recomp. Update LAS Recomp first."));
        return false;
    }
    for (char *line = strtok(t, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        if (!strncmp(line, "app ", 4)) snprintf(info->app, sizeof info->app, "%s", line + 4);
        else if (!strncmp(line, "game ", 5)) snprintf(info->game, sizeof info->game, "%s", line + 5);
        else if (!strncmp(line, "slot ", 5)) {
            // the seed name may hold spaces: the slot and time are the last two fields
            char *b = strrchr(line, ' ');
            if (!b) continue;
            long long mt = atoll(b + 1);
            *b = 0;
            char *a = strrchr(line + 5, ' ');
            if (!a) continue;
            int s = atoi(a + 1);
            *a = 0;
            const char *seed = !strcmp(line + 5, "-") ? "" : line + 5;
            if (s < 0 || s >= SAVES_SLOTS || (*seed && !seed_name_ok(seed, strlen(seed)))) continue;
            SavesGame *g = info_game(info, seed);
            if (!g) { free(t); say(err, n, LN("That file isn't a LAS Recomp saves file.")); return false; }
            g->mtime[s] = mt;
        }
    }
    free(t);
    return true;
}

static void *read_entry(ZipReader *z, int i, size_t *sz) {
    ZipEntry e;
    if (!zip_entry_at(z, i, &e) || e.is_dir || e.is_symlink || e.size > MAX_ENTRY) return NULL;
    return zip_read(z, i, sz);
}

// A seed's id (seed_id.txt) here and in the bundle; "" where it has none
static void seed_id_here(const char *seed, char *out, size_t n) {
    char p[300];
    snprintf(p, sizeof p, "rando/%s/seed_id.txt", seed);
    FILE *f = fopen(p, "rb");
    size_t k = f ? fread(out, 1, n - 1, f) : 0;
    if (f) fclose(f);
    out[k] = 0;
    out[strcspn(out, "\r\n")] = 0;
}
static void seed_id_there(ZipReader *z, const char *seed, char *out, size_t n) {
    char p[300];
    snprintf(p, sizeof p, "rando/%s/seed_id.txt", seed);
    out[0] = 0;
    int i = zip_find(z, p);
    size_t sz = 0;
    char *t = i >= 0 ? read_entry(z, i, &sz) : NULL;
    if (t) snprintf(out, n, "%.*s", (int)strcspn(t, "\r\n"), t);
    free(t);
}

// info from an open bundle; every save read and checked
static bool inspect(ZipReader *z, SavesInfo *info, char *err, size_t n) {
    saves_info_free(info);
    memset(info, 0, sizeof *info);
    if (!read_manifest(z, info, err, n)) return false;
    for (int i = 0, k = zip_count(z); i < k; i++) {
        ZipEntry ze;
        if (!zip_entry_at(z, i, &ze) || ze.is_dir) continue;
        Entry e = classify(ze.name);
        if (e.kind == E_SEED) {
            SavesGame *g = info_game(info, e.seed);
            if (!g) goto no_memory;
            if (!strcmp(e.file, "seed.json")) g->seed_files = true;
            continue;
        }
        if (e.kind != E_SLOT || !is_data_file(e.file)) continue;
        SavesGame *g = info_game(info, e.seed);
        if (!g) goto no_memory;
        size_t sz = 0;
        unsigned char *b = read_entry(z, i, &sz);
        const char *why = b ? saves_check(b, sz) : "damaged (can't be read)";
        free(b);
        if (why) {
            LOG("[saves] %s: %s\n", ze.name, why);
            if (!g->problem) g->problem = LN("A save in it is damaged.");
            continue;
        }
        g->slots |= 1u << e.slot;
    }
    // games named only in the manifest, or with no good save, are dropped; the rest checked against this data folder
    int w = 0;
    for (int i = 0; i < info->n; i++) {
        SavesGame g = info->g[i];
        if (!g.slots && !g.problem) continue;
        for (int s = 0; s < SAVES_SLOTS; s++)
            if (!(g.slots >> s & 1)) g.mtime[s] = 0;
        if (*g.seed && !g.problem) {
            char p[400];
            snprintf(p, sizeof p, "rando/%s", g.seed);
            if (is_dir(p)) {
                // a seed here by that name must be the same seed: both ids the same, or neither has one (seeds
                // older than seed_id.txt, which can't be told apart)
                char here[128], there[128];
                seed_id_here(g.seed, here, sizeof here);
                seed_id_there(z, g.seed, there, sizeof there);
                if (strcmp(here, there)) g.problem = LN("Another seed with the same name is here.");
            } else if (!g.seed_files) g.problem = LN("Its seed isn't here and isn't in the file.");
        }
        info->g[w++] = g;
    }
    info->n = w;
    if (!info->n) { say(err, n, LN("There are no saves in that file.")); return false; }
    return true;
no_memory:
    saves_info_free(info);
    say(err, n, LN("That file isn't a LAS Recomp saves file."));
    return false;
}

void saves_info_free(SavesInfo *info) {
    free(info->g);
    info->g = NULL;
    info->n = 0;
}

bool saves_inspect(const char *zip, SavesInfo *info, char *err, size_t n) {
    char why[200];
    ZipReader *z = zip_open(zip, why, sizeof why);
    if (!z) {
        LOG("[saves] %s: %s\n", zip, why);
        say(err, n, LN("That file isn't a LAS Recomp saves file."));
        return false;
    }
    bool ok = inspect(z, info, err, n);
    zip_close(z);
    return ok;
}

// ---- writing a bundle: each slot and seed from the data folder

static bool add_local_slot(ZipWriter *w, const char *seed, int slot) {
    char d[400];
    save_dir(seed, d, sizeof d);
    DIR *dir = opendir(d);
    if (!dir) return false;
    struct dirent *e;
    bool ok = true;
    while ((e = readdir(dir)) && ok) {
        if (slot_of_file(e->d_name) != slot) continue;
        char p[700], name[700];
        snprintf(p, sizeof p, "%s/%s", d, e->d_name);
        if (file_mtime(p) == 0) continue;
        snprintf(name, sizeof name, "%s/%s", d, e->d_name);
        ok = zip_add_file(w, name, p, true);
    }
    closedir(dir);
    return ok;
}
static bool add_local_seed_files(ZipWriter *w, const char *seed) {
    char d[400];
    snprintf(d, sizeof d, "rando/%s", seed);
    bool ok = true;
    for (int pass = 0; pass < 2 && ok; pass++) {  // the folder's own files, then exefs/
        char dd[450];
        snprintf(dd, sizeof dd, pass ? "%s/exefs" : "%s", d);
        DIR *dir = opendir(dd);
        if (!dir) continue;
        struct dirent *e;
        while ((e = readdir(dir)) && ok) {
            char rel[300], p[800], name[800];
            snprintf(rel, sizeof rel, pass ? "exefs/%s" : "%s", e->d_name);
            snprintf(p, sizeof p, "%s/%s", d, rel);
            if (!seed_file_ok(rel) || file_mtime(p) == 0) continue;
            snprintf(name, sizeof name, "rando/%s/%s", seed, rel);
            ok = zip_add_file(w, name, p, true);
        }
        closedir(dir);
    }
    return ok;
}

static const char *game_version_shown(void);

static bool build(const char *out, const char *app, char *err, size_t n) {
    Local lg, *l = &lg;
    if (!local_scan(l)) { say(err, n, LN("Couldn't write the saves file.")); return false; }
    char why[200];
    ZipWriter *w = zip_create(out, why, sizeof why);
    if (!w) {
        LOG("[saves] %s: %s\n", out, why);
        local_free(l);
        say(err, n, LN("Couldn't write the saves file."));
        return false;
    }
    size_t cap = 4096 + (size_t)l->n * SAVES_SLOTS * 200, used = 0;
    char *man = malloc(cap);
    bool ok = man != NULL;
    if (ok) used = (size_t)snprintf(man, cap, "LAS Recomp saves %d\napp %s\ngame %s\n", FORMAT, app ? app : "",
                                    game_version_shown());
    for (int i = 0; i < l->n && ok; i++) {
        const char *seed = l->g[i].seed;
        bool any = false;
        for (int s = 0; s < SAVES_SLOTS && ok; s++) {
            long long lt = l->g[i].mtime[s];
            if (!lt) continue;
            // a save that doesn't check out stays out
            char p[500];
            char d[400];
            save_dir(seed, d, sizeof d);
            snprintf(p, sizeof p, "%s/savedata%d.bin", d, s);
            FILE *f = fopen(p, "rb");
            unsigned char *buf = malloc(SAVE_SIZE + 1);
            size_t got = f && buf ? fread(buf, 1, SAVE_SIZE + 1, f) : 0;
            if (f) fclose(f);
            const char *bad = buf ? saves_check(buf, got) : "";
            free(buf);
            if (bad) { LOG("[saves] %s left out: %s\n", p, bad); continue; }
            ok = add_local_slot(w, seed, s);
            any = true;
            if (ok && used + 300 < cap)
                used += (size_t)snprintf(man + used, cap - used, "slot %s %d %lld\n", *seed ? seed : "-", s, lt);
        }
        if (any && *seed && ok && l->g[i].seed_files) ok = add_local_seed_files(w, seed);
    }
    if (ok) ok = zip_add_mem(w, MANIFEST, man, used);
    free(man);
    local_free(l);
    if (!zip_finish(w) || !ok) {
        remove(out);
        say(err, n, LN("Couldn't write the saves file."));
        return false;
    }
    return true;
}

static const char *g_app_version = "";
void saves_app_version(const char *v) { g_app_version = v ? v : ""; }
bool saves_export(const char *zip, const char *app, char *err, size_t n) {
    if (app) g_app_version = app;
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.part", zip);
    if (!build(tmp, g_app_version, err, n)) return false;
    if (!replace_file(tmp, zip)) {
        remove(tmp);
        say(err, n, LN("Couldn't write the saves file."));
        return false;
    }
    sync_dir_of(zip);
    LOG("[saves] exported to %s\n", zip);
    return true;
}

// ---- import
// Backups by age, oldest first: saves-<date>-<time>.zip, then -2.zip, -3.zip, ... made in the same second
static int backup_number(const char *name) {
    const char *dash = strlen(name) > 21 ? name + 21 : "";  // (after "saves-YYYYMMDD-HHMMSS")
    return *dash == '-' ? atoi(dash + 1) : 1;
}
static int backup_order(const void *a, const void *b) {
    int c = strncmp(a, b, 21);
    return c ? c : backup_number(a) - backup_number(b);
}
static void prune_backups(void) {
    DIR *d = opendir(BACKUPS);
    if (!d) return;
    char names[256][64];
    int k = 0;
    struct dirent *e;
    while ((e = readdir(d)) && k < 256)
        if (!strncmp(e->d_name, "saves-", 6) && strlen(e->d_name) < 64) snprintf(names[k++], 64, "%s", e->d_name);
    closedir(d);
    if (k <= KEEP_BACKUPS) return;
    qsort(names, (size_t)k, 64, backup_order);
    for (int i = 0; i < k - KEEP_BACKUPS; i++) {
        char p[100];
        snprintf(p, sizeof p, BACKUPS "/%s", names[i]);
        remove(p);
    }
}
// The files the import replaces (each slot taken that is here), zipped into backups/saves-<date>.zip
static bool backup(const SavesInfo *info, const unsigned *take, char *made, size_t made_n) {
    made[0] = 0;
    bool any = false;
    for (int i = 0; i < info->n; i++) {
        long long mt[SAVES_SLOTS];
        local_slots(info->g[i].seed, mt);
        for (int s = 0; s < SAVES_SLOTS; s++) any |= (take[i] >> s & 1) && mt[s];
    }
    if (!any) return true;
    mkdir(BACKUPS, 0755);
    time_t now = time(NULL);
    struct tm tm;
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char p[100];
    for (int k = 1; k < 100; k++) {  // (two in the same second: the second one numbered)
        int m = snprintf(p, sizeof p, BACKUPS "/saves-%04d%02d%02d-%02d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1,
                         tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
        snprintf(p + m, sizeof p - (size_t)m, k > 1 ? "-%d.zip" : ".zip", k);
        if (!file_mtime(p)) break;
    }
    char why[200];
    ZipWriter *w = zip_create(p, why, sizeof why);
    if (!w) { LOG("[saves] backup %s: %s\n", p, why); return false; }
    size_t cap = 4096 + (size_t)info->n * SAVES_SLOTS * 200;
    char *man = malloc(cap);
    if (!man) { zip_finish(w); remove(p); return false; }
    size_t used = (size_t)snprintf(man, cap, "LAS Recomp saves %d\napp %s\ngame %s\n", FORMAT, g_app_version,
                                   game_version_shown());
    bool ok = true;
    for (int i = 0; i < info->n && ok; i++) {
        long long mt[SAVES_SLOTS];
        local_slots(info->g[i].seed, mt);
        for (int s = 0; s < SAVES_SLOTS && ok; s++) {
            if (!(take[i] >> s & 1) || !mt[s]) continue;
            ok = add_local_slot(w, info->g[i].seed, s);
            if (used + 200 < cap)
                used += (size_t)snprintf(man + used, cap - used, "slot %s %d %lld\n",
                                         *info->g[i].seed ? info->g[i].seed : "-", s, mt[s]);
        }
    }
    ok = ok && zip_add_mem(w, MANIFEST, man, used);
    free(man);
    if (!zip_finish(w) || !ok) { remove(p); LOG("[saves] backup %s failed\n", p); return false; }
    sync_dir(BACKUPS);
    snprintf(made, made_n, "%s", p);
    prune_backups();
    return true;
}

// A seed the data folder doesn't have, from the bundle: into rando/<seed>.import, then renamed into place. A failure
// leaves nothing behind (the rando tab would list a half-written folder, and the next try would add to it).
static bool import_seed(ZipReader *z, const char *seed) {
    char tmp[300], dst[300], ex[320];
    snprintf(tmp, sizeof tmp, "rando/%s.import", seed);
    snprintf(dst, sizeof dst, "rando/%s", seed);
    snprintf(ex, sizeof ex, "%s/exefs", tmp);
    if (is_dir(tmp)) plat_remove_tree(tmp);  // (an import cut off earlier)
    mkdirs(ex);
    bool ok = true;
    for (int i = 0, k = zip_count(z); i < k && ok; i++) {
        ZipEntry ze;
        if (!zip_entry_at(z, i, &ze) || ze.is_dir) continue;
        Entry e = classify(ze.name);
        if (e.kind != E_SEED || strcmp(e.seed, seed)) continue;
        char p[700];
        if ((size_t)snprintf(p, sizeof p, "%s/%s", tmp, e.file) >= sizeof p) { ok = false; break; }
        size_t sz = 0;
        void *b = read_entry(z, i, &sz);
        ok = b && write_file(p, b, sz);
        free(b);
    }
    if (ok) {
        sync_dir(ex);
        sync_dir(tmp);
        ok = rename(tmp, dst) == 0;
    }
    if (ok) sync_dir("rando");
    else {
        plat_remove_tree(tmp);
        LOG("[saves] couldn't add seed %s\n", seed);
    }
    return ok;
}

// One slot, in the order runtime/services/fs.c commits a save (its mount finishes a cut-off one the same way):
// every file of it in the bundle written as a working copy (.pending), the old files the bundle doesn't have removed
// (a .flagsx of the old save must never pair with the new one), then the save renamed into place and its sidecars
// after it. Once the save is in place a sidecar's working copy that couldn't be renamed stays for the next mount.
static bool import_slot(ZipReader *z, const char *seed, int slot, long long mtime) {
    enum { MAX_FILES = 16, NAME = 64 };
    char d[400];
    save_dir(seed, d, sizeof d);
    mkdirs(d);
    // working copies of this slot left by an import or save that was cut off: this import replaces the slot whole
    DIR *dir = opendir(d);
    struct dirent *e;
    while (dir && (e = readdir(dir))) {
        char base[300];
        size_t k = strlen(e->d_name);
        if (k <= 8 || k - 8 >= sizeof base || strcmp(e->d_name + k - 8, ".pending")) continue;
        snprintf(base, sizeof base, "%.*s", (int)(k - 8), e->d_name);
        if (slot_of_file(base) != slot) continue;
        char p[700];
        snprintf(p, sizeof p, "%s/%s", d, e->d_name);
        remove(p);
    }
    if (dir) closedir(dir);

    char files[MAX_FILES][NAME];
    int nf = 0;
    bool ok = true;
    for (int i = 0, k = zip_count(z); i < k && ok; i++) {
        ZipEntry ze;
        if (!zip_entry_at(z, i, &ze) || ze.is_dir) continue;
        Entry en = classify(ze.name);
        if (en.kind != E_SLOT || en.slot != slot || strcmp(en.seed, seed)) continue;
        if (nf == MAX_FILES || strlen(en.file) >= NAME) { ok = false; break; }
        size_t sz = 0;
        unsigned char *b = read_entry(z, i, &sz);
        if (!b || (is_data_file(en.file) && saves_check(b, sz))) { free(b); ok = false; break; }
        char p[600];
        snprintf(p, sizeof p, "%s/%s.pending", d, en.file);
        snprintf(files[nf++], NAME, "%s", en.file);
        ok = write_file(p, b, sz);
        free(b);
    }
    int data = -1;
    for (int f = 0; f < nf; f++)
        if (is_data_file(files[f])) data = f;
    if (data < 0) ok = false;
    if (ok) {  // this slot's files here that the bundle doesn't have (a sidecar of the old save, its screenshot)
        dir = opendir(d);
        while (dir && (e = readdir(dir))) {
            if (slot_of_file(e->d_name) != slot) continue;
            bool keep = false;
            for (int f = 0; f < nf; f++) keep |= !strcmp(files[f], e->d_name);
            if (keep) continue;
            char p[600];
            snprintf(p, sizeof p, "%s/%s", d, e->d_name);
            remove(p);
        }
        if (dir) closedir(dir);
    }
    char from[600], to[600];
    if (ok) {
        snprintf(to, sizeof to, "%s/%s", d, files[data]);
        snprintf(from, sizeof from, "%s.pending", to);
        ok = replace_file(from, to);
    }
    if (!ok) {  // (the save isn't replaced: every working copy goes)
        for (int f = 0; f < nf; f++) {
            snprintf(from, sizeof from, "%s/%s.pending", d, files[f]);
            remove(from);
        }
        return false;
    }
    set_mtime(to, mtime);
    sync_dir(d);
    for (int f = 0; f < nf; f++) {
        if (f == data) continue;
        snprintf(to, sizeof to, "%s/%s", d, files[f]);
        snprintf(from, sizeof from, "%s.pending", to);
        if (!replace_file(from, to)) {
            LOG("[saves] imported %s, but not %s yet: the next start finishes it\n", files[data], files[f]);
            ok = false;
            continue;
        }
        set_mtime(to, mtime);
    }
    sync_dir(d);
    return ok;
}

bool saves_import(const char *zip, const SavesInfo *info, const unsigned *take, char *err, size_t n) {
    char why[200];
    ZipReader *z = zip_open(zip, why, sizeof why);
    if (!z) { say(err, n, LN("That file isn't a LAS Recomp saves file.")); return false; }
    // checked again, as it is now (the file may have changed since it was inspected)
    SavesInfo nowi = {0}, *now = &nowi;
    bool ok = inspect(z, now, err, n);
    unsigned *t = ok ? calloc((size_t)now->n, sizeof *t) : NULL;
    ok = ok && t;
    for (int i = 0; ok && i < info->n; i++) {
        if (!take[i]) continue;
        int j = 0;
        while (j < now->n && strcmp(now->g[j].seed, info->g[i].seed)) j++;
        if (j == now->n || now->g[j].problem || (take[i] & ~now->g[j].slots)) {
            say(err, n, LN("That file changed. Pick it again."));
            ok = false;
        } else t[j] = take[i];
    }
    char made[200] = "";
    if (ok && !backup(now, t, made, sizeof made)) {
        say(err, n, LN("Couldn't back up your saves first. Nothing was changed."));
        ok = false;
    }
    if (ok && *made) LOG("[saves] backed up the saves being replaced: %s\n", made);
    for (int i = 0; ok && i < now->n; i++) {
        const SavesGame *g = &now->g[i];
        if (!t[i]) continue;
        if (*g->seed) {
            char p[300];
            snprintf(p, sizeof p, "rando/%s", g->seed);
            if (!is_dir(p) && !import_seed(z, g->seed)) { say(err, n, LN("Couldn't add a seed.")); ok = false; break; }
        }
        for (int s = 0; s < SAVES_SLOTS && ok; s++) {
            if (!(t[i] >> s & 1)) continue;
            if (!import_slot(z, g->seed, s, g->mtime[s])) {
                say(err, n, LN("Couldn't write a save. The ones before it were imported."));
                ok = false;
            } else LOG("[saves] imported %s %s\n", *g->seed ? g->seed : "vanilla", saves_slot_name(s));
        }
    }
    free(t);
    saves_info_free(now);
    zip_close(z);
    return ok;
}

// ---- the game version, for the manifest (a save is the same on all of them: docs/save-format.md)
#include "game_addresses.h"
static const char *game_version_shown(void) {
    static char v[48];
    if (!v[0]) {
        const GameVersion *g = game_version_of("extracted");
        snprintf(v, sizeof v, "%s", g ? g->label : "");
    }
    return v;
}
