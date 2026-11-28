#include "../achievements.h"
#include "../game_addresses.h"
#include "../paths.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "i18n.h"
#include "plastic.h"
#include "../menu_sfx.h"
#include "../settings.h"
#include <algorithm>
#include <chrono>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <initializer_list>
#include <mutex>
extern "C" {
#include "../hle.h"
int game_in_play(void);
const char *rando_seed_dir(void);
}

static achievements::Tracker tracker;
static bool initialized;
static std::mutex read_lock;
static std::deque<std::array<uint8_t, achievements::SaveSize>> reads;
static std::deque<size_t> pending;
static double clock_prev, elapsed, next_sample;
static std::string last_error;
static double now_s() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Reads happen on a guest thread; consume their snapshots on the render thread only when they enter live memory.
extern "C" void achievements_save_read(uint64_t addr, int64_t off, int64_t size) {
    if (off || size < (int64_t)achievements::SaveSize) return;
    std::array<uint8_t, achievements::SaveSize> save;
    memcpy(save.data(), (const void *)(uintptr_t)addr, save.size());
    std::lock_guard<std::mutex> lock(read_lock);
    if (reads.size() == 16) reads.pop_front();
    reads.push_back(save);
}
RT_ON_RESET(achievements_reset) {
    tracker.hold_import(false);  // (a load follows: the import may run)
    tracker.baseline(); pending.clear(); elapsed = clock_prev = next_sample = 0;
    std::lock_guard<std::mutex> lock(read_lock);
    reads.clear();
}

static void open_history() {
    if (!initialized) {
        const char *override_path = getenv("LARECOMP_ACHIEVEMENTS_FILE");
        std::string path = override_path ? override_path : std::string(rt_data_root()) + "/achievements-las-v1.log";
        if (!tracker.open(path)) LOG("[achievements] %s\n", tracker.error().c_str());
        initialized = true;
    }
}

// The popup's timeline (seconds): fade and badge in, widen, read, narrow, fade out.
static const float T_BADGE = .25f, T_WIDEN = .6f, T_TEXT = .85f, T_NARROW = 3.4f, T_FADE = 3.75f, T_END = 4.1f;

extern "C" bool achievements_overlay_wanted(bool menu_open, bool kbd) {
    double now = now_s(), dt = clock_prev ? now - clock_prev : 0;
    clock_prev = now;
    // The first set describes the vanilla game's collection totals.
    if (rando_seed_dir()) {
        tracker.baseline(); pending.clear(); elapsed = 0;
        return false;
    }
    if (!game_in_play()) {
        tracker.hold_import(false);  // (the next save loaded may be imported)
        tracker.baseline();
        return false;  // keep earned notifications queued across a level transition
    }
    open_history();
    // Our menu holds the save editor: whatever changes while it is open is not earned. Reopen the baseline after it.
    if (menu_open) tracker.baseline();
    else if (now >= next_sample) {
        next_sample = now + .1;
        std::array<uint8_t, achievements::SaveSize> save;
        memcpy(save.data(), (const void *)(uintptr_t)GA(LIVE_SAVE), save.size());
        {
            std::lock_guard<std::mutex> lock(read_lock);
            for (auto it = reads.begin(); it != reads.end();) {
                // Ignore file-select previews until their UUID, CRC and inventory have become the live save.
                bool same_progress = true;
                for (const auto &rule : achievements::catalog())
                    same_progress &= achievements::progress(rule, it->data()) == achievements::progress(rule, save.data());
                if (same_progress && !memcmp(it->data() + 8, save.data() + 8, 16) &&
                    !memcmp(it->data() + 0x5ec4, save.data() + 0x5ec4, 4)) {
                    tracker.hold_import(false);  // (a save file was loaded)
                    tracker.baseline();
                    it = reads.erase(it);
                } else ++it;
            }
        }
        for (size_t id : tracker.sample(save.data(), save.size())) {
            pending.push_back(id);
            LOG("[achievements] unlocked %s\n", achievements::catalog()[id].id);
        }
        if (tracker.error() != last_error) {
            last_error = tracker.error();
            if (!last_error.empty()) LOG("[achievements] %s\n", last_error.c_str());
        }
    }
    // Popups are off by default (vanilla): unlocks are still recorded and listed in the menu.
    if (!g_settings.achievement_popups) { pending.clear(); elapsed = 0; return false; }
    if (menu_open || kbd || pending.empty()) return false;
    elapsed += std::min(dt, .1);  // menus and a suspended window do not consume the popup's reading time
    if (elapsed >= T_END) { pending.pop_front(); elapsed = 0; }
    return !pending.empty();
}

