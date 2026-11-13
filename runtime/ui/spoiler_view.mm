// In-game spoiler page (Randomizer tab -> a seed's "Spoiler log"): rando/<seed>/spoiler_view.txt, written by
// tools/rando/spoiler_view.py, in two panes like the other two-column tabs (option_rows.h): the log's sections on the
// left (Route, Spheres, Items by area, entrances, rooms, hints, the seed's settings) with the search and the switches
// under them, the chosen section's rows on the right.
//   Route    a stepper: the stops done fold into one row, the next stop to do is lit (title colour); with the running
//            seed, how many of the route's checks are collected
//   others   headers that open and close (Spheres, areas, "Small items"), rows "check  ->  item"
// For the running seed a check the tracker has collected is greyed ("Hide checked" leaves it out). The cursor follows
// the left column: the section it's on shows; A / Right goes into its rows, B / Left back; B on the left: back to the
// Randomizer tab.
// Markup: '# ' section, '## ' sub-heading ("12. Mabe Village": a numbered route stop), '### ' folded group inside a
// sub-heading, '- ' bullet, two-space continuation, plain text otherwise. A line may end in TAB <location> TAB <flags>
// (flag 'u': an item that opens things up). The file is English (the same for every player of a seed); its lines show
// in the menus' language, put together from their parts (i18n.h L_line).
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <set>
#include <string>
#include <vector>

#include "../platform.h"  // (PLAT_TOUCH)
#include "i18n.h"
#include "imgui.h"
#include "option_rows.h"
#include "osk.h"
#include "plastic.h"

extern "C" {
#include "../rando.h"
int tracker_check_collected(const char *loc);  // tracker_overlay.mm: 1 / 0 / -1 (can't tell)
}

namespace {  // (tracker_overlay.mm has its own Node: keep both local or vector<Node> code gets merged)
struct Line {
    std::string text, loc;
    bool useful = false;
};
struct Group {  // "### " (title empty: the lines before any group)
    std::string title;
    std::vector<Line> lines;
};
struct Sub {
    std::string title;
    std::vector<Group> groups;
};
struct Node {
    std::string title;
    std::vector<Line> lines;  // text directly under the heading
    std::vector<Sub> subs;    // "## " blocks
};
struct Filter {
    std::string q;  // (lower case)
    bool hide_done, useful_only, linked;
};
}  // namespace

static std::string g_loaded;  // seed whose file is loaded
static std::vector<Node> g_doc;
static bool g_missing;
static int g_sec = 1;                 // the section shown on the right (g_doc index)
static std::set<std::string> g_open;  // open headers (section index + title)
static int g_enter_right, g_back_left = -1;  // (cursor moves: into the right pane / back to section row n)

static Line parse_line(const std::string &l) {
    Line ln;
    size_t t = l.find('\t');
    ln.text = l.substr(0, t);
    if (t != std::string::npos) {
        size_t t2 = l.find('\t', t + 1);
        ln.loc = l.substr(t + 1, t2 == std::string::npos ? std::string::npos : t2 - t - 1);
        ln.useful = t2 != std::string::npos && l.find('u', t2 + 1) != std::string::npos;
    }
    return ln;
}

static void load(const std::string &seed) {
    g_loaded = seed;
    g_doc.clear();
    g_open.clear();
    g_missing = false;
    g_sec = 1;
    std::string p = "rando/" + seed + "/spoiler_view.txt";
    FILE *f = fopen(p.c_str(), "r");
    if (!f) { g_missing = true; return; }
    char buf[1024];
    while (fgets(buf, sizeof buf, f)) {
        std::string l(buf);
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
        if (l.rfind("# ", 0) == 0) { g_doc.push_back({l.substr(2), {}, {}}); continue; }
        if (g_doc.empty()) g_doc.push_back({"", {}, {}});
        Node &n = g_doc.back();
        if (l.rfind("## ", 0) == 0) { n.subs.push_back({l.substr(3), {{"", {}}}}); continue; }
        if (n.subs.empty()) { if (!l.empty()) n.lines.push_back(parse_line(l)); continue; }
        if (l.rfind("### ", 0) == 0) { n.subs.back().groups.push_back({l.substr(4), {}}); continue; }
        if (!l.empty()) n.subs.back().groups.back().lines.push_back(parse_line(l));
    }
    fclose(f);
    if (g_doc.size() < 2) g_sec = 0;
}

