// LAS Recomp's item steps: the Hint Sheet and the seed's hints (tools/rando/hint_sheet.py apply, seed_hints.py apply),
// ported (ours_items.cpp).
//
// What the solver worked out comes from the decisions (tools/rando/record_ours_items.py): "hint-sheet" (the steps as
// hint_sheet.apply's texts see them: landmarks and shuffled doors filled in, and the ways into the dungeons) and
// "seed-hints" (seed_hints.json's hints, door names, the ways in). The texts are composed here as the Python does,
// tables from ours_items_hints_data.cpp (tools/rando/export_ours_items_hints.py). Decide mode makes the two decisions
// first (ours_items_decide.cpp).
//
// hints.tsv (the runtime's steps: entry, location, item, copy n, the gated groups it needs) takes the copy numbers and
// needs from hint_steps.json, which the decisions don't hold: it is made only when the build has that file (decide
// mode: Decider::input).
//
// Python quirks kept (each makes the same file):
//   - hint_sheet._src / seed_hints._flow_path copy a game file into the seed before reading it: a write of the game's
//     bytes (logged as hint_sheet._src);
//   - the Tarin flow is read from the seed only (no Tarin edit yet: nothing at all is written);
//   - Items.gsheet's HintSheet row copies the Yoshi Doll's row (npcKey, itemID changed), as pets.py does.
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>

#include "../../../third_party/yyjson/yyjson.h"
#include "decide.h"
#include "ours_items_hints_data.h"
#include "ours_items_lib.h"

