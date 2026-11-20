#include "../achievements.h"
#include "../game_addresses.h"
#include "../paths.h"
#include "imgui.h"
#include "i18n.h"
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

extern "C" bool achievements_overlay_wanted(bool visible) {
    double now = now_s(), dt = clock_prev ? now - clock_prev : 0;
    clock_prev = now;
    // The first set describes the vanilla game's collection totals.
    if (rando_seed_dir()) {
        tracker.baseline(); pending.clear(); elapsed = 0;
        return false;
    }
    if (!game_in_play()) {
        tracker.baseline();
        return false;  // keep earned notifications queued across a level transition
    }
    open_history();
    if (now >= next_sample) {
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
    if (!visible || pending.empty()) return false;
    elapsed += std::min(dt, .1);  // menus and a suspended window do not consume the popup's reading time
    if (elapsed >= 7) { pending.pop_front(); elapsed = 0; }
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
    float expand = t < 1.4f ? 0 : t < 2.2f ? ease((t-1.4f)/.8f) : t < 5.67f ? 1 : 1-ease((t-5.67f)/.7f);
    float w = diameter + (full-diameter)*expand;
    float alpha = t < .3f ? ease(t/.3f) : t < 6.4f ? 1 : 1-ease((t-6.4f)/.6f);
    float x=(display.x-w)*.5f, y=display.y-124*u;
    auto color = [&](int r,int g,int b,float a=1.f) { return IM_COL32(r,g,b,(int)(255*alpha*a)); };
    ImDrawList *dl=ImGui::GetForegroundDrawList();
    dl->AddRectFilled(ImVec2(x,y),ImVec2(x+w,y+diameter),color(23,37,28),diameter*.5f);
    ImVec2 center(x+40*u,y+40*u);
    dl->AddCircleFilled(center,32*u,color(27,52,36));
    float angle=t<1.6f ? t*8 : t<5.67f ? 12.8f : 12.8f+(t-5.67f)*9;
    for(int j=0;j<2;++j) {
        dl->PathArcTo(center,35*u,angle+j*3.14159f,angle+j*3.14159f+2.3f,40);
        dl->PathStroke(color(64,184,88),0,3*u);
    }
    if (t > .6f) {
        float reveal_badge = ease((t - .6f) / .45f);
        draw_badge(dl, rule.badge, center, 32 * u, badge_color(rule.badge, (int)(255 * alpha * reveal_badge)));
    }
    if(t<1.9f) return;
    float reveal=ease((t-1.9f)/.45f), edge=x+w-18*u, tx=x+94*u;
    erase_text(dl,font,label,ImVec2(tx,y+9*u),color(152,198,162,reveal),heading,edge);
    erase_text(dl,font,title,ImVec2(tx,y+25*u),color(237,244,239,reveal),rule.title,edge);
    erase_text(dl,font,detail,ImVec2(tx,y+52*u),color(180,201,186,reveal),rule.description,edge);
}

extern "C" void achievements_menu_draw(void) {
    open_history();
    const auto &rules = achievements::catalog();
    unsigned total = (unsigned)rules.size();
    unsigned earned = 0;
    for (size_t i = 0; i < rules.size(); ++i) earned += tracker.unlocked(i);
    if (!tracker.error().empty()) ImGui::TextWrapped("%s", L(tracker.error().c_str()));

    char summary[96];
    snprintf(summary, sizeof summary, L("%u of %u unlocked"), earned, total);
    ImGui::ProgressBar((float)earned / total, ImVec2(-FLT_MIN, 0), summary);
    enum { All, Unlocked, Locked };
    static int filter = All;
    int previous_filter = filter;
    ImGui::RadioButton(L("All"), &filter, All);
    ImGui::SameLine();
    ImGui::RadioButton(L("Unlocked"), &filter, Unlocked);
    ImGui::SameLine();
    ImGui::RadioButton(L("Locked"), &filter, Locked);

    // Only display counters when there is a vanilla save in play. History is available from the title screen too.
    std::array<uint8_t, achievements::SaveSize> save;
    bool show_progress = game_in_play() && !rando_seed_dir();
    if (show_progress) {
        memcpy(save.data(), (const void *)(uintptr_t)GA(LIVE_SAVE), save.size());
        show_progress = save[0] == 14 && (save[0x5388] || save[0x5389]);
    }

    if (previous_filter != filter) ImGui::SetNextWindowScroll(ImVec2(0, 0));
    if (ImGui::BeginChild("##achievement_list", ImVec2(0, 0))) {
        bool any = false;
        for (size_t i = 0; i < rules.size(); ++i) {
            bool unlocked = tracker.unlocked(i);
            if ((filter == Unlocked && !unlocked) || (filter == Locked && unlocked)) continue;
            if (any) ImGui::Separator();
            any = true;
            const auto &rule = rules[i];
            bool hidden = !unlocked && !strcmp(rule.id, "LA-080");
            Badge badge = hidden ? Badge::Hidden : rule.badge;
            float size = 2.4f * ImGui::GetTextLineHeight();
            ImVec2 position = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(size, size));
            if (ImGui::IsItemVisible()) {
                ImDrawList *draw = ImGui::GetWindowDrawList();
                ImVec2 center(position.x + size * .5f, position.y + size * .5f);
                draw->AddCircleFilled(center, size * .48f,
                    unlocked ? IM_COL32(32, 60, 43, 255) : IM_COL32(77, 70, 61, 255));
                // The compact list badge keeps the sword tips inside its circle.
                float glyph_size = size * (badge == Badge::Sword ? .48f : .57f);
                draw_badge(draw, badge, center, glyph_size,
                    unlocked ? badge_color(badge) : IM_COL32(224, 211, 193, 255));
            }
            ImGui::SameLine(0, ImGui::GetTextLineHeight() * .5f);
            ImGui::BeginGroup();
            ImGui::TextWrapped("%s", hidden ? L("Hidden achievement") : rule.title);
            ImGui::TextDisabled("%s", unlocked ? L("Unlocked") : L("Locked"));
            if (!hidden) ImGui::TextWrapped("%s", rule.description);
            if (!unlocked && show_progress && rule.target > 1 &&
                (rule.mode == achievements::Rule::Count || rule.mode == achievements::Rule::Hearts)) {
                unsigned current = std::min(achievements::progress(rule, save.data()), rule.target);
                ImGui::TextDisabled("%u / %u", current, rule.target);
            }
            ImGui::EndGroup();
        }
        if (!any) ImGui::TextUnformatted(filter == Unlocked ? L("No achievements unlocked yet.") : L("All achievements unlocked."));
    }
    ImGui::EndChild();
}
