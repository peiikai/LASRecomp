// The check tracker's data (tracker.json, tracker.tsv), as tools/rando/tracker.py builds it from the seed folder
// (outputs.h). Inputs from the file-writing steps: check_flags.tsv, shell_spots.tsv, apple_trees.tsv, pots.tsv,
// bushes.tsv, rocks.tsv, dungeon_rooms.tsv, dungeon_map.tsv, the seed's rooms and GlobalFlags.gsheet.
//
// Python behaviour kept:
//   - pots, bushes and rocks are named by scanning the game's rooms (pots.scan, bushes.scan with pits, rocks.scan)
//     and found by their actor's position in the seed's copy of the room, rounded to 2 places;
//   - dungeon_entrances reads spoiler.txt for shuffled dungeons without settings' dungeon-entrances, but the tracker
//     runs before las_headless renames the spoiler: never there for a new seed;
//   - the map's screens without a place of their own take the nearest screen's, the ones already filled included.
#if defined(__clang__)
#pragma clang fp contract(off)  // (float sums as Python rounds them: no fused multiply-add)
#endif
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>

#include "../fmt/fixed_hash.h"
#include "../fmt/gsheet.h"
#include "../fmt/leb.h"
#include "../fmt/sarc.h"
#include "outputs_lib.h"

