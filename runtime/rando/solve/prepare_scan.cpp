// The -sanity modules' scans of the game's rooms and their add_to_logic (prepare_lib.h): tools/rando/pots.py,
// bushes.py, rocks.py, digs.py, apple_trees.py, with field_regions.py and seashell_spots.absorb. Their tables come
// from tools/rando/export_solve_data.py (prep_data()).
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <regex>
#include <set>

#include "../fmt/fixed_hash.h"
#include "../fmt/leb.h"
#include "prepare_lib.h"

namespace rando {
namespace solve {

// ---------------------------------------------------------------- Python's semantics

Value py_and(const std::vector<Value> &conds) {
    std::string out;
    bool any = false;
    for (const Value &c : conds) {
        if (!c.truthy()) continue;
        out += (any ? " & (" : "(") + py_str(c) + ")";
        any = true;
    }
    return any ? Value(out) : Value();
}

double py_floordiv(double vx, double wx) {  // (Objects/floatobject.c _float_div_mod)
    double mod = fmod(vx, wx), div = (vx - mod) / wx, fd;
    if (mod) {
        if ((wx < 0) != (mod < 0)) div -= 1.0;
    }
    if (div) {
        fd = floor(div);
        if (div - fd > 0.5) fd += 1.0;
    } else {
        fd = copysign(0.0, vx / wx);
    }
    return fd;
}

int64_t py_round(double x) { return (int64_t)nearbyint(x); }  // (the default rounding mode: ties to even)

std::string py_str(const Value &v) {
    if (v.is_str()) return v.s();
    return v.repr();
}

std::vector<std::string> str_list(const Value &list) {
    std::vector<std::string> out;
    for (const Value &v : list.list()) out.push_back(v.s());
    return out;
}

std::string hex16(uint64_t key) {
    char b[24];
    snprintf(b, sizeof b, "%016llX", (unsigned long long)key);
    return b;
}

static int64_t qty(const Value &item_defs, const std::string &k) { return item_defs.at(k).at("quantity").i(); }
static void add_qty(Value &item_defs, const std::string &k, int64_t n) {
    item_defs.at(k).set("quantity", Value((long long)(qty(item_defs, k) + n)));
}

// ---------------------------------------------------------------- game files

static Bytes read_game(const GameFiles &game, const std::string &path) {
    std::string out;
    if (!game.read(path, out)) throw PyError("FileNotFoundError", "No such file: " + py_repr(path));
    return out;
}

static bool ends_with(const std::string &s, const char *suffix) {
    size_t n = strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// glob(<romfs>/region_common/level/<level or *>/<prefix>*.leb), sorted: [(level, file name)]
static std::vector<std::pair<std::string, std::string>> level_files(const GameFiles &game, const std::string &only_level,
                                                                    const std::string &prefix) {
    const std::string base = "region_common/level";
    std::vector<std::string> levels;
    if (!only_level.empty()) levels.push_back(only_level);
    else if (!game.list(base, levels)) return {};
    std::vector<std::string> paths;
    for (const std::string &lv : levels) {
        if (lv.empty() || lv[0] == '.') continue;  // (glob's * skips hidden names: ._ files from other drives)
        std::vector<std::string> names;
        if (!game.list(base + "/" + lv, names)) continue;
        for (const std::string &f : names)
            if (!f.empty() && f[0] != '.' && f.compare(0, prefix.size(), prefix) == 0 && ends_with(f, ".leb"))
                paths.push_back(lv + "/" + f);
    }
    std::sort(paths.begin(), paths.end());  // (sorted() of the whole paths: the same common prefix)
    std::vector<std::pair<std::string, std::string>> out;
    for (const std::string &p : paths) {
        size_t s = p.find('/');
        out.push_back({p.substr(0, s), p.substr(s + 1)});
    }
    return out;
}

static LebRoom read_room(const GameFiles &game, const std::string &level, const std::string &file, bool grid = false) {
    return LebRoom::parse(read_game(game, "region_common/level/" + level + "/" + file), grid);
}

// seashell_spots.shell_index: the seashell index a host drops (ItemDrop key Seashell<n> in parameter 0 or 1)
static Value shell_index(const LebActor &a) {
    static const std::regex drop("^Seashell([0-9]+)$");
    for (int k = 0; k < 2; k++) {
        std::smatch m;
        if (a.params[k].kind == LebParam::Str && std::regex_match(a.params[k].s, m, drop))
            return Value((long long)strtoll(m[1].str().c_str(), nullptr, 10));
    }
    return Value();
}

// bushes._screen_pos: 'Field_10D' -> (row 10, column 3)
static bool screen_pos(const std::string &room, int &row, int &col) {
    static const std::regex re("^Field_([0-9][0-9])([A-P])$");
    std::smatch m;
    if (!std::regex_match(room, m, re)) return false;
    row = atoi(m[1].str().c_str()), col = m[2].str()[0] - 'A';
    return true;
}

// bushes._tile: the screen tile (0-79) an actor stands on, None off the screen
static Value actor_tile(const std::string &room, const LebActor &a) {
    int row, col;
    if (!screen_pos(room, row, col)) return Value();
    int64_t tx = (int64_t)py_floordiv(a.pos[0] - col * 15.0, 1.5), tz = (int64_t)py_floordiv(a.pos[2] - (row - 1) * 12.0, 1.5);
    return 0 <= tx && tx < 10 && 0 <= tz && tz < 8 ? Value((long long)(tz * 10 + tx)) : Value();
}

// bushes._pit_tiles: the tiles Link falls into, and the grid's corner
static std::set<std::pair<int64_t, int64_t>> pit_tiles(const LebRoom &room, double &ox, double &oz) {
    std::set<std::pair<int64_t, int64_t>> out;
    ox = oz = 0.0;
    const FhEntry *g = room.fh.find("grid");
    if (!g || !g->sub) return out;
    const FhEntry *info = nullptr, *data = nullptr;
    for (const FhEntry &e : g->sub->entries) {  // (a dict by name: the last of a name)
        if (e.name == "info") info = &e;
        if (e.name == "data") data = &e;
    }
    if (!info || !data) return out;
    uint64_t rows = read_le(info->data, 0, 2), cols = read_le(info->data, 2, 2);
    ox = read_f32(info->data, 8), oz = read_f32(info->data, 12);
    for (uint64_t i = 0; i < rows * cols; i++) {
        if (0x10 * i >= data->data.size()) throw PyError("IndexError", "index out of range");
        if ((uint8_t)data->data[0x10 * i] & 1) out.insert({(int64_t)(i % cols), (int64_t)(i / cols)});
    }
    return out;
}

static bool has_prefix(const Bytes &s, const char *p) { return s.compare(0, strlen(p), p) == 0; }

// ---------------------------------------------------------------- field_regions.py

TileZones tile_zones(const GameFiles &game) {
    TileZones out;
    std::string data;
    if (!game.read("region_common/level/Field/Field.lvb", data)) return out;
    FixedHash fh = FixedHash::parse(data);
    const FhEntry *z = fh.find("zone"), *grid = fh.find("grid"), *cfg = fh.find("gridConfig");
    if (!z || !z->sub || !grid || !cfg) throw PyError("AttributeError", "Field.lvb: no zone / grid / gridConfig");
    std::vector<int64_t> zones;
    for (const FhEntry &e : z->sub->entries) zones.push_back((int64_t)read_le(e.data, 0, 4));
    uint64_t gh = read_le(cfg->data, 0, 2), gw = read_le(cfg->data, 2, 2);
    for (int row = 0; row < 16; row++)
        for (int col = 0; col < 16; col++) {
            std::vector<int64_t> zs;
            for (int zz = 0; zz < 8; zz++)
                for (int x = 0; x < 10; x++) {
                    uint64_t gx = col * 10 + x, gz = row * 8 + zz;
                    uint64_t zi = gz < gh && gx < gw ? read_le(grid->data, (gz * gw + gx) * 8 + 5, 1) : 255;
                    zs.push_back(zi < zones.size() ? zones[zi] : -1);
                }
            char name[16];
            snprintf(name, sizeof name, "Field_%02d%c", row + 1, 'A' + col);
            out[name] = zs;
        }
    return out;
}

static Value zone_at(const TileZones &zones, const std::string &room, const Value &tile) {
    auto it = zones.find(room);
    if (it == zones.end()) return Value();
    if (!tile.is_int()) throw PyError("TypeError", "list indices must be integers or slices, not NoneType");
    return Value((long long)it->second.at((size_t)tile.i()));
}

// field_regions.region_of: (region, condition or None)
static std::pair<Value, Value> region_of(const std::string &room, const Value &tile) {
    const Value &screens = prep_data().at("field").at("screens");
    if (!screens.has(room)) throw PyError("LookupError", room + ": not a Field screen of field_regions.SCREENS");
    Value e = screens.at(room);
    if (e.is_none())
        throw PyError("LookupError", room + " tile " + py_str(tile) +
                                         ": no check was reviewed on this screen (field_regions.SCREENS is None)");
    if (e.is_list() && e.list().size() == 2 && e.list()[1].is_list()) {  // (default, [(tiles, region), ...])
        Value parts = e.list()[1];
        e = e.list()[0];
        for (const Value &p : parts.list())
            if (p.list()[0].contains(tile)) {
                e = p.list()[1];
                break;
            }
    }
    if (e.is_str()) return {e, Value()};
    return {e.list()[0], e.list()[1]};
}

static Value area_of(const Value &region, const Value &zone) {
    const Value &f = prep_data().at("field");
    if (zone.is_int() && f.at("zone_area").has(std::to_string(zone.i()))) return f.at("zone_area").at(std::to_string(zone.i()));
    if (region.is_str() && f.at("region_area").has(region.s())) return f.at("region_area").at(region.s());
    return Value("mabe-village");
}

static Value standing(const Value &region, const Value &area, const Value &cond) {
    Value loc = Value::new_dict();
    loc.set("type", "item"), loc.set("subtype", "standing"), loc.set("content", "rupee-5");
    loc.set("region", region), loc.set("spoiler-region", area), loc.set("condition-basic", cond);
    return loc;
}

// field_regions.logic_entry
static Value logic_entry(const std::string &room, const Value &tile, const Value &zone, const std::vector<Value> &conds) {
    std::pair<Value, Value> rc = region_of(room, tile);
    std::vector<Value> parts{rc.second};
    parts.insert(parts.end(), conds.begin(), conds.end());
    return standing(rc.first, area_of(rc.first, zone), py_and(parts));
}

// ---------------------------------------------------------------- seashell_spots.absorb

static Value absorb(Value &logic_defs, int64_t index, Value &loc) {
    std::string las;
    for (const auto &e : logic_defs.dict()) {
        const Value &v = e.value;
        if (v.get("type") != Value("item") || v.get("content") != Value("seashell") || !v.has("index")) continue;
        Value st = v.get("subtype");
        if (!(st == Value("dig") || st == Value("hidden") || st == Value("bomb-hole"))) continue;
        if (v.at("index").to_int() == index) {
            las = e.key;
            break;
        }
    }
    if (las.empty()) return Value();
    Value v = logic_defs.at(las);
    logic_defs.dict().erase(las);
    Value base = py_and({loc.get("condition-basic"), v.get("region") != loc.get("region") ? v.at("region") : Value()});
    loc.set("condition-basic", py_and({base, v.get("condition-basic")}));
    for (const auto &e : v.dict())
        if (e.key.compare(0, 10, "condition-") == 0 && e.key != "condition-basic") loc.set(e.key, py_and({base, e.value}));
    return Value(las);
}

// ---------------------------------------------------------------- filler

static void filler_defs(Value &item_defs, const Value &defs) {
    for (const auto &e : defs.dict()) {
        if (item_defs.has(e.key)) continue;
        Value d = e.value.deepcopy();
        d.set("type", "junk"), d.set("quantity", 0), d.set("chest-type", "junk");
        item_defs.set(e.key, d);
    }
}

// the trap share of the filler (pots.POT_TRAPS by the seed's trap setting), drawn from the module's own stream
static int64_t add_traps(Value &item_defs, int64_t n, const std::string &traps, PyRandom &rng) {
    std::vector<std::string> kinds;
    for (const auto &e : item_defs.dict())
        if (ends_with(e.key, "-trap") && e.key.compare(0, 3, "zol") != 0) kinds.push_back(e.key);
    const Value &share = prep_data().at("pots").at("traps");
    double frac = share.has(traps) ? share.at(traps).f() : 0.0;
    int64_t n_traps = kinds.empty() ? 0 : py_round((double)n * frac);
    for (int64_t i = 0; i < n_traps; i++) add_qty(item_defs, rng.choice(kinds), 1);
    return n_traps;
}

// pots / bushes / rocks: the rest of the filler by the module's JUNK_MIX
static void junk_mix(Value &item_defs, int64_t left, const Value &mix) {
    int64_t m = left;
    const List &l = mix.list();
    for (size_t i = 0; i < l.size(); i++) {
        std::string item = l[i].list()[0].s();
        int64_t k = i + 1 == l.size() ? left : py_round((double)m * l[i].list()[1].f());
        if (!item_defs.has(item)) {
            Value d = Value::new_dict();
            d.set("quantity", 0);
            item_defs.set(item, d);
        }
        add_qty(item_defs, item, k);
        left -= k;
    }
}

// ---------------------------------------------------------------- pots.py

Value pots_add(const GameFiles &game, const std::vector<std::string> &missable, Value &logic_defs, Value &item_defs,
               const std::string &traps, PyRandom &rng) {
    const Value &P = prep_data().at("pots");
    // scan
    std::vector<Value> pots;
    std::set<int64_t> types;
    for (const Value &t : P.at("types").list()) types.insert(t.i());
    std::string skip_prefix = P.at("skip_level_prefix").s();
    for (const auto &lf : level_files(game, "", "")) {
        const std::string &level = lf.first;
        if (level.compare(0, skip_prefix.size(), skip_prefix) == 0 || P.at("skip_levels").contains(Value(level))) continue;
        std::string room_name = lf.second.substr(0, lf.second.size() - 4);
        if (std::find(missable.begin(), missable.end(), room_name) != missable.end()) continue;
        LebRoom room = read_room(game, level, lf.second);
        int64_t n = 0;
        for (const LebActor &a : room.actors) {
            if (!types.count(a.type)) continue;
            Value p = Value::new_dict();
            p.set("level", level), p.set("room", room_name), p.set("key", hex16(a.key)), p.set("n", (long long)n);
            p.set("shell", shell_index(a));
            pots.push_back(p);
            n++;
        }
    }

    // add_to_logic
    const Value &doors = P.at("doors"), &dungeons = P.at("dungeons"), &vanilla = P.at("vanilla_rooms");
    std::map<std::string, Value> spoiler_of;
    for (const auto &e : logic_defs.dict()) {
        const Value &v = e.value;
        if (v.get("type") == Value("item") && v.get("region").truthy() && v.get("spoiler-region").truthy() &&
            !spoiler_of.count(py_str(v.at("region"))))
            spoiler_of[py_str(v.at("region"))] = v.at("spoiler-region");
    }
    // (every dungeon's, before the first pot joins the logic: a pot location is a D<n>- item location too)
    std::map<std::string, Value> dungeon_cond;
    for (const auto &dl : dungeons.dict()) {
        const std::string &level = dl.key;
        static const std::regex beak("\\s*&\\s*stone-beak-D\\d\\b|\\bstone-beak-D\\d\\s*&\\s*");
        std::string prefix = dungeons.at(level).list()[0].s() + "-";
        std::set<std::string> conds;  // (sorted(set(conds)): str order = UTF-8 byte order)
        for (const auto &e : logic_defs.dict()) {
            const Value &v = e.value;
            if (v.get("type") != Value("item") || e.key.compare(0, prefix.size(), prefix) != 0 ||
                !v.get("condition-basic").truthy())
                continue;
            std::string c = v.at("condition-basic").s();
            if (v.get("subtype") == Value("dungeon-statue")) c = std::regex_replace(c, beak, "");
            conds.insert(c);
        }
        std::vector<Value> parts(conds.begin(), conds.end());
        dungeon_cond[level] = py_and(parts);
    }
    Value locs = Value::new_dict();
    int64_t absorbed = 0;
    for (Value &p : pots) {
        std::string lv = p.at("level").s(), cell = p.at("room").s().substr(p.at("room").s().rfind('_') + 1);
        Value region, cond, spoiler;
        bool in_dungeon = dungeons.has(lv);
        if (in_dungeon) {
            const List &d = dungeons.at(lv).list();
            region = d[1], spoiler = d[2];
            Value own = vanilla.get(d[0].s(), Value::new_dict()).get(cell);
            cond = py_and({own.truthy() ? own : dungeon_cond[lv], Value("break-pot")});
        } else if (P.at("special").has(lv)) {
            const List &s = P.at("special").at(lv).list();
            region = s[0];
            cond = py_and({s[1], Value("break-pot")});
            spoiler = spoiler_of.count(region.s()) ? spoiler_of[region.s()] : Value();
        } else if (doors.has(lv)) {
            const List &s = doors.at(lv).list();
            region = s[0];
            cond = py_and({s[1], Value("break-pot")});
            spoiler = spoiler_of.count(py_str(region)) ? spoiler_of[py_str(region)] : Value();
        } else {
            continue;  // (skipped: no logic for that level)
        }
        // location_name
        int64_t n = p.at("n").i();
        std::string name = in_dungeon ? dungeons.at(lv).list()[0].s() + "-pot-" + cell + "-" + std::to_string(n + 1)
                                      : "pot-" + lv + "-" + cell + "-" + std::to_string(n + 1);
        Value loc = Value::new_dict();
        loc.set("type", "item"), loc.set("subtype", "standing"), loc.set("content", "rupee-5"), loc.set("region", region);
        loc.set("spoiler-region", spoiler.truthy() ? spoiler : Value("mabe-village"));
        loc.set("condition-basic", cond);
        if (in_dungeon) loc.set("room-local", "break-pot");
        if (!p.at("shell").is_none() && absorb(logic_defs, p.at("shell").i(), loc).truthy()) absorbed++;
        logic_defs.set(name, loc);
        locs.set(name, p);
    }
    filler_defs(item_defs, P.at("filler_defs"));
    int64_t n = (int64_t)locs.dict().size() - absorbed;
    int64_t n_traps = add_traps(item_defs, n, traps, rng);
    junk_mix(item_defs, n - n_traps, P.at("junk_mix"));
    return locs;
}

// ---------------------------------------------------------------- bushes.py

Value bushes_add(const GameFiles &game, const TileZones &zones, bool open_mabe, Value &logic_defs, Value &item_defs,
                 const std::string &traps, PyRandom &rng) {
    const Value &B = prep_data().at("bushes");
    int64_t type = B.at("type").i();
    std::set<std::string> removed;
    for (const Value &k : B.at("open_mabe_removed").list()) removed.insert(k.s());
    std::vector<Value> found;
    for (const auto &lf : level_files(game, "Field", "Field_")) {
        std::string room_name = lf.second.substr(0, lf.second.size() - 4);
        LebRoom room = read_room(game, lf.first, lf.second);
        double ox, oz;
        auto holes = pit_tiles(room, ox, oz);
        std::vector<std::pair<double, double>> tips;
        for (const LebActor &t : room.actors)
            if (has_prefix(t.name, "ObjFieldTip")) tips.push_back({t.pos[0], t.pos[2]});
        int64_t n = 0;
        for (const LebActor &a : room.actors) {
            if (a.type != type) continue;
            bool pit = holes.count({(int64_t)py_floordiv(a.pos[0] - ox, 1.5), (int64_t)py_floordiv(a.pos[2] - oz, 1.5)});
            for (auto &t : tips) pit = pit || (fabs(t.first - a.pos[0]) < 0.1 && fabs(t.second - a.pos[2]) < 0.1);
            bool gone = open_mabe && removed.count(hex16(a.key));
            if (!pit && !gone) {
                Value tile = actor_tile(room_name, a);
                Value b = Value::new_dict();
                b.set("level", "Field"), b.set("room", room_name), b.set("key", hex16(a.key)), b.set("n", (long long)n);
                b.set("tile", tile), b.set("shell", shell_index(a));
                b.set("zone", zones.count(room_name) && !tile.is_none() ? zone_at(zones, room_name, tile) : Value());
                found.push_back(b);
            }
            n++;
        }
    }
    Value locs = Value::new_dict();
    int64_t absorbed = 0;
    for (Value &b : found) {
        std::string room = b.at("room").s();
        std::string name = "bush-" + room.substr(room.rfind('_') + 1) + "-" + std::to_string(b.at("n").i() + 1);
        Value loc = logic_entry(room, b.at("tile"), b.at("zone"), {Value("break-bush")});
        if (!b.at("shell").is_none() && absorb(logic_defs, b.at("shell").i(), loc).truthy()) absorbed++;
        logic_defs.set(name, loc);
        locs.set(name, b);
    }
    filler_defs(item_defs, prep_data().at("pots").at("filler_defs"));
    int64_t n = (int64_t)locs.dict().size() - absorbed;
    int64_t n_traps = add_traps(item_defs, n, traps, rng);
    junk_mix(item_defs, n - n_traps, B.at("junk_mix"));
    return locs;
}

// ---------------------------------------------------------------- rocks.py

Value rocks_add(const GameFiles &game, const TileZones &zones, bool classic_d2, bool open_mabe, Value &logic_defs,
                Value &item_defs, const std::string &traps, PyRandom &rng) {
    const Value &R = prep_data().at("rocks");
    int64_t type = R.at("type").i();
    std::set<std::string> removed;
    for (const Value &k : prep_data().at("bushes").at("open_mabe_removed").list()) removed.insert(k.s());
    std::vector<Value> found;
    for (const auto &lf : level_files(game, "Field", "Field_")) {
        std::string room_name = lf.second.substr(0, lf.second.size() - 4);
        LebRoom room = read_room(game, lf.first, lf.second);
        double ox, oz;
        auto holes = pit_tiles(room, ox, oz);
        int64_t n = 0;
        for (const LebActor &a : room.actors) {
            if (a.type != type) continue;
            bool pit = holes.count({(int64_t)py_floordiv(a.pos[0] - ox, 1.5), (int64_t)py_floordiv(a.pos[2] - oz, 1.5)});
            std::string key = hex16(a.key);
            bool gone = (classic_d2 && key == R.at("classic_d2").s()) || (open_mabe && removed.count(key));
            if (!(a.switches[0].first == 1 || key == R.at("golden_leaf").s() || gone || pit)) {
                Value tile = actor_tile(room_name, a);
                Value r = Value::new_dict();
                r.set("level", "Field"), r.set("room", room_name), r.set("key", key), r.set("n", (long long)n);
                r.set("tile", tile), r.set("zone", zone_at(zones, room_name, tile)), r.set("shell", shell_index(a));
                found.push_back(r);
            }
            n++;
        }
    }
    Value locs = Value::new_dict();
    int64_t absorbed = 0;
    for (Value &r : found) {
        std::string room = r.at("room").s();
        std::string name = "rock-" + room.substr(room.rfind('_') + 1) + "-" + std::to_string(r.at("n").i() + 1);
        Value fixed = R.at("border").get(name);
        if (!fixed.truthy()) fixed = R.at("screens").get(room);
        Value loc;
        if (!fixed.truthy()) {
            loc = logic_entry(room, r.at("tile"), r.at("zone"), {Value("break-rock")});
        } else {
            const List &f = fixed.list();
            loc = standing(f[0], area_of(f[0], r.at("zone")), py_and({f[1], Value("break-rock")}));
        }
        if (!r.at("shell").is_none() && absorb(logic_defs, r.at("shell").i(), loc).truthy()) absorbed++;
        logic_defs.set(name, loc);
        locs.set(name, r);
    }
    filler_defs(item_defs, prep_data().at("pots").at("filler_defs"));
    int64_t n = (int64_t)locs.dict().size() - absorbed;
    int64_t n_traps = add_traps(item_defs, n, traps, rng);
    junk_mix(item_defs, n - n_traps, R.at("junk_mix"));
    return locs;
}

// ---------------------------------------------------------------- digs.py

Value digs_add(const GameFiles &game, const TileZones &zones, Value &logic_defs, Value &item_defs) {
    const Value &D = prep_data().at("digs");
    std::vector<std::string> open_actors = str_list(D.at("open_actors"));
    const Value &rank = D.at("rank");
    int64_t bush = prep_data().at("bushes").at("type").i(), lawn = D.at("lawn").i(), shell_type = D.at("shell_type").i();
    std::vector<Value> found;
    for (const auto &lf : level_files(game, "Field", "Field_")) {
        std::string name = lf.second.substr(0, lf.second.size() - 4);
        int row, col;
        if (!screen_pos(name, row, col)) continue;
        LebRoom room = read_room(game, lf.first, lf.second, true);
        if (!room.has_grid || room.grid.room_height != 8) continue;
        auto tile_of = [&](const LebActor &a, int64_t &tx, int64_t &tz) {
            tx = (int64_t)py_floordiv(a.pos[0] - col * 15.0, 1.5), tz = (int64_t)py_floordiv(a.pos[2] - (row - 1) * 12.0, 1.5);
        };
        std::map<int64_t, std::string> blocked;  // (digs._blockers)
        for (const LebActor &a : room.actors) {
            int64_t tx, tz;
            tile_of(a, tx, tz);
            if (!(0 <= tx && tx < 10 && 0 <= tz && tz < 8)) continue;
            bool open = false;
            for (const std::string &p : open_actors) open = open || has_prefix(a.name, p.c_str());
            std::string kind = open ? "" : a.type == bush ? "bush" : a.type == lawn ? "grass" : "other";
            int64_t t = tz * 10 + tx;
            if (!kind.empty() && rank.at(kind).i() > (blocked.count(t) ? rank.at(blocked[t]).i() : 0)) blocked[t] = kind;
        }
        std::map<int64_t, Value> shells;
        for (const LebActor &a : room.actors) {
            if (a.type != shell_type || a.params[0].kind != LebParam::Int) continue;
            int64_t tx, tz;
            tile_of(a, tx, tz);
            shells[tz * 10 + tx] = Value((long long)a.params[0].i);
        }
        for (size_t i = 0; i < room.grid.tiles.size(); i++) {
            const LebTile &t = room.grid.tiles[i];
            if (!(t.flags[2] & 1) || (t.flags[0] & 2) || (t.flags[0] & 1)) continue;
            Value d = Value::new_dict();
            d.set("room", name), d.set("tile", (long long)i), d.set("y", t.elevation);
            d.set("blocker", blocked.count(i) ? Value(blocked[i]) : Value());
            d.set("shell", shells.count(i) ? shells[i] : Value());
            d.set("zone", zones.count(name) ? Value((long long)zones.at(name).at(i)) : Value());
            found.push_back(d);
        }
    }
    std::set<int64_t> proven;
    for (const Value &v : D.at("verified").list()) proven.insert(v.i());
    Value locs = Value::new_dict();
    int64_t absorbed = 0;
    for (Value &d : found) {
        std::string room = d.at("room").s();
        int row, col;
        screen_pos(room, row, col);
        int64_t tile = d.at("tile").i(), id = ((int64_t)(row - 1) * 16 + col) * 80 + tile;
        bool shell = !d.at("shell").is_none();
        if (!shell && (d.at("blocker") == Value("other") || (!proven.empty() && !proven.count(id)))) continue;
        char tail[16];
        snprintf(tail, sizeof tail, "-%02lld", (long long)tile);
        std::string name = "dig-" + room.substr(room.rfind('_') + 1) + tail;
        Value cut = d.at("blocker").is_str() ? D.at("cut").get(d.at("blocker").s()) : Value();
        Value loc = logic_entry(room, d.at("tile"), d.at("zone"), {Value("shovel"), cut});
        if (shell) {
            if (!absorb(logic_defs, d.at("shell").i(), loc).truthy()) continue;  // (no such LAS spot: not a check)
            absorbed++;
        }
        logic_defs.set(name, loc);
        locs.set(name, d);
    }
    filler_defs(item_defs, prep_data().at("pots").at("filler_defs"));
    filler_defs(item_defs, D.at("filler_defs"));
    int64_t n = (int64_t)locs.dict().size() - absorbed, left = n;
    const List &mix = D.at("junk_mix").list();
    for (size_t i = 0; i < mix.size(); i++) {
        int64_t k = i + 1 == mix.size() ? left : py_round((double)n * mix[i].list()[1].f());
        add_qty(item_defs, mix[i].list()[0].s(), k);
        left -= k;
    }
    return locs;
}

// ---------------------------------------------------------------- apple_trees.py

Value apples_add(const GameFiles &game, const TileZones &zones, Value &logic_defs, Value &item_defs,
                 const std::string &traps, PyRandom &rng) {
    const Value &A = prep_data().at("apples");
    int64_t tree = A.at("tree").i(), slots = A.at("slots").i();
    std::vector<Value> trees;
    for (const auto &lf : level_files(game, "Field", "Field_")) {
        std::string room = lf.second.substr(0, lf.second.size() - 4);
        LebRoom r = read_room(game, lf.first, lf.second);
        for (const LebActor &a : r.actors) {
            const LebParam &p = a.params[1];
            bool set = p.kind == LebParam::Str ? !p.s.empty() : p.kind == LebParam::Float ? p.f != 0 : p.i != 0;
            if (a.type != tree || !set) continue;
            if (!A.at("stand").has(room)) throw PyError("RuntimeError", room + ": an apple tree with no reviewed STAND tile");
            Value tile = A.at("stand").at(room);
            region_of(room, tile);  // (raises for a screen without a reviewed region, as the Python does here)
            // (the item landing tiles - seashell_spots.landings - are the seed build's: no logic depends on them)
            Value t = Value::new_dict();
            t.set("room", room), t.set("key", hex16(a.key)), t.set("tile", tile);
            t.set("zone", zones.count(room) ? zone_at(zones, room, tile) : Value());
            trees.push_back(t);
        }
    }
    Value locs = Value::new_dict();
    for (const Value &t : trees) {
        std::string room = t.at("room").s();
        for (int64_t k = 0; k < slots; k++) {
            std::string name = "apple-tree-" + room.substr(room.size() - 3) + "-" + std::to_string(k + 1);
            logic_defs.set(name, logic_entry(room, t.at("tile"), t.at("zone"), {Value("boots")}));
            Value at = t.copy();
            at.set("slot", (long long)k);
            locs.set(name, at);
        }
    }
    filler_defs(item_defs, A.at("filler_defs"));
    int64_t n = (int64_t)locs.dict().size();
    int64_t n_traps = add_traps(item_defs, n, traps, rng), left = n - n_traps;
    const List &filler = A.at("filler").list();
    for (size_t i = 0; i < filler.size(); i++) {  // (traps take red apples' places first)
        int64_t k = i + 1 == filler.size() ? left : std::max<int64_t>(0, filler[i].list()[1].i() - n_traps);
        add_qty(item_defs, filler[i].list()[0].s(), k);
        left -= k;
    }
    return locs;
}

}  // namespace solve
}  // namespace rando
