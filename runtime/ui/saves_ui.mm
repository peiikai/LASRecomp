// The start screen's saves rows and windows (runtime/ui/saves_ui.h).
#include "saves_ui.h"

#include <SDL3/SDL.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <string>
#include <vector>

#include "imgui.h"
#include "i18n.h"
#include "option_rows.h"
#include "plastic.h"
#include "../i18n_mark.h"
#include "../platform.h"
#include "../platform/ios_sdl.h"  // (iOS: SDL's file dialogs as the Files app's)
#include "../paths.h"
#include "../saves.h"
#ifdef __ANDROID__
#include "../platform/android_storage.h"
#endif

static SavesUiHost g_h;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;  // (pickers and threads call back on any thread)

enum { S_NONE, S_WORKING, S_IMPORT_ASK, S_MESSAGE };
static int g_state;                      // (main thread)
static bool g_open;                      // the popup is open (main thread)
static SavesInfo g_info;                 // the import's bundle
static std::vector<char> g_take;         // the import's ticks, one per game of g_info
static std::string g_import_path;        // the bundle being imported
static std::string g_msg;                // S_MESSAGE's text (English, for L_line)
static bool g_msg_bad;

// filled by callbacks / threads (g_mu), taken by saves_popups
static std::string g_picked_import, g_picked_export, g_done_msg;
static bool g_has_import, g_has_export, g_has_done, g_done_bad;
static bool g_bg;                        // a thread is working (g_mu): rows wait

static void finish(const char *msg, bool bad) {  // (any thread) the work's end: a message, or none
    pthread_mutex_lock(&g_mu);
    g_bg = false;
    if (msg) g_done_msg = msg, g_done_bad = bad, g_has_done = true;
    else g_has_done = true, g_done_msg.clear();
    pthread_mutex_unlock(&g_mu);
}
static bool run(void *(*fn)(void *)) {
    pthread_mutex_lock(&g_mu);
    g_bg = true;
    pthread_mutex_unlock(&g_mu);
    pthread_t th;
    if (pthread_create(&th, NULL, fn, NULL) != 0) { finish(LN("Couldn't start."), true); return false; }
    pthread_detach(th);
    return true;
}

static void got_import(const char *path);

void saves_ui_init(const SavesUiHost &host) {
    static bool done;
    g_h = host;
    saves_app_version(host.app_version);
    if (done) return;
    done = true;
    const char *t = getenv("LARECOMP_SAVES_IMPORT");  // (test: a saves file as if picked for Import saves)
    if (t && *t) got_import(t);
}

