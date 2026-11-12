// The iOS app exported from the start screen (runtime/iosexport.h). Any desktop OS: the game code library is built by
// the bundled zig and the kit's lld (runtime/gamebuild.c GAMEBUILD_IOS), the .ipa written here: the kit's Payload/
// entries copied as they are (runtime/zipfile.h) plus the library.
#include "iosexport.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#include <SDL3/SDL_cpuinfo.h>

#include "gamebuild.h"
#include "paths.h"
#include "platform.h"
#include "romfs_build.h"
#include "rt.h"
#include "zipfile.h"

const char *larecomp_version(void);  // runtime/ui/launcher.mm

#ifdef _WIN32
#define SEP "\\"
#else
#define SEP "/"
#endif

// The kit opened for the export (one reader for the check, the build and the .ipa), or NULL: why.
static ZipReader *kit_open(const char *kit, char *why, size_t why_size) {
    ZipReader *z = gamebuild_ios_kit_open(kit, why, why_size);
    if (!z) return NULL;
    // A kit from another release would make an .ipa that only fails on the device: refused here, both versions named.
    int i = zip_find(z, "version.txt");
    size_t size = 0;
    char *v = i >= 0 ? zip_read(z, i, &size) : NULL;
    if (v) v[strcspn(v, "\r\n")] = 0;
    bool same = v && !strcmp(v, larecomp_version());
    if (!same) snprintf(why, why_size, "this iOS kit is from %s, and this is LAS Recomp %s: choose the kit of the same release",
                        v && *v ? v : "another release", larecomp_version());
    free(v);
    if (!same) { zip_close(z); return NULL; }
    return z;
}

bool iosexport_available(const char *kit, char *why, size_t why_size) {
    ZipReader *z = kit_open(kit, why, why_size);
    zip_close(z);
    return z != NULL;
}

// The iOS build's own folder, <data root>/cache/ios-build (its .gamebuild/ and library apart from this machine's;
// nothing is written into the player's folder), with a copy of the game's exefs/ (27 MB; the generator reads main
// and sdk from there - a link would be refused by the build cache's checks, and Windows has none). The exefs comes
// from wherever setup would find it in game_dir (runtime/romfs_build.h: exefs/ inside, the folder itself, nested);
// another game's exefs than last time replaces the copy, and the build cache then rebuilds for it.
static bool ios_dir(const char *game_dir, char *out, size_t n, char *why, size_t why_size) {
    char exefs[2200], romfs[2200];
    bool unusable = false;
    romfs_find_game(game_dir, exefs, sizeof exefs, romfs, sizeof romfs, &unusable);
    if (!*exefs) {
        snprintf(why, why_size, unusable ? "that folder holds game packages (.xci / .nsp / .nca), not the game's files"
                                         : "no game files in that folder (nothing with main and main.npdm)");
        return false;
    }
    char parent[1100];
    snprintf(parent, sizeof parent, "%s" SEP "cache", rt_data_root());
    mkdir(parent, 0755);
    iosexport_build_dir(out, n);
    mkdir(out, 0755);
    char to[1100], main_nso[1100], src_main[2300];
    snprintf(to, sizeof to, "%s" SEP "exefs", out);
    snprintf(main_nso, sizeof main_nso, "%s" SEP "main", to);
    snprintf(src_main, sizeof src_main, "%s" SEP "main", exefs);
    struct stat a, b;
    bool same = stat(main_nso, &a) == 0 && stat(src_main, &b) == 0 && a.st_size == b.st_size && a.st_mtime == b.st_mtime;
    if (!same) {
        plat_remove_tree(to);
        mkdir(to, 0755);
        // (the exefs files by name: in a flat layout the folder holds the romfs too - 6 GB not to copy)
        static const char *const names[] = {"main", "main.npdm", "sdk", "rtld", "subsdk0", "subsdk1", "subsdk2", "subsdk3"};
        for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
            char from[2300], dst[1200];
            snprintf(from, sizeof from, "%s" SEP "%s", exefs, names[i]);
            snprintf(dst, sizeof dst, "%s" SEP "%s", to, names[i]);
            struct stat st;
            if (stat(from, &st) != 0) continue;
            if (!romfs_copy_file(from, dst)) { snprintf(why, why_size, "can't copy the game's %s into %s", names[i], out); return false; }
        }
    }
    LOG("[iosexport] exefs %s -> %s%s\n", exefs, to, same ? " (there already)" : "");
    return true;
}