namespace rando {
namespace ours {
namespace {

using namespace hints;
using las::Flow;

// ---- JSON (the decisions)
std::string jstr(yyjson_val *o, const char *k) {
    const char *s = yyjson_get_str(yyjson_obj_get(o, k));
    return s ? s : "";
}

// a dungeon's way in (hint_sheet.via_doors): another dungeon's key, or a house / cave door
struct Via {
    bool door = false;
    std::string key, name, area;
};
using ViaMap = std::map<std::string, Via>;
ViaMap read_via(yyjson_val *o) {
    ViaMap out;
    yyjson_val *k, *v;
    yyjson_obj_iter it = yyjson_obj_iter_with(o);
    while ((k = yyjson_obj_iter_next(&it))) {
        v = yyjson_obj_iter_get_val(k);
        Via w;
        if (yyjson_is_obj(v)) {
            w.door = true, w.name = jstr(v, "name"), w.area = jstr(v, "area");
        } else if (yyjson_is_str(v)) {
            w.key = yyjson_get_str(v);
        } else {
            continue;  // (None: no way in)
        }
        out[yyjson_get_str(k)] = w;
    }
    return out;
}
const Via *via_of(const ViaMap &m, const std::string &key) {
    if (key.empty()) return nullptr;
    auto it = m.find(key);
    if (it == m.end() || (!it->second.door && it->second.key.empty())) return nullptr;  // (falsy)
    return &it->second;
}

// ---- tables
const char *get(const StrTable &t, const std::string &k) { return text::lookup(t, k); }
std::string need(const StrTable &t, const std::string &k) {
    const char *v = get(t, k);
    if (!v) throw std::runtime_error("hint text: no " + k);  // (the Python's KeyError)
    return v;
}
bool in(const std::vector<const char *> &v, const std::string &k) {
    for (const char *x : v)
        if (k == x) return true;
    return false;
}
std::string key_of_dungeon(const std::string &dn) {  // next(k for k, v in DUNGEON_KEY.items() if v == dn)
    for (const auto &e : DUNGEON_KEY)
        if (dn == e.second) return e.first;
    return "";
}
std::string get_or(const std::map<std::string, std::string> &m, const std::string &k, const std::string &dflt) {
    auto it = m.find(k);
    return it == m.end() ? dflt : it->second;
}

// ---- strings (Python str methods on ASCII / UTF-8)
bool starts(const std::string &s, const std::string &p) { return s.compare(0, p.size(), p) == 0; }
bool ends(const std::string &s, const std::string &p) {
    return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}
std::string title(const std::string &s) {  // str.title() (ASCII)
    std::string o = s;
    bool prev = false;
    for (char &c : o) {
        if (isalpha((unsigned char)c)) {
            c = prev ? (char)tolower((unsigned char)c) : (char)toupper((unsigned char)c);
            prev = true;
        } else {
            prev = false;
        }
    }
    return o;
}
std::vector<std::string> split(const std::string &s, char sep) {
    std::vector<std::string> out(1);
    for (char c : s) {
        if (c == sep) out.emplace_back();
        else out.back() += c;
    }
    return out;
}
uint32_t upper_cp(uint32_t c) {  // str.upper() of one character (the scripts the game's languages use)
    if (c >= 'a' && c <= 'z') return c - 32;
    if (c == 0xB5) return 0x39C;
    if ((c >= 0xE0 && c <= 0xFE && c != 0xF7)) return c - 32;
    if (c == 0xFF) return 0x178;
    if (c >= 0x100 && c <= 0x17F) {
        if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return (c & 1) ? c : c - 1;
        if (c == 0x131) return 'I';
        if (c == 0x17F) return 'S';
        if (c == 0x138 || c == 0x149 || c == 0x130) return c;
        return (c & 1) ? c - 1 : c;
    }
    if (c >= 0x3B1 && c <= 0x3C9 && c != 0x3C2) return c - 32;
    if (c == 0x3C2) return 0x3A3;
    if (c >= 0x430 && c <= 0x44F) return c - 32;
    if (c >= 0x450 && c <= 0x45F) return c - 80;
    if (c >= 0xFF41 && c <= 0xFF5A) return c - 32;
    return c;
}
std::string up_first(const std::string &s, size_t max_bytes = 4) {  // s[:1].upper() + s[1:]
    if (s.empty()) return s;
    unsigned char b = (unsigned char)s[0];
    size_t n = b < 0x80 ? 1 : b < 0xE0 ? 2 : b < 0xF0 ? 3 : 4;
    if (n > max_bytes || n > s.size()) return s;
    uint32_t c = n == 1 ? b : n == 2 ? (b & 0x1F) : n == 3 ? (b & 0x0F) : (b & 0x07);
    for (size_t i = 1; i < n; i++) c = c << 6 | ((unsigned char)s[i] & 0x3F);
    uint32_t u = upper_cp(c);
    if (u == c) return s;
    std::string e;
    if (u < 0x80) e = std::string(1, (char)u);
    else if (u < 0x800) e = {(char)(0xC0 | u >> 6), (char)(0x80 | (u & 0x3F))};
    else e = {(char)(0xE0 | u >> 12), (char)(0x80 | ((u >> 6) & 0x3F)), (char)(0x80 | (u & 0x3F))};
    return e + s.substr(n);
}
std::string strip_article(const std::string &s) {  // re.sub(r'^(the|a|an) ', '', s)
    for (const char *p : {"the ", "a ", "an "})
        if (starts(s, p)) return s.substr(strlen(p));
    return s;
}
std::string remove_the(const std::string &s) { return starts(s, "the ") ? s.substr(4) : s; }  // .removeprefix('the ')
bool is_digits(const std::string &s) {
    if (s.empty()) return false;
    for (char c : s)
        if (!isdigit((unsigned char)c)) return false;
    return true;
}
bool cell_ok(const std::string &s) {  // \d\d[A-P]
    return s.size() == 3 && isdigit((unsigned char)s[0]) && isdigit((unsigned char)s[1]) && s[2] >= 'A' && s[2] <= 'P';
}
// ^pot-([A-Za-z0-9]+)-(\d\d[A-P])-(\d+)$ -> the level
bool pot_loc(const std::string &loc, std::string &lv) {
    if (!starts(loc, "pot-")) return false;
    std::vector<std::string> p = split(loc.substr(4), '-');
    if (p.size() != 3 || p[0].empty() || !cell_ok(p[1]) || !is_digits(p[2])) return false;
    for (char c : p[0])
        if (!isalnum((unsigned char)c)) return false;
    lv = p[0];
    return true;
}
// ^(D\d)-(.*)$
bool dungeon_loc(const std::string &loc, std::string &dn, std::string &rest) {
    if (loc.size() < 3 || loc[0] != 'D' || !isdigit((unsigned char)loc[1]) || loc[2] != '-') return false;
    dn = loc.substr(0, 2), rest = loc.substr(3);
    return rest.find('\n') == std::string::npos;
}
bool rock_loc(const std::string &loc) {  // re.match(r'rock-\d\d[A-P]-', loc)
    return starts(loc, "rock-") && loc.size() >= 9 && cell_ok(loc.substr(5, 3)) && loc[8] == '-';
}
// ^(key|nightmare-key|compass|map|stone-beak)-(D\d)$
bool dungeon_item(const std::string &item, std::string &kind, std::string &dn) {
    for (const char *k : {"key", "nightmare-key", "compass", "map", "stone-beak"}) {
        std::string p = std::string(k) + "-";
        if (starts(item, p) && item.size() == p.size() + 2 && item[p.size()] == 'D' &&
            isdigit((unsigned char)item[p.size() + 1])) {
            kind = k, dn = item.substr(p.size());
            return true;
        }
    }
    return false;
}
std::string words(const std::string &s) {  // re.sub(r'(?<=[a-z])(?=[A-Z0-9])', ' ', s)
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (i && islower((unsigned char)s[i - 1]) && (isupper((unsigned char)s[i]) || isdigit((unsigned char)s[i]))) o += ' ';
        o += s[i];
    }
    return o;
}

// ---- seed files (hint_sheet._src / _read)
bool written(SeedBuild &b, const std::string &rel) { return b.files().count(rel) != 0; }
void src(SeedBuild &b, const std::string &rel) {  // copies the game's file into the seed when the seed has none
    if (written(b, rel)) return;
    std::string bytes;
    if (!b.read_bytes(rel, bytes)) throw std::runtime_error("no game file " + rel);
    b.write_bytes(rel, bytes);
}

// ======== hint_sheet.py: the text
struct Step {
    std::string loc, item, region, landmark, door_area, door_name;
};
using DungeonOf = std::map<std::string, std::string>;

std::string dungeon_en(const std::string &dn) { return need(DUNGEON, dn); }

// a text that's either ours (str) or the game's (bytes): hint_sheet's dungeon_name in another language
struct Txt {
    std::string s;
    bool game = false;
};
Txt dungeon_name(const std::string &dn, const std::string &lang, const Names *names) {
    if (lang == "en") return {dungeon_en(dn), false};
    std::string p;
    if (names && names->place(dungeon_label(dn), p)) return {p, true};
    return {dungeon_en(dn), false};
}

std::string item_phrase(const std::string &item) {
    std::string kind, dn;
    if (dungeon_item(item, kind, dn)) return need(DUNGEON_ITEM, kind) + " for " + dungeon_en(dn);
    if (starts(item, "soul-")) {
        std::string name = title(replace_all(item.substr(5), "-", " "));
        return name + (ends(name, "s") ? "' Soul" : "'s Soul");
    }
    const char *v = get(ITEM, item);
    if (v && *v) return v;
    return "the " + title(replace_all(item, "-", " "));
}

std::string via_phrase(const Via &v) {
    const char *a = get(AREA, v.area);
    std::string where = a ? a : "";
    std::string name = v.name == "a cave" ? v.name : "the door of " + v.name;
    return where.empty() ? name : name + " in " + where;
}

std::string place_phrase(const Step &st, const DungeonOf &dof, const ViaMap &via) {
    const std::string &loc = st.loc, &region = st.region;
    if (!st.door_name.empty()) {
        const char *a = get(AREA, st.door_area);
        std::string where = a && *a ? std::string(" in ") + a : "";
        if (st.door_name == "a cave") where = "in a cave" + where;
        else where = "behind the door of " + st.door_name + where;
        if (starts(loc, "pot-")) return "in a pot, " + where;
        std::string who;
        if (const char *pl = get(PLACE, loc)) {  // re.sub(r' (in|on|of|at) .*$', '', PLACE[loc])
            who = pl;
            size_t cut = std::string::npos;
            for (const char *w : {" in ", " on ", " of ", " at "}) cut = std::min(cut, who.find(w));
            if (cut != std::string::npos) who = who.substr(0, cut);
        }
        if (!starts(who, "with ")) {
            who = "";
            for (auto pw : std::vector<std::pair<const char *, const char *>>{
                     {"shop-", "in the shop"}, {"trendy-prize", "at the Trendy Game"},
                     {"fishing-", "at the fishing pond"}, {"dampe-", "with Dampe's chambers"}})
                if (starts(loc, pw.first)) { who = pw.second; break; }
        }
        return who.empty() ? where : who + ", " + where;
    }
    const char *ar = get(AREA, region);
    std::string area = ar ? ar : "";
    auto dungeon = [&](const std::string &dn, const std::string &detail = "") {
        std::string text = "in " + dungeon_en(dn) + (detail.empty() ? "" : " (" + detail + ")");
        std::string key = key_of_dungeon(dn);
        const Via *v = via_of(via, key);
        if (v && v->door) text += ", through " + via_phrase(*v);
        else if (v && v->key != key) text += ", through " + remove_the(dungeon_en(need(DUNGEON_KEY, v->key))) + "'s entrance";
        return text;
    };
    if (const char *pl = get(PLACE, loc)) return pl;
    std::string lv, dn, rest;
    if (pot_loc(loc, lv)) {
        if (const char *d = get(LEVEL_DUNGEON, lv)) return "in a pot " + dungeon(get_or(dof, loc, d));
        return "in a pot in " + words(lv) + (area.empty() ? "" : " (" + area + ")");
    }
    bool rock = rock_loc(loc);
    if (starts(loc, "bush-") || starts(loc, "dig-") || starts(loc, "apple-tree-") || rock) {
        std::string lm = st.landmark;
        if (!lm.empty() && (starts(lm, "a ") || starts(lm, "an ")) && !area.empty()) lm += " in " + area;
        std::string wild = area.empty() ? "the wilds" : area;
        if (starts(loc, "bush-")) return lm.empty() ? "under a bush in " + wild : "under a bush by " + lm;
        if (starts(loc, "apple-tree-")) return lm.empty() ? "in an apple tree in " + wild : "in an apple tree by " + lm;
        if (rock) return lm.empty() ? "under a rock in " + wild : "under a rock by " + lm;
        return lm.empty() ? "buried in " + (area.empty() ? std::string("the ground") : area) : "buried near " + lm;
    }
    if (ends(loc, "-dig")) return "buried in " + (area.empty() ? std::string("the ground") : area);
    if (starts(loc, "shop-")) return "in the Mabe Village shop";
    if (starts(loc, "trendy-prize")) return "at the Trendy Game in Mabe Village";
    if (starts(loc, "fishing-")) return "at the fishing pond in Mabe Village";
    if (starts(loc, "dampe-")) return "with Dampe's chambers";
    if (starts(loc, "rapids-race")) return "as a Rapids Ride race prize";
    if (ends(loc, "-seashell-reward") && is_digits(loc.substr(0, loc.size() - 16)))
        return "at the Seashell Mansion (" + loc.substr(0, loc.size() - 16) + " shells)";
    if (dungeon_loc(loc, dn, rest)) {
        dn = get_or(dof, loc, dn);
        if (rest == "instrument") return "at the end of " + dungeon(dn).substr(3);
        static const char *const boss[] = {"moldorm", "genie", "slime-eye", "angler", "slime-eel", "facade", "eagle", "hothead"};
        for (const char *x : boss)
            if (rest == x) return "with the boss " + dungeon(dn);
        if (starts(rest, "pot-")) return "in a pot " + dungeon(dn);
        return dungeon(dn, replace_all(rest, "-", " "));
    }
    std::vector<std::string> parts = split(loc, '-'), kept;
    const std::vector<const char *> *aw = nullptr;
    for (const auto &e : AREA_WORDS)
        if (region == e.first) aw = &e.second;
    for (const std::string &w : parts)
        if (!aw || !in(*aw, w)) kept.push_back(w);
    if (kept.empty()) kept = parts;
    std::string detail;
    for (size_t i = 0; i < kept.size(); i++) detail += (i ? " " : "") + (kept[i] == "loose" ? std::string("on the ground") : kept[i]);
    if (ends(loc, "-sunken")) return area.empty() ? "under water" : "under water in " + area;
    if (loc.find("bonk-tree") != std::string::npos)
        return area.empty() ? "in a tree (dash into it)" : "in a tree in " + area + " (dash into it)";
    if (!area.empty()) return "in " + area + " (" + detail + ")";
    return "at " + detail;
}

// hint_text: [(text, red)] of one hint, English
std::vector<std::pair<std::string, bool>> hint_text(const Step &st, const DungeonOf &dof, const ViaMap &via) {
    std::string it = item_phrase(st.item);
    it = std::string(1, (char)toupper((unsigned char)it[0])) + it.substr(1);
    bool plural = in(PLURAL, st.item);
    return {{it, true}, {std::string(plural ? " lie " : " lies ") + place_phrase(st, dof, via) + "!", false}};
}

// hint_sheet.wrap: English, lines of at most LINE characters, words glued to a coloured name stay on its line
std::string hs_wrap(const std::vector<std::pair<std::string, bool>> &segs, const std::string &wait) {
    struct W { std::string w; bool red, glued; };
    std::vector<W> ws;
    std::string prev;
    auto space = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; };
    for (const auto &sg : segs) {
        const std::string &t = sg.first;
        size_t k = 0;
        for (size_t i = 0; i < t.size();) {
            while (i < t.size() && space(t[i])) i++;
            if (i == t.size()) break;
            size_t j = i;
            while (j < t.size() && !space(t[j])) j++;
            bool glued = k == 0 && !ws.empty() && !space(t[0]) && !(prev.size() && space(prev.back()));
            ws.push_back({t.substr(i, j - i), sg.second, glued});
            k++, i = j;
        }
        prev = t;
    }
    auto length = [](const std::vector<W> &line) {
        size_t n = 0;
        for (size_t i = 0; i < line.size(); i++) n += cp_len(line[i].w) + (i == 0 || line[i].glued ? 0 : 1);
        return n;
    };
    std::vector<std::vector<W>> lines;
    std::vector<W> cur;
    for (const W &w : ws) {
        std::vector<W> with = cur;
        with.push_back(w);
        if (!cur.empty() && length(with) > (size_t)LINE && !w.glued) {
            lines.push_back(cur);
            cur.clear();
        }
        cur.push_back(w);
    }
    if (!cur.empty()) lines.push_back(cur);
    std::string out;
    bool red = false;
    for (size_t i = 0; i < lines.size(); i++) {
        if (i) out += i % 2 == 0 ? wait + PAGE : std::string("\n");
        for (size_t j = 0; j < lines[i].size(); j++) {
            const W &w = lines[i][j];
            if (j && !w.glued) out += ' ';
            if (w.red != red) {
                out += w.red ? RED : WHITE;
                red = w.red;
            }
            out += w.w;
        }
    }
    if (red) out += WHITE;
    return out + wait + std::string(1, '\0');
}

std::string fmt_n(const std::string &t, const std::string &n) { return replace_all(t, "{n}", n); }  // .format(n=...)
std::string colon_of(const std::string &lang) { return ours::get_or(text::table(text::T, "colon"), lang, ": "); }
bool cjk(const std::string &lang) { return lang == "ja" || lang == "zh_cn" || lang == "zh_tw"; }

// place_parts: (the game's place name or None, our detail or None)
std::pair<Txt, Txt> place_parts(const Step &st, const DungeonOf &dof, const Names &names, const std::string &lang) {
    const std::string &loc = st.loc;
    std::string region = !st.door_area.empty() ? st.door_area : st.region;
    Txt area;
    if (const char *l = get(text::AREA_LABEL, region)) {
        std::string p;
        if (names.place(l, p)) area = {p, true};
    }
    auto det = [&](const char *key, const std::string &n = "") -> Txt {
        std::string t = tr1(text::table(text::DETAIL, key), lang);
        return {t.empty() ? t : fmt_n(t, n), false};
    };
    auto place = [&](const char *label) -> Txt {
        std::string p;
        return names.place(label, p) ? Txt{p, true} : Txt{};
    };
    std::string lv, dn, rest;
    if (pot_loc(loc, lv)) {
        if (const char *d = get(LEVEL_DUNGEON, lv)) return {dungeon_name(get_or(dof, loc, d), lang, &names), det("pot")};
        return {area, det("pot")};
    }
    if (starts(loc, "bush-")) return {area, det("bush")};
    if (starts(loc, "apple-tree-")) return {area, det("apple")};
    if (rock_loc(loc)) return {area, det("rock")};
    if (starts(loc, "dig-") || ends(loc, "-dig")) return {area, det("buried")};
    for (auto pl : std::vector<std::pair<const char *, const char *>>{
             {"shop-", "ToolShop"}, {"trendy-prize", "GameShop"}, {"fishing-", "FishingPond"}, {"dampe-", "DanpeiShack"}})
        if (starts(loc, pl.first)) return {place(pl.second), {}};
    if (starts(loc, "rapids-race")) return {place("RapidsRide"), det("race")};
    if (ends(loc, "-seashell-reward") && is_digits(loc.substr(0, loc.size() - 16)))
        return {place("ShellMansion"), det("shells", loc.substr(0, loc.size() - 16))};
    if (dungeon_loc(loc, dn, rest) && !get(PLACE, loc)) {
        Txt where = dungeon_name(get_or(dof, loc, dn), lang, &names);
        if (rest == "instrument") return {where, det("end")};
        if (in(BOSSES, rest)) return {where, det("boss")};
        if (starts(rest, "pot-")) return {where, det("pot")};
        return {where, {}};
    }
    if (ends(loc, "-sunken")) return {area, det("underwater")};
    if (loc.find("bonk-tree") != std::string::npos) return {area, det("tree")};
    return {area, area.s.empty() ? det("somewhere") : Txt{}};
}

// item_name: the item as the game names it in `lang` (bytes)
std::string item_name(const std::string &item, const Names &names, const std::string &lang) {
    std::string kind, dn, got;
    if (dungeon_item(item, kind, dn)) {
        std::string base;
        if (!names.item(kind, base)) {
            std::string di = need(DUNGEON_ITEM, kind);
            base = di.substr(di.find(' ') + 1);
        }
        return base + " (" + dungeon_name(dn, lang, &names).s + ")";
    }
    if (starts(item, "soul-")) {
        std::string soul = tr1(text::table(text::ITEM_OWN, "soul"), lang);
        return (soul.empty() ? "Soul" : soul) + " (" + title(replace_all(item.substr(5), "-", " ")) + ")";
    }
    std::string own = tr1(text::table(text::ITEM_OWN, item), lang);
    if (!own.empty()) return own;
    if (names.item(item, got)) return got;
    return strip_article(item_phrase(item));
}

std::string check_dungeon(const std::string &loc, const DungeonOf &dof) {  // 'D1'... or ""
    std::string lv, dn, rest;
    if (pot_loc(loc, lv))
        if (const char *d = get(LEVEL_DUNGEON, lv)) return get_or(dof, loc, d);
    if (dungeon_loc(loc, dn, rest) && !get(PLACE, loc)) return get_or(dof, loc, dn);
    return "";
}

// via_segments: ' (<entrance>: <area>)' for a door, else nothing
std::vector<Seg> via_segments(const Via &v, const Names *names, const std::string &lang) {
    const char *label = get(text::AREA_LABEL, v.area);
    std::string area;
    if (!label || !names || !names->place(label, area)) return {};
    bool c = cjk(lang);
    return {our((c ? "\xef\xbc\x88" : " (") + tr1(text::table(text::DETAIL, "entrance"), lang) + colon_of(lang)),
            game(area), our(c ? "\xef\xbc\x89" : ")")};
}

std::vector<Seg> entrance_segments(const std::string &via_key, const Names *names, const std::string &lang) {
    bool c = cjk(lang);
    Txt vn = dungeon_name(need(DUNGEON_KEY, via_key), lang, names);
    return {our((c ? "\xef\xbc\x88" : " (") + tr1(text::table(text::DETAIL, "entrance"), lang) + colon_of(lang)),
            {vn.s, false, vn.game}, our(c ? "\xef\xbc\x89" : ")")};
}

std::vector<Seg> hint_segments(const Step &st, const DungeonOf &dof, const Names &names, const std::string &lang,
                               const ViaMap &via) {
    std::pair<Txt, Txt> pd = place_parts(st, dof, names, lang);
    const Txt &place = pd.first, &detail = pd.second;
    std::string colon = colon_of(lang);
    std::string it = up_first(item_name(st.item, names, lang), 2);
    std::vector<Seg> seg = {game(it, true), our(colon)};
    if (!place.s.empty() && !detail.s.empty()) {
        seg.push_back({place.s, false, place.game});
        seg.push_back(our(cjk(lang) ? "\xef\xbc\x88" + detail.s + "\xef\xbc\x89" : " (" + detail.s + ")"));
    } else if (!place.s.empty()) {
        seg.push_back({place.s, false, place.game});
    } else {
        std::string d = !detail.s.empty() ? detail.s : tr1(text::table(text::DETAIL, "somewhere"), lang);
        seg.push_back(our(up_first(d)));
    }
    std::string key = key_of_dungeon(check_dungeon(st.loc, dof));
    const Via *v = via_of(via, key);
    if (v && v->door) {
        for (const Seg &s : via_segments(*v, &names, lang)) seg.push_back(s);
    } else if (v && v->key != key) {
        for (const Seg &s : entrance_segments(v->key, &names, lang)) seg.push_back(s);
    }
    return seg;
}

DungeonOf dungeon_of(SeedBuild &b) {
    DungeonOf out;
    yyjson_val *o = las::setting_val(b.decisions(), "dungeon-of"), *k;
    yyjson_obj_iter it = yyjson_obj_iter_with(o);
    while ((k = yyjson_obj_iter_next(&it))) {
        const char *v = yyjson_get_str(yyjson_obj_iter_get_val(k));
        if (v) out[yyjson_get_str(k)] = v;
    }
    return out;
}

// _item_row: Items.gsheet HintSheet, a copy of the Yoshi Doll's row with the dungeon map's model
void item_row(SeedBuild &b) {
    const std::string rel = "region_common/datasheets/Items.gsheet";
    src(b, rel);
    GSheet sheet = b.read_sheet("Items.gsheet");
    std::set<int> used;
    const GStruct *base = nullptr;
    for (const GStruct &r : sheet.values) {
        if (las::at(r, "symbol").s == ENTRY) return;
        used.insert(las::at(r, "itemID").i);
    }
    int free = -1;
    for (int i = 78; i > 70 && free < 0; i--)
        if (!used.count(i)) free = i;
    for (int i = 150; i < 200 && free < 0; i++)
        if (!used.count(i)) free = i;
    if (free < 0) throw std::runtime_error("hint sheet: no free itemID");  // (StopIteration)
    for (const GStruct &r : sheet.values)
        if (las::at(r, "symbol").s == "YoshiDoll" && !base) base = &r;
    if (!base) throw std::runtime_error("hint sheet: no YoshiDoll row");
    GStruct row = *base;
    las::at(row, "symbol") = las::gs(ENTRY);
    las::at(row, "itemID") = las::gi(free);
    las::at(row, "npcKey") = las::gs(MODEL_NPC);
    las::at(row, "gettingFlag") = las::gs("");
    sheet.values.push_back(row);
    b.write_sheet("Items.gsheet", sheet);
}

// an entry per name showing System:<name> (Item.bfevfl / Common.bfevfl)
void show_entries(SeedBuild &b, const std::string &file, const std::vector<std::string> &entries) {
    src(b, "region_common/event/" + file);
    EventFlow flow = b.read_flow(file);
    Flow f(b, flow.flowchart);
    for (const std::string &n : entries) {
        if (f.find_entry(n)) continue;
        f.add_entry_point(n);
        f.action_chain(n, {{"Dialog", "Show", {{"message", "System:" + n}}}});
    }
    b.write_flow(file, flow);
}

}  // namespace

