// The start screen's "Recomp for iOS" page (runtime/ui/ios_export_ui.h).
#include "ios_export_ui.h"

#include <SDL3/SDL.h>
#include <pthread.h>
#include <stdio.h>
#include <string>

#include "i18n.h"
#include "imgui.h"
#include "option_rows.h"

extern "C" {
#include "../iosexport.h"
}

extern "C" const char *larecomp_version(void);  // ui/launcher.mm

static IosExportHost g_host;
// the picked kit and game files folder, the .ipa being written, where the last export went (under the host's lock)
static std::string g_kit, g_src, g_save, g_out;
static bool g_page_request;
static std::string g_why, g_why_for;  // why the picked kit can't be used (checked once per pick)
static bool g_can;

void ios_export_ui_init(const IosExportHost &host) { g_host = host; }

static bool possible(void) {
    g_host.lock();
    std::string kit = g_kit;
    g_host.unlock();
    if (g_why_for != kit || kit.empty()) {
        char why[512] = "";
        g_can = iosexport_available(kit.c_str(), why, sizeof why);
        g_why = why;
        g_why_for = kit;
    }
    return g_can;
}

// the dialogs' answers (SDL calls back on any thread)
static void SDLCALL picked_kit(void *, const char *const *files, int) {
    if (!files || !files[0]) return;
    g_host.lock();
    g_kit = files[0];
    g_host.unlock();
}
static void SDLCALL picked_src(void *, const char *const *files, int) {
    if (!files || !files[0]) return;
    g_host.lock();
    g_src = files[0];
    g_host.unlock();
}

static void *export_thread(void *) {
    g_host.begin_background_work();
    int cores = SDL_GetNumLogicalCPUCores(), ram_gb = SDL_GetSystemRAM() / 1024;
    g_host.lock();
    std::string kit = g_kit, src = g_src.empty() ? *g_host.game_root : g_src, out = g_save;
    g_host.unlock();
    IosExportOpts o = {g_host.progress, g_host.cancelled, cores < ram_gb - 2 ? cores : ram_gb - 2 > 1 ? ram_gb - 2 : 1,
                       kit.c_str(), out.c_str()};
    char why[512] = "";
    bool ok = iosexport_run(src.c_str(), &o, why, sizeof why);
    // (the lines stay English: the host shows them in the menus' language, as its own setup lines)
    if (ok) {
        g_host.lock();
        g_out = out;
        g_host.unlock();
        char line[1300];
        snprintf(line, sizeof line, LN("iOS app exported to %s"), out.c_str());
        g_host.set_job(line, false, "");
    } else if (g_host.cancelled()) {
        g_host.set_job(LN("Export cancelled. Completed files are kept."), false, "");
    } else {
        fprintf(stderr, "[launcher] iOS export: %s\n", why);
        char hint[700];
        snprintf(hint, sizeof hint, LN("%s. Details are in the log."), why);
        g_host.set_job(LN("The iOS app couldn't be exported"), true, hint);
    }
    g_host.end_background_work();
    g_host.lock();
    *g_host.busy = false;
    g_host.unlock();
    return NULL;
}

// The save dialog's answer (any thread): where to write the .ipa, then the export starts; cancelled: nothing
static void SDLCALL picked_save(void *, const char *const *files, int) {
    if (!files || !files[0] || !files[0][0]) return;
    std::string path = files[0];
    if (path.size() < 4 || path.compare(path.size() - 4, 4, ".ipa") != 0) path += ".ipa";
    g_host.lock();
    if (*g_host.busy) { g_host.unlock(); return; }
    g_save = path;
    *g_host.busy = true;
    g_host.unlock();
    pthread_t t;
    pthread_create(&t, NULL, export_thread, NULL);
    pthread_detach(t);
}

#ifdef _WIN32
#define SHOW_IN "Show in Explorer"
#elif defined(__linux__)
#define SHOW_IN "Open folder"
#else
#define SHOW_IN "Show in Finder"
#endif

static std::string folder_of(const std::string &path) {
    size_t sl = path.find_last_of("/\\");
    return sl == std::string::npos ? path : path.substr(0, sl);
}

