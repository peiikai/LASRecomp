// Decide mode for the Hint Sheet and the seed's hints (decide.h): the "hint-sheet" and "seed-hints" decisions
// tools/rando/record_ours_items.py records, made here as hint_sheet.apply / seed_hints.apply make what they record.
//
// Their inputs come from the rest of the native pipeline (Decider::input): hint_steps.json and seed_hints.json (the
// post-fill outputs, solve/outputs.h) and post_entrances.json (the entrance shuffle's 'ent' and the logic). What the
// Python adds to them:
//   - a step's landmark (bushes, digs, rocks, apple trees: hint_sheet.landmarks / nearest): the closest building door
//     (entrance_data.yml) or Field object (the game's Field rooms), on the screen or just over its edge;
//   - a step behind a shuffled door (hint_sheet.door_of): that door's name and area (door_name / door_area);
//   - the dungeons' ways in (hint_sheet.via_doors): a house or cave door's name and area in place of its id.
//
// Python behaviour kept: a Hint Sheet without steps records the ways in as {} (they're recorded with each step's
// text); with no Tarin flow the record is empty; the landmark is null (recorded) when nothing is near.
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <set>
#include <stdexcept>

#include "../../../third_party/yyjson/yyjson.h"
#include "../fmt/fixed_hash.h"
#include "../solve/entrances.h"
#include "../solve/outputs_data.h"
#include "decide.h"
#include "ours_items_hints_data.h"
#include "ours_items_lib.h"

