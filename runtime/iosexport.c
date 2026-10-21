// The iOS app exported from the start screen (runtime/iosexport.h). Any desktop OS: the game code library is built by
// the bundled zig and the skeleton's lld (runtime/gamebuild.c GAMEBUILD_IOS), the .ipa written here (a zip: the
// skeleton app and the library, stored; iOS unpacks it anyway, and the library doesn't compress much).
#include "iosexport.h"

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#include "gamebuild.h"
#include "paths.h"
#include "platform.h"
#include "rt.h"

const char *larecomp_version(void);  // runtime/ui/launcher.mm

#ifdef _WIN32
#define SEP "\\"
#else
#define SEP "/"
#endif

static bool exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

bool iosexport_available(const char *skeleton, char *why, size_t why_size) {
    if (!skeleton || !*skeleton) { snprintf(why, why_size, "choose the unpacked iOS skeleton folder"); return false; }
    return gamebuild_ios_available(skeleton, why, why_size);
}

void iosexport_default_path(char *out, size_t n) {
#ifdef _WIN32
    const char *home = getenv("USERPROFILE");
#else
    const char *home = getenv("HOME");
#endif
    snprintf(out, n, "%s" SEP "Downloads" SEP "LAS Recomp-%s-ios.ipa", home && *home ? home : ".", larecomp_version());
}

// ------------------------------------------------------------------ a zip writer (store only, zip64 where needed)
// Entries: files, directories, symlinks (the Unix mode in the external attributes, as Info-ZIP writes them; the
// Python extensions' pointer links need it). CRC-32 as the format wants it even for stored data.
typedef struct {
    FILE *f;
    uint64_t off;
    struct { char *name; uint64_t off, size; uint32_t crc, mode, dostime; } *e;
    int n, cap;
    bool failed;
} Zip;