using achievements::Badge;

static ImU32 badge_color(Badge badge, int alpha = 255) {
    switch (badge) {
    case Badge::Heart: return IM_COL32(255, 160, 167, alpha);
    case Badge::Shell: case Badge::Rapids: return IM_COL32(146, 229, 208, alpha);
    case Badge::Figurine: case Badge::Prize: return IM_COL32(255, 213, 132, alpha);
    case Badge::Music: case Badge::Moon: return IM_COL32(211, 184, 252, alpha);
    case Badge::Fish: case Badge::Bottle: return IM_COL32(161, 218, 253, alpha);
    case Badge::Trade: case Badge::Upgrade: return IM_COL32(255, 188, 145, alpha);
    case Badge::Dungeon: return IM_COL32(174, 226, 159, alpha);
    default: return IM_COL32(209, 230, 223, alpha);
    }
}

static void draw_badge(ImDrawList *draw, Badge badge, ImVec2 center, float size, ImU32 color) {
    // The glyphs share a 32-unit drawing area so they scale with both the list and the popup.
    float unit = size / 32.f;
    if (badge == Badge::Sword) unit *= 1.75f;
    float stroke = std::max(1.f, 2.f * unit);
    auto point = [&](float x, float y) { return ImVec2(center.x + x * unit, center.y + y * unit); };
    auto line = [&](float x, float y, float end_x, float end_y) {
        draw->AddLine(point(x, y), point(end_x, end_y), color, stroke);
    };
    auto outline = [&](std::initializer_list<ImVec2> points, bool closed = false) {
        for (ImVec2 p : points) draw->PathLineTo(point(p.x, p.y));
        draw->PathStroke(color, closed ? ImDrawFlags_Closed : ImDrawFlags_None, stroke);
    };
    switch (badge) {
    case Badge::Heart:
        draw->PathLineTo(point(0, 12));
        draw->PathBezierCubicCurveTo(point(-28, -6), point(-8, -21), point(0, -8));
        draw->PathBezierCubicCurveTo(point(8, -21), point(28, -6), point(0, 12));
        draw->PathStroke(color, ImDrawFlags_Closed, stroke);
        break;
    case Badge::Shell:
        draw->PathArcTo(point(0,-3), 15 * unit, 3.4f, 6.02f, 24);
        draw->PathLineTo(point(5,13));
        draw->PathLineTo(point(-5,13));
        draw->PathStroke(color, ImDrawFlags_Closed, stroke);
        for (int i = -1; i <= 1; ++i) line(i * 3.f, 10, i * 9.f, -13);
        break;
    case Badge::Figurine:
        draw->AddCircle(point(0, -8), 4 * unit, color, 0, stroke);
        outline({{-7, 6}, {-5, 0}, {5, 0}, {7, 6}});
        draw->AddRect(point(-11, 6), point(11, 12), color, 2 * unit, 0, stroke);
        break;
    case Badge::Music:
        outline({{-7, 8}, {-7, -9}, {9, -13}, {9, 4}});
        draw->AddCircleFilled(point(-10, 8), 4 * unit, color);
        draw->AddCircleFilled(point(6, 4), 4 * unit, color);
        break;
    case Badge::Sword:
        outline({{-8, 4}, {5, -11}, {12, -12}, {11, -5}, {-4, 8}}, true);
        line(-12, 0, 0, 12);
        line(-6, 6, -12, 12);
        break;
    case Badge::Shield:
        outline({{-11,-9}, {0,-13}, {11,-9}, {10,3}, {6,9}, {0,13}, {-6,9}, {-10,3}}, true);
        line(0, -7, 0, 6);
        line(-5, -1, 5, -1);
        break;
    case Badge::Trade:
        outline({{-12, -5}, {11, -5}, {5, -11}});
        outline({{12, 5}, {-11, 5}, {-5, 11}});
        break;
    case Badge::Upgrade:
        outline({{-10, 1}, {0, -9}, {10, 1}});
        outline({{-10, 9}, {0, -1}, {10, 9}});
        break;
    case Badge::Bottle:
        outline({{-5,-12}, {5,-12}, {5,-4}, {10,1}, {10,12}, {-10,12}, {-10,1}, {-5,-4}}, true);
        line(-9, 4, 9, 4);
        break;
    case Badge::Dungeon:
        outline({{-12,12}, {-12,-4}, {-8,-10}, {8,-10}, {12,-4}, {12,12}}, true);
        outline({{-5,12}, {-5,1}, {0,-3}, {5,1}, {5,12}});
        break;
    case Badge::Fish:
        outline({{-14,0}, {-8,-6}, {-1,-7}, {6,-2}, {13,-7}, {13,7},
                 {6,2}, {-1,7}, {-8,6}}, true);
        draw->AddCircleFilled(point(-8, -1), 1.4f * unit, color);
        break;
    case Badge::Prize:
        outline({{-8,-11}, {8,-11}, {7,-1}, {3,3}, {-3,3}, {-7,-1}}, true);
        outline({{-8,-8}, {-13,-8}, {-12,-3}, {-7,-1}});
        outline({{8,-8}, {13,-8}, {12,-3}, {7,-1}});
        line(0, 3, 0, 10);
        line(-7, 11, 7, 11);
        break;
    case Badge::Rapids:
        for (int y = -8; y <= 8; y += 8)
            outline({{-13,(float)y}, {-7,y-3.f}, {0,y+3.f}, {7,y-3.f}, {13,(float)y}});
        break;
    case Badge::Moon:
        draw->PathArcTo(center, 12 * unit, 1.1f, 4.95f, 24);
        draw->PathBezierCubicCurveTo(point(-6, -5), point(-4, 8), point(5.44f, 10.69f));
        draw->PathStroke(color, ImDrawFlags_Closed, stroke);
        break;
    case Badge::Hidden:
        draw->PathArcTo(point(0,-2), 6 * unit, 3.14159f, 6.28319f, 16);
        draw->PathStroke(color, ImDrawFlags_None, stroke);
        draw->AddRect(point(-9,-2), point(9,11), color, 2 * unit, 0, stroke);
        line(0, 3, 0, 6);
        break;
    }
}