namespace rando {
namespace solve {
namespace out {
namespace {

using Cell = std::pair<double, double>;

std::vector<std::string> list_dir(const std::string &path) {
    std::vector<std::string> out;
    if (DIR *d = opendir(path.c_str())) {
        while (dirent *e = readdir(d)) {
            std::string n = e->d_name;
            if (n == "." || n == ".." || starts(n, "._") || n == ".DS_Store") continue;  // (las_headless's listdir filter)
            out.push_back(n);
        }
        closedir(d);
    }
    return out;
}

// sorted(glob(<romfs>/region_common/level/<dir>/<prefix>*.leb)) as level-relative paths
std::vector<std::pair<std::string, std::string>> room_files(const Ctx &c, const std::string &only_level,
                                                           const std::string &prefix) {
    // (a folder of the game's: OutputsInput::list_game, else the romfs folder on disk)
    auto list = [&](const std::string &rel) {
        std::vector<std::string> names;
        if (!c.in.list_game) return list_dir(c.in.romfs + "/" + rel);
        if (!c.in.list_game(rel, names)) return std::vector<std::string>();
        std::vector<std::string> out;
        for (const std::string &n : names)
            if (!starts(n, "._") && n != ".DS_Store") out.push_back(n);
        return out;
    };
    std::vector<std::string> full;
    std::string base = "region_common/level";
    for (const std::string &lv : only_level.empty() ? list(base) : std::vector<std::string>{only_level})
        for (const std::string &f : list(base + "/" + lv))
            if (ends(f, ".leb") && starts(f, prefix) && f[0] != '.') full.push_back(lv + "/" + f);
    std::sort(full.begin(), full.end());
    std::vector<std::pair<std::string, std::string>> out;  // (level, room)
    for (const std::string &p : full) {
        size_t s = p.find('/');
        out.emplace_back(p.substr(0, s), p.substr(s + 1, p.size() - s - 5));
    }
    return out;
}

std::string key2(double x, double z) {  // (round(x, 2), round(z, 2)) as a dict key (-0.0 == 0.0)
    std::string a = fmt("%.2f", x), b = fmt("%.2f", z);
    if (a == "-0.00") a = "0.00";
    if (b == "-0.00") b = "0.00";
    return a + "," + b;
}

double py_floordiv(double a, double b) {  // a // b on floats (CPython's float_floor_div)
    double mod = fmod(a, b);
    double div = (a - mod) / b;
    if (mod != 0) {
        if ((b < 0) != (mod < 0)) div -= 1.0;
    }
    if (div != 0) {
        double f = floor(div);
        if (div - f > 0.5) f += 1.0;
        return f;
    }
    return copysign(0.0, a / b);
}

bool cell_of_field(const std::string &room, Cell &c) {  // re.match(r'Field_(\d\d)([A-P])', room)
    if (room.size() < 9 || room.compare(0, 6, "Field_") != 0 || !isdigit((unsigned char)room[6]) ||
        !isdigit((unsigned char)room[7]) || room[8] < 'A' || room[8] > 'P')
        return false;
    c = Cell(room[8] - 65 + 0.5, atoi(room.substr(6, 2).c_str()) - 1 + 0.5);
    return true;
}

// the level of a room name: re.match(r'(.+)_\d\d[A-Z]', room).group(1) (greedy: the last such '_')
bool room_level(const std::string &room, std::string &lv) {
    for (size_t p = room.size(); p-- > 1;) {
        if (room[p] == '_' && p + 3 < room.size() && isdigit((unsigned char)room[p + 1]) &&
            isdigit((unsigned char)room[p + 2]) && room[p + 3] >= 'A' && room[p + 3] <= 'Z') {
            lv = room.substr(0, p);
            return true;
        }
    }
    return false;
}

std::string spelled(const std::string &name) {
    std::string s = name;
    std::replace(s.begin(), s.end(), '-', ' ');
    if (!s.empty()) s[0] = (char)toupper((unsigned char)s[0]);
    return s;
}

std::string house(const std::string &level) {
    for (const auto &e : odata::CN_HOUSES)
        if (starts(level, e.first)) return e.second;
    std::string o;  // re.sub(r'(?<=[a-z])(?=[A-Z0-9])', ' ', level)
    for (size_t i = 0; i < level.size(); i++) {
        if (i && islower((unsigned char)level[i - 1]) && (isupper((unsigned char)level[i]) || isdigit((unsigned char)level[i])))
            o += ' ';
        o += level[i];
    }
    return o;
}

std::string direction(double fx, double fz) {
    std::string v = fz < 1.0 / 3 ? "north" : fz > 2.0 / 3 ? "south" : "";
    std::string h = fx < 1.0 / 3 ? "west" : fx > 2.0 / 3 ? "east" : "";
    if (!v.empty() && !h.empty()) return v + "-" + h;
    return !v.empty() ? v : !h.empty() ? h : "centre";
}

std::string display_name(const std::string &name, const std::string &kind = "", const std::string &area = "",
                         const std::string &where = "") {
    if (const char *n = odata::get(odata::CN_NAMES, name)) return n;
    static const std::regex trendy("^trendy-prize-(\\d+)$"), shop("^shop-slot(\\d)-(2nd|3rd)$"), dn("^(D\\d)-(.*)$"),
        pot("^pot-\\d\\d[A-P]-");
    std::smatch m;
    if (std::regex_search(name, m, trendy)) return "Trendy Game: prize " + m[1].str();
    if (std::regex_search(name, m, shop))
        return "Town Tool Shop: shelf " + m[1].str() + ", " + (m[2].str() == "2nd" ? "second" : "third") + " item";
    if (std::regex_search(name, m, dn)) {
        std::string rest = m[2].str();
        if (!std::regex_search(rest, pot)) return spelled(rest);
    }
    if (const char *w = odata::get(odata::CN_KIND_WORD, kind)) {
        std::string s = w;
        if (!area.empty()) s += ", " + area;
        return where.empty() ? s : s + " (" + where + ")";
    }
    return spelled(name);
}

std::string kind_of(const std::string &name, const Value &v, const std::map<std::string, std::pair<std::string, std::string>> &shell_rooms) {
    static const std::regex rock("^rock-\\d\\d[A-P]-"), dig("^dig-\\d\\d[A-P]-\\d\\d$"), dpot("^D\\d-pot-");
    if (starts(name, "bush-")) return "bush";
    if (std::regex_search(name, rock)) return "rock";
    if (std::regex_search(name, dig)) return "dig";
    if (starts(name, "pot-") || std::regex_search(name, dpot)) return "pot";
    Value content = v.get("content"), sub = v.get("subtype");
    if (content == Value("seashell") || shell_rooms.count(name) || ends(name, "-seashell-reward")) return "shell";
    if (content == Value("heart-piece")) return "heart";
    if (sub == Value("shop") || starts(name, "shop-") || starts(name, "trendy-prize")) return "shop";
    std::string cond;
    bool first = true;
    for (const auto &e : v.dict())
        if (starts(e.key, "condition")) cond += (first ? "" : " ") + py_str(e.value), first = false;
    bool soul = false;
    for (size_t p = cond.find("soul-"); p != std::string::npos && !soul; p = cond.find("soul-", p + 1))
        soul = p == 0 || !(isalnum((unsigned char)cond[p - 1]) || cond[p - 1] == '_');
    if (sub == Value("boss") || content == Value("heart-container") || soul) return "boss";
    if (sub == Value("chest")) return "chest";
    return "other";
}

std::string condition(const Value &node, const odata::StrList &levels) {
    std::string out;
    for (const char *lv : levels) {
        Value c = node.get(std::string("condition-") + lv);
        if (!c.truthy()) continue;
        out += (out.empty() ? "(" : " | (") + py_str(c) + ")";
    }
    return out;
}

std::vector<std::vector<std::string>> tsv_rows(const std::string &data) {
    std::vector<std::vector<std::string>> rows;
    for (std::string ln : split(data, '\n')) rows.push_back(split(ln, '\t'));
    if (!data.empty() && data.back() == '\n') rows.pop_back();  // (no line after the last newline)
    if (data.empty()) rows.clear();
    return rows;
}

std::vector<std::string> ws_split(const std::string &s) {  // str.split()
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && isspace((unsigned char)s[i])) i++;
        size_t j = i;
        while (j < s.size() && !isspace((unsigned char)s[j])) j++;
        if (j > i) out.push_back(s.substr(i, j - i));
        i = j;
    }
    return out;
}

bool is_num(const std::string &s) {  // re.match(r'-?[\d.]+$', s)
    size_t i = s.size() && s[0] == '-' ? 1 : 0;
    if (i >= s.size()) return false;
    for (; i < s.size(); i++)
        if (!isdigit((unsigned char)s[i]) && s[i] != '.') return false;
    return true;
}

// tracker.field_areas
struct FieldAreas {
    bool ok = false;
    int gh = 0, gw = 0;
    std::vector<std::vector<int>> cells;
    std::map<std::pair<int, int>, int> screen;
    std::map<int, std::array<double, 4>> boxes;
    int area_at(double x, double z) const {
        long c = (long)(x / 1.5), r = (long)(z / 1.5);
        if (0 <= r && r < gh && 0 <= c && c < gw && cells[(size_t)r][(size_t)c] >= 0) return cells[(size_t)r][(size_t)c];
        int sr = (int)std::min(15.0, std::max(0.0, py_floordiv(z, 12))), sc = (int)std::min(15.0, std::max(0.0, py_floordiv(x, 15)));
        auto it = screen.find({sr, sc});
        return it == screen.end() ? -1 : it->second;
    }
};

FieldAreas field_areas(Ctx &c) {
    FieldAreas fa;
    std::string data, ln_data, ui_data;
    if (!c.romfs_file("region_common/level/Field/Field.lvb", data) ||
        !c.romfs_file("region_common/datasheets/LocationName.gsheet", ln_data) ||
        !c.romfs_file("region_common/datasheets/UiFieldMapLocationName.gsheet", ui_data))
        return fa;
    FixedHash fh = FixedHash::parse(data);
    std::vector<int64_t> zones;
    for (const FhEntry &z : fh.find("zone")->sub->entries) zones.push_back((int64_t)read_le(z.data, 0, 4));
    const std::string &grid = fh.find("grid")->data;
    const std::string &gc = fh.find("gridConfig")->data;
    fa.gh = (int)read_le(gc, 0, 2), fa.gw = (int)read_le(gc, 2, 2);
    GSheet ln = GSheet::parse(ln_data), ui = GSheet::parse(ui_data);
    auto num = [](const GValue *v) -> double { return !v ? 0 : v->kind == GValue::Float ? v->f : v->kind == GValue::Bool ? v->b : v->i; };
    struct LocRow { double lo, hi; std::string label; };
    std::vector<LocRow> locs;
    for (const GStruct &r : ln.values)
        if (num(gfield(r, "mType")) == 0)
            locs.push_back({num(gfield(r, "mMinZoneID")), num(gfield(r, "mMaxZoneID")), gfield(r, "mNameLabel")->s});
    std::map<std::string, int> label;
    for (const GStruct &r : ui.values) {
        std::string l = gfield(r, "mNameLabel")->s;
        if (ends(l, "_map")) label[l.substr(0, l.size() - 4)] = (int)num(gfield(r, "mId"));
    }
    std::vector<int> zp;
    for (int64_t z : zones) {
        int p = -1;
        for (const LocRow &r : locs)
            if (r.lo <= (double)z && (double)z <= r.hi) {
                auto it = label.find(r.label);
                p = it == label.end() ? -1 : it->second;
                break;
            }
        zp.push_back(p);
    }
    fa.cells.assign((size_t)fa.gh, std::vector<int>((size_t)fa.gw, -1));
    for (int r = 0; r < fa.gh; r++)
        for (int cc = 0; cc < fa.gw; cc++) {
            int zi = (unsigned char)grid[(size_t)((r * fa.gw + cc) * 8 + 5)];
            fa.cells[(size_t)r][(size_t)cc] = zi < (int)zp.size() ? zp[(size_t)zi] : -1;
        }
    std::vector<std::pair<int, int>> order;  // (the dict's order: row-major)
    for (int row = 0; row < 16; row++)
        for (int col = 0; col < 16; col++) {
            std::vector<std::pair<int, int>> n;  // (place, count) in first-seen order
            for (int r = row * 8; r < row * 8 + 8; r++)
                for (int cc = col * 10; cc < col * 10 + 10; cc++)
                    if (r < fa.gh && cc < fa.gw && fa.cells[(size_t)r][(size_t)cc] >= 0) {
                        int v = fa.cells[(size_t)r][(size_t)cc];
                        auto it = std::find_if(n.begin(), n.end(), [&](const std::pair<int, int> &p) { return p.first == v; });
                        if (it == n.end()) n.emplace_back(v, 1);
                        else it->second++;
                    }
            int best = -1, bc = 0;
            for (const auto &p : n)
                if (p.second > bc) best = p.first, bc = p.second;
            fa.screen[{row, col}] = best;
            order.emplace_back(row, col);
        }
    for (const auto &rc : order) {
        if (fa.screen[rc] >= 0) continue;
        bool found = false;
        std::pair<int, int> best(0, -1);
        for (const auto &o : order) {  // (live: screens filled before count)
            int b = fa.screen[o];
            if (b < 0) continue;
            std::pair<int, int> t(abs(o.first - rc.first) + abs(o.second - rc.second), b);
            if (!found || t < best) best = t, found = true;
        }
        fa.screen[rc] = best.second;
    }
    for (int r = 0; r < fa.gh; r++)
        for (int cc = 0; cc < fa.gw; cc++) {
            int a = fa.cells[(size_t)r][(size_t)cc];
            if (a < 0) {
                auto it = fa.screen.find({r / 8, cc / 10});
                a = it == fa.screen.end() ? -1 : it->second;
            }
            if (a < 0) continue;
            auto it = fa.boxes.find(a);
            std::array<double, 4> b = it == fa.boxes.end() ? std::array<double, 4>{1e9, 1e9, -1e9, -1e9} : it->second;
            b = {std::min(b[0], cc * 1.5), std::min(b[1], r * 1.5), std::max(b[2], cc * 1.5 + 1.5), std::max(b[3], r * 1.5 + 1.5)};
            fa.boxes[a] = b;
        }
    fa.ok = true;
    return fa;
}

// dungeon_gen.map_shifts / map_anims
std::map<int, Cell> map_shifts(Ctx &c) {
    std::map<int, Cell> out;
    std::string raw;
    if (!c.romfs_file("region_common/ui/Game.arc", raw)) throw PyError("FileNotFoundError", "Game.arc");
    Sarc sarc = Sarc::parse(raw);
    static const std::regex name("^anim/Dg_DgnMap_Dgn_(\\d+)\\.bflan$");
    for (const auto &f : sarc.files) {
        std::smatch m;
        if (!std::regex_search(f.first, m, name)) continue;
        const std::string &b = f.second;
        size_t p = b.find("N_MapPosition_00");
        size_t t = p != std::string::npos ? b.find("FLPA", p) : std::string::npos;
        if (t == std::string::npos) continue;
        double xy[2] = {0.0, 0.0};
        int n = (unsigned char)b[t + 4];
        for (int i = 0; i < n; i++) {
            size_t io = t + (size_t)read_le(b, t + 8 + (size_t)i * 4, 4);
            int target = (unsigned char)b[io + 1];
            int nkeys = (int)read_le(b, io + 4, 2);
            size_t koff = (size_t)read_le(b, io + 8, 4);
            if (target < 2 && nkeys) xy[target] = read_f32(b, io + koff + 4);
        }
        out[atoi(m[1].str().c_str())] = Cell(xy[0], xy[1]);
    }
    return out;
}

}  // namespace

std::string tracker_check_name(const std::string &name) { return display_name(name); }

void build_tracker(Ctx &c, const Value &settings) {
    using namespace odata;
    const Value logic = Value::parse_json(c.file("logic.json")->data(), c.file("logic.json")->size());
    std::string logic_level = settings.get("logic", Value("basic")).s();
    const StrList *lp = get(TR_LEVELS, logic_level);
    static const StrList basic = {"basic"};
    const StrList &levels = lp ? *lp : basic;
    // flags: the seed's GlobalFlags sheet, else the game's
    std::map<std::string, int64_t> flags;
    {
        std::string data;
        if (!c.seed_file("romfs/region_common/datasheets/GlobalFlags.gsheet", data))
            c.romfs_file("region_common/datasheets/GlobalFlags.gsheet", data);
        GSheet sh = GSheet::parse(data);
        for (const GStruct &r : sh.values) flags[gfield(r, "symbol")->s] = gfield(r, "index")->i;
    }
    std::map<std::string, int64_t> check_flags;
    {
        std::string data;
        if (c.seed_file("check_flags.tsv", data))
            for (const std::string &ln : split(data, '\n'))
                if (ln.find('\t') != std::string::npos) {
                    std::vector<std::string> f = split(ln, '\t');
                    check_flags[f[0]] = atoll(f[1].c_str());
                }
    }
    std::map<std::string, std::unique_ptr<LebRoom>> room_cache;
    auto room_file = [&](const std::string &room) -> LebRoom * {
        std::string lv;
        if (!room_level(room, lv)) throw PyError("AttributeError", "'NoneType' object has no attribute 'group'");
        auto it = room_cache.find(room);
        if (it != room_cache.end()) return it->second.get();
        std::string data;
        LebRoom *r = nullptr;
        if (c.level_file("region_common/level/" + lv + "/" + room + ".leb", data)) r = new LebRoom(LebRoom::parse(data));
        room_cache[room].reset(r);
        return r;
    };
    // where interiors are
    std::map<std::string, Cell> level_cell, loc_door_cell, door_cell;
    for (const Door &d : DOORS) {
        Cell cl;
        bool ok = cell_of_field(d.field_room, cl);
        if (ok) door_cell[d.id] = cl;
        if (ok && !level_cell.count(d.dest_level)) level_cell[d.dest_level] = cl;
        // (setdefault with None: a door without a cell takes the level first; none in entrance_data.yml)
        for (const char *l : d.inside)
            if (ok) loc_door_cell[l] = cl;
            else loc_door_cell.erase(l);
    }
    Value nodes = Value::new_list();
    for (const auto &e : logic.dict()) {
        Value t = e.value.get("type");
        static const char *const kinds[] = {"region", "item", "room", "interaction", "enemy", "trick", "follower"};
        bool keep = false;
        for (const char *k : kinds) keep = keep || t == Value(k);
        if (!keep) continue;
        Value n = Value::new_dict();
        n.set("n", e.key), n.set("t", t), n.set("r", e.value.get("region", Value(""))), n.set("c", condition(e.value, levels));
        std::string tl = condition(e.value, TR_TRICK_LEVELS.size() > levels.size() ? TR_TRICK_LEVELS : levels);
        if (tl != n.at("c").s()) n.set("trick", tl);
        for (const char *x : {"pit", "heavy"})
            if (e.value.get(std::string("condition-") + x).truthy()) n.set(x, e.value.at(std::string("condition-") + x));
        nodes.list().push_back(n);
    }
    // dungeon icons
    Value groups = Value::new_list();
    std::map<std::string, int> gidx;
    std::map<std::string, Cell> door_of;
    {
        Value ent = settings.get("dungeon-entrances");
        if (!ent.truthy()) ent = Value::new_dict();  // (shuffle-dungeons without it: spoiler.txt isn't there yet)
        std::vector<std::pair<std::string, std::string>> de;
        if (settings.get("mix-dungeon-entrances").truthy()) {
            for (const auto &e : ent.dict()) de.emplace_back(e.key, py_str(e.value));
        } else
            for (const auto &k : TR_ENTRANCE_KEYS) de.emplace_back(k.first, ent.has(k.first) ? py_str(ent.at(k.first)) : k.first);
        for (const auto &d : de) {
            const char *lv_door = get(TR_ENTRANCE_KEYS, d.first), *lv_in = get(TR_ENTRANCE_KEYS, d.second);
            const std::map<std::string, Cell> &m = lv_door ? level_cell : door_cell;
            auto it = m.find(lv_door ? lv_door : d.first);
            if (lv_in && it != m.end()) door_of[get(TR_DUNGEONS, lv_in)] = it->second;
        }
    }
    auto group = [&](const std::string &dk) -> int {
        auto it = gidx.find(dk);
        if (it != gidx.end()) return it->second;
        Cell cl(8.0, 8.0);
        auto d = door_of.find(dk);
        if (d != door_of.end()) cl = d->second;
        else {
            for (const auto &e : TR_DUNGEONS)
                if (dk == e.second) {
                    auto l = level_cell.find(e.first);
                    if (l != level_cell.end()) cl = l->second;
                    break;
                }
        }
        int g = (int)groups.list().size();
        gidx[dk] = g;
        Value gr = Value::new_dict();
        gr.set("label", dk), gr.set("x", cl.first), gr.set("y", cl.second);
        groups.list().push_back(gr);
        return g;
    };
    // shuffled layouts
    std::vector<std::pair<std::pair<std::string, std::string>, std::string>> layout;  // (insertion order)
    auto layout_find = [&](const std::pair<std::string, std::string> &k) -> std::string * {
        for (auto &e : layout)
            if (e.first == k) return &e.second;
        return nullptr;
    };
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> rooms_at;
    {
        std::string data;
        if (c.seed_file("dungeon_rooms.tsv", data))
            for (const std::string &ln : split(data, '\n')) {
                std::vector<std::string> f = split(ln, '\t');
                if (f.size() < 3) continue;
                if (std::string *r = layout_find({f[0], f[1]})) *r = f[2];
                else layout.push_back({{f[0], f[1]}, f[2]});
                if (!(f.size() > 3 && f[3] == "copy")) rooms_at[f[2]].emplace_back(f[0], f[1]);
            }
    }
    std::map<std::pair<std::string, std::string>, Value> shifts;
    {
        std::string data;
        if (c.seed_file("dungeon_map.tsv", data))
            for (const std::string &ln : split(data, '\n')) {
                std::vector<std::string> f = split(ln, '\t');
                if (f.size() >= 4) shifts[{f[0], f[1]}] = Value(List{Value(strtod(f[2].c_str(), nullptr)), Value(strtod(f[3].c_str(), nullptr))});
            }
    }
    Value dungeon_of = settings.get("dungeon-of");
    if (!dungeon_of.truthy()) dungeon_of = Value::new_dict();
    std::set<std::string> shuffled_levels;
    for (const auto &e : layout) shuffled_levels.insert(e.first.first);
    // (dungeon key, cell) a dungeon check sits in now ("" / Value(): None)
    auto where = [&](const std::string &name, const std::string &room, const std::string &level) -> std::pair<Value, Value> {
        Value dk = dungeon_of.get(name);
        auto ra = rooms_at.find(room);
        if (!room.empty() && ra != rooms_at.end()) {
            const auto &places = ra->second;
            std::pair<std::string, std::string> pl = places[0];
            for (const auto &p : places) {
                const char *d = get(TR_DUNGEONS, p.first);
                if (dk.truthy() && d && dk == Value(d)) { pl = p; break; }
            }
            const char *d = get(TR_DUNGEONS, pl.first);
            return {d ? Value(d) : Value(), Value(pl.second)};
        }
        const char *dl = get(TR_DUNGEONS, level);
        if (dl && !shuffled_levels.count(level))
            return {dk.truthy() ? dk : Value(dl), room.empty() ? Value() : Value(room.substr(room.rfind('_') + 1))};
        if (dk.truthy()) return {dk, Value()};
        if (name.size() >= 3 && name[0] == 'D' && isdigit((unsigned char)name[1]) && name[2] == '-') return {Value(name.substr(0, 2)), Value()};
        return {dl ? Value(dl) : Value(), Value()};
    };
    // Secret Seashell spots and apple trees: their rooms and positions
    std::map<std::string, std::pair<std::string, std::string>> shell_rooms, apple_rooms;
    std::map<std::string, Cell> shell_pos;
    std::map<std::string, int64_t> shell_flags;
    {
        std::string data;
        if (c.seed_file("shell_spots.tsv", data))
            for (const auto &cr : tsv_rows(data)) {
                if (cr.size() <= 4 || starts(cr[0], "#")) continue;
                shell_rooms[cr[0]] = {cr[3], cr[4]};
                if (cr[3] == "Field" && cr.size() > 7 && is_num(cr[5]) && is_num(cr[7]))
                    shell_pos[cr[0]] = Cell(strtod(cr[5].c_str(), nullptr), strtod(cr[7].c_str(), nullptr));
                if (cr.size() > 12) {
                    std::string s = cr[12];
                    size_t k = 0;
                    while (k < s.size() && s[k] == '-') k++;
                    bool dig = k < s.size();
                    for (size_t q = k; q < s.size(); q++) dig = dig && isdigit((unsigned char)s[q]);
                    if (dig) shell_flags[cr[0]] = atoll(s.c_str());
                }
            }
        if (c.seed_file("apple_trees.tsv", data))
            for (const auto &cr : tsv_rows(data)) {
                if (cr.size() <= 7 || starts(cr[0], "#")) continue;
                apple_rooms[cr[0]] = {cr[3], cr[4]};
                shell_pos[cr[0]] = Cell(strtod(cr[5].c_str(), nullptr), strtod(cr[7].c_str(), nullptr));
            }
    }
    std::set<std::string> absent = str_set(settings.get("absent-locations")), ex = str_set(settings.get("excluded-locations"));
    absent.insert(ex.begin(), ex.end());
    std::map<std::string, std::pair<std::string, std::string>> loc_rooms;
    for (const LocRoom &r : LOC_ROOMS) loc_rooms[r.name] = {r.room ? r.room : "", r.level ? r.level : ""};
    Value checks = Value::new_list();
    std::vector<std::pair<Value, std::vector<Cell>>> region_cells;
    struct Pos { bool has_cell; Cell cell; int g; std::string room; std::pair<Value, Value> dg; };
    std::vector<std::pair<std::string, Pos>> pos;
    static const std::regex skip("^(D\\d-pot-|rock-\\d\\d[A-P]-)"), digre("^dig-(\\d\\d)([A-P])-(\\d\\d)$"), dre("^D(\\d)-");
    for (const auto &e : logic.dict()) {
        const std::string &name = e.key;
        const Value &v = e.value;
        if (v.get("type") != Value("item") || starts(name, "starting-") || absent.count(name)) continue;
        std::string room, level;
        auto lr = loc_rooms.find(name);
        if (lr != loc_rooms.end()) room = lr->second.first, level = lr->second.second;
        if (room.empty() && shell_rooms.count(name)) level = shell_rooms[name].first, room = shell_rooms[name].second;
        if (room.empty() && apple_rooms.count(name)) level = apple_rooms[name].first, room = apple_rooms[name].second;
        if (starts(name, "pot-") || starts(name, "bush-") || std::regex_search(name, skip)) continue;
        std::smatch m;
        Value region = v.get("region");
        if (std::regex_search(name, m, digre)) {
            int row = atoi(m[1].str().c_str()), col = m[2].str()[0] - 65, tile = atoi(m[3].str().c_str());
            Cell cl;
            cell_of_field("Field_" + m[1].str() + m[2].str(), cl);
            Value ck = Value::new_dict();
            ck.set("n", name), ck.set("x", cl.first), ck.set("y", cl.second), ck.set("g", -1), ck.set("k", "dig");
            ck.set("a", region.truthy() ? region : Value(""));
            ck.set("wx", col * 15.0 + tile % 10 * 1.5 + 0.75), ck.set("wz", (row - 1) * 12.0 + tile / 10 * 1.5 + 0.75);
            ck.set("flag", 24576 + ((row - 1) * 16 + col) * 80 + tile);
            checks.list().push_back(ck);
            continue;
        }
        Pos p{false, Cell(), -1, room, {Value(), Value()}};
        if (get(TR_DUNGEONS, level) || dungeon_of.has(name) || (std::regex_search(name, dre) && room.empty())) {
            p.dg = where(name, room, level);
            if (p.dg.first.truthy()) {
                p.g = group(p.dg.first.s());
                const Value &gr = groups.list()[(size_t)p.g];
                p.cell = Cell(gr.at("x").f(), gr.at("y").f()), p.has_cell = true;
            }
        } else if (!room.empty() && starts(room, "Field")) {
            p.has_cell = cell_of_field(room, p.cell);
        } else if (loc_door_cell.count(name)) {
            p.cell = loc_door_cell[name], p.has_cell = true;
        } else if (!level.empty() && level_cell.count(level)) {
            p.cell = level_cell[level], p.has_cell = true;
        }
        if (p.has_cell && p.g < 0) {
            auto it = std::find_if(region_cells.begin(), region_cells.end(), [&](const std::pair<Value, std::vector<Cell>> &x) { return x.first == region; });
            if (it == region_cells.end()) region_cells.push_back({region, {p.cell}});
            else it->second.push_back(p.cell);
        }
        pos.emplace_back(name, p);
    }
    auto centroid = [&](const Value &region, Cell &out) {
        for (const auto &r : region_cells)
            if (r.first == region) {
                double sx = 0, sy = 0;
                for (const Cell &cl : r.second) sx += cl.first;
                for (const Cell &cl : r.second) sy += cl.second;
                out = Cell(sx / (double)r.second.size(), sy / (double)r.second.size());
                return true;
            }
        return false;
    };
    for (auto &pe : pos) {
        const std::string &name = pe.first;
        Pos &p = pe.second;
        const Value &node = logic.at(name);
        Cell cell = p.cell;
        if (!p.has_cell && !centroid(node.get("region"), cell)) continue;
        int64_t flag = check_flags.count(name) ? check_flags[name] : -1;
        Value sub = node.get("subtype");
        try {
            if (check_flags.count(name)) {
            } else if (const char *f1 = get(TR_EVENT_FLAGS, name)) {
                flag = flags.count(f1) ? flags[f1] : -1;
            } else if (const char *f2 = get(TR_HEART_FLAGS, name)) {
                flag = flags.count(f2) ? flags[f2] : -1;
            } else if (const char *f3 = get(TR_SHELL_FLAGS, name)) {
                flag = flags.count(f3) ? flags[f3] : -1;
            } else if (shell_flags.count(name)) {
                flag = shell_flags[name];
            } else if (!p.room.empty() && (sub == Value("chest") || sub == Value("standing") || sub == Value("drop"))) {
                LebRoom *r = room_file(p.room);
                std::vector<const LebActor *> boxes, keys;
                if (r)
                    for (const LebActor &a : r->actors) {
                        if (a.type == 0xf7) boxes.push_back(&a);
                        if (a.type == 0xa9) keys.push_back(&a);
                    }
                if (sub == Value("chest") && boxes.size() == 1 && boxes[0]->switches[1].first == 1) flag = boxes[0]->switches[1].second;
                else if (keys.size() == 1 && keys[0]->switches[1].first == 1) flag = keys[0]->switches[1].second;
            }
        } catch (const std::exception &) {
        }
        Value ck = Value::new_dict();
        Value region = node.get("region");
        ck.set("n", name), ck.set("x", cell.first), ck.set("y", cell.second), ck.set("g", p.g), ck.set("flag", (long long)flag);
        ck.set("k", kind_of(name, node, shell_rooms)), ck.set("a", region.truthy() ? region : Value(""));
        ck.set("d", p.dg.first), ck.set("dc", p.dg.second), ck.set("in", loc_door_cell.count(name) != 0);
        auto sp = shell_pos.find(name);
        if (sp != shell_pos.end()) ck.set("wx", sp->second.first), ck.set("wz", sp->second.second);
        checks.list().push_back(ck);
    }
    // item pots (pots.tsv), named by pots.scan
    std::string tsv;
    if (c.seed_file("pots.tsv", tsv)) {
        bool collapsed = settings.get("dungeon-room-shuffle", Value("off")) != Value("off") &&
                         !settings.get("dungeon-room-shuffle", Value("off")).is_none() &&
                         settings.get("dungeon-room-shuffle", Value("off")) != Value(false);
        const StrList &missable = collapsed ? POT_MISSABLE_ROOMS_COLLAPSED : POT_MISSABLE_ROOMS;
        std::map<std::string, std::pair<std::string, std::string>> names;  // (lv, x, z) -> (name, room)
        for (const auto &f : room_files(c, "", "")) {
            const std::string &level = f.first, &room = f.second;
            if (starts(level, POT_SKIP_LEVEL_PREFIX) || in(POT_SKIP_LEVELS, level) || in(missable, room)) continue;
            std::string data;
            if (!c.romfs_file("region_common/level/" + level + "/" + room + ".leb", data)) continue;
            LebRoom r = LebRoom::parse(data);
            int n = 0;
            for (const LebActor &a : r.actors) {
                if (std::find(POT_TYPES.begin(), POT_TYPES.end(), (int)a.type) == POT_TYPES.end()) continue;
                const char *d = get(POT_PREFIX, level);
                std::string cell = room.substr(room.rfind('_') + 1);
                std::string pname = d ? fmt("%s-pot-%s-%d", d, cell.c_str(), n + 1) : fmt("pot-%s-%s-%d", level.c_str(), cell.c_str(), n + 1);
                n++;
                LebRoom *sr = room_file(room);
                if (!sr) continue;
                for (const LebActor &b : sr->actors)
                    if (b.key == a.key) {
                        names[level + "|" + key2(b.pos[0], b.pos[2])] = {pname, room};
                        break;
                    }
            }
        }
        for (const std::string &line : split(tsv, '\n')) {
            std::vector<std::string> f = ws_split(line);
            if (f.size() < 5) continue;
            const std::string &lv = f[0];
            auto it = names.find(lv + "|" + key2(strtod(f[1].c_str(), nullptr), strtod(f[3].c_str(), nullptr)));
            if (it == names.end()) continue;
            const std::string &n = it->second.first, &proom = it->second.second;
            if (!logic.has(n) || absent.count(n)) continue;
            int g = -1;
            std::pair<Value, Value> dg;
            Cell cell;
            bool has = false;
            if (get(TR_DUNGEONS, lv) || dungeon_of.has(n)) {
                dg = where(n, proom, lv);
                if (!dg.first.truthy()) continue;
                g = group(dg.first.s());
                const Value &gr = groups.list()[(size_t)g];
                cell = Cell(gr.at("x").f(), gr.at("y").f()), has = true;
            } else if (level_cell.count(lv)) {
                cell = level_cell[lv], has = true;
            }
            if (!has) continue;
            Value ck = Value::new_dict();
            Value region = logic.at(n).get("region");
            ck.set("n", n), ck.set("x", cell.first), ck.set("y", cell.second), ck.set("g", g);
            ck.set("flag", (long long)atoll(f[4].c_str())), ck.set("k", "pot"), ck.set("a", region.truthy() ? region : Value(""));
            ck.set("d", dg.first), ck.set("dc", dg.second), ck.set("lv", lv);
            if (lv == "Field") ck.set("wx", strtod(f[1].c_str(), nullptr)), ck.set("wz", strtod(f[3].c_str(), nullptr));
            checks.list().push_back(ck);
        }
    }
    // bushes / rocks (bushes.tsv / rocks.tsv), named by bushes.scan(pits=True) / rocks.scan
    for (int pass = 0; pass < 2; pass++) {
        bool rocks = pass == 1;
        if (!c.seed_file(rocks ? "rocks.tsv" : "bushes.tsv", tsv)) continue;
        std::map<std::string, std::string> names;
        for (const auto &f : room_files(c, "Field", "Field_")) {
            const std::string &room = f.second;
            std::string data;
            if (!c.romfs_file("region_common/level/Field/" + room + ".leb", data)) continue;
            LebRoom r = LebRoom::parse(data);
            std::set<std::pair<long, long>> holes;
            double ox = 0, oz = 0;
            if (rocks) {  // (bushes._pit_tiles)
                const FhEntry *g = r.fh.find("grid");
                if (g && g->sub) {
                    const FhEntry *info = g->sub->find("info"), *dat = g->sub->find("data");
                    if (info && dat) {
                        int rows = (int)read_le(info->data, 0, 2), cols = (int)read_le(info->data, 2, 2);
                        ox = read_f32(info->data, 8), oz = read_f32(info->data, 12);
                        for (int i = 0; i < rows * cols; i++)
                            if ((unsigned char)dat->data[(size_t)(0x10 * i)] & 1) holes.insert({i % cols, i / cols});
                    }
                }
            }
            int n = 0;
            for (const LebActor &a : r.actors) {
                if ((int)a.type != (rocks ? ROCK_TYPE : BUSH_TYPE)) continue;
                bool keep = true;
                if (rocks) {
                    bool pit = holes.count({(long)py_floordiv(a.pos[0] - ox, 1.5), (long)py_floordiv(a.pos[2] - oz, 1.5)}) != 0;
                    keep = !(a.switches[0].first == 1 || a.key == GOLDEN_LEAF_ROCK || pit);
                }
                std::string name = fmt("%s-%s-%d", rocks ? "rock" : "bush", room.substr(room.rfind('_') + 1).c_str(), n + 1);
                n++;
                if (!keep) continue;
                LebRoom *sr = room_file(room);
                if (!sr) {
                    if (rocks) throw PyError("AttributeError", "'NoneType' object has no attribute 'actors'");
                    continue;
                }
                for (const LebActor &b : sr->actors)
                    if (b.key == a.key) {
                        names[key2(b.pos[0], b.pos[2])] = name;
                        break;
                    }
            }
        }
        for (const std::string &line : split(tsv, '\n')) {
            std::vector<std::string> f = ws_split(line);
            if (f.size() < 5) continue;
            auto it = names.find(key2(strtod(f[1].c_str(), nullptr), strtod(f[3].c_str(), nullptr)));
            if (it == names.end()) continue;
            const std::string &n = it->second;
            std::vector<std::string> parts = split(n, '-');
            Cell cell;
            bool has = parts.size() > 1 && cell_of_field("Field_" + parts[1], cell);
            if (!logic.has(n) || absent.count(n) || !has) continue;
            Value ck = Value::new_dict();
            Value region = logic.at(n).get("region");
            ck.set("n", n), ck.set("x", cell.first), ck.set("y", cell.second), ck.set("g", -1);
            ck.set("flag", (long long)atoll(f[4].c_str())), ck.set("k", rocks ? "rock" : "bush");
            ck.set("a", region.truthy() ? region : Value(""));
            ck.set("wx", strtod(f[1].c_str(), nullptr)), ck.set("wz", strtod(f[3].c_str(), nullptr));
            checks.list().push_back(ck);
        }
    }
    {
        std::set<std::string> dks;
        for (const auto &e : TR_DUNGEONS) dks.insert(e.second);
        for (const std::string &dk : dks)
            for (const Value &ck : checks.list())
                if (ck.get("d") == Value(dk)) { group(dk); break; }
    }
    // name_checks
    {
        FieldAreas fa = field_areas(c);
        if (fa.ok) {
            std::map<std::string, int> seen;
            static const std::regex dpot("^D\\d-pot-(\\d\\d[A-P])-"), numre("^(bush|dig|pot|rock)-"),
                apple("^apple-tree-\\d\\d[A-P]-(\\d)$");
            for (Value &ck : checks.list()) {
                const std::string n = ck.at("n").s(), k = ck.at("k").s();
                std::smatch m;
                if (ck.at("g").i() >= 0) {
                    std::string dn;
                    if (std::regex_search(n, m, dpot)) dn = "Pot (" + (ck.get("dc").truthy() ? py_str(ck.at("dc")) : m[1].str()) + ")";
                    else dn = display_name(n, k);
                    int cnt = ++seen[dn];
                    if (cnt > 1) dn += " " + std::to_string(cnt);
                    ck.set("dn", dn);
                    continue;
                }
                double x, z;
                int a;
                if (ck.has("wx")) {
                    x = ck.at("wx").f(), z = ck.at("wz").f();
                    a = fa.area_at(x, z);
                } else {
                    x = ck.at("x").f() * 15.0, z = ck.at("y").f() * 12.0;
                    auto it = fa.screen.find({std::min(15, (int)ck.at("y").f()), std::min(15, (int)ck.at("x").f())});
                    a = it == fa.screen.end() ? -1 : it->second;
                }
                ck.set("ar", a);
                std::string place = 0 <= a && a < (int)TR_PLACE.size() ? TR_PLACE[(size_t)a] : "";
                std::string wh;
                auto b = fa.boxes.find(a);
                if (b != fa.boxes.end()) {
                    const auto &bx = b->second;
                    wh = direction((x - bx[0]) / std::max(1.0, bx[2] - bx[0]), (z - bx[1]) / std::max(1.0, bx[3] - bx[1]));
                }
                bool numbered = (k == "bush" || k == "dig" || k == "pot" || k == "rock") && std::regex_search(n, numre);
                std::string name;
                if (std::regex_search(n, m, apple)) {
                    std::string g1 = m[1].str();
                    name = display_name(n, "apple", place, !wh.empty() ? wh + ", " + g1 + " of 3" : g1 + " of 3");
                } else if (numbered && k == "pot" && ck.get("lv").truthy() && ck.get("lv") != Value("Field"))
                    name = "Pot in " + house(ck.at("lv").s());
                else if (numbered)
                    name = display_name(n, k, place, wh);
                else
                    name = display_name(n, k);
                int cnt = ++seen[name];
                ck.set("dn", cnt == 1 ? name : name + " " + std::to_string(cnt));
                ck.set("sp", std::string(ck.get("in").truthy() ? "inside, " : "") + (wh != "centre" ? wh : "middle"));
            }
            for (Value &ck : checks.list()) {
                Value dn = ck.get("dn");
                if (dn.truthy() && seen.count(dn.s()) && seen[dn.s()] > 1) ck.set("dn", dn.s() + " 1");
            }
        }
    }
    // the dungeon cells holding checks: their floor's map shift
    for (const Value &ck : checks.list()) {
        Value d = ck.get("d"), dc = ck.get("dc");
        if (!d.truthy() || !dc.truthy()) continue;
        const char *lv = nullptr;
        for (const auto &e : TR_DUNGEONS)
            if (d == Value(e.second)) lv = e.first;
        if (lv && !layout_find({lv, dc.s()})) layout.push_back({{lv, dc.s()}, std::string(lv) + "_" + dc.s()});
    }
    {
        std::map<int, Cell> sh;
        std::map<std::string, int> anims;
        bool loaded = false, failed = false;
        for (const auto &e : layout) {
            if (shifts.count(e.first)) continue;
            Value v(List{Value(0.0), Value(0.0)});
            try {
                if (!loaded && !failed) {
                    loaded = true;
                    sh = map_shifts(c);
                    std::string data;
                    if (!c.romfs_file("region_common/datasheets/UiDungeonArea.gsheet", data)) throw PyError("FileNotFoundError", "UiDungeonArea.gsheet");
                    GSheet s = GSheet::parse(data);
                    for (const GStruct &r : s.values) anims[gfield(r, "mLeaName")->s] = gfield(r, "mAnimFloorIndex")->i;
                }
                if (failed) throw PyError("Error", "");
                std::string cell = e.first.first + "_" + e.first.second;
                int anim = 0;
                if (anims.count(cell)) anim = anims[cell];
                else {
                    const char *en = get(DG_ENTRANCES, e.first.first);
                    std::string k = en ? en : "";
                    anim = anims.count(k) ? anims[k] : 0;
                }
                auto it = sh.find(anim);
                if (it != sh.end()) v = Value(List{Value(it->second.first), Value(it->second.second)});
            } catch (const std::exception &) {
                failed = true;
            }
            shifts[e.first] = v;
        }
    }
    std::vector<std::pair<std::pair<std::string, std::string>, std::string>> lsorted = layout;
    std::sort(lsorted.begin(), lsorted.end());
    Value rooms = Value::new_list();
    for (const auto &e : lsorted) {
        Value r = Value::new_dict();
        r.set("level", e.first.first), r.set("cell", e.first.second), r.set("room", e.second);
        r.set("shift", shifts.count(e.first) ? shifts[e.first] : Value(List{Value(0), Value(0)}));
        rooms.list().push_back(r);
    }
    // seen_rows
    std::vector<std::tuple<std::string, int64_t, int>> seen_rows;
    {
        std::set<std::string> read;
        for (const char *i : INSTRUMENTS) read.insert(i);
        read.insert(TRIFORCE);
        for (const auto &e : logic.dict())
            for (const auto &f : e.value.dict())
                if (starts(f.key, "condition-") && f.value.is_str())
                    for (const std::string &t : words(f.value.s())) read.insert(base_item(t));
        auto useful = [&](const std::string &item) {
            std::string b = base_item(item);
            if (c.hs_junk.match(b)) return false;
            for (const char *p : TR_NOT_PROGRESS)
                if (starts(b, std::string(p) + "-")) return false;
            return read.count(b) != 0;
        };
        for (const Value &ck : checks.list()) {
            const std::string &n = ck.at("n").s();
            for (const auto &sa : TR_SEEN_AT)
                if (starts(n, sa.first) && flags.count(sa.second)) {
                    Value item = c.in.placements.get(n);
                    seen_rows.emplace_back(n, flags[sa.second], item.is_none() || useful(py_str(item)) ? 1 : 0);
                }
        }
    }
    Value tsettings = Value::new_dict();
    std::vector<std::string> off;
    for (const auto &e : settings.dict()) {
        if (e.value.is_bool() && e.value.b()) tsettings.set(e.key, 1);
        if (e.value.is_bool() && !e.value.b()) off.push_back(e.key);
    }
    std::sort(off.begin(), off.end());
    Value offv = Value::new_list();
    for (const std::string &k : off) offv.list().push_back(k);
    Value doc = Value::new_dict();
    doc.set("settings", tsettings), doc.set("nodes", nodes), doc.set("checks", checks), doc.set("groups", groups);
    doc.set("rooms", rooms), doc.set("logic", settings.get("logic", Value("basic"))), doc.set("off", offv);
    c.write("tracker.json", py_json(doc));
    // tracker.tsv
    auto f3 = [](const Value &v) { return fmt("%.3f", v.f()); };
    auto or_dash = [](const Value &v) { return v.truthy() ? py_str(v) : std::string("-"); };
    std::string o = "V\t2\n";
    for (const auto &e : tsettings.dict()) o += "S\t" + e.key + "\n";
    for (const std::string &k : off) o += "F\t" + k + "\n";
    Value rs = settings.get("dungeon-room-shuffle");
    std::string rss = rs.truthy() ? py_str(rs) : "off";
    if (rss == "chaos") rss = "coupled";
    else if (rss == "madness") rss = "decoupled";
    o += "O\tdungeon-room-shuffle\t" + rss + "\n";
    for (const Value &n : nodes.list())
        o += "N\t" + py_str(n.at("t")) + "\t" + n.at("n").s() + "\t" + or_dash(n.at("r")) + "\t" + or_dash(n.at("c")) + "\t" +
             (n.has("pit") ? py_str(n.at("pit")) : "-") + "\t" + (n.has("heavy") ? py_str(n.at("heavy")) : "-") + "\t" +
             (n.has("trick") ? or_dash(n.at("trick")) : "-") + "\n";
    for (const Value &g : groups.list()) o += "G\t" + g.at("label").s() + "\t" + f3(g.at("x")) + "\t" + f3(g.at("y")) + "\n";
    for (const Value &ck : checks.list())
        o += "C\t" + ck.at("n").s() + "\t" + f3(ck.at("x")) + "\t" + f3(ck.at("y")) + "\t" + std::to_string(ck.at("g").i()) + "\t" +
             std::to_string(ck.at("flag").i()) + "\t" + ck.at("k").s() + "\t" + or_dash(ck.at("a")) + "\t" + or_dash(ck.get("d")) +
             "\t" + or_dash(ck.get("dc")) + "\t" + std::to_string(ck.get("ar", Value(-1)).i()) + "\t" + or_dash(ck.get("dn")) +
             "\t" + or_dash(ck.get("sp")) + "\n";
    for (const Value &r : rooms.list()) {
        const List &s = r.at("shift").list();
        o += "R\t" + r.at("level").s() + "\t" + r.at("cell").s() + "\t" + r.at("room").s() + "\t" + py_g(s[0].f()) + "\t" +
             py_g(s[1].f()) + "\n";
    }
    for (const auto &h : seen_rows) o += "H\t" + std::get<0>(h) + "\t" + std::to_string(std::get<1>(h)) + "\t" + std::to_string(std::get<2>(h)) + "\n";
    c.write("tracker.tsv", o);
    c.res.log.push_back(fmt("tracker: (%zu, %zu)", checks.list().size(), groups.list().size()));
}

}  // namespace out
}  // namespace solve
}  // namespace rando
