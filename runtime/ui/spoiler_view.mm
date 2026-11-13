// In-game spoiler page (Randomizer tab -> a seed's "Spoiler" button): shows rando/<seed>/spoiler_view.txt, written by
// tools/rando/spoiler_view.py (the route, everything useful by step, items by area, dungeon entrances and rooms,
// entrances, hints).
// Markup: '# ' section, '## ' sub-heading ("12. Mabe Village": a numbered route stop), '### ' folded group inside a
// sub-heading, '- ' bullet, two-space continuation, plain text otherwise. A line may end in TAB <location> TAB <flags>:
// for the seed that's running, a check the tracker has collected is greyed and struck out ("Hide checked" leaves it
// out); flag 'u' = an item that opens things up ("Useful items only").
// The file is English (the same for every player of a seed); its lines show in the menus' language, put together from
// their parts (i18n.h L_line).
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

#include "i18n.h"
#include "imgui.h"
#include "osk.h"

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

}  // namespace

static std::string g_loaded;  // seed whose file is loaded
static std::vector<Node> g_doc;
static bool g_missing;

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
    g_missing = false;
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
        if (n.subs.empty()) { n.lines.push_back(parse_line(l)); continue; }
        if (l.rfind("### ", 0) == 0) { n.subs.back().groups.push_back({l.substr(4), {}}); continue; }
        n.subs.back().groups.back().lines.push_back(parse_line(l));
    }
    fclose(f);
}

// "12. Mabe Village": the number stays, the rest in the menus' language
static std::string shown(const std::string &en) {
    size_t i = 0;
    while (i < en.size() && isdigit((unsigned char)en[i])) i++;
    if (i > 0 && en.compare(i, 2, ". ") == 0) return en.substr(0, i + 2) + L_line(en.substr(i + 2));
    return L_line(en);
}

static bool matches(const std::string &en, const char *q) {
    if (!q[0]) return true;
    std::string a = shown(en), b = q;  // (as shown: the search is in the menus' language)
    for (auto &c : a) c = (char)tolower(c);
    for (auto &c : b) c = (char)tolower(c);
    return a.find(b) != std::string::npos;
}

struct Filter {
    const char *q;
    bool hide_done, useful_only, linked;  // linked: the spoiler's seed is the one running (the tracker knows it)
};

static int done_of(const Line &l, const Filter &f) {
    return f.linked && !l.loc.empty() ? tracker_check_collected(l.loc.c_str()) : -1;
}

// a check line passes the switches (lines without a location - text, area labels - always do)
static bool passes(const Line &l, const Filter &f) {
    if (l.loc.empty()) return true;
    if (f.useful_only && !l.useful) return false;
    if (f.hide_done && done_of(l, f) == 1) return false;
    return true;
}

// lines shown under a heading whose own title matched (all) or not (only the matching ones); an area label
// ("- Mabe Village:") shows only with something shown under it
static void visible(const std::vector<Line> &ls, const Filter &f, bool title_hit, std::vector<char> &out) {
    out.assign(ls.size(), 0);
    for (size_t i = 0; i < ls.size(); i++) out[i] = passes(ls[i], f) && (title_hit || matches(ls[i].text, f.q));
    for (size_t i = 0; i < ls.size(); i++) {
        const std::string &t = ls[i].text;
        if (!ls[i].loc.empty() || t.rfind("- ", 0) != 0 || t.empty() || t.back() != ':') continue;
        bool any = false;
        for (size_t k = i + 1; k < ls.size() && ls[k].text.rfind("  ", 0) == 0; k++) any |= out[k] != 0;
        bool filtering = f.q[0] || f.hide_done || f.useful_only;
        out[i] = any || (!filtering && out[i]);
    }
}

static void line(const Line &l, const Filter &f) {
    int done = done_of(l, f);
    const std::string &t = l.text;
    std::string s = t.rfind("- ", 0) == 0 || t.rfind("  ", 0) == 0 ? t.substr(2) : t;
    if (s.empty()) return;
    s = L_line(s);
    bool indent = t.rfind("  ", 0) == 0;
    if (indent) ImGui::Indent();
    if (t.rfind("- ", 0) == 0) ImGui::Bullet();
    if (done == 1) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    if (t.rfind("- ", 0) == 0 || indent) ImGui::TextUnformatted(s.c_str());
    else ImGui::TextWrapped("%s", s.c_str());
    if (done == 1) {  // (struck out, like the tracker's collected checks)
        ImVec2 a = ImGui::GetItemRectMin(), z = ImGui::GetItemRectMax();
        float y = (a.y + z.y) * 0.5f;
        ImGui::GetWindowDrawList()->AddLine(ImVec2(a.x, y), ImVec2(z.x, y), ImGui::GetColorU32(ImGuiCol_TextDisabled),
                                            ImGui::GetFontSize() / 14.0f);
        ImGui::PopStyleColor();
    }
    if (indent) ImGui::Unindent();
}

