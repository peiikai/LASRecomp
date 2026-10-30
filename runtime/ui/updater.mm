#include "updater.h"
#ifdef _WIN32
#include <SDL3/SDL.h>
#include <windows.h>
#include <stdio.h>
#include <string>
#include "imgui.h"
#include "option_rows.h"
#include "update_tags.h"
extern "C" {
#include "platform.h"
#include "paths.h"
#include "settings.h"
const char *larecomp_version(void);
const char *larecomp_test_of(void);
}

static bool open_requested, install_ask;
static PlatProc process;
static std::string state, tag, message = "Check for new LAS Recomp releases on GitHub.";
static std::string kind = "idle";
static std::string last_result;
static float progress;

static std::string helper(void) {
    char path[4096];
    return rt_resource("lasupdate/lasupdate.exe", "build/helper/lasupdate.exe", path, sizeof path) ? path : "";
}

static void poll(void) {
    if (state.empty()) state = std::string(rt_data_root()) + "/cache/updater";
    // Also show the result of the detached installer after restarting, including failures.
    static Uint64 last;
    Uint64 now = SDL_GetTicks();
    if (now - last < 200) return;
    last = now;
    if (FILE *f = fopen((state + "/result.txt").c_str(), "rb")) {
        char k[32] = {}, t[128] = {}, p[32] = {}, m[1024] = {};
        if (fgets(k, sizeof k, f) && fgets(t, sizeof t, f) && fgets(p, sizeof p, f) && fgets(m, sizeof m, f)) {
            k[strcspn(k, "\r\n")] = 0; t[strcspn(t, "\r\n")] = 0; m[strcspn(m, "\r\n")] = 0;
            std::string result = std::string(k) + t + p + m;
            if (result != last_result) {
                last_result = result;
                kind = k, tag = t, message = m, progress = strtof(p, NULL);
                long long current = !strncmp(larecomp_version(), "test-", 5) ? tag_rank(larecomp_test_of()) - 1
                                                                                  : tag_rank(larecomp_version());
                if ((kind == "ready" || kind == "available") && tag_rank(tag) <= current) {
                    kind = "idle"; message = "The cached update is not newer than this build. Check for updates again.";
                }
            }
        }
        fclose(f);
    }
    if (process) {
        int code = plat_proc_wait(process, false);
        if (code >= 0) {
            process = 0;
            if (code != 0 || kind == "checking" || kind == "downloading") {
                kind = "error";
                if (code != 0 && message.empty()) message = "The update helper failed. Try again or use the release page.";
            }
        }
    }
}

static void run(const char *action) {
    std::string exe = helper();
    if (exe.empty()) { kind = "error"; message = "This build has no update helper. Use the release page."; return; }
    mkdir((std::string(rt_data_root()) + "/cache").c_str(), 0755);
    mkdir(state.c_str(), 0755);
    remove((state + "/result.txt").c_str());
    last_result.clear();
    const char *av[] = {exe.c_str(), action, "--state", state.c_str(), "--current", larecomp_version(),
                        "--preview", larecomp_test_of(), NULL};
    process = plat_spawn(av, (state + "/update.log").c_str(), false);
    kind = process ? !strcmp(action, "check") ? "checking" : "downloading" : "error";
    message = process ? !strcmp(action, "check") ? "Checking GitHub releases..." : "Downloading..."
                      : "The update helper could not be started. Use the release page.";
}

static bool install(void) {
    std::string src = helper();
    char exe[4096];
    if (src.empty() || !plat_exe_path(exe, sizeof exe)) return false;
    std::string target = exe;
    target.erase(target.find_last_of("/\\"));
    // The one-file worker runs from the data cache while its shipped copy is replaced.
    std::string worker = state + "/install-worker.exe";
    wchar_t wsrc[4096], wdest[4096];
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, src.c_str(), -1, wsrc, 4096) ||
        !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, worker.c_str(), -1, wdest, 4096) ||
        !CopyFileW(wsrc, wdest, FALSE)) return false;
    char pid[32]; snprintf(pid, sizeof pid, "%lu", GetCurrentProcessId());
    const char *av[] = {worker.c_str(), "install", "--state", state.c_str(), "--target", target.c_str(),
                        "--data-root", rt_data_root(), "--parent", pid, "--current", larecomp_version(),
                        "--preview", larecomp_test_of(), NULL};
    if (!plat_spawn_detached(av, (state + "/update.log").c_str())) return false;
    settings_save();
    SDL_Event quit = {}; quit.type = SDL_EVENT_QUIT; SDL_PushEvent(&quit);
    return true;
}

void updater_row(void) {
    if (opt_action("Updates", "Open...", "Check for new LAS Recomp releases, download an update and install it after closing the game."))
        open_requested = true;
}

void updater_draw(bool in_game) {
    static bool test_opened, test_checked;
    const char *test = getenv("LARECOMP_UPDATE_DIALOG");  // screenshot tests: open / check / ask
    if (test && !test_opened) open_requested = test_opened = true;
    if (open_requested) {
        ImGui::OpenPopup("LAS Recomp updates");
        open_requested = false;
    }
    ImGui::SetNextWindowSize(ImVec2(720, 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("LAS Recomp updates", NULL, ImGuiWindowFlags_AlwaysAutoResize)) return;
    poll();
    if (test && !test_checked) {
        test_checked = true;
        if (!strcmp(test, "check")) run("check");
        if (!strcmp(test, "ask")) install_ask = true;
    }
    ImGui::Text("Installed: %s", tag_label(larecomp_version(), false).c_str());
    if (!tag.empty()) ImGui::Text("Available: %s", tag_label(tag, false).c_str());
    ImGui::Spacing();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 670);
    ImGui::TextUnformatted(message.c_str());
    ImGui::PopTextWrapPos();
    bool busy = process != 0;
    if (busy && kind == "downloading") ImGui::ProgressBar(progress);
    ImGui::Spacing();
    if (install_ask) {
        ImGui::TextWrapped("LAS Recomp will close, install the verified update and restart. Your saves, seeds, fonts and settings are kept.");
        if (in_game) ImGui::TextWrapped("Save in the game first. Progress since your last save will be lost.");
        if (ImGui::Button("Install and restart")) {
            if (!install()) { kind = "error"; message = "Could not start the installer. The installation was not changed."; }
            install_ask = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) install_ask = false;
    } else {
        ImGui::BeginDisabled(busy);
        if (ImGui::Button("Check for updates")) run("check");
        if (kind == "available") {
            ImGui::SameLine();
            if (ImGui::Button("Download update")) run("download");
        }
        if (kind == "ready") {
            ImGui::SameLine();
            if (ImGui::Button("Install update...")) install_ask = true;
        }
        ImGui::EndDisabled();
        if (busy && ImGui::Button("Cancel download / check")) {
            plat_proc_kill(process); process = 0;
            remove((state + "/result.txt").c_str()); last_result.clear();
            kind = "idle"; message = "Cancelled. The installation was not changed.";
        }
        if (ImGui::Button("Release page")) SDL_OpenURL("https://github.com/peiikai/LASRecomp/releases");
        ImGui::SameLine();
        if (ImGui::Button("Close") || ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
            ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}
#else
void updater_row(void) {}
void updater_draw(bool) {}
#endif