// "12. Mabe Village": the number stays, the rest in the menus' language
static std::string shown(const std::string &en) {
    size_t i = 0;
    while (i < en.size() && isdigit((unsigned char)en[i])) i++;
    if (i > 0 && en.compare(i, 2, ". ") == 0) return en.substr(0, i + 2) + L_line(en.substr(i + 2));
    return L_line(en);
}
static std::string lower(std::string s) {
    for (auto &c : s) c = (char)tolower((unsigned char)c);
    return s;
}
static bool hit(const std::string &en, const Filter &f) {
    return f.q.empty() || lower(shown(en)).find(f.q) != std::string::npos;
}
static std::string body(const Line &l) {  // (the line without its markup)
    const std::string &t = l.text;
    return t.rfind("- ", 0) == 0 || t.rfind("  ", 0) == 0 ? t.substr(2) : t;
}
static bool is_label(const Line &l) {  // "- Mabe Village:" over the Spheres' rows
    return l.loc.empty() && l.text.rfind("- ", 0) == 0 && !l.text.empty() && l.text.back() == ':';
}
static bool is_text(const Line &l) { return l.text.rfind("- ", 0) != 0 && l.text.rfind("  ", 0) != 0; }
static int done_of(const Line &l, const Filter &f) {
    return f.linked && !l.loc.empty() ? tracker_check_collected(l.loc.c_str()) : -1;
}
// a row shows (switches and search; title_hit: its header matched the search)
static bool shows(const Line &l, const Filter &f, bool title_hit) {
    if (is_label(l) || is_text(l)) return false;
    if (!l.loc.empty() && f.useful_only && !l.useful) return false;
    if (f.hide_done && done_of(l, f) == 1) return false;
    return title_hit || hit(body(l), f);
}
static int count_rows(const std::vector<Line> &ls, const Filter &f, bool title_hit) {
    int n = 0;
    for (auto &l : ls) n += shows(l, f, title_hit);
    return n;
}
static int count_sub(const Sub &s, const Filter &f) {
    bool th = !f.q.empty() && hit(s.title, f);
    int n = 0;
    for (auto &g : s.groups) n += count_rows(g.lines, f, th || (!g.title.empty() && !f.q.empty() && hit(g.title, f)));
    return n;
}
static int count_node(const Node &n, const Filter &f) {
    int c = count_rows(n.lines, f, false);
    for (auto &s : n.subs) c += count_sub(s, f);
    return c;
}