// ---- export
static std::string export_name(void) {
    time_t now = time(NULL);
    struct tm tm;
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    char b[64];
    snprintf(b, sizeof b, "LAS Recomp saves %04d-%02d-%02d.zip", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return b;
}
#if defined(__ANDROID__) || PLAT_IOS
// Mobile: written in the cache first, then the system's own picker puts a copy where the player chooses
static std::string g_export_tmp;
static void exported(int r) {  // (any thread)
    remove(g_export_tmp.c_str());
    finish(r > 0 ? LN("Saves exported.") : r < 0 ? LN("Couldn't save the file there.") : NULL, r < 0);
}
static void *export_thread(void *) {
    char err[200];
    mkdir("cache", 0755);
    g_export_tmp = std::string(rt_data_root()) + "/cache/" + export_name();
    if (!saves_export(g_export_tmp.c_str(), NULL, err, sizeof err)) { finish(err, true); return NULL; }
#ifdef __ANDROID__
    if (!android_export_saves(g_export_tmp.c_str(), export_name().c_str(), exported)) exported(-1);
#else
    std::string *p = new std::string(g_export_tmp);  // (the picker shows on the main thread)
    SDL_RunOnMainThread([](void *u) {
        std::string *s = (std::string *)u;
        ios_export_file(*g_h.win, s->c_str(), exported);
        delete s;
    }, p, false);
#endif
    return NULL;
}
static void start_export(void) { run(export_thread); }
#else
static void SDLCALL picked_export(void *, const char *const *files, int) {
    pthread_mutex_lock(&g_mu);
    if (files && files[0]) g_picked_export = files[0], g_has_export = true;
    pthread_mutex_unlock(&g_mu);
}
static void *export_thread(void *) {
    pthread_mutex_lock(&g_mu);
    std::string to = g_picked_export;
    pthread_mutex_unlock(&g_mu);
    size_t k = to.size();
    struct stat st;  // (".zip" added only where it replaces nothing: the dialog asked about the name without it)
    if ((k < 4 || SDL_strcasecmp(to.c_str() + k - 4, ".zip")) && stat((to + ".zip").c_str(), &st) != 0) to += ".zip";
    char err[200];
    bool ok = saves_export(to.c_str(), NULL, err, sizeof err);
    finish(ok ? LN("Saves exported.") : err, !ok);
    return NULL;
}
static void start_export(void) {
    static SDL_DialogFileFilter f[] = {{"Zip", "zip"}};
    std::string def = export_name();
    SDL_ShowSaveFileDialog(picked_export, NULL, *g_h.win, f, 1, def.c_str());
}
#endif

// ---- import
static void got_import(const char *path) {  // (any thread) "" / NULL: nothing picked
    pthread_mutex_lock(&g_mu);
    if (path && *path) g_picked_import = path, g_has_import = true;
    pthread_mutex_unlock(&g_mu);
}
#ifdef __ANDROID__
static void picked_import_android(const char *path) {
    if (!path) finish(LN("The file picker couldn't open."), true);
    got_import(path);
}
#else
static void SDLCALL picked_import(void *, const char *const *files, int) { got_import(files ? files[0] : NULL); }
#endif
static void start_import(void) {
#ifdef __ANDROID__
    if (!android_pick_saves(picked_import_android)) finish(LN("The file picker couldn't open."), true);
#else
    static SDL_DialogFileFilter f[] = {{"Zip", "zip"}};
    SDL_ShowOpenFileDialog(picked_import, NULL, *g_h.win, f, 1, NULL, false);
#endif
}
// Android: the picked file is a copy in the app's cache (LASRecompActivity's PICK_SAVES), removed once it's used
static void drop_picked(void) {
#ifdef __ANDROID__
    static const char tail[] = "/picked-saves.zip";
    size_t k = g_import_path.size(), m = sizeof tail - 1;
    if (k > m && !g_import_path.compare(k - m, m, tail)) remove(g_import_path.c_str());
#endif
}
static void *import_thread(void *) {
    std::vector<unsigned> take(g_info.n ? g_info.n : 1, 0);
    for (int i = 0; i < g_info.n; i++)
        if (g_take[i]) take[i] = g_info.g[i].slots;
    char err[200];
    bool ok = saves_import(g_import_path.c_str(), &g_info, take.data(), err, sizeof err);
    drop_picked();
    finish(ok ? LN("Saves imported.") : err, !ok);
    return NULL;
}

void saves_rows(bool busy) {
    pthread_mutex_lock(&g_mu);
    bool bg = g_bg;
    pthread_mutex_unlock(&g_mu);
    bool off = busy || bg || g_state != S_NONE;
    opt_section("Saves");
    static bool any;  // (looked up once a second, not every frame: it reads the save folders)
    static Uint64 any_at;
    if (!any_at || SDL_GetTicks() - any_at > 1000) any = saves_any(), any_at = SDL_GetTicks();
    if (off || !any) opt_disable_next();
    if (opt_action("Export saves", "Export...", "All your saves and the seeds they use, in one file.")) start_export();
    if (off) opt_disable_next();
    if (opt_action("Import saves", "Import...", "Saves from a file made with Export saves. The saves it replaces are "
                   "backed up first."))
        start_import();
#ifndef __ANDROID__  // (Android: other apps can't open the app's own folder)
#if PLAT_IOS
    const char *show = LN("Open in Files");
#elif defined(_WIN32)
    const char *show = LN("Show in Explorer");
#elif defined(__linux__)
    const char *show = LN("Open folder");
#else
    const char *show = LN("Show in Finder");
#endif
    if (opt_action("Saves folder", L(show), "Opens the folder with your saves.") && g_h.reveal) g_h.reveal(rt_data_root());
#endif
}

static const char *game_name(const SavesGame &g) { return g.seed[0] ? g.seed : L("Vanilla"); }
static std::string slot_list(unsigned slots) {
    std::string s;
    for (int i = 0; i < SAVES_SLOTS; i++)
        if (slots >> i & 1) s += (s.empty() ? "" : ", ") + std::string(L(saves_slot_name(i)));
    return s;
}

void saves_popups(float s) {
    // what the callbacks and threads left
    pthread_mutex_lock(&g_mu);
    if (g_has_done) {
        g_has_done = false;
        if (!g_done_msg.empty()) g_msg = g_done_msg, g_msg_bad = g_done_bad, g_state = S_MESSAGE;
        else if (g_state == S_WORKING) g_state = S_NONE;
    }
    if (g_has_export && g_state == S_NONE) { g_has_export = false; pthread_mutex_unlock(&g_mu); run(export_thread); pthread_mutex_lock(&g_mu); g_state = S_WORKING; }
    if (g_has_import && g_state == S_NONE) {
        g_has_import = false;
        g_import_path = g_picked_import;
        pthread_mutex_unlock(&g_mu);
        char err[200];
        if (saves_inspect(g_import_path.c_str(), &g_info, err, sizeof err)) {
            g_take.assign(g_info.n, 0);
            for (int i = 0; i < g_info.n; i++) g_take[i] = !g_info.g[i].problem;
            g_state = S_IMPORT_ASK;
        } else g_msg = err, g_msg_bad = true, g_state = S_MESSAGE, drop_picked();
        pthread_mutex_lock(&g_mu);
    }
    pthread_mutex_unlock(&g_mu);
    if (g_state == S_NONE || g_state == S_WORKING) {
        if (g_open && g_state == S_NONE) g_open = false;
        if (g_state == S_NONE) return;
    }

    float sc = s * 720.0f / 1080.0f;
    if (!g_open) ImGui::OpenPopup("##saves"), g_open = true;
    ImVec2 ds = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(ds.x * 0.5f, ds.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("##saves", NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        g_open = false;
        return;
    }
    const ImVec4 red = plastic_col(plastic_theme().warn), gold(0.93f, 0.76f, 0.33f, 1);
    ImVec2 bw(240 * sc, 0);
    ImGui::PushFont(*g_h.font_small);
    ImGui::PushTextWrapPos(900 * sc);
    bool esc = ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    int next = g_state;
    auto title = [&](const char *t) {
        ImGui::PushFont(*g_h.font_menu);
        ImGui::TextColored(gold, "%s", t);
        ImGui::PopFont();
    };
    if (g_state == S_WORKING) {
        title(L("Saves"));
        ImGui::TextUnformatted(L("Working..."));
    } else if (g_state == S_MESSAGE) {
        title(L("Saves"));
        if (g_msg_bad) ImGui::TextColored(red, "%s", L_line(g_msg.c_str()).c_str());
        else ImGui::TextUnformatted(L_line(g_msg.c_str()).c_str());
        ImGui::Spacing();
        ImGui::SetItemDefaultFocus();
        if (ImGui::Button(L("OK"), bw) || esc) next = S_NONE;
    } else if (g_state == S_IMPORT_ASK) {
        title(L("Import saves"));
        bool any = false;
        // (many seeds: the list scrolls, so the buttons stay on screen)
        const ImGuiStyle &st = ImGui::GetStyle();
        float list_w = 0, list_h = SDL_min(g_info.n * ImGui::GetFrameHeightWithSpacing(), 420 * sc);
        for (int i = 0; i < g_info.n; i++) {
            const SavesGame &g = g_info.g[i];
            const char *what = g.problem ? L(g.problem) : NULL;
            std::string slots = what ? "" : slot_list(g.slots);
            list_w = SDL_max(list_w, ImGui::GetFrameHeight() + st.ItemInnerSpacing.x + ImGui::CalcTextSize(game_name(g)).x +
                                         st.ItemSpacing.x + ImGui::CalcTextSize(what ? what : slots.c_str()).x);
        }
        ImGui::BeginChild("##games", ImVec2(list_w + st.ScrollbarSize + st.ItemSpacing.x, list_h));
        for (int i = 0; i < g_info.n; i++) {
            const SavesGame &g = g_info.g[i];
            ImGui::PushID(i);
            ImGui::BeginDisabled(g.problem != NULL);
            bool on = g_take[i];
            if (ImGui::Checkbox(game_name(g), &on)) g_take[i] = on;
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (g.problem) ImGui::TextColored(red, "%s", L(g.problem));
            else ImGui::TextDisabled("%s", slot_list(g.slots).c_str());
            ImGui::PopID();
            any |= g_take[i] && !g.problem;
        }
        ImGui::EndChild();
        ImGui::Spacing();
        ImGui::TextUnformatted(L("These saves replace the ones you have. A backup goes to the backups folder."));
        ImGui::Spacing();
        ImGui::BeginDisabled(!any);
        if (ImGui::Button(L("Import"), bw) && any) next = S_WORKING, run(import_thread);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::SetItemDefaultFocus();
        if (ImGui::Button(L("Cancel"), bw) || esc) next = S_NONE, drop_picked();
    }
    ImGui::PopTextWrapPos();
    ImGui::PopFont();
    if (next == S_NONE) ImGui::CloseCurrentPopup(), g_open = false;
    g_state = next;
    ImGui::EndPopup();
}
