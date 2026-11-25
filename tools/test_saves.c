// Save bundles (runtime/saves.c) on synthetic saves, without game data: export, inspect and import (backups,
// sidecars, seeds, damaged saves, unsafe names, a different seed by the same name).
//
// make build/miniz/.fetched && cc -O1 -g -Iruntime -Ibuild/miniz $(pkg-config --cflags sdl3) tools/test_saves.c \
//   runtime/saves.c runtime/zipfile.c build/miniz/miniz.c $(pkg-config --libs sdl3) -o build/test-saves && build/test-saves
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

#include "game_addresses.h"
#include "saves.h"
#include "zipfile.h"

const GameVersion *game_version_of(const char *game_dir) { (void)game_dir; return NULL; }  // (no game files here)

static int g_fail;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static uint32_t crc(const uint8_t *p, size_t n) {
    uint32_t c = 0xEDB88320u;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = c & 1 ? (c >> 1) ^ 0xEDB88320u : c >> 1;
    }
    return c;
}
static void put32(uint8_t *p, uint32_t v) { p[0] = v, p[1] = v >> 8, p[2] = v >> 16, p[3] = v >> 24; }
// A save that passes the game's checks; tag marks it apart
static void make_save(uint8_t *b, uint8_t tag) {
    memset(b, 0, 0x5EE0);
    put32(b, 0xE);
    b[0x40] = tag;
    b[0x1000] = tag;
    put32(b + 0x5EC4, 0);
    put32(b + 0x5EC4, crc(b + 0xE0, 0x5EE0 - 0xE0));
    put32(b + 4, 0);
    put32(b + 4, crc(b, 0xE0));
}
static void mkdirs(const char *path) {
    char p[1024];
    snprintf(p, sizeof p, "%s", path);
    for (char *c = p + 1; *c; c++)
        if (*c == '/') { *c = 0; mkdir(p, 0755); *c = '/'; }
    mkdir(p, 0755);
}
static void write_bytes(const char *path, const void *b, size_t n, long long mtime) {
    char d[1024];
    snprintf(d, sizeof d, "%s", path);
    char *s = strrchr(d, '/');
    if (s) *s = 0, mkdirs(d);
    FILE *f = fopen(path, "wb");
    fwrite(b, 1, n, f);
    fclose(f);
    if (mtime) {
        struct utimbuf u = {(time_t)mtime, (time_t)mtime};
        utime(path, &u);
    }
}
static void write_save(const char *path, uint8_t tag, long long mtime) {
    static uint8_t b[0x5EE0];
    make_save(b, tag);
    write_bytes(path, b, sizeof b, mtime);
}
static int tag_of(const char *path) {
    uint8_t b[0x5EE0];
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t n = fread(b, 1, sizeof b, f);
    fclose(f);
    return n == sizeof b ? b[0x40] : -1;
}
static bool exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}
static long long mtime_of(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 ? (long long)st.st_mtime : 0;
}
static int count_files(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    int k = 0;
    struct dirent *e;
    while ((e = readdir(d)))
        if (e->d_name[0] != '.') k++;
    closedir(d);
    return k;
}
static const SavesGame *game(const SavesInfo *i, const char *seed) {
    for (int k = 0; k < i->n; k++)
        if (!strcmp(i->g[k].seed, seed)) return &i->g[k];
    return NULL;
}