// ---- right pane rows
static void heading(const std::string &t, bool lit) {  // (a section's own line: already in the menus' language)
    if (ImGui::GetCursorPosY() > ImGui::GetStyle().WindowPadding.y + 1) ImGui::Dummy(ImVec2(0, 6 * ImGui::GetFontSize() / 16));
    const PlasticTheme &th = plastic_theme();
    ImGui::PushTextWrapPos(0);  // ("2. Tail Cave (D1) - enter through Bottle Grotto's entrance": long in some languages)
    ImGui::TextColored(plastic_col(lit ? th.title : th.section), "%s", i18n_upper(t.c_str()).c_str());
    ImGui::PopTextWrapPos();
}
static void row(const Line &l, const Filter &f, const std::string &where) {
    std::string b = body(l), label, value, desc = where;
    size_t a = b.find("  ->  ");
    if (a != std::string::npos) label = shown(b.substr(0, a)), value = shown(b.substr(a + 6));
    else if (l.loc.empty() && (a = b.find(": ")) != std::string::npos && b.find(": ", a + 2) == std::string::npos)
        label = shown(b.substr(0, a)), desc = shown(b.substr(a + 2));  // ("Owl statue, Tal Tal Heights: <hint>": below)
    else label = shown(b);
    // "Small Key (Tail Cave)" under Tail Cave: the heading already says whose (a key for another dungeon keeps it)
    size_t o = value.rfind(" (");
    if (o != std::string::npos && value.back() == ')' && where.find(value.substr(o + 2, value.size() - o - 3)) != std::string::npos)
        value.erase(o);
    ImGui::PushID(&l);
    if (done_of(l, f) == 1) opt_disable_next();
    opt_shown_next();
    opt_action(label.c_str(), value.c_str(), desc.empty() ? NULL : desc.c_str(), false, l.useful);
    ImGui::PopID();
}
static void rows(const std::vector<Line> &ls, const Filter &f, bool title_hit, const std::string &where) {
    std::string area = where;
    for (size_t i = 0; i < ls.size(); i++) {
        const Line &l = ls[i];
        if (is_label(l)) {  // (a label shows only with a row under it)
            std::string b = body(l);
            area = shown(b.substr(0, b.size() - 1));
            int n = 0;
            for (size_t k = i + 1; k < ls.size() && ls[k].text.rfind("  ", 0) == 0; k++) n += shows(ls[k], f, title_hit);
            if (n) heading(area, false);
            continue;
        }
        if (is_text(l)) {
            if (f.q.empty() && !f.useful_only && !f.hide_done) {
                ImGui::PushTextWrapPos(0);
                ImGui::TextDisabled("%s", shown(l.text).c_str());
                ImGui::PopTextWrapPos();
            }
            continue;
        }
        if (shows(l, f, title_hit)) row(l, f, area);
    }
}
// a header that opens and closes (its count on the right); true when open
static bool fold(const std::string &key, const std::string &title, int count, const Filter &f) {
    bool open = !f.q.empty() || g_open.count(key) > 0;
    char v[16];
    snprintf(v, sizeof v, "%d", count);
    ImGui::PushID(key.c_str());
    if (open) opt_mark_next();
    opt_shown_next();
    bool clicked = opt_link(title.c_str(), NULL, v);
    ImGui::PopID();
    if (clicked && f.q.empty()) {
        if (g_open.count(key)) g_open.erase(key);
        else g_open.insert(key);
        open = !open;
    }
    return open;
}

// the route's checks the tracker has collected, and all of them
static void route_progress(const Node &n, const Filter &f, int *got, int *total) {
    *got = *total = 0;
    for (auto &s : n.subs)
        for (auto &g : s.groups)
            for (auto &l : g.lines) if (!l.loc.empty()) (*total)++, *got += done_of(l, f) == 1;
}

static void draw_route(int si, const Filter &f) {
    const Node &n = g_doc[si];
    // each stop: done when every check of it the tracker knows is collected (shop shelves and instruments it can't
    // tell: they don't hold a stop open)
    std::vector<char> done(n.subs.size(), 0);
    for (size_t k = 0; k < n.subs.size(); k++) {
        int known = 0, got = 0;
        for (auto &g : n.subs[k].groups)
            for (auto &l : g.lines) {
                int d = done_of(l, f);
                known += d >= 0, got += d == 1;
            }
        done[k] = known > 0 && got == known;
    }
    int current = -1;
    for (size_t k = 0; k < n.subs.size(); k++)
        if (!done[k] && n.subs[k].title != "Finish") { current = (int)k; break; }
    bool searching = !f.q.empty();
    int ndone = 0;
    for (char d : done) ndone += d;
    bool show_done = false;
    if (ndone && !searching && !f.hide_done) {
        ImGui::Spacing();
        show_done = fold("route-done", L("Stops done"), ndone, f);
    }
    for (size_t k = 0; k < n.subs.size(); k++) {
        const Sub &s = n.subs[k];
        bool finish = s.title == "Finish";
        if (done[k] && !searching && !show_done) continue;
        if (finish && searching) continue;
        bool th = searching && hit(s.title, f);
        if (!finish && !count_sub(s, f)) continue;
        heading(shown(s.title), (int)k == current);
        if (finish) {
            for (auto &g : s.groups)
                for (auto &l : g.lines) ImGui::TextWrapped("%s", shown(body(l)).c_str());
            continue;
        }
        if ((int)k == current && g_enter_right) opt_focus_next_row(), g_enter_right = 0;
        for (auto &g : s.groups) rows(g.lines, f, th, shown(s.title));
    }
}