static int draw_lines(const std::vector<Line> &ls, const Filter &f, bool title_hit) {
    std::vector<char> vis;
    visible(ls, f, title_hit, vis);
    int n = 0;
    for (size_t i = 0; i < ls.size(); i++) if (vis[i]) line(ls[i], f), n++;
    return n;
}

static bool any_visible(const std::vector<Line> &ls, const Filter &f, bool title_hit) {
    std::vector<char> vis;
    visible(ls, f, title_hit, vis);
    for (size_t i = 0; i < ls.size(); i++)
        if (vis[i] && (!ls[i].loc.empty() || ls[i].text.rfind("- ", 0) == 0 || ls[i].text.rfind("  ", 0) == 0)) return true;
    return false;
}

// Returns false when the page was closed (Back).
bool spoiler_view_draw(const std::string &seed, float scale) {
    static bool revealed;
    static std::string revealed_for;
    if (revealed_for != seed) { revealed = getenv("LARECOMP_SPOILER_TEST") != NULL; revealed_for = seed; }
    if (g_loaded != seed) load(seed);
    if (ImGui::Button(L("< Back"))) return false;
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::Text(L("Spoiler log - %s"), seed.c_str());
    ImGui::Separator();
    if (g_missing) {
        ImGui::TextWrapped("%s", L("This seed has no spoiler log to show. Seeds made with Spoiler log off or by an older "
                                   "version don't have one."));
        return true;
    }
    if (!revealed) {
        ImGui::Spacing();
        ImGui::TextWrapped("%s", L("This shows where every item is, plus a step-by-step route to beat the seed."));
        if (ImGui::Button(L("Reveal spoilers"), ImVec2(320 * scale, 0))) revealed = true;
        return true;
    }
    // the seed that's running: lines of checks already collected are greyed (the tracker's own data)
    const char *dir = rando_seed_dir();
    const char *base = dir ? strrchr(dir, '/') : NULL;
    bool linked = dir && seed == (base ? base + 1 : dir);
    static char q[64];
    static bool hide_done, useful_only;
    ImGui::SetNextItemWidth(420 * scale);
    ui_text_inline(true);  // (filters as typed: typed into directly, not through a prompt)
    ui_input_text("##q", q, sizeof q, 0, L("Search items and places"), L("Search the spoiler log"));
    ui_text_inline(false);
    ImGui::SameLine();
    ImGui::Checkbox(L("Useful items only"), &useful_only);
    if (linked) {
        ImGui::SameLine();
        ImGui::Checkbox(L("Hide checked"), &hide_done);
    }
    Filter f = {q, linked && hide_done, useful_only, linked};
    bool filtering = q[0] || f.hide_done || f.useful_only;
    ImGui::BeginChild("##spoiler", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NavFlattened);
    for (size_t i = 0; i < g_doc.size(); i++) {
        Node &n = g_doc[i];
        bool route = n.title == "Route";  // (its stops start open: read top to bottom)
        ImGui::PushID((int)i);
        if (n.title.empty() || ImGui::CollapsingHeader(shown(n.title).c_str(), i <= 1 || route ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
            draw_lines(n.lines, f, false);
            for (size_t k = 0; k < n.subs.size(); k++) {
                Sub &sb = n.subs[k];
                bool hit = !q[0] || matches(sb.title, q);
                bool any = false;
                for (auto &g : sb.groups) any |= any_visible(g.lines, f, hit || (!g.title.empty() && matches(g.title, q)));
                if (!any && filtering) continue;
                ImGui::PushID((int)k);
                if (filtering) ImGui::SetNextItemOpen(true);
                if (ImGui::TreeNodeEx(shown(sb.title).c_str(), route ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
                    for (size_t g = 0; g < sb.groups.size(); g++) {
                        Group &gr = sb.groups[g];
                        bool ghit = hit || (!gr.title.empty() && matches(gr.title, q));
                        if (gr.title.empty()) { draw_lines(gr.lines, f, ghit); continue; }
                        if (!any_visible(gr.lines, f, ghit)) continue;
                        ImGui::PushID((int)g);
                        if (q[0]) ImGui::SetNextItemOpen(true);  // (a search opens the folded group too)
                        if (ImGui::TreeNode(shown(gr.title).c_str())) {
                            draw_lines(gr.lines, f, ghit);
                            ImGui::TreePop();
                        }
                        ImGui::PopID();
                    }
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    return true;
}