int main(void) {
    char root[400];
    const char *tmp = getenv("TMPDIR");
    snprintf(root, sizeof root, "%s/lasrecomp-test-saves-XXXXXX", tmp && *tmp ? tmp : "/tmp");
    if (!mkdtemp(root)) return 1;
    char A[300], B[300], bundle[300];
    snprintf(A, sizeof A, "%s/A", root), snprintf(B, sizeof B, "%s/B", root);
    snprintf(bundle, sizeof bundle, "%s/bundle.zip", root);
    mkdirs(A), mkdirs(B);
    long long t0 = 1700000000;

    // ---- data folder A: vanilla files 1 and autosave, a seed with two slots (one with a flags sidecar), an old seed
    chdir(A);
    write_save("save/save_0/savedata0.bin", 1, t0);
    write_bytes("save/save_0/screenshot0.bin", "shot", 4, t0);
    write_save("save/save_0/savedata3.bin", 4, t0);
    write_bytes("save/save_0/savedata3.bin.pending", "junk", 4, t0);  // (a working copy: never exported)
    write_save("rando/Zora1/save/save_0/savedata0.bin", 11, t0);
    write_bytes("rando/Zora1/save/save_0/savedata0.bin.flagsx", "FLAGS-A", 7, t0);
    write_save("rando/Zora1/save/save_0/savedata1.bin", 12, t0);
    write_bytes("rando/Zora1/seed.json", "{\"seed\":1}", 10, 0);
    write_bytes("rando/Zora1/seed_id.txt", "AAAAA-11111\n", 12, 0);
    write_bytes("rando/Zora1/exefs/1.0.0.pchtxt", "patch", 5, 0);
    write_bytes("rando/Zora1/romfs/Game.bin", "GAMEFILE", 8, 0);  // (game files: never exported)
    write_save("rando/Old2/save/save_0/savedata2.bin", 22, t0);    // (an older seed: no seed.json)
    write_bytes("rando/Old2/romfs/x.bin", "GAMEFILE", 8, 0);
    write_bytes("rando/NoSaves/seed.json", "{}", 2, 0);           // (a seed nobody played: not exported)

    char err[200];
    CHECK(saves_any());
    CHECK(saves_export(bundle, "vtest", err, sizeof err));
    ZipReader *z = zip_open(bundle, err, sizeof err);
    CHECK(z != NULL);
    if (z) {
        CHECK(zip_find(z, "save/save_0/savedata0.bin") >= 0);
        CHECK(zip_find(z, "save/save_0/screenshot0.bin") >= 0);
        CHECK(zip_find(z, "save/save_0/savedata3.bin.pending") < 0);
        CHECK(zip_find(z, "rando/Zora1/save/save_0/savedata0.bin.flagsx") >= 0);
        CHECK(zip_find(z, "rando/Zora1/seed.json") >= 0);
        CHECK(zip_find(z, "rando/Zora1/exefs/1.0.0.pchtxt") >= 0);
        CHECK(zip_find(z, "rando/Zora1/romfs/Game.bin") < 0);
        CHECK(zip_find(z, "rando/Old2/romfs/x.bin") < 0);
        CHECK(zip_find(z, "rando/NoSaves/seed.json") < 0);
        zip_close(z);
    }
    SavesInfo info;
    CHECK(saves_inspect(bundle, &info, err, sizeof err));
    CHECK(info.n == 3 && !strcmp(info.app, "vtest"));
    const SavesGame *v = game(&info, ""), *zr = game(&info, "Zora1"), *old = game(&info, "Old2");
    CHECK(v && v->slots == 0x9 && v->mtime[0] == t0 && !v->problem);
    CHECK(zr && zr->slots == 0x3 && zr->seed_files && !zr->problem);
    CHECK(old && old->slots == 0x4 && !old->seed_files && !old->problem);  // (the seed is here in A)

    // ---- into an empty data folder B: vanilla and Zora1 import; Old2 can't (its seed isn't in B or in the file)
    chdir(B);
    CHECK(saves_inspect(bundle, &info, err, sizeof err));
    old = game(&info, "Old2");
    CHECK(old && old->problem);
    unsigned take[SAVES_MAX_GAMES] = {0};
    for (int i = 0; i < info.n; i++) take[i] = info.g[i].problem ? 0 : info.g[i].slots;
    CHECK(saves_import(bundle, &info, take, err, sizeof err));
    CHECK(tag_of("save/save_0/savedata0.bin") == 1 && tag_of("save/save_0/savedata3.bin") == 4);
    CHECK(exists("save/save_0/screenshot0.bin"));
    CHECK(mtime_of("save/save_0/savedata0.bin") == t0);
    CHECK(tag_of("rando/Zora1/save/save_0/savedata0.bin") == 11 && exists("rando/Zora1/save/save_0/savedata0.bin.flagsx"));
    CHECK(exists("rando/Zora1/seed.json") && exists("rando/Zora1/exefs/1.0.0.pchtxt"));
    CHECK(!exists("rando/Zora1/romfs") && !exists("rando/Old2"));
    CHECK(!exists("backups"));  // (nothing was replaced: no backup)

    // ---- over existing slots: backup first, an old sidecar the bundle lacks goes
    write_save("save/save_0/savedata0.bin", 99, t0 + 50);
    write_bytes("save/save_0/savedata0.bin.dpad", "old", 3, t0 + 50);
    CHECK(saves_inspect(bundle, &info, err, sizeof err));
    memset(take, 0, sizeof take);
    for (int i = 0; i < info.n; i++)
        if (!info.g[i].seed[0]) take[i] = 1;  // (file 1 only)
    CHECK(saves_import(bundle, &info, take, err, sizeof err));
    CHECK(tag_of("save/save_0/savedata0.bin") == 1 && !exists("save/save_0/savedata0.bin.dpad"));
    CHECK(count_files("backups") == 1);
    CHECK(!exists("save/save_0/savedata0.bin.pending"));

    // ---- a damaged save, unsafe names, a different seed by the same name
    char bad[300];
    snprintf(bad, sizeof bad, "%s/bad.zip", root);
    ZipWriter *w = zip_create(bad, err, sizeof err);
    static uint8_t sb[0x5EE0];
    make_save(sb, 7);
    sb[0x2000] ^= 1;  // (body CRC no longer matches)
    zip_add_mem(w, "save/save_0/savedata1.bin", sb, sizeof sb);
    make_save(sb, 8);
    zip_add_mem(w, "../escape.bin", sb, sizeof sb);
    zip_add_mem(w, "rando/../save/save_0/savedata2.bin", sb, sizeof sb);
    zip_add_mem(w, "rando/Zora1/../../x/savedata0.bin", sb, sizeof sb);
    zip_add_mem(w, "rando/Zora1/save/save_0/savedata2.bin", sb, sizeof sb);
    zip_add_mem(w, "rando/Zora1/seed_id.txt", "BBBBB-22222\n", 12);
    const char *man = "LAS Recomp saves 1\napp x\ngame y\nslot - 1 5\nslot Zora1 2 5\n";
    zip_add_mem(w, "lasrecomp-saves.txt", man, strlen(man));
    CHECK(zip_finish(w));
    CHECK(saves_inspect(bad, &info, err, sizeof err));
    v = game(&info, ""), zr = game(&info, "Zora1");
    CHECK(v && v->problem && !v->slots);
    CHECK(zr && zr->problem);  // (another seed called Zora1)
    CHECK(info.n == 2);
    // a newer format, and not a bundle at all
    w = zip_create(bad, err, sizeof err);
    zip_add_mem(w, "lasrecomp-saves.txt", "LAS Recomp saves 9\n", 19);
    zip_finish(w);
    CHECK(!saves_inspect(bad, &info, err, sizeof err) && strstr(err, "newer"));
    write_bytes(bad, "not a zip", 9, 0);
    CHECK(!saves_inspect(bad, &info, err, sizeof err));

    char cmd[400];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
    system(cmd);
    if (g_fail) { fprintf(stderr, "%d failed\n", g_fail); return 1; }
    printf("test_saves: all passed\n");
    return 0;
}