static void draw_section(int si, const Filter &f) {
    const Node &n = g_doc[si];
    if (n.title == "Route") { draw_route(si, f); return; }
    if (si == 0) {  // (the seed's own lines: its ID, settings, starting items)
        for (auto &l : n.lines) ImGui::TextWrapped("%s", shown(l.text).c_str()), ImGui::Spacing();
        return;
    }
    rows(n.lines, f, false, "");
    bool folds = n.subs.size() > 3;  // (Spheres, areas: headers that open; a few blocks (entrances): open, titled)
    for (size_t k = 0; k < n.subs.size(); k++) {
        const Sub &s = n.subs[k];
        if (!count_sub(s, f)) continue;
        bool th = !f.q.empty() && hit(s.title, f);
        std::string key = std::to_string(si) + "/" + s.title;
        if (folds) {
            if (!fold(key, shown(s.title), count_sub(s, f), f)) continue;
            opt_indent(1);
        } else heading(shown(s.title), false);
        for (auto &g : s.groups) {
            if (g.title.empty()) { rows(g.lines, f, th, shown(s.title)); continue; }
            bool gh = th || (!f.q.empty() && hit(g.title, f));
            int gc = count_rows(g.lines, f, gh);
            if (!gc) continue;
            if (fold(key + "/" + g.title, shown(g.title.substr(0, g.title.rfind(" ("))), gc, f)) {  // ("Small items")
                opt_indent(1);
                rows(g.lines, f, gh, shown(s.title));
                opt_indent(-1);
            }
        }
        if (folds) opt_indent(-1);
    }
}