namespace rando {
namespace ours {
namespace {

namespace odata = solve::odata;

// ---- Python's arithmetic
double py_floordiv(double a, double b) {  // a // b on floats (Objects/floatobject.c float_divmod)
    double mod = fmod(a, b), div = (a - mod) / b;
    if (mod != 0 && ((b < 0) != (mod < 0))) div -= 1.0;
    if (div == 0) return copysign(0.0, a / b);
    double fl = floor(div);
    if (div - fl > 0.5) fl += 1.0;
    return fl;
}

double num(yyjson_val *v) { return yyjson_is_real(v) ? yyjson_get_real(v) : (double)yyjson_get_sint(v); }
std::string str(yyjson_val *v) { return yyjson_is_str(v) ? yyjson_get_str(v) : ""; }

// ---- entrance_data.yml and the tables exported with it (solve/entrances_data.cpp)
yyjson_val *entrance_tables() {
    static yyjson_doc *doc = [] {
        std::string j = solve::entrance_tables_json();
        return yyjson_read(j.data(), j.size(), 0);
    }();
    return yyjson_doc_get_root(doc);
}
yyjson_val *door_by_id(const std::string &id) {  // entrance_logic.load()['doors'] by id (nullptr: none)
    static const std::map<std::string, yyjson_val *> by_id = [] {
        std::map<std::string, yyjson_val *> m;
        yyjson_val *d;
        yyjson_arr_iter it = yyjson_arr_iter_with(yyjson_obj_get(yyjson_obj_get(entrance_tables(), "entrance_data"), "doors"));
        while ((d = yyjson_arr_iter_next(&it))) m[str(yyjson_obj_get(d, "id"))] = d;
        return m;
    }();
    auto it = by_id.find(id);
    return it == by_id.end() ? nullptr : it->second;
}

// hint_sheet.door_name: what a door looks like from outside, by the level it leads into: DOOR_NAME[lv], else
// DOOR_NAME[re.sub(r'\d+(_.*)?$', '', lv)] (a numbered level's family), else 'a cave'
std::string door_name(yyjson_val *door) {
    std::string lv = str(yyjson_obj_get(door, "dest_level"));
    if (const char *n = odata::get(odata::DOOR_NAME, lv)) return n;
    for (size_t i = 0; i < lv.size(); i++) {  // (the leftmost digit run that ends the name or a '_')
        if (!isdigit((unsigned char)lv[i])) continue;
        size_t j = i;
        while (j < lv.size() && isdigit((unsigned char)lv[j])) j++;
        if (j == lv.size() || lv[j] == '_') {
            lv.resize(i);
            break;
        }
        i = j;
    }
    if (const char *n = odata::get(odata::DOOR_NAME, lv)) return n;
    return "a cave";
}

// field_regions.tile_zones: {Field room: [80 zone ids]} from the game's Field.lvb ({} without it); read once per
// decision (Zones)
using ZoneMap = std::map<std::string, std::vector<int64_t>>;
ZoneMap tile_zones(SeedBuild &b) {
    ZoneMap out;
    std::string data;
    if (!b.read_game("region_common/level/Field/Field.lvb", data)) return out;
    FixedHash fh = FixedHash::parse(data);
    std::vector<int64_t> zones;
    for (const FhEntry &z : fh.find("zone")->sub->entries) zones.push_back((int64_t)read_le(z.data, 0, 4));
    const std::string &grid = fh.find("grid")->data, &cfg = fh.find("gridConfig")->data;
    int gh = (int)read_le(cfg, 0, 2), gw = (int)read_le(cfg, 2, 2);
    for (int row = 0; row < 16; row++)
        for (int col = 0; col < 16; col++) {
            std::vector<int64_t> zs;
            for (int z = 0; z < 8; z++)
                for (int x = 0; x < 10; x++) {
                    int gx = col * 10 + x, gz = row * 8 + z;
                    int zi = gz < gh && gx < gw ? (uint8_t)grid.at((size_t)((gz * gw + gx) * 8 + 5)) : 255;
                    zs.push_back(zi < (int)zones.size() ? zones[(size_t)zi] : -1);
                }
            char name[16];
            snprintf(name, sizeof name, "Field_%02d%c", row + 1, 65 + col);
            out[name] = zs;
        }
    return out;
}

struct Zones {
    explicit Zones(SeedBuild &build) : b(build) {}
    SeedBuild &b;
    bool read = false;
    ZoneMap map;
    const ZoneMap &get() {
        if (!read) map = tile_zones(b), read = true;
        return map;
    }
};

// hint_sheet.door_area: the area a door stands in: its spot's place on the Field, else its region's
std::string door_area(Zones &zs, yyjson_val *door) {
    std::string room = str(yyjson_obj_get(door, "field_room"));
    yyjson_val *xz = yyjson_obj_get(door, "pos_xz");
    double x = num(yyjson_arr_get(xz, 0)), z = num(yyjson_arr_get(xz, 1));
    int col = room.back() - 65, row = atoi(room.substr(room.size() - 3, 2).c_str());
    long tx = std::min(9L, std::max(0L, (long)((x - col * 15) / 1.5)));  // (int(): towards zero)
    long tz = std::min(7L, std::max(0L, (long)((z - (row - 1) * 12) / 1.5)));
    const ZoneMap &zones = zs.get();
    auto it = zones.find(room);
    if (it != zones.end())  // (area_of: ZONE_AREA.get(zone) or REGION_AREA.get(region, 'mabe-village'))
        for (const auto &e : odata::ZONE_AREA)
            if (e.first == it->second.at((size_t)(tz * 10 + tx)) && *e.second) return e.second;
    yyjson_val *region = yyjson_obj_get(door, "outer_region");
    const char *a = yyjson_is_str(region) ? odata::get(odata::REGION_AREA, yyjson_get_str(region)) : nullptr;
    return a ? a : "mabe-village";
}

// hint_sheet.via_doors: a dungeon's way in that is a house / cave door (an entrance_data id) -> {name, area}
void put_via(Zones &zs, yyjson_mut_doc *doc, yyjson_mut_val *obj, const char *key, yyjson_val *way) {
    yyjson_val *door = yyjson_is_str(way) ? door_by_id(yyjson_get_str(way)) : nullptr;
    yyjson_mut_val *v;
    if (door) {
        v = yyjson_mut_obj(doc);
        yyjson_mut_obj_add_strcpy(doc, v, "name", door_name(door).c_str());
        yyjson_mut_obj_add_strcpy(doc, v, "area", door_area(zs, door).c_str());
    } else {
        v = yyjson_val_mut_copy(doc, way);
    }
    yyjson_mut_obj_put(obj, yyjson_mut_strcpy(doc, key), v);  // (a repeated key: the later value, as a dict)
}

// ---- landmarks (hint_sheet.landmarks, nearest)
struct Mark { std::string name; double x, z; };

std::vector<std::string> field_rooms(SeedBuild &b) {  // sorted(glob('Field/Field_*.leb')): the game's
    std::vector<std::string> out;
    for (const std::string &n : b.list_game("region_common/level/Field"))
        if (n.size() > 10 && !n.compare(0, 6, "Field_") && !n.compare(n.size() - 4, 4, ".leb")) out.push_back(n);
    return out;
}
LebRoom game_room(SeedBuild &b, const std::string &file) {
    std::string bytes;
    if (!b.read_game("region_common/level/Field/" + file, bytes)) throw std::runtime_error("hint sheet: no " + file);
    return LebRoom::parse(bytes);
}

std::vector<Mark> landmarks(SeedBuild &b) {
    std::vector<Mark> out;
    yyjson_val *d;
    yyjson_arr_iter it = yyjson_arr_iter_with(yyjson_obj_get(yyjson_obj_get(entrance_tables(), "entrance_data"), "doors"));
    while ((d = yyjson_arr_iter_next(&it))) {
        yyjson_val *xz = yyjson_obj_get(d, "pos_xz");
        out.push_back({door_name(d), num(yyjson_arr_get(xz, 0)), num(yyjson_arr_get(xz, 1))});
    }
    for (const std::string &f : field_rooms(b))
        for (const LebActor &a : game_room(b, f).actors)
            for (const auto &k : hints::ACTOR_NAME)  // (every prefix it starts with: no break)
                if (!a.name.compare(0, strlen(k.first), k.first)) out.push_back({k.second, a.pos[0], a.pos[2]});
    return out;
}

// the landmark on the same screen closest to (x, z), else one just over the screen edge (within 6), else none
bool nearest(const std::vector<Mark> &marks, double x, double z, std::string &name) {
    double sx = (double)(int64_t)py_floordiv(x, 15), sz = (double)(int64_t)py_floordiv(z, 12);
    bool found = false, best_off = false;
    double best_d = 0;
    for (const Mark &m : marks) {
        double d = pow(pow(m.x - x, 2) + pow(m.z - z, 2), 0.5);
        bool same = (double)(int64_t)py_floordiv(m.x, 15) == sx && (double)(int64_t)py_floordiv(m.z, 12) == sz;
        if (!same && d > 6) continue;
        bool off = !same;
        if (!found || off < best_off || (off == best_off && d < best_d)) found = true, best_off = off, best_d = d, name = m.name;
    }
    return found;
}

// tile_xz: the centre of tile 0-79 of Field screen `room` ('12C' or 'Field_12C')
void tile_xz(const std::string &room, int tile, double &x, double &z) {
    int row = atoi(room.substr(room.size() - 3, 2).c_str()), col = room.back() - 65;
    x = (double)(col * 15) + (tile % 10) * 1.5 + 0.75;
    z = (double)((row - 1) * 12) + (tile / 10) * 1.5 + 0.75;
}

// bushes._tile: the screen tile an actor stands on (false: off the screen)
bool actor_tile(const std::string &room, const LebActor &a, int &tile) {
    int row = atoi(room.substr(room.size() - 3, 2).c_str()), col = room.back() - 65;
    double tx = py_floordiv(a.pos[0] - col * 15.0, 1.5), tz = py_floordiv(a.pos[2] - (row - 1) * 12.0, 1.5);
    if (!(0 <= tx && tx < 10 && 0 <= tz && tz < 8)) return false;
    tile = (int)tz * 10 + (int)tx;
    return true;
}

bool screen_room(const std::string &room) {  // re.match(r'Field_(\d\d)([A-P])$')
    return room.size() == 9 && !room.compare(0, 6, "Field_") && isdigit((unsigned char)room[6]) &&
           isdigit((unsigned char)room[7]) && room[8] >= 'A' && room[8] <= 'P';
}

// a bush's / rock's (bushes.scan(pits=True) / rocks.scan) Field room and tile, by its location name: the n-th actor of
// that type on the screen (n counts every one, so the scans' filters don't change it). false: none, or no tile.
bool scanned_tile(SeedBuild &b, const std::string &loc, size_t prefix, int type, std::string &room, int &tile, bool &has_tile) {
    size_t dash = loc.find('-', prefix);
    if (dash == std::string::npos) return false;
    room = "Field_" + loc.substr(prefix, dash - prefix);
    int k = atoi(loc.c_str() + dash + 1);
    std::string file = room + ".leb", bytes;
    if (!b.read_game("region_common/level/Field/" + file, bytes)) return false;
    int n = 0;
    for (const LebActor &a : LebRoom::parse(bytes).actors) {
        if (a.type != type) continue;
        if (++n == k) {
            has_tile = screen_room(room) && actor_tile(room, a, tile);
            return true;
        }
    }
    return false;
}

bool re_match(const std::string &s, const char *prefix, const char *body, std::string &room, int &n) {
    // ^<prefix>(\d\d[A-P])-(\d+)$ (body "dig") / ^apple-tree-(\d\d[A-P])-\d$
    size_t p = strlen(prefix);
    if (s.compare(0, p, prefix) || s.size() < p + 5) return false;
    if (!isdigit((unsigned char)s[p]) || !isdigit((unsigned char)s[p + 1]) || s[p + 2] < 'A' || s[p + 2] > 'P' || s[p + 3] != '-')
        return false;
    std::string tail = s.substr(p + 4);
    if (tail.empty() || tail.find_first_not_of("0123456789") != std::string::npos) return false;
    if (!strcmp(body, "apple") && tail.size() != 1) return false;
    room = s.substr(p, 3), n = atoi(tail.c_str());
    return true;
}

bool is_rock(const std::string &loc) {  // re.match(r'rock-\d\d[A-P]-')
    return loc.size() >= 9 && !loc.compare(0, 5, "rock-") && isdigit((unsigned char)loc[5]) && isdigit((unsigned char)loc[6]) &&
           loc[7] >= 'A' && loc[7] <= 'P' && loc[8] == '-';
}

// ---- hint_sheet.door_of: the door the player walks through to a check in a shuffled interior / connector cave
yyjson_val *door_of(const std::string &loc, yyjson_val *logic_defs, yyjson_val *ent) {
    if (!ent || !las::py_truthy(ent)) return nullptr;
    yyjson_val *doors = yyjson_arr_get(ent, 0), *entry = yyjson_arr_get(ent, 1);
    std::string region = str(yyjson_obj_get(yyjson_obj_get(logic_defs, loc.c_str()), "region"));
    std::set<std::string> targets;
    yyjson_val *d;
    if (!region.compare(0, 7, "inside-")) {
        targets.insert(region.substr(7));
    } else if (!region.compare(0, 5, "conn-")) {
        yyjson_arr_iter it = yyjson_arr_iter_with(doors);
        while ((d = yyjson_arr_iter_next(&it))) {
            yyjson_val *c = yyjson_obj_get(d, "connector");
            if (!c || !las::py_truthy(c)) continue;
            if ("conn-" + str(c) + "-" + str(yyjson_obj_get(d, "node")) == region) targets.insert(str(yyjson_obj_get(d, "id")));
        }
    } else {
        return nullptr;
    }
    std::string first;
    bool any = false;
    yyjson_val *k;
    yyjson_obj_iter oi = yyjson_obj_iter_with(entry);
    while ((k = yyjson_obj_iter_next(&oi))) {
        yyjson_val *t = yyjson_obj_iter_get_val(k);
        if (yyjson_is_str(t) && targets.count(yyjson_get_str(t)) && (!any || yyjson_get_str(k) < first)) first = yyjson_get_str(k), any = true;
    }
    if (!any || targets.count(first)) return nullptr;  // (its own door: where it always was)
    yyjson_arr_iter it = yyjson_arr_iter_with(doors);
    while ((d = yyjson_arr_iter_next(&it)))
        if (str(yyjson_obj_get(d, "id")) == first) return d;
    throw std::runtime_error("hint sheet: no door " + first);  // (by_id[...]: KeyError)
}

struct Json {  // a parsed input (freed with it)
    yyjson_doc *doc = nullptr;
    explicit Json(const std::string *text) {
        if (text && !(doc = yyjson_read(text->data(), text->size(), 0))) throw std::runtime_error("decide: bad JSON input");
    }
    ~Json() { yyjson_doc_free(doc); }
    Json(const Json &) = delete;
    Json &operator=(const Json &) = delete;
    yyjson_val *root() const { return yyjson_doc_get_root(doc); }
};

std::string write(yyjson_mut_doc *doc) {
    size_t n;
    char *t = yyjson_mut_write(doc, 0, &n);
    std::string s(t, n);
    free(t);
    yyjson_mut_doc_free(doc);
    return s;
}

}  // namespace

std::string decide_hint_sheet(SeedBuild &b) {
    Decider *dc = b.decider();
    const std::string *steps_json = dc->input("hint_steps.json");
    if (!steps_json) return "";  // (no hint_steps.json: hint_sheet.apply returns before the record starts)
    if (!b.files().count("region_common/event/Tarin.bfevfl")) return "{\"steps\":[],\"entered-via\":{}}";
    Json data(steps_json), post(dc->input("post_entrances.json"));
    yyjson_mut_doc *doc = yyjson_mut_doc_new(nullptr);
    yyjson_mut_val *rec = yyjson_mut_obj(doc), *steps = yyjson_mut_arr(doc), *via = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, rec);
    yyjson_mut_obj_add_val(doc, rec, "steps", steps);