static float ease(float t) { t = std::max(0.f, std::min(1.f, t)); return t * t * (3 - 2 * t); }
static void erase_text(ImDrawList *dl, ImFont *font, float size, ImVec2 pos, ImU32 color,
                       const char *text, float edge) {
    // The catalog uses ASCII: drop a whole letter when the collapsing inner edge touches its advance.
    for (const char *p = text; *p; ++p) {
        // CalcTextSizeA rounds each result up; rounding each letter would widen the line and erase its end early.
        const ImFontGlyph *glyph = font->FindGlyph((ImWchar)(unsigned char)*p);
        float advance = (glyph ? glyph->AdvanceX : font->FallbackAdvanceX) * size / font->FontSize;
        if (pos.x + advance >= edge) break;
        dl->AddText(font, size, pos, color, p, p + 1);
        pos.x += advance;
    }
}
extern "C" void achievements_overlay_draw(ImVec2 display, ImFont *font, float scale) {
    if (pending.empty()) return;
    const auto &rule = achievements::catalog()[pending.front()];
    float u = scale * 1.25f, t = (float)elapsed;
    float title = 24*u, detail = 17*u, label = 13*u;
    auto width = [&](float s, const char *text) { return font->CalcTextSizeA(s, 10000, 0, text).x; };
    const char *heading = "ACHIEVEMENT UNLOCKED";
    float text_width = std::max(width(label, heading), std::max(width(title, rule.title), width(detail, rule.description)));
    float full = 94*u + text_width + 24*u;
    if (full > display.x - 24*u) {
        float ratio = (display.x - 24*u) / full;
        u *= ratio; title *= ratio; detail *= ratio; label *= ratio; full *= ratio;
    }
    float diameter = 80*u;
    float expand = t < T_WIDEN ? 0 : t < T_TEXT ? ease((t-T_WIDEN)/(T_TEXT-T_WIDEN)) : t < T_NARROW ? 1
                 : 1-ease((t-T_NARROW)/(T_FADE-T_NARROW));
    float w = diameter + (full-diameter)*expand;
    float alpha = t < .2f ? ease(t/.2f) : t < T_FADE ? 1 : 1-ease((t-T_FADE)/(T_END-T_FADE));
    float x=(display.x-w)*.5f, y=display.y-124*u;
    // In the menu's theme, as the rest of our UI.
    const PlasticTheme &th = plastic_theme();
    auto fade = [&](ImU32 c, float a = 1.f) {
        ImVec4 v = ImGui::ColorConvertU32ToFloat4(c); v.w *= alpha * a; return ImGui::ColorConvertFloat4ToU32(v);
    };
    auto fade4 = [&](ImVec4 v, float a = 1.f) { v.w *= alpha * a; return ImGui::ColorConvertFloat4ToU32(v); };
    ImDrawList *dl=ImGui::GetForegroundDrawList();
    float r = diameter*.5f;
    dl->AddRectFilled(ImVec2(x+2*u,y+3*u),ImVec2(x+w+2*u,y+diameter+3*u),IM_COL32(0,0,0,(int)(60*alpha)),r);
    dl->AddRectFilled(ImVec2(x,y),ImVec2(x+w,y+diameter),fade4(th.panel),r);
    dl->AddRect(ImVec2(x,y),ImVec2(x+w,y+diameter),fade4(th.ring_sel),r,0,2.5f*u);
    ImVec2 center(x+40*u,y+40*u);
    dl->AddCircleFilled(center,31*u,fade4(th.inset));
    float angle = t < T_TEXT ? t*9 : T_TEXT*9 + (t < T_NARROW ? 0 : (t-T_NARROW)*9);
    for(int j=0;j<2;++j) {
        dl->PathArcTo(center,34.5f*u,angle+j*3.14159f,angle+j*3.14159f+2.3f,40);
        dl->PathStroke(fade4(th.ring_sel),0,3*u);
    }
    if (t > T_BADGE * .5f) {
        float reveal_badge = ease((t - T_BADGE * .5f) / T_BADGE);
        draw_badge(dl, rule.badge, center, 32 * u, fade(th.title, reveal_badge));
    }
    if(t<T_TEXT-.15f) return;
    float reveal=ease((t-(T_TEXT-.15f))/.3f), edge=x+w-18*u, tx=x+94*u;
    erase_text(dl,font,label,ImVec2(tx,y+9*u),fade(th.section,reveal),heading,edge);
    erase_text(dl,font,title,ImVec2(tx,y+25*u),fade(th.title,reveal),rule.title,edge);
    erase_text(dl,font,detail,ImVec2(tx,y+52*u),fade(th.text_dim,reveal),rule.description,edge);
}

