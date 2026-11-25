// Saves: export, import and the sync folder (runtime/saves.h). Runs in the data folder (the working directory).
#include "saves.h"

#include <SDL3/SDL.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
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
#define SYNC_FILE "saves_sync.txt"     // in the data folder: where the sync folder is
#define SYNC_NAME "LAS Recomp saves.zip"
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
// Written in full and flushed (a cut-off write never replaces anything: the caller renames it into place)
static bool write_file(const char *path, const void *data, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, n, f) == n && fflush(f) == 0;
#ifndef _WIN32
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
#if !defined(__ANDROID__) && !defined(__APPLE__)  // (plat_sync_*: the plain folders' copy)
static bool copy_file(const char *from, const char *to) {
    FILE *in = fopen(from, "rb");
    if (!in) return false;
    FILE *out = fopen(to, "wb");
    if (!out) { fclose(in); return false; }
    char buf[1 << 15];
    size_t k;
    bool ok = true;
    while ((k = fread(buf, 1, sizeof buf, in)) > 0) ok &= fwrite(buf, 1, k, out) == k;
    ok &= !ferror(in);
    fclose(in);
    ok &= fflush(out) == 0;
    ok &= fclose(out) == 0;
    if (!ok) remove(to);
    return ok;
}
#endif
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
// A seed's folder name as the app makes them; nothing that could leave rando/ (no separators, no "..", no dot first)
static bool seed_name_ok(const char *s, size_t n) {
    if (!n || n >= sizeof(((SavesGame *)0)->seed) || s[0] == '.') return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
            c == '>' || c == '|' || c == 0x7F)
            return false;
    }
    return true;
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
        (!f[13] || (f[13] == '.' && f[14] && !strchr(f + 14, '/') && !strchr(f + 14, '\\'))))
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
        return m > 7 && f[0] != '.' && !strchr(f, '/') && !strchr(f, '\\') && !strcmp(f + m - 7, ".pchtxt");
    }
    return !strchr(rel, '/') && !strchr(rel, '\\');
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
typedef struct {
    int n;
    struct { char seed[128]; long long mtime[SAVES_SLOTS]; bool seed_files; } g[SAVES_MAX_GAMES];
} Local;

static int local_find(Local *l, const char *seed) {
    for (int i = 0; i < l->n; i++)
        if (!strcmp(l->g[i].seed, seed)) return i;
    return -1;
}
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
// Every game with at least one save: vanilla first, then the seeds
static void local_scan(Local *l) {
    memset(l, 0, sizeof *l);
    long long mt[SAVES_SLOTS];
    local_slots("", mt);
    if (mt[0] || mt[1] || mt[2] || mt[3]) {
        memcpy(l->g[0].mtime, mt, sizeof mt);
        l->n = 1;
    }
    DIR *d = opendir("rando");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && l->n < SAVES_MAX_GAMES) {
        if (!seed_name_ok(e->d_name, strlen(e->d_name))) continue;
        char p[400];
        snprintf(p, sizeof p, "rando/%s", e->d_name);
        if (!is_dir(p)) continue;
        local_slots(e->d_name, mt);
        if (!(mt[0] || mt[1] || mt[2] || mt[3])) continue;
        snprintf(l->g[l->n].seed, sizeof l->g[l->n].seed, "%s", e->d_name);
        memcpy(l->g[l->n].mtime, mt, sizeof mt);
        l->g[l->n].seed_files = has_seed_record(e->d_name);
        l->n++;
    }
    closedir(d);
}

bool saves_any(void) {
    Local *l = malloc(sizeof *l);
    if (!l) return false;
    local_scan(l);
    bool any = l->n > 0;
    free(l);
    return any;
}