static uint32_t crc_table[256];
static void crc_init(void) {
    if (crc_table[1]) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
}
static uint32_t crc_update(uint32_t crc, const uint8_t *p, size_t n) {
    crc = ~crc;
    for (size_t i = 0; i < n; i++) crc = crc_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

static void w8(Zip *z, uint8_t v) { if (fputc(v, z->f) == EOF) z->failed = true; z->off++; }
static void w16(Zip *z, uint16_t v) { w8(z, v & 0xFF); w8(z, v >> 8); }
static void w32(Zip *z, uint32_t v) { w16(z, v & 0xFFFF); w16(z, v >> 16); }
static void w64(Zip *z, uint64_t v) { w32(z, (uint32_t)v); w32(z, (uint32_t)(v >> 32)); }
static void wbuf(Zip *z, const void *p, size_t n) { if (fwrite(p, 1, n, z->f) != n) z->failed = true; z->off += n; }

static uint32_t dos_time(time_t t) {
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    if (tmv.tm_year < 80) return 0x21;  // (1980-01-01)
    return ((uint32_t)(tmv.tm_year - 80) << 25) | ((uint32_t)(tmv.tm_mon + 1) << 21) | ((uint32_t)tmv.tm_mday << 16) |
           ((uint32_t)tmv.tm_hour << 11) | ((uint32_t)tmv.tm_min << 5) | ((uint32_t)tmv.tm_sec >> 1);
}

// One entry: data from a file (path), or bytes (a link's target), or nothing (a directory)
static void zip_entry(Zip *z, const char *name, const char *path, const char *bytes, uint32_t mode, time_t mtime) {
    if (z->failed) return;
    if (z->n == z->cap) {
        z->cap = z->cap ? z->cap * 2 : 256;
        z->e = realloc(z->e, sizeof *z->e * z->cap);
    }
    FILE *in = path ? fopen(path, "rb") : NULL;
    if (path && !in) { z->failed = true; return; }
    uint64_t size = 0;
    if (in) { fseek(in, 0, SEEK_END); size = (uint64_t)ftell(in); fseek(in, 0, SEEK_SET); }
    else if (bytes) size = strlen(bytes);
    bool big = size >= 0xFFFFFFFFull || z->off >= 0xFFFFFFFFull;
    uint64_t at = z->off;
    size_t nl = strlen(name);
    // local header: the CRC and sizes follow the data (bit 3), as streaming writers do
    w32(z, 0x04034B50); w16(z, big ? 45 : 20); w16(z, 0x0808); w16(z, 0); w32(z, dos_time(mtime));
    w32(z, 0); w32(z, big ? 0xFFFFFFFF : 0); w32(z, big ? 0xFFFFFFFF : 0);
    w16(z, (uint16_t)nl); w16(z, big ? 20 : 0);
    wbuf(z, name, nl);
    if (big) { w16(z, 1); w16(z, 16); w64(z, 0); w64(z, 0); }
    uint32_t crc = 0;
    if (in) {
        static uint8_t buf[1 << 20];
        size_t got;
        while ((got = fread(buf, 1, sizeof buf, in)) > 0) { crc = crc_update(crc, buf, got); wbuf(z, buf, got); }
        fclose(in);
    } else if (bytes) {
        crc = crc_update(crc, (const uint8_t *)bytes, size);
        wbuf(z, bytes, size);
    }
    w32(z, 0x08074B50); w32(z, crc);  // the data descriptor
    if (big) { w64(z, size); w64(z, size); } else { w32(z, (uint32_t)size); w32(z, (uint32_t)size); }
    z->e[z->n].name = strdup(name);
    z->e[z->n].off = at, z->e[z->n].size = size, z->e[z->n].crc = crc, z->e[z->n].mode = mode;
    z->e[z->n].dostime = dos_time(mtime);
    z->n++;
}

static void zip_finish(Zip *z) {
    uint64_t cd = z->off;
    for (int i = 0; i < z->n; i++) {
        bool big = z->e[i].size >= 0xFFFFFFFFull || z->e[i].off >= 0xFFFFFFFFull;
        size_t nl = strlen(z->e[i].name);
        w32(z, 0x02014B50); w16(z, 0x0300 | (big ? 45 : 20)); w16(z, big ? 45 : 20); w16(z, 0x0808); w16(z, 0);
        w32(z, z->e[i].dostime); w32(z, z->e[i].crc);
        w32(z, big ? 0xFFFFFFFF : (uint32_t)z->e[i].size); w32(z, big ? 0xFFFFFFFF : (uint32_t)z->e[i].size);
        w16(z, (uint16_t)nl); w16(z, big ? 28 : 0); w16(z, 0); w16(z, 0); w16(z, 0);
        w32(z, z->e[i].mode << 16);  // (made by Unix, 0x03 above: the mode in the high half)
        w32(z, big ? 0xFFFFFFFF : (uint32_t)z->e[i].off);
        wbuf(z, z->e[i].name, nl);
        if (big) { w16(z, 1); w16(z, 24); w64(z, z->e[i].size); w64(z, z->e[i].size); w64(z, z->e[i].off); }
    }
    uint64_t cd_size = z->off - cd;
    bool big = z->n >= 0xFFFF || cd >= 0xFFFFFFFFull || cd_size >= 0xFFFFFFFFull;
    if (big) {
        uint64_t eocd64 = z->off;
        w32(z, 0x06064B50); w64(z, 44); w16(z, 0x032D); w16(z, 45); w32(z, 0); w32(z, 0);
        w64(z, (uint64_t)z->n); w64(z, (uint64_t)z->n); w64(z, cd_size); w64(z, cd);
        w32(z, 0x07064B50); w32(z, 0); w64(z, eocd64); w32(z, 1);
    }
    w32(z, 0x06054B50); w16(z, 0); w16(z, 0);
    w16(z, big ? 0xFFFF : (uint16_t)z->n); w16(z, big ? 0xFFFF : (uint16_t)z->n);
    w32(z, big ? 0xFFFFFFFF : (uint32_t)cd_size); w32(z, big ? 0xFFFFFFFF : (uint32_t)cd); w16(z, 0);
    for (int i = 0; i < z->n; i++) free(z->e[i].name);
    free(z->e);
}

// A folder into the zip under prefix (entries sorted by name: reproducible archives)
static int by_name(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }
static bool zip_tree(Zip *z, const char *dir, const char *prefix, bool (*cancelled)(void)) {
    DIR *d = opendir(dir);
    if (!d) return false;
    char **names = NULL;
    int n = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || !strcmp(e->d_name, ".DS_Store")) continue;
        if (n == cap) names = realloc(names, sizeof *names * (size_t)(cap = cap ? cap * 2 : 64));
        names[n++] = strdup(e->d_name);
    }
    closedir(d);
    qsort(names, (size_t)n, sizeof *names, by_name);
    bool ok = true;
    for (int i = 0; i < n && ok; i++) {
        if (cancelled && cancelled()) { ok = false; break; }
        char path[2048], name[2048];
        snprintf(path, sizeof path, "%s" SEP "%s", dir, names[i]);
        snprintf(name, sizeof name, "%s/%s", prefix, names[i]);
        struct stat st;
#ifdef _WIN32
        if (stat(path, &st) != 0) { ok = false; break; }
#else
        if (lstat(path, &st) != 0) { ok = false; break; }
        if (S_ISLNK(st.st_mode)) {
            char target[2048];
            ssize_t len = readlink(path, target, sizeof target - 1);
            if (len < 0) { ok = false; break; }
            target[len] = 0;
            zip_entry(z, name, NULL, target, 0120755, st.st_mtime);
            continue;
        }
#endif
        size_t nl = strlen(names[i]);
        if (nl > 8 && !strcmp(names[i] + nl - 8, ".symlink") && S_ISREG(st.st_mode)) {
            // <name>.symlink holding the target: a symlink in the app (tools/bundle_ios.sh; the skeleton zip is
            // unpacked on Windows too, where links don't survive)
            char target[2048] = "";
            FILE *f = fopen(path, "rb");
            if (f) {
                size_t got = fread(target, 1, sizeof target - 1, f);
                fclose(f);
                while (got && (target[got - 1] == '\n' || target[got - 1] == '\r')) got--;
                target[got] = 0;
            }
            if (!target[0]) { ok = false; break; }
            name[strlen(name) - 8] = 0;
            zip_entry(z, name, NULL, target, 0120755, st.st_mtime);
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            char dname[2060];
            snprintf(dname, sizeof dname, "%s/", name);
            zip_entry(z, dname, NULL, NULL, 040755, st.st_mtime);
            ok = zip_tree(z, path, name, cancelled);
        } else {
            zip_entry(z, name, path, NULL, (st.st_mode & 0111) ? 0100755 : 0100644, st.st_mtime);
        }
        if (z->failed) ok = false;
    }
    for (int i = 0; i < n; i++) free(names[i]);
    free(names);
    return ok;
}

