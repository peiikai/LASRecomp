// The start screen's Game Files page on iOS: ios_launcher.h.
#include "ios_launcher.h"

#include <SDL3/SDL.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string>

#include "option_rows.h"

extern "C" {
#include "../platform.h"
#include "../paths.h"
const char *plat_ios_documents(void);  // runtime/platform/ios_glue.m
}

#if PLAT_IOS

static IosLauncherHost g_h;
void ios_launcher_init(const IosLauncherHost &host) {
    g_h = host;
    if (getenv("LARECOMP_IOS_UNSETUP")) {  // (test) the set-up files back to a player's drop: Documents/Game files/
        std::string docs = plat_ios_documents(), drop = docs + "/Game files";
        mkdir(drop.c_str(), 0755);
        for (const char *n : {"exefs", "romfs"})
            fprintf(stderr, "[launcher] unsetup %s: %s\n", n,
                    rename((*g_h.game_root + "/" + n).c_str(), (drop + "/" + n).c_str()) == 0 ? "moved" : strerror(errno));
        g_h.game_files_changed();
    }
}

void ios_open_documents(void) {
    // (the Files app's own scheme for a file-sharing app's Documents folder)
    std::string url = std::string("shareddocuments://") + plat_ios_documents();
    if (!SDL_OpenURL(url.c_str())) fprintf(stderr, "[launcher] Files: %s\n", SDL_GetError());
}

enum { FAIL_NOT_FOUND = 1, FAIL_UNUSABLE, FAIL_COPY, FAIL_VERSION, FAIL_MISSING };  // (launcher.mm's)

static bool is_dir(const std::string &p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// The game folders anywhere in Documents but the set-up copy itself (extracted/): the player's drop. A folder with
// both wins; else the first exefs and the first complete romfs (a seed's output has a region_common of its own too)
static void find_in_documents(std::string &exefs, std::string &romfs, bool &unusable) {
    std::string docs = plat_ios_documents();
    const std::string &root = *g_h.game_root;
    DIR *d = opendir(docs.c_str());
    if (!d) return;
    std::string ex1, ro1, ro_part;  // (ro_part: an incomplete romfs, named in the failure when there is no other)
    while (struct dirent *e = readdir(d)) {
        std::string n = e->d_name;
        if (n[0] == '.' || n == "rando" || n == "save" || n == "cache") continue;  // (LAS Recomp's own)
        std::string p = docs + "/" + n;
        if (p == root || !is_dir(p)) continue;
        std::string ex, ro;
        if (n == "exefs" || n == "romfs") g_h.find_game(docs, ex, ro, unusable);  // (the two folders themselves)
        else g_h.find_game(p, ex, ro, unusable);
        if (!ex.empty() && !ro.empty()) { exefs = ex, romfs = ro; break; }
        if (ex1.empty()) ex1 = ex;
        if (!ro.empty() && ro1.empty()) {
            if (g_h.count_files(ro) >= g_h.romfs_files) ro1 = ro;
            else if (ro_part.empty()) ro_part = ro;
        }
    }
    closedir(d);
    if (exefs.empty() || romfs.empty()) exefs = ex1, romfs = ro1.empty() ? ro_part : ro1;
}

// Moved into <root>/exefs and <root>/romfs (the same volume: a rename), the old set-up copy removed first
static void *setup_thread(void *) {
    const std::string &root = *g_h.game_root;
    std::string exefs, romfs;
    bool unusable = false;
    g_h.set_job("Looking for the game files...", false, "");
    g_h.progress(0.1, "Looking for the game files");
    find_in_documents(exefs, romfs, unusable);
    if (exefs.empty() || romfs.empty()) {
        // nothing new: the set-up copy as it is (a Files-app copy straight into extracted/ counts)
        bool u = false;
        g_h.find_game(root, exefs, romfs, u);
        if (exefs.empty() || romfs.empty()) { g_h.set_failure(unusable ? FAIL_UNUSABLE : FAIL_NOT_FOUND, -1); goto done; }
        if (!g_h.check_version(exefs)) { g_h.set_failure(FAIL_VERSION, -1); goto done; }
        if (int have = g_h.count_files(romfs); have < g_h.romfs_files) { g_h.set_failure(FAIL_MISSING, have); goto done; }
        g_h.game_files_changed();
        g_h.set_job("Game files ready.", false, "");
        goto done;
    }
    if (!g_h.check_version(exefs)) { g_h.set_failure(FAIL_VERSION, -1); goto done; }
    g_h.progress(0.3, "Checking the game files");
    if (int have = g_h.count_files(romfs); have < g_h.romfs_files) {
        fprintf(stderr, "[launcher] %s: %d of %d romfs files\n", romfs.c_str(), have, g_h.romfs_files);
        g_h.set_failure(FAIL_MISSING, have);
        goto done;
    }
    g_h.progress(0.5, "Moving the game files into place");
    g_h.set_job("Moving the game files into place...", false, "");
    rt_remove_game_files(root.c_str());  // (an earlier or interrupted setup's)
    g_h.game_files_changed();
    mkdir(root.c_str(), 0755);
    if (rename(exefs.c_str(), (root + "/exefs").c_str()) != 0 || rename(romfs.c_str(), (root + "/romfs").c_str()) != 0) {
        fprintf(stderr, "[launcher] move into %s: %s\n", root.c_str(), strerror(errno));
        g_h.set_failure(FAIL_COPY, -1);
        goto done;
    }
    {   // the folder they came in, if it is empty now (a "Game files" folder with just the two)
        std::string parent = exefs.substr(0, exefs.rfind('/'));
        if (parent != plat_ios_documents()) rmdir(parent.c_str());
    }
    g_h.game_files_changed();
    g_h.set_job("Game files ready.", false, "");
done:
    g_h.end_background_work();
    g_h.lock();
    *g_h.busy = false;
    g_h.unlock();
    return NULL;
}

void ios_game_files_rows(bool busy) {
    opt_section("Your copy of the game");
    if (opt_action("Open in Files", "", "LAS Recomp's folder in the Files app (On My iPhone > LAS Recomp). Copy your own "
                   "extracted game files of The Legend of Zelda: Link's Awakening for Nintendo Switch there: the folder "
                   "with its exefs and romfs folders, or the two folders themselves - from a computer (Finder / iTunes "
                   "file sharing), AirDrop or a cloud drive. The game isn't included with LAS Recomp.", true))
        ios_open_documents();
    opt_section("Set up");
    if (busy) opt_disable_next();
    if (opt_action("Set up game files", busy ? "Working..." : "", "Finds the game files you copied into LAS Recomp's "
                   "folder, moves them into place and checks them. The game code is part of the app: nothing to build.", true)) {
        g_h.lock();
        *g_h.busy = true;
        g_h.unlock();
        g_h.begin_background_work();
        pthread_t t;
        pthread_create(&t, NULL, setup_thread, NULL);
        pthread_detach(t);
    }
}

#else
void ios_launcher_init(const IosLauncherHost &) {}
void ios_game_files_rows(bool) {}
void ios_open_documents(void) {}
#endif