// The page's reset corner: every unlock forgotten, the next save opened imported again.
extern "C" void achievements_clear_all(void) {
    open_history();
    pending.clear(), elapsed = 0;
    tracker.hold_import(true);  // (not the save already open: the next one loaded)
    if (tracker.reset()) LOG("[achievements] reset\n");
    else LOG("[achievements] reset failed: %s\n", tracker.error().c_str());
}

// The page: filter tabs at the top (All / Unlocked / Locked; L / R or Q / E switch them), the total, then one card per
// achievement. Every card takes the controller's cursor, so Up / Down walk the list and the page scrolls with it.
extern "C" void achievements_menu_draw(void) {
    open_history();
    const auto &rules = achievements::catalog();
    const PlasticTheme &pt = plastic_theme();
    float scale = ImGui::GetFontSize() / 34, fs = ImGui::GetFontSize();
    unsigned total = (unsigned)rules.size(), earned = 0;
    for (size_t i = 0; i < rules.size(); ++i) earned += tracker.unlocked(i);

    enum { All, Unlocked, Locked, N_FILTERS };
    static const char *names[N_FILTERS] = {LN("All"), LN("Unlocked"), LN("Locked")};
    static int filter = All;
    static int drawn_frame = -10;
    bool opened = ImGui::GetFrameCount() - drawn_frame > 1;  // (the page just came up: the cursor onto the tabs)
    drawn_frame = ImGui::GetFrameCount();
    int previous_filter = filter;
    bool busy = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
    if (!busy) {
        int step = ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) || ImGui::IsKeyPressed(ImGuiKey_E, false) ? 1
                 : ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) || ImGui::IsKeyPressed(ImGuiKey_Q, false) ? -1 : 0;
        if (step && filter + step >= 0 && filter + step < N_FILTERS) filter += step, menu_sfx_play(MENU_SFX_TAB);
    }

    // the tabs, in the Settings tabs' look
    float gap = 8 * scale, pad = 20 * scale, th = fs + 20 * scale;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    for (int k = 0; k < N_FILTERS; k++) {
        if (k) ImGui::SameLine(0, gap);
        float tw = ImGui::CalcTextSize(L(names[k])).x + 2 * pad;
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));  // (the piece shows the cursor)
        ImGui::PushID(k);
        if ((opened && k == filter) || (previous_filter != filter && k == filter)) {
            ImGui::SetKeyboardFocusHere();
            ImGui::SetNavCursorVisible(true);
        }
        if (ImGui::InvisibleButton("##filter", ImVec2(tw, th), ImGuiButtonFlags_EnableNav | ImGuiButtonFlags_PressedOnClick) &&
            filter != k)
            filter = k, menu_sfx_play(MENU_SFX_TAB);
        menu_sfx_tab_item();
        ImGui::PopID();
        ImGui::PopStyleColor();
        bool sel = k == filter, lit = (ImGui::IsItemFocused() && ImGui::GetCurrentContext()->NavCursorVisible) ||
                                      ImGui::IsItemHovered();
        bool hot = lit && !sel;
        ImVec4 col = sel ? pt.tab_sel : hot ? pt.tab_hot : pt.tab;
        if (sel && lit) col = ImVec4(col.x * 0.88f, col.y * 0.88f, col.z * 0.88f, col.w);
        plastic_pill(dl, ImVec2(p.x, p.y + (sel || hot ? 0 : 2 * scale)), ImVec2(p.x + tw, p.y + th - 2 * scale),
                     10 * scale, col, (sel || hot ? 7 : 4) * scale, sel || hot ? 1.0f : 0.55f, scale, sel || hot);
        ImVec2 ts = ImGui::CalcTextSize(L(names[k]));
        dl->AddText(ImVec2(p.x + (tw - ts.x) * 0.5f, p.y + (th - ts.y) * 0.5f - (sel ? 2 : 1) * scale),
                    sel || hot ? pt.text_sel : pt.text, L(names[k]));
    }

    ImGui::Dummy(ImVec2(0, 4 * scale));
    if (!tracker.error().empty()) ImGui::TextWrapped("%s", L(tracker.error().c_str()));
    char summary[96];
    snprintf(summary, sizeof summary, L("%u of %u unlocked"), earned, total);
    ImGui::ProgressBar((float)earned / total, ImVec2(-FLT_MIN, 0), summary);
    ImGui::Dummy(ImVec2(0, 4 * scale));

    // Only display counters when there is a vanilla save in play. History is available from the title screen too.
    std::array<uint8_t, achievements::SaveSize> save;
    bool show_progress = game_in_play() && !rando_seed_dir();
    if (show_progress) {
        memcpy(save.data(), (const void *)(uintptr_t)GA(LIVE_SAVE), save.size());
        show_progress = save[0] == 14 && (save[0x5388] || save[0x5389]);
    }

    if (previous_filter != filter) ImGui::SetNextWindowScroll(ImVec2(0, 0));
    if (ImGui::BeginChild("##achievement_list", ImVec2(0, 0), ImGuiChildFlags_NavFlattened)) {
        bool any = false;
        float w = ImGui::GetContentRegionAvail().x, cpad = 14 * scale, badge = 2.2f * fs;
        float tx = cpad + badge + 14 * scale, text_w = w - tx - cpad - 8 * scale, dfs = fs * .8f;
        ImDrawList *ldl = ImGui::GetWindowDrawList();
        for (size_t i = 0; i < rules.size(); ++i) {
            bool unlocked = tracker.unlocked(i);
            if ((filter == Unlocked && !unlocked) || (filter == Locked && unlocked)) continue;
            any = true;
            const auto &rule = rules[i];
            bool hidden = !unlocked && !strcmp(rule.id, "LA-080");
            Badge b = hidden ? Badge::Hidden : rule.badge;
            const char *title = hidden ? L("Hidden achievement") : rule.title;
            const char *desc = hidden ? "" : rule.description;
            char count[32] = "";
            if (!unlocked && show_progress && rule.target > 1 &&
                (rule.mode == achievements::Rule::Count || rule.mode == achievements::Rule::Hearts))
                snprintf(count, sizeof count, "%u / %u", std::min(achievements::progress(rule, save.data()), rule.target),
                         rule.target);
            float dh = *desc ? ImGui::GetFont()->CalcTextSizeA(dfs, FLT_MAX, text_w, desc).y : 0;
            float h = std::max(badge + 2 * cpad, cpad * 2 + fs * 1.15f + dh + (*count ? dfs * 1.2f : 0));

            ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
            ImGui::PushID((int)i);
            ImGui::InvisibleButton("##card", ImVec2(w - 8 * scale, h), ImGuiButtonFlags_EnableNav);
            ImGui::PopID();
            ImGui::PopStyleColor();
            if (!ImGui::IsItemVisible()) { ImGui::Dummy(ImVec2(0, 2 * scale)); continue; }
            bool sel = (ImGui::IsItemFocused() && ImGui::GetCurrentContext()->NavCursorVisible) || ImGui::IsItemHovered();
            ImVec4 col = sel ? pt.row_sel : pt.row;
            if (!unlocked && !sel) col.w *= 0.6f;
            plastic_pill(ldl, ImVec2(p.x + 3 * scale, p.y + 1 * scale), ImVec2(p.x + w - 8 * scale, p.y + h - 3 * scale),
                         std::min(h * .5f, fs * .9f), col, (sel ? 7 : 3.5f) * scale, sel ? 1.f : .6f, scale, sel);
            ImVec2 c(p.x + cpad + badge * .5f, p.y + h * .5f);
            ldl->AddCircleFilled(c, badge * .5f, ImGui::ColorConvertFloat4ToU32(pt.inset));
            ImU32 ink = sel ? pt.text_sel : pt.text, dim = sel ? pt.text_sel : pt.text_dim;
            draw_badge(ldl, b, c, badge * (b == Badge::Sword ? .48f : .57f), unlocked ? pt.title : pt.text_dim);
            float y = p.y + cpad;
            ldl->AddText(ImVec2(p.x + tx, y), ink, title);
            if (unlocked) {  // (a check after the title)
                float cx = p.x + tx + ImGui::CalcTextSize(title).x + fs * .5f, cy = y + fs * .5f;
                ldl->AddPolyline(std::array<ImVec2, 3>{ImVec2(cx, cy), ImVec2(cx + fs * .2f, cy + fs * .22f),
                                                       ImVec2(cx + fs * .55f, cy - fs * .25f)}.data(),
                                 3, sel ? pt.text_sel : pt.title, 0, 3 * scale);
            }
            y += fs * 1.15f;
            if (*desc) ldl->AddText(ImGui::GetFont(), dfs, ImVec2(p.x + tx, y), dim, desc, NULL, text_w), y += dh;
            if (*count) ldl->AddText(ImGui::GetFont(), dfs, ImVec2(p.x + tx, y + dfs * .2f), dim, count);
            ImGui::Dummy(ImVec2(0, 2 * scale));
        }
        if (!any) ImGui::TextUnformatted(filter == Unlocked ? L("No achievements unlocked yet.") : L("All achievements unlocked."));
    }
    ImGui::EndChild();
}