void hint_sheet_apply(SeedBuild &b) {
    if (b.decider()) {
        std::string rec = decide_hint_sheet(b);
        if (!rec.empty()) b.decider()->set("hint-sheet", rec);
    }
    yyjson_val *hs = b.decisions().raw("hint-sheet");
    if (!hs) return;  // (no hint_steps.json)
    std::vector<Step> steps;
    yyjson_val *sv;
    yyjson_arr_iter it = yyjson_arr_iter_with(yyjson_obj_get(hs, "steps"));
    while ((sv = yyjson_arr_iter_next(&it)))
        steps.push_back({jstr(sv, "loc"), jstr(sv, "item"), jstr(sv, "region"), jstr(sv, "landmark"),
                         jstr(sv, "door-area"), jstr(sv, "door-name")});
    ViaMap via = read_via(yyjson_obj_get(hs, "entered-via"));
    DungeonOf dof = dungeon_of(b);

    // Tarin: after the Escape Bottle's AddItem, before the starting items and his own item
    if (!written(b, "region_common/event/Tarin.bfevfl")) return;
    EventFlow flow = b.read_flow("Tarin.bfevfl");
    {
        Flow f(b, flow.flowchart);
        auto next_name = [&](const std::string &n) -> std::string {
            const EvflEvent &e = f.event(n);
            if (e.next == EVFL_NONE) throw std::runtime_error("hint sheet: " + n + " leads nowhere");
            return flow.flowchart.events.at(e.next).name;
        };
        std::string after = "Event36", name = next_name("Event36");
        for (int i = 0; i < 4; i++) {
            const EvflEvent &e = f.event(name);
            if (e.type != EvflEvent::Action) break;  // (no actor_action: AttributeError)
            const EvflActor &a = flow.flowchart.actors.at(e.actor);
            const std::string &act = a.actions.at(e.action);
            const EvflValue *k = e.params.get("itemKey"), *ix = e.params.get("index");
            if (act == "AddItemByKey" && k && k->type == EvflValue::String && k->s == "Bottle" && ix &&
                ((ix->type == EvflValue::Int && ix->i == 2) || (ix->type == EvflValue::Float && ix->f == 2.0f))) {
                after = name;
                break;
            }
            if (e.next == EVFL_NONE) break;
            name = flow.flowchart.events.at(e.next).name;
        }
        std::string nxt = next_name(after);
        f.action_chain(after, {{"Link", "GenericItemGetSequenceByKey", {{"itemKey", ENTRY}, {"keepCarry", false}, {"messageEntry", ENTRY}}},
                               {"Inventory", "AddItemByKey", {{"itemKey", "Bottle"}, {"count", 1}, {"index", 0}, {"autoEquip", false}}}},
                       nxt);
    }
    b.write_flow("Tarin.bfevfl", flow);
    item_row(b);
    show_entries(b, "Item.bfevfl", {ENTRY});
    std::vector<std::string> names;
    for (size_t i = 0; i < steps.size(); i++) {
        char buf[32];
        snprintf(buf, sizeof buf, "HintSheet%02zu", i);
        names.push_back(buf);
    }
    names.push_back(DONE);
    show_entries(b, "Common.bfevfl", names);

    std::vector<std::vector<std::pair<std::string, bool>>> texts;
    for (const Step &s : steps) texts.push_back(hint_text(s, dof, via));
    std::vector<std::pair<std::string, bool>> get_text, done_text;
    for (const auto &s : GET_TEXT) get_text.push_back({s.first, s.second});
    for (const auto &s : DONE_TEXT) done_text.push_back({s.first, s.second});
    for (const std::string &rel : system_msbts(b)) {
        Msbt m = need_msbt(b, rel);
        const std::string *gb = m.text("GetBottle");
        std::string wait = last_wait(gb ? *gb : "");
        std::string lang = lang_of(rel);
        if (lang == "en") {
            m.set(ENTRY, hs_wrap(get_text, wait));
            for (size_t i = 0; i < steps.size(); i++) m.set(names[i], hs_wrap(texts[i], wait));
            m.set(DONE, hs_wrap(done_text, wait));
        } else {
            Names gn(b, rel);
            m.set(ENTRY, pages({fill(tr1(text::table(text::T, "hint-get"), lang),
                                     {{"name", our(tr1(text::table(text::T, "hint-name"), lang), true)}})},
                               wait, lang));
            for (size_t i = 0; i < steps.size(); i++) m.set(names[i], pages({hint_segments(steps[i], dof, gn, lang, via)}, wait, lang));
            m.set(DONE, pages({{our(tr1(text::table(text::T, "hint-done"), lang))}}, wait, lang));
        }
        write_msbt(b, rel, m);
    }

    // hints.tsv: S <entry> <location> <item> <copy n> <gated groups it needs, or ->; D <entry>
    const std::string *sj = b.decider() ? b.decider()->input("hint_steps.json") : nullptr;
    if (!sj) return;
    yyjson_doc *doc = yyjson_read(sj->data(), sj->size(), 0);
    if (!doc) throw std::runtime_error("hint sheet: bad hint_steps.json");
    std::string tsv;
    size_t i = 0;
    yyjson_val *st;
    yyjson_arr_iter si = yyjson_arr_iter_with(yyjson_obj_get(yyjson_doc_get_root(doc), "steps"));
    while ((st = yyjson_arr_iter_next(&si)) && i < steps.size()) {
        std::string needs;
        yyjson_val *nv;
        yyjson_arr_iter ni = yyjson_arr_iter_with(yyjson_obj_get(st, "needs"));
        while ((nv = yyjson_arr_iter_next(&ni))) needs += (needs.empty() ? "" : ",") + std::string(yyjson_get_str(nv));
        tsv += "S\t" + names[i++] + "\t" + jstr(st, "loc") + "\t" + jstr(st, "item") + "\t" +
               std::to_string(yyjson_get_sint(yyjson_obj_get(st, "n"))) + "\t" + (needs.empty() ? "-" : needs) + "\n";
    }
    yyjson_doc_free(doc);
    b.table_write("hints.tsv", tsv + "D\t" + DONE + "\n");
}