// Returns 0 when the page was closed (B on its left column, Back, or the X on touch screens), 2 when B / Left stepped
// back within it (the menu mustn't take that B too), else 1.
int spoiler_view_draw(const std::string &seed, float scale) {
    static bool revealed;
    static std::string revealed_for;
    if (revealed_for != seed) { revealed = getenv("LARECOMP_SPOILER_TEST") != NULL; revealed_for = seed; }
    if (g_loaded != seed) load(seed);
    static bool in_right;
    bool free_b = !ui_text_busy() && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
    int ret = 1;
    if (free_b && in_right && (opt_pressed_back() || (opt_pressed_left() && !opt_focused_lr()))) g_back_left = g_sec, ret = 2;
    else if (free_b && !in_right && opt_pressed_back()) return 0;

    ImGui::TextColored(plastic_col(plastic_theme().title), "%s", seed.c_str());
    ImGui::TextDisabled("%s", L("Spoiler log"));
    ImGui::Spacing();
    if (g_missing || !revealed) {
#if PLAT_TOUCH
        if (plastic_close_corner(44 * scale, scale)) return 0;  // (one column: the screen's corner)
#endif
        opt_begin("##spoiler_gate", scale);
        if (g_missing) {
            ImGui::TextWrapped("%s", L("This seed has no spoiler log to show. Seeds made with Spoiler log off or by an older "
                                       "version don't have one."));
        } else {
            ImGui::TextWrapped("%s", L("This shows where every item is, plus a step-by-step route to beat the seed."));
            ImGui::Spacing();
            opt_button_next();
            if (opt_action("Reveal spoilers", "", NULL)) revealed = true;
        }
        opt_button_next();
        bool back = opt_action("Back", "", NULL);
        opt_end();
        return back ? 0 : ret;
    }
    // the seed that's running: its collected checks are greyed (the tracker's own data)
    const char *dir = rando_seed_dir();
    const char *base = dir ? strrchr(dir, '/') : NULL;
    static char q[64];
    static bool hide_done, useful_only;
    static bool q_test;  // (test: LARECOMP_SPOILER_QUERY fills the search once)
    if (!q_test && (q_test = true) && getenv("LARECOMP_SPOILER_QUERY")) snprintf(q, sizeof q, "%s", getenv("LARECOMP_SPOILER_QUERY"));
    Filter f;
    f.linked = dir && seed == (base ? base + 1 : dir);
    f.hide_done = f.linked && hide_done, f.useful_only = useful_only, f.q = lower(q);

    // (the left column's rows: Search, the sections from g_doc[1] on, then the seed's own as the last one)
    if (g_back_left >= 0) opt_focus_row(g_back_left == 0 ? (int)g_doc.size() : g_back_left), g_back_left = -1;
    opt_begin("##spoiler", scale, true);
    opt_compact();
    ui_text_inline(true);  // (filters as typed: typed into directly, not through a prompt)
    opt_text("Search", q, sizeof q, "Search items and places", NULL);
    ui_text_inline(false);
    for (size_t i = 1; i <= g_doc.size(); i++) {
        int si = i < g_doc.size() ? (int)i : 0;
        const Node &n = g_doc[si];
        char v[32] = "";
        if (n.title == "Route" && f.linked) {
            int got, total;
            route_progress(n, f, &got, &total);
            snprintf(v, sizeof v, "%d / %d", got, total);
        } else if (n.title == "Spheres") {  // (how many spheres, not how many items)
            int c = 0;
            for (auto &sb : n.subs) c += count_sub(sb, f) > 0;
            snprintf(v, sizeof v, "%d", c);
        } else if (si != 0) snprintf(v, sizeof v, "%d", count_node(n, f));
        if (si == g_sec) opt_mark_next();
        opt_shown_next();
        std::string t = si == 0 ? std::string(L("Seed settings")) : shown(n.title);
        if (opt_link(t.c_str(), NULL, v)) g_sec = si, g_enter_right = 1;
        if (opt_item_nav_focused() || (ImGui::IsItemHovered() && !in_right)) g_sec = si;
    }
    ImGui::Dummy(ImVec2(0, 6 * scale));
    opt_toggle("Useful items only", &useful_only, false, NULL);
    if (f.linked) opt_toggle("Hide checked", &hide_done, false, NULL);
    // ---- right: the chosen section
    opt_split();
#if PLAT_TOUCH
    if (plastic_close_pane(44 * scale, scale)) { opt_split_end(); return 0; }
#endif
    if (g_sec >= 0 && g_sec < (int)g_doc.size()) {
        opt_list_begin();
        if (g_enter_right && g_doc[g_sec].title != "Route") opt_focus_next_row(), g_enter_right = 0;
        if (!f.q.empty()) {  // (searching: what matches in every section, live as it's typed)
            int shown_any = 0;
            for (size_t si = 1; si < g_doc.size(); si++) {
                const Node &n = g_doc[si];
                if (n.title == "Route" || n.title == "Spheres" || !count_node(n, f)) continue;  // (the same items again)
                heading(shown(n.title), false);
                draw_section((int)si, f);
                shown_any = 1;
            }
            if (!shown_any) ImGui::TextDisabled("%s", L("No matches."));
        } else draw_section(g_sec, f);
        g_enter_right = 0;
        opt_list_end();
        opt_desc_text();
    }
    in_right = opt_in_right();
    opt_split_end();
    return ret;
}