    // dungeon-entrances: entrance -> the dungeon it leads into; the texts need dungeon -> its way in
    std::vector<std::pair<std::string, yyjson_val *>> inv;
    yyjson_val *k;
    yyjson_obj_iter oi = yyjson_obj_iter_with(yyjson_obj_get(data.root(), "dungeon-entrances"));
    while ((k = yyjson_obj_iter_next(&oi))) {
        yyjson_val *inside = yyjson_obj_iter_get_val(k);
        std::string key = str(inside);
        bool seen = false;
        for (auto &e : inv)
            if (e.first == key) e.second = k, seen = true;  // (a dict: the later door, in the first one's place)
        if (!seen) inv.push_back({key, k});
    }
    Zones zs(b);
    for (const auto &e : inv) put_via(zs, doc, via, e.first.c_str(), e.second);

    yyjson_val *all = yyjson_obj_get(data.root(), "steps"), *st;
    // landmarks: only when a step is a bush / dig / apple tree / rock (the Python reads the Field only then)
    std::vector<Mark> marks;
    bool any_spot = false, any_rock = false;
    yyjson_arr_iter it = yyjson_arr_iter_with(all);
    while ((st = yyjson_arr_iter_next(&it))) {
        std::string loc = str(yyjson_obj_get(st, "loc"));
        any_spot |= !loc.compare(0, 5, "bush-") || !loc.compare(0, 4, "dig-") || !loc.compare(0, 11, "apple-tree-") || is_rock(loc);
        any_rock |= is_rock(loc);
    }
    if (any_spot) marks = landmarks(b);
    yyjson_val *ent = post.doc ? yyjson_obj_get(post.root(), "ent") : nullptr;
    yyjson_val *logic = post.doc ? yyjson_obj_get(post.root(), "logic_defs") : nullptr;
    int bush_type = (int)yyjson_get_int(yyjson_obj_get(entrance_tables(), "bush_type"));
    int rock_type = (int)yyjson_get_int(yyjson_obj_get(entrance_tables(), "rock_type"));
    it = yyjson_arr_iter_with(all);
    while ((st = yyjson_arr_iter_next(&it))) {
        std::string loc = str(yyjson_obj_get(st, "loc"));
        yyjson_mut_val *o = yyjson_mut_obj(doc);
        for (const char *key : {"loc", "item", "region"})
            if (yyjson_val *v = yyjson_obj_get(st, key)) yyjson_mut_obj_add_val(doc, o, key, yyjson_val_mut_copy(doc, v));
        if (any_spot) {
            std::string room;
            int n = 0, tile = 0;
            bool spot = false, has_tile = false;
            if (re_match(loc, "dig-", "dig", room, n)) {
                spot = true, has_tile = true, tile = n;
            } else if (!loc.compare(0, 5, "bush-")) {
                spot = scanned_tile(b, loc, 5, bush_type, room, tile, has_tile);
            } else if (any_rock && is_rock(loc)) {
                spot = scanned_tile(b, loc, 5, rock_type, room, tile, has_tile);
            }
            if (re_match(loc, "apple-tree-", "apple", room, n)) {  // (the tile Link dashes into the tree from)
                yyjson_val *stand = yyjson_obj_get(yyjson_obj_get(entrance_tables(), "apple_stand"), ("Field_" + room).c_str());
                if (!stand) throw std::runtime_error("hint sheet: no apple tree stand tile in " + room);  // (KeyError)
                spot = true, has_tile = true, tile = (int)yyjson_get_int(stand);
            }
            if (spot && has_tile) {
                double x, z;
                tile_xz(room, tile, x, z);
                std::string name;
                if (nearest(marks, x, z, name)) yyjson_mut_obj_add_strcpy(doc, o, "landmark", name.c_str());
                else yyjson_mut_obj_add_null(doc, o, "landmark");
            }
        }
        if (yyjson_val *door = door_of(loc, logic, ent)) {  // (entrance rando: a shuffled house or cave is where its door is)
            yyjson_mut_obj_add_strcpy(doc, o, "door-area", door_area(zs, door).c_str());
            yyjson_mut_obj_add_strcpy(doc, o, "door-name", door_name(door).c_str());
        }
        yyjson_mut_arr_append(steps, o);
    }
    // (the ways in are recorded with each step's text: none without steps)
    yyjson_mut_obj_add_val(doc, rec, "entered-via", yyjson_mut_arr_size(steps) ? via : yyjson_mut_obj(doc));
    return write(doc);
}

std::string decide_seed_hints(SeedBuild &b) {
    const std::string *text = b.decider()->input("seed_hints.json");
    if (!text) return "";  // (no seed_hints.json: nothing recorded)
    Json data(text);
    yyjson_mut_doc *doc = yyjson_mut_doc_new(nullptr);
    yyjson_mut_val *rec = yyjson_mut_obj(doc), *hints = yyjson_mut_arr(doc), *via = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, rec);
    yyjson_val *h;
    yyjson_arr_iter it = yyjson_arr_iter_with(yyjson_obj_get(data.root(), "hints"));
    while ((h = yyjson_arr_iter_next(&it))) {
        yyjson_mut_val *o = yyjson_val_mut_copy(doc, h);
        yyjson_val *id = yyjson_obj_get(h, "door");
        if (yyjson_val *door = yyjson_is_str(id) ? door_by_id(yyjson_get_str(id)) : nullptr)
            yyjson_mut_obj_put(o, yyjson_mut_str(doc, "door-name"), yyjson_mut_strcpy(doc, door_name(door).c_str()));
        yyjson_mut_arr_append(hints, o);
    }
    yyjson_val *k;
    Zones zs(b);
    yyjson_obj_iter oi = yyjson_obj_iter_with(yyjson_obj_get(data.root(), "entered-via"));  // (None / {}: no ways)
    while ((k = yyjson_obj_iter_next(&oi))) put_via(zs, doc, via, yyjson_get_str(k), yyjson_obj_iter_get_val(k));
    yyjson_mut_obj_add_val(doc, rec, "hints", hints);
    yyjson_mut_obj_add_val(doc, rec, "via", via);
    return write(doc);
}

}  // namespace ours
}  // namespace rando