bool iosexport_run(const char *game_dir, const IosExportOpts *o, char *why, size_t why_size) {
    uint64_t t0 = plat_now_ns();
    char skel[1100], main_nso[1100];
    if (!iosexport_available(o->skeleton, why, why_size)) return false;
    snprintf(skel, sizeof skel, "%s" SEP "LAS Recomp.app", o->skeleton);
    snprintf(main_nso, sizeof main_nso, "%s" SEP "exefs" SEP "main", game_dir);
    if (!exists(main_nso)) { snprintf(why, why_size, "the game files folder has no exefs/main"); return false; }

    // 1. the game code library for iOS (<game_dir>/ios/lasgame-ios.dylib; its own build cache)
    char ios_dir[1100];
    if (!gamebuild_ios_dir(game_dir, ios_dir, sizeof ios_dir)) {
        snprintf(why, why_size, "can't create the iOS build folder in the game files");
        return false;
    }
    GameBuildOpts b = {o->progress, o->jobs, o->cancelled, GAMEBUILD_IOS, o->skeleton};
    if (!gamebuild_run(ios_dir, &b, why, why_size)) return false;

    // 2. the .ipa: Payload/LAS Recomp.app/ = the skeleton + Frameworks/LASGame.framework/{LASGame,LASGame.id}
    if (o->progress) o->progress(0.97, "Writing the .ipa...");
    char tmp[1200], lib[1100], id[1100];
    snprintf(tmp, sizeof tmp, "%s.tmp", o->out_ipa);
    snprintf(lib, sizeof lib, "%s" SEP "lasgame-ios.dylib", ios_dir);
    snprintf(id, sizeof id, "%s.id", lib);
    crc_init();
    Zip z = {0};
    z.f = fopen(tmp, "wb");
    if (!z.f) { snprintf(why, why_size, "can't write %s", tmp); return false; }
    const char *app = "Payload/LAS Recomp.app";
    zip_entry(&z, "Payload/", NULL, NULL, 040755, time(NULL));
    char dname[1100];
    snprintf(dname, sizeof dname, "%s/", app);
    zip_entry(&z, dname, NULL, NULL, 040755, time(NULL));
    bool ok = zip_tree(&z, skel, app, o->cancelled);
    char name[1200];
    struct stat st;
    stat(lib, &st);
    snprintf(name, sizeof name, "%s/Frameworks/LASGame.framework/LASGame", app);
    if (ok) zip_entry(&z, name, lib, NULL, 0100755, st.st_mtime);
    snprintf(name, sizeof name, "%s/Frameworks/LASGame.framework/LASGame.id", app);
    if (ok) zip_entry(&z, name, id, NULL, 0100644, st.st_mtime);
    zip_finish(&z);
    ok = ok && !z.failed;
    if (fclose(z.f) != 0) ok = false;
    if (!ok) {
        remove(tmp);
        snprintf(why, why_size, "%s", o->cancelled && o->cancelled() ? "Export cancelled." : "couldn't write the .ipa");
        return false;
    }
    remove(o->out_ipa);
    if (rename(tmp, o->out_ipa) != 0) {
        snprintf(why, why_size, "couldn't write %s", o->out_ipa);
        remove(tmp);
        return false;
    }
    stat(o->out_ipa, &st);
    LOG("[iosexport] %s (%.0f MB, %d entries) in %.0fs\n", o->out_ipa, (double)st.st_size / 1e6, z.n,
        (double)(plat_now_ns() - t0) / 1e9);
    if (o->progress) o->progress(1, "Done");
    return true;
}