void ios_export_rows(bool ready, bool busy, SDL_Window *win) {
    g_host.lock();
    std::string kit = g_kit, src = g_src, out = g_out;
    g_host.unlock();
    // (the descriptions are put together here, each part through L(): the rows translate only whole literals)
    opt_section("Files");
    char kit_name[64];
    snprintf(kit_name, sizeof kit_name, "LAS Recomp-%s-iOS.laskit", larecomp_version());
    char kit_line[512];
    snprintf(kit_line, sizeof kit_line, L("The iOS kit of this release: %s from the release page (the app without game "
                                          "code, and the tools that build it on this machine). Used as it is: don't unpack it."), kit_name);
    std::string kit_d = kit_line;
    if (!kit.empty()) kit_d += std::string("\n\n") + L("Chosen:") + " " + kit;
    if (busy) opt_disable_next();
    if (opt_action("iOS kit", kit.empty() ? "Choose..." : "Selected", kit_d.c_str(), true)) {
        static SDL_DialogFileFilter kit_filters[] = {{NULL, "laskit"}};  // (kept until the dialog answers)
        kit_filters[0].name = L("iOS kit");
        SDL_ShowOpenFileDialog(picked_kit, NULL, win, kit_filters, 1, NULL, false);
    }
    std::string src_d = L("The game files set up here, or your own copy of the game: any folder the setup would accept "
                          "(exefs and romfs in it, or their contents). Nothing is written into it.");
    if (!src.empty()) src_d += std::string("\n\n") + L("Chosen:") + " " + src;
    else if (ready) src_d += std::string("\n\n") + L("Chosen:") + " " + L("the game files set up here.");
    if (busy) opt_disable_next();
    if (opt_action("Game files folder", !src.empty() ? "Selected" : ready ? "Set up here" : "Choose...", src_d.c_str(), true))
        SDL_ShowOpenFolderDialog(picked_src, NULL, win, NULL, false);
    opt_section("Export");
    bool can = possible();
    std::string d = L("Builds the game code for iPhone / iPad and saves an unsigned iOS app (.ipa) where you choose. "
                      "Sign and install it with your own tool and Apple ID (a free developer account is enough: the app "
                      "needs the increased-memory-limit entitlement, which your tool reads from the app). On the "
                      "device, put your extracted/ folder (exefs and romfs) in the app's Documents through the Files "
                      "app or Finder.");
    bool have_src = !src.empty() || ready;
    if (kit.empty()) d += std::string("\n\n") + L("Choose the iOS kit first.");
    else if (!can) d += std::string("\n\n") + L("Not available:") + " " + L_line(g_why) + ".";
    else if (!have_src) d += std::string("\n\n") + L("Choose the game files folder first.");
    if (busy || !can || !have_src) opt_disable_next();
    if (opt_action("Export iOS app...", busy ? "Working..." : "", d.c_str(), true)) {
        static SDL_DialogFileFilter filters[] = {{NULL, "ipa"}};
        filters[0].name = L("iOS app");
        char def[1100];
        iosexport_default_path(def, sizeof def);
        SDL_ShowSaveFileDialog(picked_save, NULL, win, filters, 1, def);
    }
    if (!out.empty() && !busy) {
        size_t sl = out.find_last_of("/\\");
        if (opt_action(SHOW_IN, sl == std::string::npos ? out.c_str() : out.c_str() + sl + 1,
                       (std::string(L("The exported app:")) + " " + out).c_str(), true))
            g_host.reveal(folder_of(out));
    }
}

void ios_export_prelaunch(bool busy) {
    g_host.lock();
    std::string out = g_out;
    g_host.unlock();
    if (busy) opt_disable_next();
    if (opt_action("Recomp for iOS...", "", "Builds the game code for iPhone / iPad with the release's iOS kit and "
                   "saves an unsigned .ipa for your own signing tool and Apple ID.", true))
        g_page_request = true;
    if (!out.empty()) {
        size_t sl = out.find_last_of("/\\");
        if (opt_action(SHOW_IN, sl == std::string::npos ? out.c_str() : out.c_str() + sl + 1,
                       (std::string(L("The exported app:")) + " " + out).c_str(), true))
            g_host.reveal(folder_of(out));
    }
}

bool ios_export_page_requested(void) {
    bool r = g_page_request;
    g_page_request = false;
    return r;
}