// ---- reading a bundle
static SavesGame *info_game(SavesInfo *info, const char *seed) {
    for (int i = 0; i < info->n; i++)
        if (!strcmp(info->g[i].seed, seed)) return &info->g[i];
    if (info->n >= SAVES_MAX_GAMES) return NULL;
    SavesGame *g = &info->g[info->n++];
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
            if (g) g->mtime[s] = mt;
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

// info from an open bundle; every save read and checked (a bundle a cloud drive synced only partly fails here)
static bool inspect(ZipReader *z, SavesInfo *info, char *err, size_t n) {
    memset(info, 0, sizeof *info);
    if (!read_manifest(z, info, err, n)) return false;
    unsigned seen[SAVES_MAX_GAMES] = {0};
    for (int i = 0, k = zip_count(z); i < k; i++) {
        ZipEntry ze;
        if (!zip_entry_at(z, i, &ze) || ze.is_dir) continue;
        Entry e = classify(ze.name);
        if (e.kind == E_SEED) {
            SavesGame *g = info_game(info, e.seed);
            if (g && !strcmp(e.file, "seed.json")) g->seed_files = true;
            if (g && !strcmp(e.file, "seed_id.txt")) {  // a seed here by that name must be the same seed
                char p[300], here[128] = "";
                snprintf(p, sizeof p, "rando/%s/seed_id.txt", e.seed);
                FILE *f = fopen(p, "rb");
                size_t k = f ? fread(here, 1, sizeof here - 1, f) : 0;
                if (f) fclose(f);
                here[k] = 0;
                size_t sz = 0;
                char *there = f ? read_entry(z, i, &sz) : NULL;
                if (there) {
                    here[strcspn(here, "\r\n")] = 0;
                    there[strcspn(there, "\r\n")] = 0;
                    if (*here && strcmp(here, there)) g->problem = LN("Another seed with the same name is here.");
                }
                free(there);
            }
            continue;
        }
        if (e.kind != E_SLOT || !is_data_file(e.file)) continue;
        SavesGame *g = info_game(info, e.seed);
        if (!g) continue;
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
        seen[g - info->g] |= 1u << e.slot;
    }
    // games named only in the manifest, or with no good save, are dropped; the rest checked against this data folder
    int w = 0;
    for (int i = 0; i < info->n; i++) {
        SavesGame g = info->g[i];
        if (!g.slots && !g.problem) continue;
        for (int s = 0; s < SAVES_SLOTS; s++)
            if (!(seen[i] >> s & 1)) g.mtime[s] = 0;
        if (*g.seed && !g.problem) {
            char p[400];
            snprintf(p, sizeof p, "rando/%s", g.seed);
            if (!is_dir(p) && !g.seed_files) g.problem = LN("Its seed isn't here and isn't in the file."), g.seed_missing = true;
        }
        info->g[w++] = g;
    }
    info->n = w;
    if (!info->n) { say(err, n, LN("There are no saves in that file.")); return false; }
    return true;
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

// ---- writing a bundle: each slot from the data folder, or from another bundle (the sync folder's) where that one is
// newer. changed: anything taken from the data folder that `from` didn't have as it is.
typedef struct { const char *app; ZipReader *from; const SavesInfo *from_info; bool changed; } Build;

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
static bool add_from_slot(ZipWriter *w, ZipReader *z, const char *seed, int slot) {
    bool ok = true;
    for (int i = 0, k = zip_count(z); i < k && ok; i++) {
        ZipEntry ze;
        if (!zip_entry_at(z, i, &ze) || ze.is_dir) continue;
        Entry e = classify(ze.name);
        if (e.kind == E_SLOT && e.slot == slot && !strcmp(e.seed, seed)) ok = zip_add_from(w, z, i);
    }
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
static bool add_from_seed_files(ZipWriter *w, ZipReader *z, const char *seed) {
    bool ok = true;
    for (int i = 0, k = zip_count(z); i < k && ok; i++) {
        ZipEntry ze;
        if (!zip_entry_at(z, i, &ze) || ze.is_dir) continue;
        Entry e = classify(ze.name);
        if (e.kind == E_SEED && !strcmp(e.seed, seed)) ok = zip_add_from(w, z, i);
    }
    return ok;
}

static const char *game_version_shown(void);

static bool build(const char *out, Build *b, char *err, size_t n) {
    Local *l = malloc(sizeof *l);
    if (!l) { say(err, n, LN("Couldn't write the saves file.")); return false; }
    local_scan(l);
    // the games: here, then any only in `from` (with a good save)
    const SavesInfo *fi = b->from_info;
    if (fi)
        for (int i = 0; i < fi->n && l->n < SAVES_MAX_GAMES; i++)
            if ((!fi->g[i].problem || fi->g[i].seed_missing) && fi->g[i].slots && local_find(l, fi->g[i].seed) < 0) {
                memset(&l->g[l->n], 0, sizeof l->g[l->n]);
                snprintf(l->g[l->n].seed, sizeof l->g[l->n].seed, "%s", fi->g[i].seed);
                l->n++;
            }
    char why[200];
    ZipWriter *w = zip_create(out, why, sizeof why);
    if (!w) { LOG("[saves] %s: %s\n", out, why); free(l); say(err, n, LN("Couldn't write the saves file.")); return false; }
    size_t cap = 4096 + (size_t)l->n * SAVES_SLOTS * 200, used = 0;
    char *man = malloc(cap);
    bool ok = man != NULL;
    if (ok) used = (size_t)snprintf(man, cap, "LAS Recomp saves %d\napp %s\ngame %s\n", FORMAT, b->app ? b->app : "",
                                    game_version_shown());
    for (int i = 0; i < l->n && ok; i++) {
        const char *seed = l->g[i].seed;
        const SavesGame *fg = NULL;
        if (fi)
            for (int j = 0; j < fi->n; j++)
                if ((!fi->g[j].problem || fi->g[j].seed_missing) && !strcmp(fi->g[j].seed, seed)) fg = &fi->g[j];
        bool any = false;
        for (int s = 0; s < SAVES_SLOTS && ok; s++) {
            long long lt = l->g[i].mtime[s], ft = fg && (fg->slots >> s & 1) ? fg->mtime[s] : 0;
            bool from_there = fg && (fg->slots >> s & 1) && ft > lt;
            if (!lt && !from_there) continue;
            if (from_there) ok = add_from_slot(w, b->from, seed, s);
            else {
                // a save that doesn't check out stays out (and out of the sync folder's copy)
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
                if (lt != ft) b->changed = true;
            }
            any = true;
            if (ok && used + 300 < cap)
                used += (size_t)snprintf(man + used, cap - used, "slot %s %d %lld\n", *seed ? seed : "-", s,
                                         from_there ? ft : lt);
        }
        if (!any || !*seed || !ok) continue;
        if (l->g[i].seed_files) {
            ok = add_local_seed_files(w, seed);
            if (fg && !fg->seed_files) b->changed = true;
        } else if (fg && fg->seed_files) ok = add_from_seed_files(w, b->from, seed);
    }
    if (ok) ok = zip_add_mem(w, MANIFEST, man, used);
    free(man);
    free(l);
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
    Build b = {g_app_version, NULL, NULL, false};
    if (!build(tmp, &b, err, n)) return false;
    if (!replace_file(tmp, zip)) {
        remove(tmp);
        say(err, n, LN("Couldn't write the saves file."));
        return false;
    }
    LOG("[saves] exported to %s\n", zip);
    return true;
}

// ---- import
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
    qsort(names, (size_t)k, 64, (int (*)(const void *, const void *))strcmp);  // (named by date: oldest first)
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
    char man[8192];
    size_t used = (size_t)snprintf(man, sizeof man, "LAS Recomp saves %d\napp %s\ngame %s\n", FORMAT, g_app_version,
                                   game_version_shown());
    bool ok = true;
    for (int i = 0; i < info->n && ok; i++) {
        long long mt[SAVES_SLOTS];
        local_slots(info->g[i].seed, mt);
        for (int s = 0; s < SAVES_SLOTS && ok; s++) {
            if (!(take[i] >> s & 1) || !mt[s]) continue;
            ok = add_local_slot(w, info->g[i].seed, s);
            if (used + 200 < sizeof man)
                used += (size_t)snprintf(man + used, sizeof man - used, "slot %s %d %lld\n",
                                         *info->g[i].seed ? info->g[i].seed : "-", s, mt[s]);
        }
    }
    ok = ok && zip_add_mem(w, MANIFEST, man, used);
    if (!zip_finish(w) || !ok) { remove(p); LOG("[saves] backup %s failed\n", p); return false; }
    snprintf(made, made_n, "%s", p);
    prune_backups();
    return true;
}

// A seed the data folder doesn't have, from the bundle: into rando/<seed>.import, then renamed into place
static bool import_seed(ZipReader *z, const char *seed) {
    char tmp[300], dst[300];
    snprintf(tmp, sizeof tmp, "rando/%s.import", seed);
    snprintf(dst, sizeof dst, "rando/%s", seed);
    char ex[320];
    snprintf(ex, sizeof ex, "%s/exefs", tmp);
    mkdirs(ex);
    bool ok = true;
    for (int i = 0, k = zip_count(z); i < k && ok; i++) {
        ZipEntry ze;
        if (!zip_entry_at(z, i, &ze) || ze.is_dir) continue;
        Entry e = classify(ze.name);
        if (e.kind != E_SEED || strcmp(e.seed, seed)) continue;
        size_t sz = 0;
        void *b = read_entry(z, i, &sz);
        char p[700];
        snprintf(p, sizeof p, "%s/%s", tmp, e.file);
        ok = b && write_file(p, b, sz);
        free(b);
    }
    if (ok) ok = rename(tmp, dst) == 0;
    if (!ok) LOG("[saves] couldn't add seed %s\n", seed);
    return ok;
}

// One slot: every file of it in the bundle written as a working copy (.pending, as runtime/services/fs.c's saves),
// then the save renamed into place first and its sidecars after (a cut-off import leaves a sidecar's working copy
// the next mount finishes, as after a cut-off save), the old sidecars it doesn't have removed, the time set.
static bool import_slot(ZipReader *z, const char *seed, int slot, long long mtime) {
    char d[400];
    save_dir(seed, d, sizeof d);
    mkdirs(d);
    char files[16][64];
    int nf = 0;
    bool ok = true;
    for (int i = 0, k = zip_count(z); i < k && ok && nf < 16; i++) {
        ZipEntry ze;
        if (!zip_entry_at(z, i, &ze) || ze.is_dir) continue;
        Entry e = classify(ze.name);
        if (e.kind != E_SLOT || e.slot != slot || strcmp(e.seed, seed) || strlen(e.file) >= 64) continue;
        size_t sz = 0;
        unsigned char *b = read_entry(z, i, &sz);
        if (!b || (is_data_file(e.file) && saves_check(b, sz))) { free(b); ok = false; break; }
        char p[600];
        snprintf(p, sizeof p, "%s/%s.pending", d, e.file);
        ok = write_file(p, b, sz);
        free(b);
        snprintf(files[nf++], 64, "%s", e.file);
    }
    // the save first, then the rest
    for (int pass = 0; pass < 2 && ok; pass++)
        for (int f = 0; f < nf && ok; f++) {
            if (is_data_file(files[f]) != !pass) continue;
            char from[600], to[600];
            snprintf(to, sizeof to, "%s/%s", d, files[f]);
            snprintf(from, sizeof from, "%s.pending", to);
            ok = replace_file(from, to);
            if (ok) set_mtime(to, mtime);
        }
    if (!ok) {  // (nothing renamed yet unless a rename itself failed: the working copies go)
        for (int f = 0; f < nf; f++) {
            char p[600];
            snprintf(p, sizeof p, "%s/%s.pending", d, files[f]);
            remove(p);
        }
        return false;
    }
    // this slot's files here that the bundle doesn't have (a sidecar of the old save, its screenshot)
    DIR *dir = opendir(d);
    struct dirent *e;
    while (dir && (e = readdir(dir))) {
        if (slot_of_file(e->d_name) != slot) continue;
        bool keep = false;
        for (int f = 0; f < nf; f++) keep |= !strcmp(files[f], e->d_name);
        if (!keep) {
            char p[600];
            snprintf(p, sizeof p, "%s/%s", d, e->d_name);
            remove(p);
        }
    }
    if (dir) closedir(dir);
    return true;
}

bool saves_import(const char *zip, const SavesInfo *info, const unsigned *take, char *err, size_t n) {
    char why[200];
    ZipReader *z = zip_open(zip, why, sizeof why);
    if (!z) { say(err, n, LN("That file isn't a LAS Recomp saves file.")); return false; }
    // checked again, as it is now (the file may have changed since it was inspected)
    SavesInfo *now = malloc(sizeof *now);
    bool ok = now && inspect(z, now, err, n);
    unsigned *t = calloc(SAVES_MAX_GAMES, sizeof *t);
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
    free(now);
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

// ---- the sync folder
static pthread_mutex_t g_sync_lock = PTHREAD_MUTEX_INITIALIZER;  // (one merge or check at a time)
static char g_where[8192];   // the folder (path; Android: URI) - "" off
static char g_line[8192];    // as saves_sync.txt holds it (iOS: the bookmark)

#if !defined(__ANDROID__) && !defined(__APPLE__)
// A plain folder (Windows, Linux): a copy, and for the put a copy next to it renamed over it
int plat_sync_fetch(const char *where, const char *name, const char *local) {
    char p[8400];
    snprintf(p, sizeof p, "%s/%s", where, name);
    if (!file_mtime(p)) return 0;
    return copy_file(p, local) ? 1 : -1;
}
bool plat_sync_put(const char *where, const char *name, const char *local) {
    char p[8400], tmp[8500];
    snprintf(p, sizeof p, "%s/%s", where, name);
    snprintf(tmp, sizeof tmp, "%s/.%s.part", where, name);
    if (!copy_file(local, tmp)) return false;
    if (!replace_file(tmp, p)) { remove(tmp); return false; }
    return true;
}
#endif

void saves_sync_init(void) {
    pthread_mutex_lock(&g_sync_lock);
    g_where[0] = g_line[0] = 0;
    const char *test = getenv("LARECOMP_SAVE_DIR");
    FILE *f = test && *test ? NULL : fopen(SYNC_FILE, "rb");  // (test runs save elsewhere: they never sync)
    if (f) {
        size_t k = fread(g_line, 1, sizeof g_line - 1, f);
        fclose(f);
        g_line[k] = 0;
        g_line[strcspn(g_line, "\r\n")] = 0;
    }
#if PLAT_IOS
    if (g_line[0] && g_line[0] != '/' && plat_ios_bookmark_resolve(g_line, g_where, sizeof g_where) == 0) {
        LOG("[saves] the sync folder is gone: syncing is off until one is picked again\n");
        g_where[0] = 0;
    } else if (g_line[0] == '/') snprintf(g_where, sizeof g_where, "%s", g_line);
#else
    snprintf(g_where, sizeof g_where, "%s", g_line);
#endif
    if (g_where[0]) LOG("[saves] sync folder: %s\n", saves_sync_shown());
    pthread_mutex_unlock(&g_sync_lock);
}

bool saves_sync_on(void) { return g_where[0] != 0; }

const char *saves_sync_shown(void) {
#ifdef __ANDROID__
    // content://<provider>/tree/primary%3ADocuments%2FLAS -> Documents/LAS (the place as the Files app shows it)
    static char s[1024];
    const char *t = strstr(g_where, "/tree/");
    if (!t) return g_where;
    t += 6;
    size_t k = 0;
    for (; *t && *t != '/' && k + 1 < sizeof s; t++) {
        if (*t == '%' && t[1] && t[2]) {
            char h[3] = {t[1], t[2], 0};
            s[k++] = (char)strtol(h, NULL, 16);
            t += 2;
        } else s[k++] = *t;
    }
    s[k] = 0;
    char *c = strchr(s, ':');
    if (c && !strncmp(s, "primary", 7)) memmove(s, c + 1, strlen(c + 1) + 1);  // (internal storage: no volume name)
    else if (c) *c = '/';
    return s[0] ? s : "/";
#else
    return g_where;
#endif
}

// One merge into the sync folder: its bundle fetched, the newer of each slot kept, put back when anything here was
// newer. (g_sync_lock held)
static bool merge(void) {
    if (!g_where[0]) return true;
    mkdir("cache", 0755);
    const char *in = "cache/sync-in.zip", *out = "cache/sync-out.zip";
    remove(in);
    int got = plat_sync_fetch(g_where, SYNC_NAME, in);
    if (got < 0) { LOG("[saves] sync: couldn't read the sync folder's saves\n"); return false; }
    Build b = {g_app_version, NULL, NULL, false};
    SavesInfo *fi = NULL;
    char err[200];
    if (got == 1) {
        char why[200];
        b.from = zip_open(in, why, sizeof why);
        fi = malloc(sizeof *fi);
        if (b.from && fi && inspect(b.from, fi, err, sizeof err)) b.from_info = fi;
        else LOG("[saves] sync: the sync folder's saves can't be read (still syncing?): leaving it as it is\n");
        if (!b.from_info) {  // (a half-synced file is never written over: the next save tries again)
            if (b.from) zip_close(b.from);
            free(fi);
            return false;
        }
    }
    bool ok = build(out, &b, err, sizeof err);
    if (b.from) zip_close(b.from);
    free(fi);
    if (ok && (b.changed || got == 0)) {
        ok = plat_sync_put(g_where, SYNC_NAME, out);
        LOG("[saves] sync: %s\n", ok ? "saves written to the sync folder" : "couldn't write to the sync folder");
    }
    remove(out);
    remove(in);
    return ok;
}

bool saves_sync_set(const char *where, char *err, size_t n) {
    pthread_mutex_lock(&g_sync_lock);
    char line[8192];
    snprintf(line, sizeof line, "%s", where ? where : "");
#if PLAT_IOS
    if (where && !plat_ios_bookmark_make(where, line, sizeof line)) {
        pthread_mutex_unlock(&g_sync_lock);
        say(err, n, LN("LAS Recomp can't keep using that folder. Pick another one."));
        return false;
    }
#endif
#if PLAT_IOS
    char held[8192];  // (the folder's scope, held from now on: the picker's ends at the next pick)
    if (where && plat_ios_bookmark_resolve(line, held, sizeof held) == 0) {
        pthread_mutex_unlock(&g_sync_lock);
        say(err, n, LN("LAS Recomp can't keep using that folder. Pick another one."));
        return false;
    }
#endif
    if (!where) remove(SYNC_FILE);
    else {
        char l2[8200];
        int k = snprintf(l2, sizeof l2, "%s\n", line);
        char tmp[64];
        snprintf(tmp, sizeof tmp, "%s.tmp", SYNC_FILE);
        if (!write_file(tmp, l2, (size_t)k) || !replace_file(tmp, SYNC_FILE)) {
            pthread_mutex_unlock(&g_sync_lock);
            say(err, n, LN("Couldn't save the setting."));
            return false;
        }
    }
    snprintf(g_line, sizeof g_line, "%s", line);
    snprintf(g_where, sizeof g_where, "%s", where ? where : "");
    LOG("[saves] sync folder %s%s\n", where ? "set: " : "off", where ? saves_sync_shown() : "");
    bool ok = merge();
    pthread_mutex_unlock(&g_sync_lock);
    if (!ok) say(err, n, LN("Couldn't write your saves to that folder."));
    return ok;
}

// After a save: one merge a little later (a save writes several files; a few saves in a row merge once), on a
// thread of its own so the game never waits for a cloud folder.
static pthread_mutex_t g_kick_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_kick_cond = PTHREAD_COND_INITIALIZER;
static bool g_kicked, g_worker;
static uint64_t g_kick_at;
static void *sync_worker(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_kick_lock);
        while (!g_kicked) pthread_cond_wait(&g_kick_cond, &g_kick_lock);
        pthread_mutex_unlock(&g_kick_lock);
        for (;;) {  // until 2 s passed with no new save
            pthread_mutex_lock(&g_kick_lock);
            uint64_t since = SDL_GetTicks() - g_kick_at;
            if (since >= 2000) g_kicked = false;
            pthread_mutex_unlock(&g_kick_lock);
            if (since >= 2000) break;
            SDL_Delay((Uint32)(2000 - since));
        }
        pthread_mutex_lock(&g_sync_lock);
        merge();
        pthread_mutex_unlock(&g_sync_lock);
    }
    return NULL;
}
void saves_sync_kick(void) {
    if (!g_where[0]) return;
    pthread_mutex_lock(&g_kick_lock);
    g_kicked = true;
    g_kick_at = SDL_GetTicks();
    if (!g_worker) {
        pthread_t th;
        if (pthread_create(&th, NULL, sync_worker, NULL) == 0) pthread_detach(th), g_worker = true;
    }
    pthread_cond_signal(&g_kick_cond);
    pthread_mutex_unlock(&g_kick_lock);
}

bool saves_sync_check(SavesInfo *info) {
    memset(info, 0, sizeof *info);
    pthread_mutex_lock(&g_sync_lock);
    bool any = false;
    if (g_where[0]) {
        mkdir("cache", 0755);
        const char *in = "cache/sync-check.zip";
        remove(in);
        char err[200];
        if (plat_sync_fetch(g_where, SYNC_NAME, in) == 1 && saves_inspect(in, info, err, sizeof err))
            for (int i = 0; i < info->n; i++) {
                SavesGame *g = &info->g[i];
                if (g->problem) continue;
                long long mt[SAVES_SLOTS];
                local_slots(g->seed, mt);
                for (int s = 0; s < SAVES_SLOTS; s++)
                    if ((g->slots >> s & 1) && g->mtime[s] > mt[s]) g->newer |= 1u << s;
                any |= g->newer != 0;
            }
        if (!any) remove(in);
    }
    pthread_mutex_unlock(&g_sync_lock);
    if (any) LOG("[saves] the sync folder has newer saves\n");
    return any;
}

bool saves_sync_bring_in(const SavesInfo *info, char *err, size_t n) {
    unsigned take[SAVES_MAX_GAMES] = {0};
    for (int i = 0; i < info->n; i++) take[i] = info->g[i].newer;
    pthread_mutex_lock(&g_sync_lock);
    bool ok = saves_import("cache/sync-check.zip", info, take, err, n);
    remove("cache/sync-check.zip");
    pthread_mutex_unlock(&g_sync_lock);
    return ok;
}
