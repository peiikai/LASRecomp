// The readable spoiler (spoiler_view.txt, spoiler.html), as tools/rando/spoiler_view.py builds it from the seed folder
// (outputs.h): LAS's spoiler, settings.json, spheres.json, route.json and tracker.json (ours), and from the
// file-writing steps dungeon_rooms.tsv, entrances_<seed>.txt and seed_hints.txt.
//
// Python behaviour kept:
//   - a dungeon_rooms.tsv row marked 'copy' (a room's second copy) doesn't unpack into level, cell, room: the Python
//     stops with a ValueError there;
//   - pretty_loc capitalizes like str.capitalize (the rest lower case), check names keep theirs.
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
#include <stdio.h>
#include <stdlib.h>

#include <algorithm>

#include "outputs_lib.h"

namespace rando {
namespace solve {
namespace out {
namespace {

using namespace odata;
const char TAG = '\t';

std::string strip(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) a++;
    while (b > a && isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

std::string replace_all(std::string s, const std::string &a, const std::string &b) {
    for (size_t p = s.find(a); p != std::string::npos; p = s.find(a, p + b.size())) s.replace(p, a.size(), b);
    return s;
}

std::string html_escape(const std::string &s) {
    std::string o;
    for (char ch : s) {
        if (ch == '&') o += "&amp;";
        else if (ch == '<') o += "&lt;";
        else if (ch == '>') o += "&gt;";
        else if (ch == '"') o += "&quot;";
        else if (ch == '\'') o += "&#x27;";
        else o += ch;
    }
    return o;
}

std::string upper_first(const std::string &s) {
    std::string o = s;
    if (!o.empty()) o[0] = (char)toupper((unsigned char)o[0]);
    return o;
}

// natural(): text and numbers in turn (re.split(r'(\d+)')), numbers compared as numbers, text lower case
struct Natural {
    std::vector<std::string> text;  // (even positions)
    std::vector<std::string> nums;  // (odd positions: digits, compared by value)
};
Natural natural(const std::string &s) {
    Natural n;
    std::string cur;
    size_t i = 0;
    while (i <= s.size()) {
        size_t j = i;
        while (j < s.size() && !isdigit((unsigned char)s[j])) j++;
        std::string t = s.substr(i, j - i);
        for (char &ch : t) ch = (char)tolower((unsigned char)ch);
        n.text.push_back(t);
        if (j >= s.size()) break;
        size_t k = j;
        while (k < s.size() && isdigit((unsigned char)s[k])) k++;
        n.nums.push_back(s.substr(j, k - j));
        i = k;
    }
    return n;
}
int num_cmp(const std::string &a, const std::string &b) {
    size_t i = 0, j = 0;
    while (i + 1 < a.size() && a[i] == '0') i++;
    while (j + 1 < b.size() && b[j] == '0') j++;
    std::string x = a.substr(i), y = b.substr(j);
    if (x.size() != y.size()) return x.size() < y.size() ? -1 : 1;
    return x < y ? -1 : x > y ? 1 : 0;
}
bool natural_less(const std::string &a, const std::string &b) {
    Natural x = natural(a), y = natural(b);
    // [t0, n0, t1, n1, ...]
    size_t lx = x.text.size() + x.nums.size(), ly = y.text.size() + y.nums.size();
    for (size_t k = 0; k < lx && k < ly; k++) {
        int c;
        if (k % 2 == 0) {
            const std::string &p = x.text[k / 2], &q = y.text[k / 2];
            c = p < q ? -1 : p > q ? 1 : 0;
        } else
            c = num_cmp(x.nums[k / 2], y.nums[k / 2]);
        if (c) return c < 0;
    }
    return lx < ly;
}

std::string pretty_item(const std::string &it) {
    std::string base = base_item(it);
    static const std::regex a("^(D\\d)-(small-key|key|nightmare-key|compass|map|stone-beak|beak)$"),
        b("^(small-key|key|nightmare-key|compass|map|stone-beak)-?(D\\d)$");
    std::smatch m;
    if (std::regex_search(base, m, a)) {
        static const StrMap kinds = {{"small-key", "Small Key"}, {"key", "Small Key"}, {"nightmare-key", "Nightmare Key"},
                                     {"compass", "Compass"}, {"map", "Dungeon Map"}, {"stone-beak", "Stone Beak"},
                                     {"beak", "Stone Beak"}};
        return std::string(get(kinds, m[2].str())) + " (" + get(SV_DPREFIX, m[1].str()) + ")";
    }
    if (std::regex_search(base, m, b)) return pretty_item(m[2].str() + "-" + m[1].str());
    if (starts(base, "soul-")) {
        std::string name = title_case(replace_all(base.substr(5), "-", " "));
        return name + (ends(name, "s") ? "' Soul" : "'s Soul");
    }
    const char *i = get(SV_ITEMS, base);
    if (i && *i) return i;
    return title_case(replace_all(base, "-", " "));
}

std::string pretty_loc(const std::string &loc) {
    static const std::regex d("^(D\\d)-(.*)$"), dp("^pot-(\\d\\d[A-H])-(\\d+)$"), p("^pot-([A-Za-z0-9]+)-(\\d\\d[A-H])-(\\d+)$"),
        ap("^apple-tree-(\\d\\d[A-P])-(\\d)$");
    std::smatch m;
    if (std::regex_search(loc, m, d)) {
        std::string rest = m[2].str(), where = get(SV_DPREFIX, m[1].str());
        std::smatch pm;
        if (std::regex_search(rest, pm, dp)) return where + ": pot " + pm[2].str() + " in room " + pm[1].str();
        return where + ": " + replace_all(rest, "-", " ");
    }
    if (std::regex_search(loc, m, p)) {
        std::string g = m[1].str(), name;
        for (size_t i = 0; i < g.size(); i++) {
            if (i && isupper((unsigned char)g[i])) name += ' ';
            name += g[i];
        }
        return name + ": pot " + m[3].str();
    }
    if (std::regex_search(loc, m, ap)) return "Apple tree (" + m[1].str() + "), " + m[2].str() + " of 3";
    std::string s = replace_all(loc, "-", " ");
    for (char &ch : s) ch = (char)tolower((unsigned char)ch);
    return upper_first(s);
}

std::string room_word(const std::string &cell) {
    return cell.size() == 3 ? std::string(1, cell[2]) + std::to_string(atoi(cell.substr(0, 2).c_str())) : cell;
}

std::string door_title(const std::string &v) {
    if (const char *d = get(SV_DUNGEONS, v)) {
        std::string s = d;
        return s.substr(0, s.find(" ("));
    }
    for (const Door &d : DOORS)
        if (v == d.id) return door_name_of(d.dest_level);
    return v;
}

using Sections = std::vector<std::pair<std::string, std::vector<std::string>>>;
std::vector<std::string> *section(Sections &s, const std::string &k) {
    for (auto &e : s)
        if (e.first == k) return &e.second;
    return nullptr;
}

struct AreaKey {
    int a, b;
    std::string title;
    bool operator<(const AreaKey &o) const {
        if (a != o.a) return a < o.a;
        if (b != o.b) return b < o.b;
        return title < o.title;
    }
    bool operator==(const AreaKey &o) const { return a == o.a && b == o.b && title == o.title; }
};

struct SeedView {
    Ctx &c;
    Sections &S;
    const Value &settings;
    std::vector<std::pair<std::string, std::string>> loc_item_v;  // (spoiler order)
    std::map<std::string, std::string> loc_region, loc_item;
    std::map<std::string, Value> checks;
    bool rooms_on, shells_important;
    std::map<std::string, std::string> entered_via;
    std::map<std::string, int> copy_no;

    SeedView(Ctx &cx, Sections &s, const Value &st, const Value &sph, const Value &tracker) : c(cx), S(s), settings(st) {
        for (auto &sec : S) {
            if (!get(SV_AREAS, sec.first)) continue;
            for (const std::string &l : sec.second) {
                size_t p = l.find(':');
                if (p == std::string::npos) continue;
                std::string loc = strip(l.substr(0, p)), it = strip(l.substr(p + 1));
                if (!loc_item.count(loc)) loc_item_v.emplace_back(loc, it);
                else
                    for (auto &e : loc_item_v)
                        if (e.first == loc) e.second = it;
                loc_region[loc] = sec.first, loc_item[loc] = it;
            }
        }
        if (tracker.truthy() && tracker.has("checks"))
            for (const Value &ck : tracker.at("checks").list()) checks[ck.at("n").s()] = ck;
        Value rs = settings.get("dungeon-room-shuffle", Value("off"));
        rooms_on = !(rs == Value("off") || rs.is_none() || rs == Value(false));
        if (auto *de = section(S, "dungeon-entrances"))
            for (const std::string &l : *de) {
                std::vector<std::string> ab = split_arrow(l);
                if (ab[0] != ab[1]) entered_via[ab[1]] = ab[0];
            }
        shells_important = settings.get("seashells-important").truthy();
        std::map<std::string, int> count;
        if (auto *si = section(S, "starting-items"))
            for (const std::string &it : *si) count[base_item(it)]++;
        std::vector<std::string> order;
        std::set<std::string> in_order;
        if (sph.truthy())
            for (const Value &sp : sph.list())
                for (const Value &li : sp.list()) order.push_back(li.list()[0].s()), in_order.insert(li.list()[0].s());
        for (const auto &e : loc_item)  // (sorted)
            if (!in_order.count(e.first)) order.push_back(e.first);
        for (const std::string &loc : order) {
            auto it = loc_item.find(loc);
            std::string b = base_item(it == loc_item.end() ? "" : it->second);
            if (in(SV_PROGRESSIVE, b) && !starts(loc, "starting-")) copy_no[loc] = ++count[b];
        }
    }

    static std::vector<std::string> split_arrow(const std::string &l) {
        std::vector<std::string> parts;
        size_t a = 0;
        for (size_t p = l.find("->"); p != std::string::npos; p = l.find("->", a)) parts.push_back(strip(l.substr(a, p - a))), a = p + 2;
        parts.push_back(strip(l.substr(a)));
        if (parts.size() != 2) throw PyError("ValueError", "too many values to unpack (expected 2)");
        return parts;
    }

    std::string dungeon_title(const std::string &dk) const {
        std::string n = get(SV_DUNGEONS, dk);
        auto it = entered_via.find(dk);
        if (it != entered_via.end()) n += " - enter through " + door_title(it->second) + "'s entrance";
        return n;
    }

    std::pair<AreaKey, std::string> area(const std::string &loc) const {
        auto ci = checks.find(loc);
        Value ck = ci == checks.end() ? Value::new_dict() : ci->second;
        std::string d;
        if (ck.get("d").truthy()) d = py_str(ck.get("d"));
        else if (loc.size() >= 3 && loc[0] == 'D' && isdigit((unsigned char)loc[1]) && loc[2] == '-') d = loc.substr(0, 2);
        auto ri = loc_region.find(loc);
        std::string reg = ri == loc_region.end() ? "" : ri->second;
        if (d.empty() && get(SV_DUNGEONS, reg))
            for (const auto &e : SV_DKEY)
                if (reg == e.second) d = e.first;
        if (const char *dk = get(SV_DKEY, d)) {
            int idx = 0;
            for (size_t k = 0; k < SV_DUNGEON_ORDER.size(); k++)
                if (d == SV_DUNGEON_ORDER[k]) idx = (int)k;
            std::string t = dungeon_title(dk);
            return {{1, idx, ""}, t};
        }
        Value ar = ck.get("ar", Value(-1));
        if (ar.is_int() && 0 <= ar.i() && ar.i() < (int64_t)SV_PLACE.size() && *SV_PLACE[(size_t)ar.i()]) {
            int idx = 99;
            for (size_t k = 0; k < SV_MAP_ORDER.size(); k++)
                if (SV_MAP_ORDER[k] == ar.i()) { idx = (int)k; break; }
            return {{0, idx, ""}, SV_PLACE[(size_t)ar.i()]};
        }
        if (const char *a = get(SV_AREAS, reg)) {
            int idx = 49;
            for (size_t k = 0; k < SV_REGION_ORDER.size(); k++)
                if (reg == SV_REGION_ORDER[k]) { idx = (int)k; break; }
            return {{0, 50 + idx, ""}, a};
        }
        return {{2, 0, ""}, "Elsewhere"};
    }

    std::string name(const std::string &loc) const {
        auto ci = checks.find(loc);
        if (ci != checks.end() && ci->second.get("dn").truthy()) {
            std::string n = py_str(ci->second.get("dn"));
            if (rooms_on && ci->second.get("d").truthy() && ci->second.get("dc").truthy())
                n += " (room " + room_word(py_str(ci->second.get("dc"))) + ")";
            return n;
        }
        std::string n = pretty_loc(loc);
        if (loc.size() >= 3 && loc[0] == 'D' && isdigit((unsigned char)loc[1]) && loc[2] == '-') {
            std::string pre = std::string(get(SV_DPREFIX, loc.substr(0, 2))) + ": ";
            if (starts(n, pre)) n = upper_first(n.substr(pre.size()));
        }
        return n;
    }

    std::string item(const std::string &loc, const std::string &it) const {
        std::string n = pretty_item(it);
        auto c2 = copy_no.find(loc);
        if (c2 != copy_no.end() && c2->second == 2) n += " (2nd)";
        return n;
    }

    bool useful(const std::string &it) const {
        std::string b = base_item(it);
        static const std::regex cmb("(compass|map|beak)");
        return !b.empty() && !c.sv_junk.match(b) && !std::regex_search(b, cmb) && (shells_important || !starts(b, "seashell")) &&
               !starts(b, "heart-piece") && !starts(b, "heart-container");
    }

    std::string item_of(const std::string &loc) const {
        auto it = loc_item.find(loc);
        return it == loc_item.end() ? "" : it->second;
    }

    std::string line(const std::string &loc, const std::string &it, const std::string &bullet = "- ") const {
        return bullet + name(loc) + "  ->  " + item(loc, it) + TAG + loc + TAG + (useful(it) ? "u" : "");
    }
};

std::string finish_line(const Value &settings) {
    Value t = settings.get("triforce-hunt", Value(0));
    long tri = t.truthy() ? (long)t.to_int() : 0;
    if (tri) return fmt("- The last of the %ld Triforce pieces you need ends the game.", tri);
    return "- With all eight instruments, play the Ballad of the Wind Fish at the egg on Tal Tal Heights.";
}

std::string html_page(const std::vector<std::string> &lines, const std::string &seed) {
    std::string body;
    bool sec = false, sub = false, grp = false, ul = false;
    auto close = [&](int level) {  // 1 grp, 2 sub, 3 sec
        if (ul) body += "</ul>", ul = false;
        bool *ks[] = {&grp, &sub, &sec};
        for (int k = 0; k < level; k++)
            if (*ks[k]) body += "</details>", *ks[k] = false;
    };
    auto li = [&](std::string text, const std::string &cls, const std::string &loc) {
        if (!ul) body += "<ul>", ul = true;
        size_t p = text.find("  ->  ");
        if (p != std::string::npos)
            text = html_escape(text.substr(0, p)) + " &rarr; <span class=\"it\">" + html_escape(text.substr(p + 6)) + "</span>";
        else
            text = html_escape(text);
        body += cls != "sub" ? "<li class=\"" + cls + "\" data-l=\"\" data-h=\"" + html_escape(loc) + "\">" + text + "</li>"
                             : "<li class=\"sub\">" + text + "</li>";
    };
    std::string title;
    bool first_sec = true, in_route = false;
    for (const std::string &raw : lines) {
        size_t t1 = raw.find(TAG);
        std::string text = raw.substr(0, t1), rest = t1 == std::string::npos ? "" : raw.substr(t1 + 1);
        size_t t2 = rest.find(TAG);
        std::string loc = rest.substr(0, t2), flags = t2 == std::string::npos ? "" : rest.substr(t2 + 1);
        std::string cls = flags.find('u') != std::string::npos ? "u" : "";
        if (starts(text, "# ")) {
            close(3);
            if (title.empty()) {
                title = text.substr(2);
                body += "<h1>" + html_escape(title) + "</h1>";
                continue;
            }
            body += std::string("<details") + (first_sec ? " open" : "") + "><summary>" + html_escape(text.substr(2)) + "</summary>";
            first_sec = false;
            in_route = text.substr(2) == "Route";
            sec = true;
        } else if (starts(text, "## ")) {
            close(2);
            body += std::string("<details") + (in_route ? " open" : "") + "><summary>" + html_escape(text.substr(3)) + "</summary>";
            sub = true;
        } else if (starts(text, "### ")) {
            close(1);
            body += "<details><summary>" + html_escape(text.substr(4)) + "</summary>";
            grp = true;
        } else if (starts(text, "- ") && ends(text, ":") && loc.empty()) {
            li(text.substr(2, text.size() - 3), "sub", "");
        } else if (starts(text, "- ") || starts(text, "  ")) {
            li(text.substr(2), cls, loc);
        } else if (!text.empty()) {
            if (ul) body += "</ul>", ul = false;
            body += "<p>" + html_escape(text) + "</p>";
        }
    }
    close(3);
    std::string e = html_escape(seed);
    return "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
           "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>Spoiler - " + e + "</title>"
           "<style>" + SV_CSS + "</style></head><body><main>"
           "<div id=\"gate\"><h1>Spoiler log - " + e + "</h1><p>This shows where every item is, plus a route to beat "
           "the seed.</p><button id=\"show\">Reveal spoilers</button></div>"
           "<div id=\"doc\" class=\"hide\"><div class=\"bar\"><input id=\"q\" type=\"search\" placeholder=\"Search items and places\">"
           "<label><input id=\"u\" type=\"checkbox\"> Useful items only</label></div>" + body + "</div>"
           "</main><script>" + SV_JS + "</script></body></html>";
}

std::string entrance_name(const std::string &door) {
    if (const char *n = get(EN_NAMES, door)) return n;
    return upper_first(replace_all(door, "-", " "));
}

}  // namespace

std::string spoiler_item_name(const std::string &item) { return pretty_item(item); }
std::string spoiler_location_name(const std::string &loc) { return pretty_loc(loc); }

void build_spoiler_view(Ctx &c, const Value &settings) {
    const std::string *sp = c.file("spoiler.txt");
    if (!sp) return;
    std::string seed = c.in.folder_name;
    Sections S;
    {
        std::string cur;
        bool have = false;
        static const std::regex head("^[a-z-]+:\\s*$");
        for (const std::string &l0 : split(*sp, '\n')) {
            std::string line = l0;
            if (std::regex_search(line, head)) {
                cur = strip(line);
                cur = cur.substr(0, cur.size() - 1);
                if (auto *s = section(S, cur)) s->clear();
                else S.push_back({cur, {}});
                have = true;
            } else if (have && starts(line, "    ")) {
                section(S, cur)->push_back(strip(line));
            }
        }
    }
    Value sph = Value::parse_json(c.file("spheres.json")->data(), c.file("spheres.json")->size());
    const std::string *tj = c.file("tracker.json");
    Value tracker = tj ? Value::parse_json(tj->data(), tj->size()) : Value();
    SeedView sd(c, S, settings, sph, tracker);
    std::vector<std::string> out = {"# Seed " + seed};
    if (settings.get("seed").truthy() && settings.at("seed") != Value(seed)) out.push_back("Seed ID: " + py_str(settings.at("seed")));
    std::vector<std::string> on;
    for (const auto &e : settings.dict())
        if (e.value.is_bool() && e.value.b() && !in(SV_INTERNAL, e.key)) on.push_back(e.key);
    std::sort(on.begin(), on.end());
    std::string sl = "Settings: ";
    for (size_t k = 0; k < on.size(); k++) sl += (k ? ", " : "") + replace_all(on[k], "-", " ");
    out.push_back(sl);
    if (auto *si = section(S, "starting-items")) {
        std::string s = "Starting items: ";
        for (size_t k = 0; k < si->size(); k++) s += (k ? ", " : "") + pretty_item((*si)[k]);
        out.push_back(s);
    }
    // the Route
    if (const std::string *rj = c.file("route.json")) {
        Value r = Value::parse_json(rj->data(), rj->size());
        if (r.truthy() && r.get("spheres").truthy()) {
            out.push_back("");
            out.push_back("# Route");
            out.push_back("Only what you need to beat the seed. Each stop can be reached with the items from the stops before it.");
            std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>> stops;
            for (const Value &stop : r.at("spheres").list())
                for (const Value &li : stop.list()) {
                    std::string loc = li.list()[0].s(), it = py_str(li.list()[1]);
                    std::string title = sd.area(loc).second;
                    if (!stops.empty() && stops.back().first == title) stops.back().second.emplace_back(loc, it);
                    else stops.push_back({title, {{loc, it}}});
                }
            int n = 1;
            for (const auto &s : stops) {
                out.push_back(fmt("## %d. ", n++) + s.first);
                for (const auto &li : s.second) out.push_back(sd.line(li.first, li.second));
            }
            out.push_back("## Finish");
            out.push_back(finish_line(settings));
        }
    }
    // the spheres
    out.push_back("");
    out.push_back("# Spheres");
    if (!sph.truthy() || sph.list().empty()) out.push_back("(This seed has no sphere list.)");
    else {
        out.push_back("Each sphere can be done with what you had after the previous spheres (any order within a sphere).");
        out.push_back("Only items that open things up are listed; rupees, traps and similar are left out.");
        int n = 0;
        for (const Value &s : sph.list()) {
            std::vector<std::pair<std::string, std::string>> keep;
            for (const Value &li : s.list()) {
                std::string l = li.list()[0].s(), i = py_str(li.list()[1]);
                if (!starts(l, "starting-") && sd.useful(i)) keep.emplace_back(l, i);
            }
            if (keep.empty()) continue;
            out.push_back(fmt("## Sphere %d", ++n));
            std::vector<std::pair<std::pair<AreaKey, std::string>, std::vector<std::pair<std::string, std::string>>>> by_area;
            for (const auto &li : keep) {
                auto a = sd.area(li.first);
                auto it = std::find_if(by_area.begin(), by_area.end(), [&](const decltype(by_area)::value_type &x) {
                    return x.first.first == a.first && x.first.second == a.second;
                });
                if (it == by_area.end()) by_area.push_back({a, {li}});
                else it->second.push_back(li);
            }
            std::stable_sort(by_area.begin(), by_area.end(), [](const decltype(by_area)::value_type &x, const decltype(by_area)::value_type &y) {
                if (x.first.first == y.first.first) return x.first.second < y.first.second;
                return x.first.first < y.first.first;
            });
            for (auto &a : by_area) {
                out.push_back("- " + a.first.second + ":");
                std::stable_sort(a.second.begin(), a.second.end(), [&](const std::pair<std::string, std::string> &x,
                                                                        const std::pair<std::string, std::string> &y) {
                    return natural_less(sd.name(x.first), sd.name(y.first));
                });
                for (const auto &li : a.second) out.push_back(sd.line(li.first, li.second, "  "));
            }
        }
        out.push_back("## Finish");
        out.push_back(finish_line(settings));
    }
    // every item by area
    out.push_back("");
    out.push_back("# Items by area");
    {
        std::vector<std::pair<std::pair<AreaKey, std::string>, std::vector<std::string>>> by_area;
        for (const auto &e : sd.loc_item_v) {
            if (starts(e.first, "starting-item") || starts(e.first, "starting-dungeon-item")) continue;
            auto a = sd.area(e.first);
            auto it = std::find_if(by_area.begin(), by_area.end(), [&](const decltype(by_area)::value_type &x) {
                return x.first.first == a.first && x.first.second == a.second;
            });
            if (it == by_area.end()) by_area.push_back({a, {e.first}});
            else it->second.push_back(e.first);
        }
        std::stable_sort(by_area.begin(), by_area.end(), [](const decltype(by_area)::value_type &x, const decltype(by_area)::value_type &y) {
            if (x.first.first == y.first.first) return x.first.second < y.first.second;
            return x.first.first < y.first.first;
        });
        for (auto &a : by_area) {
            std::vector<std::string> locs = a.second;
            std::stable_sort(locs.begin(), locs.end(), [&](const std::string &x, const std::string &y) {
                return natural_less(sd.name(x), sd.name(y));
            });
            std::vector<std::string> small;
            for (const std::string &l : locs)
                if (c.sv_junk.match(base_item(sd.item_of(l)))) small.push_back(l);
            out.push_back("## " + a.first.second);
            for (const std::string &l : locs)
                if (std::find(small.begin(), small.end(), l) == small.end()) out.push_back(sd.line(l, sd.item_of(l)));
            if (!small.empty()) {
                out.push_back(fmt("### Small items (%zu)", small.size()));
                for (const std::string &l : small) out.push_back(sd.line(l, sd.item_of(l)));
            }
        }
    }
    // dungeon entrances
    if (auto *de = section(S, "dungeon-entrances")) {
        if (!de->empty()) {
            out.push_back("");
            out.push_back("# Dungeon entrances");
            for (const std::string &l : *de) {
                std::vector<std::string> ab = SeedView::split_arrow(l);
                const char *a = get(SV_DUNGEONS, ab[0]), *b = get(SV_DUNGEONS, ab[1]);
                out.push_back("- " + (a ? std::string(a) : door_title(ab[0])) + " entrance leads into " + (b ? b : ab[1]));
            }
        }
    }
    // shuffled dungeon rooms
    std::string tsv;
    if (sd.rooms_on && c.seed_file("dungeon_rooms.tsv", tsv)) {
        std::map<std::string, std::vector<std::array<std::string, 3>>> moved;
        std::vector<std::string> lines = split(tsv, '\n');
        if (!tsv.empty() && tsv.back() == '\n') lines.pop_back();
        for (const std::string &l : lines) {
            std::vector<std::string> f = split(l, '\t');
            if (f.size() < 3 || !get(SV_LEVEL_D, f[0])) continue;
            if (f.size() != 3) throw PyError("ValueError", "too many values to unpack (expected 3)");
            const std::string &lv = f[0], &cell = f[1], &room = f[2];
            size_t us = room.rfind('_');
            std::string src_lv = us == std::string::npos ? "" : room.substr(0, us), src_cell = us == std::string::npos ? room : room.substr(us + 1);
            if (room == lv + "_" + cell || !get(SV_LEVEL_D, src_lv)) continue;
            moved[get(SV_LEVEL_D, lv)].push_back({cell, get(SV_LEVEL_D, src_lv), src_cell});
        }
        if (!moved.empty()) {
            out.push_back("");
            out.push_back("# Dungeon rooms");
            out.push_back("Where each moved room is now and where it came from.");
            for (const char *d : SV_DUNGEON_ORDER) {
                auto it = moved.find(d);
                if (it == moved.end()) continue;
                out.push_back(std::string("## ") + get(SV_DUNGEONS, get(SV_DKEY, d)));
                std::vector<std::array<std::string, 3>> v = it->second;
                std::stable_sort(v.begin(), v.end(), [](const std::array<std::string, 3> &x, const std::array<std::string, 3> &y) {
                    std::string kx = x[0].substr(2, 1), ky = y[0].substr(2, 1);
                    if (kx != ky) return kx < ky;
                    return x[0].substr(0, 2) < y[0].substr(0, 2);
                });
                for (const auto &m : v)
                    out.push_back("- Room " + room_word(m[0]) + "  ->  " + get(SV_DPREFIX, m[1].c_str()) + ", room " + room_word(m[2]));
            }
        }
    }
    // shuffled entrances (entrances_<seed ID>.txt, else named after the folder)
    std::string ent;
    if (c.seed_file("entrances_" + py_str(settings.get("seed", Value(""))) + ".txt", ent) ||
        c.seed_file("entrances_" + seed + ".txt", ent)) {
        std::vector<std::pair<std::string, std::string>> doors, exits;
        int mode = 0;
        static const std::regex arrow("^\\s+(\\S+)\\s+->\\s+(\\S+)");
        for (const std::string &l : split(ent, '\n')) {
            if (starts(l, "Entrances")) { mode = 1; continue; }
            if (starts(l, "Exits")) { mode = 2; continue; }
            std::smatch m;
            if (mode && std::regex_search(l, m, arrow)) (mode == 1 ? doors : exits).emplace_back(m[1].str(), m[2].str());
        }
        if (!doors.empty()) {
            out.push_back("");
            out.push_back("# Entrances");
            std::vector<std::pair<std::string, std::string>> shuffled;
            size_t same = 0;
            for (const auto &d : doors)
                if (d.first != d.second) shuffled.push_back(d);
                else same++;
            auto by_name = [](const std::pair<std::string, std::string> &x, const std::pair<std::string, std::string> &y) {
                return natural_less(entrance_name(x.first), entrance_name(y.first));
            };
            out.push_back("## Doors (each door and where it now leads)");
            std::stable_sort(shuffled.begin(), shuffled.end(), by_name);
            for (const auto &d : shuffled) out.push_back("- " + entrance_name(d.first) + "  ->  " + entrance_name(d.second));
            if (same) {
                out.push_back(fmt("### Unchanged (%zu)", same));
                std::vector<std::pair<std::string, std::string>> un;
                for (const auto &d : doors)
                    if (d.first == d.second) un.push_back(d);
                std::stable_sort(un.begin(), un.end(), by_name);
                for (const auto &d : un) out.push_back("- " + entrance_name(d.first));
            }
            std::map<std::string, std::string> back;
            for (const auto &d : doors) back[d.second] = d.first;
            std::vector<std::pair<std::string, std::string>> odd;
            for (const auto &x : exits) {
                auto it = back.find(x.first);
                if (it == back.end() || it->second != x.second) odd.push_back(x);
            }
            if (!odd.empty()) {
                out.push_back("## Exits (each way out and where it puts you)");
                std::stable_sort(odd.begin(), odd.end(), by_name);
                for (const auto &d : odd) out.push_back("- " + entrance_name(d.first) + "  ->  " + entrance_name(d.second));
            }
        }
    }
    std::string hints;
    if (c.seed_file("seed_hints.txt", hints)) {
        out.push_back("");
        out.push_back("# Hints");
        std::vector<std::string> lines = split(hints, '\n');
        if (!hints.empty() && hints.back() == '\n') lines.pop_back();
        for (const std::string &l : lines) {
            size_t p = l.find(": ");
            std::string where = l.substr(0, p), text = p == std::string::npos ? "" : l.substr(p + 2);
            out.push_back("- " + where + ": " + text);
        }
    }
    std::string txt;
    for (const std::string &l : out) txt += l + "\n";
    c.write("spoiler_view.txt", txt);
    c.write("spoiler.html", html_page(out, seed));
    c.res.log.push_back(fmt("spoiler view: %zu lines", out.size()));
}

}  // namespace out
}  // namespace solve
}  // namespace rando
