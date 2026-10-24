// The start screen's "Recomp for iOS" page (runtime/ui/ios_export_ui.h).
#include "ios_export_ui.h"

#include <SDL3/SDL.h>
#include <pthread.h>
#include <stdio.h>
#include <string>

#include "imgui.h"
#include "option_rows.h"

extern "C" {
#include "../iosexport.h"
}

extern "C" const char *larecomp_version(void);  // ui/launcher.mm

static IosExportHost g_host;
// the picked folders, the .ipa being written, where the last export went (under the host's lock)
static std::string g_skel, g_src, g_save, g_out;
static bool g_page_request;
static std::string g_why, g_why_for;  // why the picked skeleton can't be used (checked once per pick)
static bool g_can;

void ios_export_ui_init(const IosExportHost &host) { g_host = host; }

static bool possible(void) {
    g_host.lock();
    std::string skel = g_skel;
    g_host.unlock();
    if (g_why_for != skel || skel.empty()) {
        char why[512] = "";
        g_can = iosexport_available(skel.c_str(), why, sizeof why);
        g_why = why;
        g_why_for = skel;
    }
    return g_can;
}

// the folder dialogs' answers (SDL calls back on any thread)
static void SDLCALL picked_skel(void *, const char *const *files, int) {
    if (!files || !files[0]) return;
    g_host.lock();
    g_skel = files[0];
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
    std::string skel = g_skel, src = g_src.empty() ? *g_host.game_root : g_src, out = g_save;
    g_host.unlock();
    IosExportOpts o = {g_host.progress, g_host.cancelled, cores < ram_gb - 2 ? cores : ram_gb - 2 > 1 ? ram_gb - 2 : 1,
                       skel.c_str(), out.c_str()};
    char why[512] = "";
    bool ok = iosexport_run(src.c_str(), &o, why, sizeof why);
    if (ok) {
        g_host.lock();
        g_out = out;
        g_host.unlock();
        g_host.set_job("iOS app exported to " + out, false, "");
    } else if (g_host.cancelled()) {
        g_host.set_job("Export cancelled. Completed files are kept.", false, "");
    } else {
        fprintf(stderr, "[launcher] iOS export: %s\n", why);
        g_host.set_job("The iOS app couldn't be exported", true, std::string(why) + ". Details are in the log.");
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
    std::string skel = g_skel, src = g_src, out = g_out;
    g_host.unlock();
    opt_section("Folders");
    std::string skel_d = std::string("The iOS skeleton of this release, unpacked: LAS Recomp-") + larecomp_version() +
                         "-ios-skeleton.zip from the release page (the app without game code, and the tools that "
                         "build it on this machine).";
    if (!skel.empty()) skel_d += "\n\nChosen: " + skel;
    if (busy) opt_disable_next();
    if (opt_action("iOS skeleton folder", skel.empty() ? "Choose..." : "Selected", skel_d.c_str(), true))
        SDL_ShowOpenFolderDialog(picked_skel, NULL, win, NULL, false);
    std::string src_d = "The folder with the game's exefs/ (and romfs/): the game files set up here, or your own copy "
                        "of the game. The iOS build's files go to an ios/ folder inside it.";
    if (!src.empty()) src_d += "\n\nChosen: " + src;
    else if (ready) src_d += "\n\nChosen: the game files set up here.";
    if (busy) opt_disable_next();
    if (opt_action("Game files folder", !src.empty() ? "Selected" : ready ? "Set up here" : "Choose...", src_d.c_str(), true))
        SDL_ShowOpenFolderDialog(picked_src, NULL, win, NULL, false);
    opt_section("Export");
    bool can = possible();
    std::string d = "Builds the game code for iPhone / iPad and saves an unsigned iOS app (.ipa) where you choose. "
                    "Sign and install it with your own tool and Apple ID (a free developer account is enough: the app "
                    "needs the increased-memory-limit entitlement, which your tool reads from the app). On the "
                    "device, put your extracted/ folder (exefs and romfs) in the app's Documents through the Files "
                    "app or Finder.";
    bool have_src = !src.empty() || ready;
    if (skel.empty()) d += "\n\nChoose the iOS skeleton folder first.";
    else if (!can) d += "\n\nNot available: " + g_why + ".";
    else if (!have_src) d += "\n\nChoose the game files folder first.";
    if (busy || !can || !have_src) opt_disable_next();
    if (opt_action("Export iOS app...", busy ? "Working..." : "", d.c_str(), true)) {
        static const SDL_DialogFileFilter filters[] = {{"iOS app", "ipa"}};
        char def[1100];
        iosexport_default_path(def, sizeof def);
        SDL_ShowSaveFileDialog(picked_save, NULL, win, filters, 1, def);
    }
    if (!out.empty() && !busy) {
        size_t sl = out.find_last_of("/\\");
        if (opt_action(SHOW_IN, sl == std::string::npos ? out.c_str() : out.c_str() + sl + 1,
                       ("The exported app: " + out).c_str(), true))
            g_host.reveal(folder_of(out));
    }
}

void ios_export_prelaunch(bool busy) {
    g_host.lock();
    std::string out = g_out;
    g_host.unlock();
    ImGui::BeginDisabled(busy);
    if (ImGui::Button("Recomp for iOS...")) g_page_request = true;
    ImGui::EndDisabled();
    if (!out.empty()) {
        ImGui::SameLine();
        if (ImGui::Button(SHOW_IN "##ios")) g_host.reveal(folder_of(out));
    }
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Builds the game code for iPhone / iPad with the release's iOS skeleton and saves an "
                        "unsigned .ipa for your own signing tool and Apple ID.");
    ImGui::PopTextWrapPos();
}

bool ios_export_page_requested(void) {
    bool r = g_page_request;
    g_page_request = false;
    return r;
}