// ======== seed_hints.py
namespace {

struct Hint {
    std::string kind, where, area, dungeon, item, quest, says, loc, room, door_name;
    long n = 0;
    bool page = false;
};

const Seg PAGE_SEG = {"\f", false, false};
bool is_page(const Seg &s) { return !s.game && !s.red && s.s == "\f"; }

std::vector<std::vector<Seg>> paged(const std::vector<Seg> &segs) {
    std::vector<std::vector<Seg>> out(1);
    for (const Seg &s : segs) {
        if (is_page(s)) out.emplace_back();
        else out.back().push_back(s);
    }
    std::vector<std::vector<Seg>> kept;
    for (auto &p : out)
        if (!p.empty()) kept.push_back(p);
    return kept;
}

std::string place_en(const Hint &h, const ViaMap &via) {
    std::string a = !h.area.empty() ? h.area : h.dungeon;
    if (get(DUNGEON, a)) {
        std::string text = dungeon_en(a), key = key_of_dungeon(a);
        const Via *v = via_of(via, key);
        if (v && v->door) text += " (through " + via_phrase(*v) + ")";
        else if (v && v->key != key) text += " (through " + remove_the(dungeon_en(need(DUNGEON_KEY, v->key))) + "'s entrance)";
        return text;
    }
    if (const char *x = get(AREA, a)) if (*x) return x;
    if (const char *x = get(AREA, a + "-east")) if (*x) return x;
    return title(replace_all(a, "-", " "));
}

std::string cap(const std::string &s) { return s.empty() ? s : std::string(1, (char)toupper((unsigned char)s[0])) + s.substr(1); }

std::vector<Seg> english(const Hint &h, const ViaMap &via) {
    const std::string &k = h.kind;
    if (k == "barren") return {our("Nothing you need lies in " + place_en(h, via) + ".")};
    if (k == "needed") return {our("Something you need lies in " + place_en(h, via) + ".")};
    if (k == "quest") {
        std::string q = cap(need(QUEST_EN, h.quest));
        if (h.says == "need") return {our(q + " holds something you need.")};
        if (h.says == "junk") return {our(q + " holds nothing of note.")};
        return {our(q + " holds "), our(item_phrase(h.item), true), our(".")};
    }
    if (k == "item") {
        std::string verb = in(PLURAL, h.item) ? " lie " : " lies ";
        std::string where = "in " + place_en(h, via);
        if (!h.door_name.empty())
            where += h.door_name == "a cave" ? ", in a cave" : ", behind the door of " + h.door_name;
        return {our(cap(item_phrase(h.item)), true), our(verb + where + ".")};
    }
    if (k == "count") {
        if (!h.n) return {our("Nothing in this dungeon is needed.")};
        return {our(h.n == 1 ? std::string("This dungeon holds one thing you need.")
                             : "This dungeon holds " + std::to_string(h.n) + " things you need.")};
    }
    if (k == "boss") {
        if (h.says == "need") return {our("The boss here guards something you need.")};
        if (h.says == "junk") return {our("The boss here guards nothing you need.")};
        return {our("The boss here guards "), our(item_phrase(h.item), true), our(".")};
    }
    throw std::runtime_error("seed hints: kind " + k);
}

std::string sh_tr(const char *key, const std::string &lang) { return tr1(text::table(T, key), lang); }

std::vector<Seg> localized_(const Hint &h, const ViaMap &via, const Names &names, const std::string &lang) {
    bool c = cjk(lang);
    std::vector<Seg> way;
    auto up = [](const Seg &s) { return Seg{up_first(s.s), s.red, s.game}; };
    auto place = [&]() -> std::vector<Seg> {
        std::string a = !h.area.empty() ? h.area : h.dungeon;
        if (get(DUNGEON, a)) {
            Txt dn = dungeon_name(a, lang, &names);
            std::string key = key_of_dungeon(a);
            const Via *v = via_of(via, key);
            if (v && v->door) way = via_segments(*v, &names, lang);
            else if (v && v->key != key) {
                way = entrance_segments(v->key, &names, lang);
                way[1].game = true;  // (vn.encode(): bytes)
            }
            return {game(dn.s)};
        }
        const char *label = get(text::AREA_LABEL, a);
        if (!label || !*label) label = get(text::AREA_LABEL, a + "-east");
        std::string p;
        if (label && names.place(label, p)) return {game(p)};
        return {game(place_en(h, via))};
    };
    auto tail = [&]() -> std::vector<Seg> {
        if (way.empty() || !c) return {};
        std::vector<Seg> t = {PAGE_SEG};
        t.insert(t.end(), way.begin(), way.end());
        return t;
    };
    auto quest = [&]() -> std::vector<Seg> {
        if (h.quest == "trade") return {our(sh_tr("trade", lang))};
        std::string p;
        if (names.place(need(QUEST_LABEL, h.quest), p)) return {game(p)};
        return {game(need(QUEST_EN, h.quest))};
    };
    auto sentence = [&](const char *key, std::vector<Seg> p) {
        std::string tpl = sh_tr(key, lang);
        size_t at = tpl.find("{place}");
        if (at == std::string::npos || tpl.find("{place}", at + 1) != std::string::npos)
            throw std::runtime_error("seed hints: template " + std::string(key));  // (the unpacking's ValueError)
        std::string pre = tpl.substr(0, at), post = tpl.substr(at + 7);
        if (pre.empty()) p[0] = up(p[0]);
        if (!way.empty() && !c) p.insert(p.end(), way.begin(), way.end());
        std::vector<Seg> out;
        if (!pre.empty()) out.push_back(our(pre));
        out.insert(out.end(), p.begin(), p.end());
        out.push_back(our(post));
        std::vector<Seg> t = tail();
        out.insert(out.end(), t.begin(), t.end());
        return out;
    };
    auto item_colon = [&](std::vector<Seg> p) {
        std::string colon = lang == "fr" ? " : " : lang == "ja" ? "\xef\xbc\x9a " : colon_of(lang);
        std::vector<Seg> out = {up(game(item_name(h.item, names, lang), true)), our(colon)};
        out.insert(out.end(), p.begin(), p.end());
        std::vector<Seg> t = c ? tail() : way;
        out.insert(out.end(), t.begin(), t.end());
        return out;
    };
    const std::string &k = h.kind;
    if (k == "barren" || k == "needed") return sentence(k.c_str(), place());
    if (k == "quest") {
        if (h.says == "item") return item_colon(quest());
        return sentence(h.says == "need" ? "needed" : "junk", quest());
    }
    if (k == "item") return item_colon(place());
    if (k == "count") {
        if (!h.n) return {our(sh_tr("count0", lang))};
        return {our(fmt_n(sh_tr("count", lang), std::to_string(h.n)))};
    }
    if (k == "boss") {
        if (h.says == "item") return fill(sh_tr("boss-item", lang), {{"item", game(item_name(h.item, names, lang), true)}});
        return {our(sh_tr(h.says == "need" ? "boss-need" : "boss-junk", lang))};
    }
    throw std::runtime_error("seed hints: kind " + k);
}

std::vector<Seg> localized(const Hint &h, const ViaMap &via, const Names &names, const std::string &lang) {
    std::vector<Seg> segs = localized_(h, via, names, lang);
    bool char_wrap = false;
    for (const char *x : text::CHAR_WRAP) char_wrap |= lang == x;
    if (char_wrap)  // (Chinese: the game's names as text, so lines break inside them too)
        for (Seg &s : segs)
            if (s.game && s.s.find('\x0e') == std::string::npos) s.game = false;
    return segs;
}

// _examine_of: the DungeonOwlStatue.Examine event an entry shows (HasItem -> fork -> Examine)
uint16_t examine_of(EvflFlowchart &fc, const std::string &entry) {
    int ep = fc.find_entry_point(entry);
    if (ep < 0) throw std::runtime_error("seed hints: no entry " + entry);
    const EvflEvent &sw = fc.events.at(fc.entry_points[ep].main_event);
    uint16_t fork = EVFL_NONE;
    for (const auto &cs : sw.cases)
        if (cs.first == 1) fork = cs.second;
    for (uint16_t f : fc.events.at(fork).forks) {
        const EvflEvent &e = fc.events.at(f);
        if (e.type == EvflEvent::Action && fc.actors.at(e.actor).actions.at(e.action) == "Examine") return f;
    }
    throw std::runtime_error("seed hints: no Examine under " + entry);
}

void owl_flows(SeedBuild &b, const std::vector<Hint> &hints) {
    src(b, "region_common/event/DungeonOwlStatue.bfevfl");
    EventFlow flow = b.read_flow("DungeonOwlStatue.bfevfl");
    EvflFlowchart &fc = flow.flowchart;
    Flow f(b, fc);
    std::set<std::string> told;
    for (const Hint &h : hints)
        if (starts(h.where, "dungeon:")) told.insert(h.where.substr(8));
    for (const auto &e : DUNGEON_OWLS) {
        std::string entry = e.first;
        if (!told.count(entry)) continue;
        std::string msg = "Hint:LAROwl_" + entry.substr(8);
        if (const char *shared_entry = get(SPLIT, entry)) {
            if (f.find_entry(entry)) throw std::runtime_error("seed hints: entry " + entry + " exists");
            const EvflEvent &shared = fc.events.at(examine_of(fc, shared_entry));
            const EvflValue *m = shared.params.get("message");
            if (!m || !starts(m->s, "Hint:")) throw std::runtime_error("seed hints: " + entry);
            std::string ed = f.subflow("", "Beak_ed", {});
            std::string st = f.subflow("", "Beak_st", {});
            std::string ex = f.action("DungeonOwlStatue", "Examine", {{"message", msg}});
            std::string wait = f.action("Link", "PlayAnimation", {{"name", "wait"}, {"blendTime", 0.1}});
            std::string fork = f.fork("", {st, ex, wait}, ed).first;
            std::string nob = f.subflow("", "examine_NoBeak", {});
            std::string sw = f.sw("Inventory", "HasItem", {{"itemType", 63}, {"count", 1}}, {{0, nob}, {1, fork}});
            f.add_entry_point(entry);
            f.insert_after(entry, sw);
        } else {
            EvflEvent &ev = fc.events.at(examine_of(fc, entry));
            const EvflValue *m = ev.params.get("message");
            if (!m || !starts(m->s, "Hint:Lv")) throw std::runtime_error("seed hints: " + entry);
            ev.params.set("message", EvflValue::of_str(msg));
        }
    }
    b.write_flow("DungeonOwlStatue.bfevfl", flow);
}

void split_rooms(SeedBuild &b, const std::vector<Hint> &hints) {
    for (const Hint &h : hints) {
        std::string entry = h.where.size() > 8 ? h.where.substr(8) : "";
        const char *shared = get(SPLIT, entry);
        if (!starts(h.where, "dungeon:") || !shared) continue;
        const std::string &cell = h.room;
        std::string rel = "region_common/level/" + cell.substr(0, cell.rfind('_')) + "/" + cell + ".leb";
        std::string bytes;
        if (!b.read_bytes(rel, bytes)) throw std::runtime_error("seed hints: no room " + rel);
        LebRoom r = LebRoom::parse(bytes);
        std::vector<LebActor *> found;
        for (LebActor &a : r.actors) {
            const LebParam &p = a.params[0];
            if (starts(std::string(a.name.begin(), a.name.end()), "ObjDungeonOw") && p.kind == LebParam::Str &&
                std::string(p.s.begin(), p.s.end()) == shared)
                found.push_back(&a);
        }
        if (found.size() != 1) throw std::runtime_error("seed hints: statues in " + cell);
        found[0]->params[0] = las::str_param(entry);
        b.write_bytes(rel, r.repack());
    }
}

void phone_flow(SeedBuild &b, size_t n) {
    const std::string rel = "region_common/datasheets/GlobalFlags.gsheet";
    src(b, rel);
    GSheet sheet = b.read_sheet("GlobalFlags.gsheet");
    std::vector<std::string> flags;
    for (size_t i = 0; i + 1 < n; i++) flags.push_back("LARHintCall" + std::to_string(i));
    for (const std::string &fl : flags) {  // seed_tables.next_flag
        int top = -1;
        bool any = false;
        for (const GStruct &v : sheet.values) {
            int i = las::at(v, "index").i;
            if (i < 3800 && (!any || i > top)) top = i, any = true;
        }
        if (!any) throw std::runtime_error("seed hints: no flags");  // (max() of nothing)
        if (top + 1 >= 3800) throw std::runtime_error("out of GlobalFlags indices");
        if (top + 1 >= 2048) b.table_append("flags_ext", "event flag " + std::to_string(top + 1) + "\n");  // (flags_ext.c)
        sheet.values.push_back({{"symbol", las::gs(fl)}, {"index", las::gi(top + 1)}});
    }
    b.write_sheet("GlobalFlags.gsheet", sheet);
    src(b, "region_common/event/Telephone.bfevfl");
    EventFlow flow = b.read_flow("Telephone.bfevfl");
    EvflFlowchart &fc = flow.flowchart;
    Flow f(b, fc);
    const EvflEvent &e98 = f.event("Event98");
    if (e98.next == EVFL_NONE) throw std::runtime_error("seed hints: Event98 leads nowhere");
    const EvflEvent &after = fc.events.at(e98.next);
    std::string before, end, actor, act;
    if (after.type == EvflEvent::SubFlow && after.entry_point == "Telephone") {  // (the tunic swap)
        before = "Event98", end = after.name, actor = "Dialog", act = "Show";
    } else {
        uint16_t pick = event_index(fc, "Event97");
        const EvflEvent &p = fc.events[pick];
        if (p.type != EvflEvent::Action || fc.actors.at(p.actor).actions.at(p.action) != "JumpToAppropriateHintEntry")
            throw std::runtime_error("Telephone.bfevfl changed");
        if (f.event("Event218").next != pick) throw std::runtime_error("Telephone.bfevfl changed");
        before = "Event218", actor = "Telephone", act = "Examine";
    }
    std::string nxt = end;
    for (const std::string &fl : flags) nxt = f.action("EventFlags", "SetFlag", {{"symbol", fl}, {"value", false}}, nxt);
    nxt = f.action(actor, act, {{"message", "Telephone:LARHint" + std::to_string(n - 1)}}, nxt);
    for (size_t i = n - 1; i-- > 0;) {
        std::string give = f.action("EventFlags", "SetFlag", {{"symbol", flags[i]}, {"value", true}}, end);
        give = f.action(actor, act, {{"message", "Telephone:LARHint" + std::to_string(i)}}, give);
        nxt = f.sw("EventFlags", "CheckFlag", {{"symbol", flags[i]}}, {{0, give}, {1, nxt}});
    }
    f.insert_after(before, nxt);
    b.write_flow("Telephone.bfevfl", flow);
}

// phone_opening: the game's call opening up to Ulrira's voice (CJK: a new line after a wait becomes a new page)
std::string phone_opening(const std::string &raw) {
    std::vector<Tok> toks = tokens(raw);
    size_t i = 0;
    while (i < toks.size() && !(toks[i].tag && toks[i].raw.find("GRANDFATHER") != std::string::npos)) i++;
    if (i == toks.size()) throw std::runtime_error("seed hints: no call opening");  // (StopIteration)
    std::string out;
    for (size_t j = 0; j <= i; j++) {
        if (!toks[j].tag && toks[j].raw == "\n" && j && is_wait(toks[j - 1])) out += PAGE;
        else out += toks[j].raw;
    }
    return out;
}

}  // namespace