static void cli_progress(double f, const char *step) {
    static int last = -1;
    int pct = (int)(f * 100);
    if (pct != last) { last = pct; fprintf(stderr, "[gamebuild] %3d%% %s\n", pct, step); }
}
// `lasrecomp --build-ios <kit.laskit> [game files]` (runtime/main.c): the export as the start screen does it, to
// ~/Downloads or LARECOMP_IOS_OUT=<path>.
bool iosexport_run_cli(const char *kit, const char *game_dir) {
    char why[512], out[1100];
    const char *e = getenv("LARECOMP_IOS_OUT");
    if (e && *e) snprintf(out, sizeof out, "%s", e);
    else iosexport_default_path(out, sizeof out);
    int cores = plat_cpu_count();  // (0 where there's no affinity mask: the machine's count)
    if (cores < 1) cores = SDL_GetNumLogicalCPUCores();
    IosExportOpts o = {cli_progress, NULL, cores > 1 ? cores : 1, kit, out};
    if (!iosexport_run(game_dir, &o, why, sizeof why)) { fprintf(stderr, "[iosexport] %s\n", why); return false; }
    return true;
}

void iosexport_build_dir(char *out, size_t n) { snprintf(out, n, "%s" SEP "cache" SEP "ios-build", rt_data_root()); }

void iosexport_default_path(char *out, size_t n) {
#ifdef _WIN32
    const char *home = getenv("USERPROFILE");
#else
    const char *home = getenv("HOME");
#endif
    snprintf(out, n, "%s" SEP "Downloads" SEP "LAS Recomp-%s-iOS.ipa", home && *home ? home : ".", larecomp_version());
}

bool iosexport_run(const char *game_dir, const IosExportOpts *o, char *why, size_t why_size) {
    uint64_t t0 = plat_now_ns();
    ZipReader *kit = kit_open(o->kit, why, why_size);
    if (!kit) return false;

    // 1. the game code library for iOS (<data root>/cache/ios-build/lasgame-ios.dylib; its own build cache)
    char dir[1100];
    GameBuildOpts b = {o->progress, o->jobs, o->cancelled, GAMEBUILD_IOS, kit};
    if (!ios_dir(game_dir, dir, sizeof dir, why, why_size) || !gamebuild_run(dir, &b, why, why_size)) {
        zip_close(kit);
        return false;
    }

    // 2. the .ipa: the kit's Payload/ entries as they are (no recompression) +
    //    Payload/LAS Recomp.app/Frameworks/LASGame.framework/{LASGame,LASGame.id}
    if (o->progress) o->progress(0.97, "Writing the .ipa...");
    char tmp[1200], lib[1100], id[1100], zwhy[256];
    snprintf(tmp, sizeof tmp, "%s.tmp", o->out_ipa);
    snprintf(lib, sizeof lib, "%s" SEP "lasgame-ios.dylib", dir);
    snprintf(id, sizeof id, "%s.id", lib);
    ZipWriter *w = zip_create(tmp, zwhy, sizeof zwhy);
    if (!w) { zip_close(kit); snprintf(why, why_size, "can't write %s: %s", tmp, zwhy); return false; }
    bool ok = true;
    int n = zip_count(kit), copied = 0;
    for (int i = 0; i < n && ok; i++) {
        if (o->cancelled && o->cancelled()) { ok = false; break; }
        ZipEntry e;
        if (!zip_entry_at(kit, i, &e)) { ok = false; break; }
        if (strncmp(e.name, "Payload/", 8) != 0) continue;
        ok = zip_add_from(w, kit, i);
        copied++;
    }
    // (the framework's folder and Info.plist are in the kit already: tools/bundle_ios.sh SKELETON=1)
    const char *fw = "Payload/LAS Recomp.app/Frameworks/LASGame.framework";
    char name[1200];
    snprintf(name, sizeof name, "%s/LASGame", fw);
    if (ok) ok = zip_add_file(w, name, lib, true);
    snprintf(name, sizeof name, "%s/LASGame.id", fw);
    if (ok) ok = zip_add_file(w, name, id, false);
    ok = zip_finish(w) && ok;
    zip_close(kit);
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
    struct stat st;
    double mb = stat(o->out_ipa, &st) == 0 ? (double)st.st_size / 1e6 : 0;
    LOG("[iosexport] %s (%.0f MB, %d entries from the kit + the library) in %.0fs\n", o->out_ipa,
        mb, copied, (double)(plat_now_ns() - t0) / 1e9);
    if (o->progress) o->progress(1, "Done");
    return true;
}

