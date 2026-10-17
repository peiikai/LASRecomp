#include "imgui.h"
#include "../save_backup.h"
#include "../gamelib.h"
#include <filesystem>
#include <algorithm>
#include <string>
#include <vector>
extern "C" {
#include "../settings.h"
void save_editor_draw(float scale);
int game_in_play(void);
}

extern "C" void save_backups_draw(float scale, bool in_game) {
    static bool editor;
    if (ImGui::RadioButton("Backups", !editor)) editor = false;
    ImGui::SameLine();
    ImGui::BeginDisabled(!in_game || !game_in_play());
    if (ImGui::RadioButton("Save Editor", editor)) editor = true;
    ImGui::EndDisabled();
    if (editor && in_game && game_in_play()) { save_editor_draw(scale); return; }
    editor = false;
    ImGui::TextWrapped("%s", "Keeps 20 recent backups, plus all Before restore snapshots. Times are UTC.");
    bool locked = gamelib_tables() != NULL;
    if (locked) ImGui::TextWrapped("%s", "Restart to create or restore backups.");
    static std::string profile, selected, message;
    static bool confirmed;
    static int seen = -10;
    static double refresh_at;
    static std::vector<SaveBackupInfo> items(SAVE_BACKUP_KEEP);
    static int count;
    static std::vector<std::string> profiles;
    if (seen != ImGui::GetFrameCount() - 1) {
        profile = g_settings.rando_seed; selected.clear(); confirmed = false; refresh_at = 0;
    }
    if (refresh_at == 0) {
        profiles = {""};
        std::error_code ec;
        if (!getenv("LARECOMP_SAVE_DIR"))
            for (auto &e : std::filesystem::directory_iterator("rando", ec))
                if (e.is_directory(ec) && !e.is_symlink(ec)) profiles.push_back(e.path().filename().u8string());
    }
    if (!profile.empty() && std::find(profiles.begin(), profiles.end(), profile) == profiles.end()) profile.clear();
    seen = ImGui::GetFrameCount();
    if (getenv("LARECOMP_SAVE_DIR")) profile.clear();
    ImGui::SetNextItemWidth(320 * scale);
    if (ImGui::BeginCombo("Save profile", profile.empty() ? "Vanilla" : profile.c_str())) {
        for (auto &p : profiles)
            if (ImGui::Selectable(p.empty() ? "Vanilla" : p.c_str(), profile == p)) {
                profile = p; selected.clear(); confirmed = false; refresh_at = 0; message.clear();
            }
        ImGui::EndCombo();
    }
    std::string base = getenv("LARECOMP_SAVE_DIR") ? getenv("LARECOMP_SAVE_DIR") :
                       profile.empty() ? "save" : "rando/" + profile + "/save";
    static int area;
    static std::vector<int> areas;
    if (refresh_at == 0) {
        areas = {0};
        for (int i = 1; i <= 255; ++i) {
            std::error_code ec;
            auto path = std::filesystem::u8path(base + "/save_" + std::to_string(i));
            if (std::filesystem::exists(path, ec) || std::filesystem::exists(path.u8string() + ".backups", ec)) areas.push_back(i);
        }
        if (std::find(areas.begin(), areas.end(), area) == areas.end()) area = 0;
    }
    if (areas.size() > 1) {
        char label[16]; snprintf(label, sizeof label, "%d", area);
        ImGui::SameLine(); ImGui::SetNextItemWidth(100 * scale);
        if (ImGui::BeginCombo("Save area", label)) {
            for (int a : areas) {
                snprintf(label, sizeof label, "%d", a);
                if (ImGui::Selectable(label, area == a)) { area = a; refresh_at = 0; selected.clear(); confirmed = false; }
            }
            ImGui::EndCombo();
        }
    } else area = 0;
    std::string root = base + "/save_" + std::to_string(area);
    if (ImGui::Button("Refresh")) refresh_at = 0;
    ImGui::SameLine();
    ImGui::BeginDisabled(locked);
    if (ImGui::Button("Create backup")) {
        char why[512];
        bool ok = save_backup_create(root.c_str(), "Manual", false, why, sizeof why);
        message = ok ? "Backup created." : why; refresh_at = 0;
    }
    ImGui::EndDisabled();
    if (ImGui::GetTime() >= refresh_at) {
        char why[512];
        for (;;) { // Before restore snapshots do not consume the ordinary retention limit.
            count = save_backup_list(root.c_str(), items.data(), (int)items.size(), why, sizeof why);
            if (count < (int)items.size()) break;
            items.resize(items.size() * 2);
        }
        if (count < 0) message = why;
        refresh_at = ImGui::GetTime() + 5;
    }
    ImGui::Spacing();
    if (count <= 0) ImGui::TextWrapped("%s", "No backups yet. Existing saves are backed up on load and after saving.");
    for (int i = 0; i < count; ++i) {
        auto &b = items[i];
        char row[256];
        snprintf(row, sizeof row, "%s  |  %s  |  %.0f KB###%s", b.id, b.reason, b.bytes / 1024.0, b.id);
        if (ImGui::Selectable(row, selected == b.id)) { selected = b.id; confirmed = false; }
    }
    ImGui::Spacing();
    ImGui::BeginDisabled(locked || selected.empty());
    if (!confirmed) {
        if (ImGui::Button("Restore selected backup")) confirmed = true;
    } else {
        ImGui::TextWrapped("%s", "Restore all slots? Current progress is backed up first.");
        if (ImGui::Button("Confirm restore")) {
            char why[512];
            bool ok = save_backup_restore(root.c_str(), selected.c_str(), why, sizeof why);
            message = ok ? "Backup restored. You can now start the game." : why;
            confirmed = false; refresh_at = 0;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) confirmed = false;
    }
    ImGui::EndDisabled();
    if (!message.empty()) ImGui::TextWrapped("%s", message.c_str());
}
