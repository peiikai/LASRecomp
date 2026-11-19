// LAS Recomp's world steps' scans and tables (ours_world_lib.h): tools/rando/seashell_spots.py (scan, _landing,
// landings), apple_trees.scan, digs.scan, pots.write_tsv, spoiler_view.pretty_item, and the -sanity locations
// las_headless.py makes before the fill (decide mode), worked out from the game's files as there.
//
// Python quirks kept for the same tables (each marked "as the Python"):
//   - seashell_spots._landing leaves out "a is not host", but the host it is given comes from another read of the
//     room, so the host's own tile always counts as taken (when it isn't grass, an enemy, a tag, an area, an item).
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <functional>
#include <regex>
#include <set>
#include <stdexcept>
#include <tuple>

#include "../../../third_party/yyjson/yyjson.h"
#include "decide.h"
#include "las_lib.h"
#include "ours_world_lib.h"
#include "../solve/outputs_data.h"
#include "../solve/prepare_lib.h"

namespace rando {
namespace las {

using solve::prep_data;
using solve::py_floordiv;
using solve::Value;

namespace {

bool ends(const std::string &s, const char *e) {
    size_t n = strlen(e);
    return s.size() >= n && !s.compare(s.size() - n, n, e);
}
bool has_prefix(const Bytes &s, const char *p) { return s.compare(0, strlen(p), p) == 0; }

// glob(<romfs>/region_common/level/<level or *>/<prefix>*.leb), sorted: [(level, file name)]
std::vector<std::pair<std::string, std::string>> game_rooms(SeedBuild &b, const std::string &only, const char *prefix) {
    std::vector<std::string> levels = only.empty() ? b.list_game("region_common/level") : std::vector<std::string>{only};
    std::vector<std::string> paths;
    for (const std::string &lv : levels) {
        if (lv.empty() || lv[0] == '.') continue;  // (glob's * skips hidden names)
        for (const std::string &f : b.list_game("region_common/level/" + lv))
            if (f[0] != '.' && !f.compare(0, strlen(prefix), prefix) && ends(f, ".leb")) paths.push_back(lv + "/" + f);
    }
    std::sort(paths.begin(), paths.end());  // (sorted() of the whole paths: the same common prefix)
    std::vector<std::pair<std::string, std::string>> out;
    for (const std::string &p : paths) out.push_back({p.substr(0, p.find('/')), p.substr(p.find('/') + 1)});
    return out;
}

LebRoom game_room(SeedBuild &b, const std::string &level, const std::string &file, bool grid = false) {
    std::string bytes;
    if (!b.read_game("region_common/level/" + level + "/" + file, bytes))
        throw std::runtime_error("ours world: no game file " + level + "/" + file);
    return LebRoom::parse(bytes, grid);
}

// re.match(r'Field_(\d\d)([A-P])$', room): (row, column)
bool field_screen(const std::string &room, int &row, int &col) {
    static const std::regex re("^Field_([0-9][0-9])([A-P])$");
    std::smatch m;
    if (!std::regex_match(room, m, re)) return false;
    row = atoi(m[1].str().c_str()), col = m[2].str()[0] - 'A';
    return true;
}

// seashell_spots.DROP_KEY.match: the n of an ItemDrop key Seashell<n> (-1: none)
int64_t drop_index(const LebParam &p) {
    if (p.kind != LebParam::Str) return -1;
    std::string s(p.s.begin(), p.s.end());
    if (s.compare(0, 8, "Seashell")) return -1;
    size_t n = s.size();
    if (n > 8 && s[n - 1] == '\n') n--;  // ($ matches before a final newline)
    if (n == 8) return -1;
    for (size_t i = 8; i < n; i++)
        if (s[i] < '0' || s[i] > '9') return -1;
    return strtoll(s.substr(8, n - 8).c_str(), nullptr, 10);
}

const uint16_t T_SHELL = 0x87, T_SHELL_DROP_AREA = 0x233, LAWN = 0xb5;

// seashell_spots._landing: the first free tile around a tree (a 2 x 2 block under its crown) or a bomb hole (its own
// tile) - nearest ring, then south, then centred - not taken, at most 2 units above / below the host
bool landing(SeedBuild &b, const std::string &room, const LebActor &host, bool tree,
             const std::set<std::pair<int64_t, int64_t>> &skip, const std::function<bool(int64_t)> &ok,
             std::array<double, 3> &out) {
    LebRoom r = game_room(b, "Field", room + ".leb", true);
    int row, col;
    if (!field_screen(room, row, col)) throw std::runtime_error("seashell spots: not a Field screen: " + room);
    double ox = col * 15.0, oz = (row - 1) * 12.0;
    int64_t x0, x1, z0, z1;
    if (tree) {
        int64_t cx = (int64_t)nearbyint((host.pos[0] - ox) / 1.5), cz = (int64_t)nearbyint((host.pos[2] - oz) / 1.5);
        x0 = cx - 1, x1 = cx, z0 = cz - 1, z1 = cz;
    } else {
        x0 = x1 = (int64_t)py_floordiv(host.pos[0] - ox, 1.5);
        z0 = z1 = (int64_t)py_floordiv(host.pos[2] - oz, 1.5);
    }
    std::set<std::pair<int64_t, int64_t>> taken;
    for (const LebActor &a : r.actors)  // (the host too: as the Python)
        if (a.type != LAWN && !has_prefix(a.name, "Enemy") && !has_prefix(a.name, "Tag") &&
            !has_prefix(a.name, "Area") && !has_prefix(a.name, "Item"))
            taken.insert({(int64_t)py_floordiv(a.pos[0] - ox, 1.5), (int64_t)py_floordiv(a.pos[2] - oz, 1.5)});
    std::vector<std::tuple<int64_t, double, double, int64_t, int64_t>> cands;
    for (int64_t ring = 1; ring <= 2; ring++)
        for (int64_t tz = z0 - ring; tz <= z1 + ring; tz++)
            for (int64_t tx = x0 - ring; tx <= x1 + ring; tx++)
                if (std::max({x0 - tx, tx - x1, z0 - tz, tz - z1}) == ring) {
                    double dx = tx - (x0 + x1) / 2.0, dz = tz - (z0 + z1) / 2.0;
                    cands.push_back(std::make_tuple(ring, -dz, fabs(dx), tx, tz));
                }
    std::sort(cands.begin(), cands.end());
    for (const auto &c : cands) {
        int64_t tx = std::get<3>(c), tz = std::get<4>(c);
        if (!(0 <= tx && tx < 10 && 0 <= tz && tz < 8) || taken.count({tx, tz}) || skip.count({tx, tz}) ||
            (ok && !ok(tz * 10 + tx)))
            continue;
        const LebTile &t = r.grid.tiles.at((size_t)(tz * 10 + tx));
        if ((t.flags[0] & 2) || (t.flags[2] & 4) || (t.flags[0] & 1) || fabs(t.elevation - host.pos[1]) > 2.0) continue;
        out = {ox + tx * 1.5 + 0.75, t.elevation, oz + tz * 1.5 + 0.75};
        return true;
    }
    return false;
}

// seashell_spots.landings: n different landing tiles around a tree, besides those in `have`
std::vector<std::array<double, 3>> landings(SeedBuild &b, const std::string &room, const LebActor &host, size_t n,
                                            const std::function<bool(int64_t)> &ok,
                                            std::vector<std::array<double, 3>> out) {
    double ox = (room.back() - 65) * 15.0, oz = (atoi(room.substr(room.size() - 3, 2).c_str()) - 1) * 12.0;
    while (out.size() < n) {
        std::set<std::pair<int64_t, int64_t>> skip;
        for (const auto &s : out) skip.insert({(int64_t)py_floordiv(s[0] - ox, 1.5), (int64_t)py_floordiv(s[2] - oz, 1.5)});
        std::array<double, 3> spot;
        if (!landing(b, room, host, true, skip, ok, spot)) break;
        out.push_back(spot);
    }
    return out;
}

// field_regions.region_of: (region, condition) of a Field tile, as the logic tables say
std::pair<Value, Value> region_of(const std::string &room, int64_t tile) {
    const Value &screens = prep_data().at("field").at("screens");
    if (!screens.has(room) || screens.at(room).is_none())
        throw std::runtime_error(room + ": no reviewed region (field_regions.SCREENS)");
    Value e = screens.at(room);
    if (e.is_list() && e.list().size() == 2 && e.list()[1].is_list()) {  // (default, [(tiles, region), ...])
        Value parts = e.list()[1];
        e = e.list()[0];
        for (const Value &p : parts.list())
            if (p.list()[0].contains(Value((long long)tile))) {
                e = p.list()[1];
                break;
            }
    }
    if (e.is_str()) return {e, Value()};
    return {e.list()[0], e.list()[1]};
}

std::string base_item(const std::string &item) {  // re.sub(r'\[\d+\]$', '', item)
    if (item.empty() || item.back() != ']') return item;
    size_t k = item.size() - 1;
    while (k > 0 && isdigit((unsigned char)item[k - 1])) k--;
    if (k == item.size() - 1 || k == 0 || item[k - 1] != '[') return item;
    return item.substr(0, k - 1);
}
std::string title_case(std::string s) {  // str.title on ASCII
    bool prev = false;
    for (char &ch : s) {
        bool alpha = isalpha((unsigned char)ch);
        if (alpha) ch = prev ? (char)tolower((unsigned char)ch) : (char)toupper((unsigned char)ch);
        prev = alpha;
    }
    return s;
}
std::string dashes_to_spaces(std::string s) {
    std::replace(s.begin(), s.end(), '-', ' ');
    return s;
}

}  // namespace

// ======== seashell_spots.scan

std::map<int64_t, ShellSpot> shell_scan(SeedBuild &b) {
    static const std::map<std::string, std::array<double, 3>> HOLE_LANDING = {{"Field_02J", {151.25, 25.5, 12.25}}};
    std::map<int64_t, ShellSpot> out;
    for (const auto &lf : game_rooms(b, "", "")) {
        const std::string &level = lf.first, room = lf.second.substr(0, lf.second.size() - 4);
        LebRoom r = game_room(b, level, lf.second);
        for (const LebActor &a : r.actors) {
            if (a.type == T_SHELL && (level == "Field" || level == "DogHouse") && a.params[0].kind == LebParam::Int) {
                out[a.params[0].i] = {"dig", level, room, a.pos[0], a.pos[1], a.pos[2], a.key, 0};
            } else if (a.type == T_SHELL_DROP_AREA && a.params[0].kind == LebParam::Int) {
                std::array<double, 3> spot;
                auto h = HOLE_LANDING.find(room);
                if (h != HOLE_LANDING.end()) spot = h->second;
                else if (!landing(b, room, a, false, {}, nullptr, spot))
                    throw std::runtime_error(room + ": no free tile beside the bomb hole");
                if (!out.count(a.params[0].i)) out[a.params[0].i] = {"hole", level, room, spot[0], spot[1], spot[2], a.key, 0};
            } else {
                for (int k = 0; k < 2; k++) {
                    int64_t idx = drop_index(a.params[k]);
                    if (idx < 0) continue;
                    bool tree = (a.type == 0xbb || a.type == 0xbc) && level == "Field";
                    std::array<double, 3> spot;
                    bool landed = tree && landing(b, room, a, true, {}, nullptr, spot);
                    if ((a.type == 0xbb || a.type == 0xbc) && !landed)
                        throw std::runtime_error(room + ": no free tile beside the tree");
                    if (out.count(idx)) continue;
                    out[idx] = landed ? ShellSpot{"land", level, room, spot[0], spot[1], spot[2], a.key, k}
                                      : ShellSpot{"drop", level, room, a.pos[0], a.pos[1], a.pos[2], a.key, k};
                }
            }
        }
    }
    return out;
}

// ======== apple_trees.scan

std::vector<AppleTree> apple_scan(SeedBuild &b) {
    const Value &stand = prep_data().at("apples").at("stand");
    std::vector<AppleTree> out;
    for (const auto &lf : game_rooms(b, "Field", "Field_")) {
        const std::string room = lf.second.substr(0, lf.second.size() - 4);
        LebRoom r = game_room(b, lf.first, lf.second);
        for (const LebActor &a : r.actors) {
            const LebParam &p = a.params[1];
            bool set = p.kind == LebParam::Str ? !p.s.empty() : p.kind == LebParam::Float ? p.f != 0 : p.i != 0;
            if (a.type != 0xbb || !set) continue;
            if (!stand.has(room)) throw std::runtime_error(room + ": an apple tree with no reviewed STAND tile");
            int64_t tile = stand.at(room).i();
            auto region = region_of(room, tile);
            // (in front of the trunk first, then beside it, behind it last: the crown hides an item there)
            int64_t z1 = (int64_t)nearbyint((a.pos[2] - (atoi(room.substr(room.size() - 3, 2).c_str()) - 1) * 12.0) / 1.5);
            std::vector<std::array<double, 3>> spots;
            for (int64_t row : {z1, z1 - 1, (int64_t)-99}) {
                auto ok = [&, row](int64_t t) {
                    auto rt = region_of(room, t);
                    return rt.first == region.first && rt.second == region.second && t / 10 >= row;
                };
                spots = landings(b, room, a, 3, ok, spots);
            }
            if (spots.size() < 3) throw std::runtime_error(room + ": too few free tiles beside the apple tree");
            out.push_back({room, a.key, spots});
        }
    }
    return out;
}

// ======== digs.scan

std::vector<DigTile> dig_scan(SeedBuild &b) {
    const Value &D = prep_data().at("digs");
    std::vector<std::string> open_actors = solve::str_list(D.at("open_actors"));
    const Value &rank = D.at("rank");
    int64_t bush = prep_data().at("bushes").at("type").i(), lawn = D.at("lawn").i(), shell_type = D.at("shell_type").i();
    std::vector<DigTile> out;
    for (const auto &lf : game_rooms(b, "Field", "Field_")) {
        const std::string name = lf.second.substr(0, lf.second.size() - 4);
        int row, col;
        if (!field_screen(name, row, col)) continue;
        LebRoom room = game_room(b, lf.first, lf.second, true);
        if (!room.has_grid || room.grid.room_height != 8) continue;
        auto tile_of = [&](const LebActor &a) {
            return std::make_pair((int64_t)py_floordiv(a.pos[0] - col * 15.0, 1.5),
                                  (int64_t)py_floordiv(a.pos[2] - (row - 1) * 12.0, 1.5));
        };
        std::map<int64_t, std::string> blocked;  // (digs._blockers: the highest rank on a tile)
        for (const LebActor &a : room.actors) {
            auto t = tile_of(a);
            if (!(0 <= t.first && t.first < 10 && 0 <= t.second && t.second < 8)) continue;
            bool open = false;
            for (const std::string &p : open_actors) open = open || has_prefix(a.name, p.c_str());
            std::string kind = open ? "" : a.type == bush ? "bush" : a.type == lawn ? "grass" : "other";
            int64_t i = t.second * 10 + t.first;
            if (!kind.empty() && rank.at(kind).i() > (blocked.count(i) ? rank.at(blocked[i]).i() : 0)) blocked[i] = kind;
        }
        std::map<int64_t, int64_t> shells;
        for (const LebActor &a : room.actors)
            if (a.type == shell_type && a.params[0].kind == LebParam::Int) {
                auto t = tile_of(a);
                shells[t.second * 10 + t.first] = a.params[0].i;
            }
        for (size_t i = 0; i < room.grid.tiles.size(); i++) {
            const LebTile &t = room.grid.tiles[i];
            if (!(t.flags[2] & 1) || (t.flags[0] & 2) || (t.flags[0] & 1)) continue;  // (dig spot; no collision, no pit)
            auto bl = blocked.find((int64_t)i);
            auto sh = shells.find((int64_t)i);
            out.push_back({name, (int)i, t.elevation, bl == blocked.end() ? "" : bl->second, sh == shells.end() ? -1 : sh->second});
        }
    }
    return out;
}

// ======== pots.write_tsv

std::string pots_tsv(SeedBuild &b, const char *prefix, double hide_scale) {
    const std::string base = "region_common/level/";
    const std::regex flag_re(std::string("^") + prefix + "([0-9]+)\n?$");
    std::string rows;
    for (const auto &f : b.files()) {  // (sorted by path, as the Python's sorted glob)
        const std::string &path = f.first;
        if (path.compare(0, base.size(), base) || !ends(path, ".leb")) continue;
        size_t slash = path.find('/', base.size());
        if (slash == std::string::npos || path.find('/', slash + 1) != std::string::npos) continue;
        const std::string level = path.substr(base.size(), slash - base.size()), file = path.substr(slash + 1);
        if (level[0] == '.' || file[0] == '.') continue;
        const std::string &raw = f.second;
        LebRoom room = LebRoom::parse(raw);
        std::map<std::array<double, 3>, const LebActor *> by_pos;  // (the bush an item is in, with its cut flag)
        for (const LebActor &a : room.actors)
            if (a.type != 0x194 && a.switches[0].first == 1) by_pos[{a.pos[0], a.pos[1], a.pos[2]}] = &a;
        for (const LebActor &a : room.actors) {
            if (a.type != 0x194 || a.params[3].kind != LebParam::Str) continue;
            std::string flag(a.params[3].s.begin(), a.params[3].s.end());
            std::smatch m;
            if (!std::regex_match(flag, m, flag_re)) continue;
            std::string cut;
            auto bush = by_pos.find({a.pos[0], a.pos[1], a.pos[2]});
            if (hide_scale != 0 && bush != by_pos.end()) {  // + the item record's offset in the room file
                float p3[3] = {(float)a.pos[0], (float)a.pos[1], (float)a.pos[2]};
                const std::string pat((const char *)p3, 12);
                size_t rec = raw.find(pat);
                while (rec != std::string::npos) {
                    float sx;
                    memcpy(&sx, raw.data() + rec + 24, 4);
                    if ((double)sx == a.scale[0]) break;
                    rec = raw.find(pat, rec + 1);
                }
                cut = "\t" + std::to_string(bush->second->switches[0].second) + "\t" + py_float_repr(a.scale[0]) + "\t" +
                      py_float_repr(a.scale[0] / hide_scale) + "\t" + file.substr(0, file.size() - 4) + "\t" +
                      (rec == std::string::npos ? std::string("-1") : std::to_string(rec));
            }
            std::string xyz = cut.empty() ? py_g(a.pos[0]) + "\t" + py_g(a.pos[1]) + "\t" + py_g(a.pos[2])
                                          : py_float_repr(a.pos[0]) + "\t" + py_float_repr(a.pos[1]) + "\t" + py_float_repr(a.pos[2]);
            rows += level + "\t" + xyz + "\t" + std::to_string(strtoll(m[1].str().c_str(), nullptr, 10)) + cut + "\n";
        }
    }
    return rows;
}

std::string py_g(double v) {  // (Python's 'g' is C's %g)
    char buf[64];
    snprintf(buf, sizeof buf, "%g", v);
    return buf;
}

std::string pretty_item(const std::string &it) {
    using namespace solve::odata;
    std::string base = base_item(it);
    static const std::regex a("^(D\\d)-(small-key|key|nightmare-key|compass|map|stone-beak|beak)$"),
        bk("^(small-key|key|nightmare-key|compass|map|stone-beak)-?(D\\d)$");
    std::smatch m;
    if (std::regex_search(base, m, a)) {
        static const StrMap kinds = {{"small-key", "Small Key"}, {"key", "Small Key"}, {"nightmare-key", "Nightmare Key"},
                                     {"compass", "Compass"}, {"map", "Dungeon Map"}, {"stone-beak", "Stone Beak"},
                                     {"beak", "Stone Beak"}};
        return std::string(get(kinds, m[2].str())) + " (" + get(SV_DPREFIX, m[1].str()) + ")";
    }
    if (std::regex_search(base, m, bk)) return pretty_item(m[2].str() + "-" + m[1].str());
    if (!base.compare(0, 5, "soul-")) {
        std::string name = title_case(dashes_to_spaces(base.substr(5)));
        return name + (ends(name, "s") ? "' Soul" : "'s Soul");
    }
    const char *i = get(SV_ITEMS, base);
    if (i && *i) return i;
    return title_case(dashes_to_spaces(base));
}

// ======== decide mode: the -sanity locations

const std::string &sanity_json(SeedBuild &b) {
    static const char *const KEY = "ours_world.sanity";
    auto &carry = b.carry();
    auto it = carry.find(KEY);
    if (it != carry.end()) return it->second;
    const Decisions &d = b.decisions();
    solve::GameFiles game;
    game.read = [&b](const std::string &p, std::string &out) { return b.read_game(p, out); };
    game.list = [&b](const std::string &dir, std::vector<std::string> &names) {
        names = b.list_game(dir);
        return !names.empty();
    };
    // (each module's add_to_logic on LAS's logic, in las_headless's order: a -sanity check standing on a Secret
    // Seashell spot takes it out of the logic - seashell_spots.absorb - so the later scans and the spots left see that.
    // The item pool here is a stand-in: with no trap kinds in it nothing is drawn, and the filler doesn't name a
    // location)
    Value logic = prep_data().at("logic").deepcopy(), items = Value::new_dict();
    PyRandom rng;
    Value out = Value::new_dict();
    const Value empty = Value::new_dict();
    yyjson_val *rs = setting_val(d, "dungeon-room-shuffle");
    bool collapsed = rs && !yyjson_is_null(rs) && !(yyjson_is_str(rs) && !strcmp(yyjson_get_str(rs), "off")) &&
                     !(yyjson_is_bool(rs) && !yyjson_get_bool(rs));  // (eagle_tower.collapsed)
    if (setting_or(d, "pots", false)) {
        const Value &P = prep_data();
        auto missable = solve::str_list(collapsed ? P.at("eagle").at("missable_collapsed") : P.at("pots").at("missable"));
        out.set("pots", solve::pots_add(game, missable, logic, items, "", rng));
    } else {
        out.set("pots", empty.copy());
    }
    solve::TileZones zones;
    bool have_zones = false;
    auto field_zones = [&]() -> const solve::TileZones & {
        if (!have_zones) zones = solve::tile_zones(game), have_zones = true;
        return zones;
    };
    bool open_mabe = setting_or(d, "open-mabe", false);
    out.set("bushes", setting_or(d, "bushes", false) ? solve::bushes_add(game, field_zones(), open_mabe, logic, items, "", rng)
                                                     : empty.copy());
    out.set("rocks", setting_or(d, "rocks", false)
                         ? solve::rocks_add(game, field_zones(), setting_or(d, "classic-d2", false), open_mabe, logic, items, "", rng)
                         : empty.copy());
    out.set("digs", setting_or(d, "digs", false) ? solve::digs_add(game, field_zones(), logic, items) : empty.copy());
    Value shells = Value::new_dict();  // seashell_spots.locations
    for (const auto &e : logic.dict()) {
        const Value &v = e.value;
        Value st = v.get("subtype");
        if (v.get("type") == Value("item") && (st == Value("dig") || st == Value("hidden") || st == Value("bomb-hole")) &&
            v.get("content") == Value("seashell") && v.has("index"))
            shells.set(e.key, Value((long long)v.at("index").to_int()));
    }
    out.set("shells", shells);
    return carry[KEY] = out.json();
}

}  // namespace las
}  // namespace rando