void seed_hints_apply(SeedBuild &b) {
    if (b.decider()) {
        std::string rec = decide_seed_hints(b);
        if (!rec.empty()) b.decider()->set("seed-hints", rec);
    }
    yyjson_val *sh = b.decisions().raw("seed-hints");
    if (!sh) return;  // (no seed_hints.json)
    std::vector<Hint> hints;
    yyjson_val *hv;
    yyjson_arr_iter it = yyjson_arr_iter_with(yyjson_obj_get(sh, "hints"));
    while ((hv = yyjson_arr_iter_next(&it))) {
        Hint h;
        h.kind = jstr(hv, "kind"), h.where = jstr(hv, "where"), h.area = jstr(hv, "area");
        h.dungeon = jstr(hv, "dungeon"), h.item = jstr(hv, "item"), h.quest = jstr(hv, "quest");
        h.says = jstr(hv, "says"), h.loc = jstr(hv, "loc"), h.room = jstr(hv, "room");
        h.door_name = jstr(hv, "door-name");
        h.n = (long)yyjson_get_sint(yyjson_obj_get(hv, "n"));
        hints.push_back(h);
    }
    ViaMap via = read_via(yyjson_obj_get(sh, "via"));
    // field / dungeon: {entry: hint} in the hints' order (a repeated entry: the later hint, in the first one's place)
    std::vector<std::pair<std::string, const Hint *>> field, dungeon;
    auto put = [](std::vector<std::pair<std::string, const Hint *>> &m, const std::string &k, const Hint *h) {
        for (auto &e : m)
            if (e.first == k) { e.second = h; return; }
        m.push_back({k, h});
    };
    std::vector<const Hint *> phone;
    for (const Hint &h : hints) {
        if (starts(h.where, "field:")) put(field, h.where.substr(6), &h);
        if (starts(h.where, "dungeon:")) put(dungeon, h.where.substr(8), &h);
        if (starts(h.where, "phone:")) phone.push_back(&h);
    }
    std::stable_sort(phone.begin(), phone.end(),
                     [](const Hint *x, const Hint *y) { return std::stoi(x->where.substr(6)) < std::stoi(y->where.substr(6)); });
    owl_flows(b, hints);
    split_rooms(b, hints);
    if (!phone.empty()) phone_flow(b, phone.size());
    for (const std::string &sys : system_msbts(b)) {
        std::string rel = replace_all(sys, "System.msbt", "Hint.msbt"), bytes;
        if (!b.read_bytes(rel, bytes)) continue;
        std::string lang = lang_of(rel);
        Msbt hm = Msbt::parse(bytes);
        std::string tel_rel = replace_all(rel, "Hint.msbt", "Telephone.msbt");
        Msbt tm = need_msbt(b, tel_rel);
        const std::string *nb = hm.text("NoBeakStoneOwl");
        if (!nb) throw std::runtime_error("seed hints: no NoBeakStoneOwl");
        std::string wait = last_wait(*nb);
        Names names(b, sys);  // (English doesn't use it)
        auto text = [&](const Hint &h) { return lang == "en" ? english(h, via) : localized(h, via, names, lang); };
        auto msg = [&](const Hint &h) { return pages(paged(text(h)), wait, lang); };
        for (const auto &e : field) hm.set(need(FIELD_OWLS, e.first), msg(*e.second));
        for (const auto &e : dungeon) {
            std::string label = "LAROwl_" + e.first.substr(8);
            hm.set(label, msg(*e.second));
            hm.copy_attr(label, "Lv1StoneOwl1");
        }
        const std::string *call = tm.text("TelephonGoldLeafCrow2");
        if (!call) throw std::runtime_error("seed hints: no TelephonGoldLeafCrow2");
        std::string opening = phone_opening(*call);
        size_t dial = opening.find("SE_ENV_ULULIRA_DIAL");
        if (dial == std::string::npos) throw std::runtime_error("seed hints: no dial sound");  // (IndexError)
        bool quote = opening.compare(dial + 19, 1, "\"") == 0;
        for (size_t i = 0; i < phone.size(); i++) {
            std::vector<Seg> seg = text(*phone[i]);
            if (quote) {
                seg.insert(seg.begin(), our("\""));
                seg.push_back(our("\""));
            }
            std::string label = "LARHint" + std::to_string(i);
            tm.set(label, opening + pages(paged(seg), wait, lang));
            tm.copy_attr(label, "TelephonGoldLeafCrow2");
        }
        write_msbt(b, rel, hm);
        write_msbt(b, tel_rel, tm);
    }
}

}  // namespace ours
}  // namespace rando
