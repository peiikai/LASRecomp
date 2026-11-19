// The enemy randomizer (las_enemies.h): mod_generator.randomizeEnemies ported from LAS-Randomizer's
// RandomizerCore/mod_generator.py and Randomizers/enemies.py (MIT, Owen-Splat and contributors), with LAS Recomp's
// additions there (mini-bosses on the Field with their kits and rails, Random mode's slots), and the tables
// tools/rando/roam.py and tools/rando/enemy_random.py write.
//
// randomizeEnemies re-seeds the shared stream with the seed (random.seed(self.seed)), so its draws depend only on the
// seed, the settings and the rooms as the steps before it left them. Decide mode makes them (roll_room, the Python's
// shuffleEnemyActors line for line) and records what they did as tools/rando/record_las_world.py does; both modes then
// edit the rooms from that record. Random mode's enemies_random.tsv needs, per room, the actors the roll made a slot
// for: every one it visited but those it made mini-bosses, which the record marks "m" (a Shadow Link or Blaino may be
// either). Apply mode finds them from the room and the record: it never draws.
//
// Python quirks kept:
//   - the restriction list is a list: an enemy group joins it again each time one of it is rolled, and an actor's own
//     type leaves it once;
//   - shuffleEnemyActors walks the actor list as it grows, so the kit actors a mini-boss brings are visited too (they
//     aren't enemies: skipped);
//   - a mini-boss skips the size roll (its `continue`), and Random mode's slots;
//   - Cue Ball's spot for the mini-boss gap is the corner its rail moved him to;
//   - a room in Random mode is written even when no draw changed it (its slots make it edited).
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>

#include "../../../third_party/yyjson/yyjson.h"
#include "decide.h"
#include "las_enemies.h"
#include "las_lib.h"

namespace rando {
namespace las {
namespace {

using namespace enemy_data;

bool S(SeedBuild &b, const char *key) { return setting_or(b.decisions(), key, false); }

// ======== mini-boss kits


// LAS Recomp's mini-boss kits (enemies.MINIBOSS_KITS): the arena room and the actor types copied from it
struct Kit { uint16_t type; const char *arena; std::vector<uint16_t> with; };
const std::vector<Kit> &kits() {
    static const std::vector<Kit> k = {
        {0x5C, "Lv06FaceShrine_05F", {0x1E4}},  // Smasher's iron ball
        {0x51, "Lv01TailCave_06G", {0x11A}},    // Rolling Bones' spike bar
        {0x58, "Lv04AnglersTunnel_03E", {}},    // Cue Ball (its rail)
        {0x57, "Field_13O", {}},                // Lanmola (his rail)
    };
    return k;
}
const uint8_t KIT_KEY_TAG = 0x6B;  // (copied kit actors get keys of their own)
const uint16_t RAIL_FIT = 0x58;    // (Cue Ball circles a 4-point rail fitted to the room)

uint32_t crc32(const std::string &s) {  // zlib.crc32
    uint32_t c = 0xFFFFFFFFu;
    for (unsigned char ch : s) {
        c ^= ch;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

uint16_t get_u16(const Bytes &d, size_t at) { return (uint16_t)read_le(d, at, 2); }
void set_u16(Bytes &d, size_t at, uint16_t v) { d[at] = (char)(v & 0xFF), d[at + 1] = (char)(v >> 8); }
void set_u32(Bytes &d, size_t at, uint32_t v) {
    for (int i = 0; i < 4; i++) d[at + i] = (char)((v >> (8 * i)) & 0xFF);
}
void set_f32(Bytes &d, size_t at, double v) {  // struct.pack_into('<f')
    float f = (float)v;
    uint32_t u;
    memcpy(&u, &f, 4);
    set_u32(d, at, u);
}

FixedHash &section(LebRoom &r, const char *name) {
    FhEntry *e = r.fh.find(name);
    if (!e || !e->sub) throw std::runtime_error(std::string("las: room has no ") + name + " section");
    return *e->sub;
}

// enemies.fit_rail: the 4-point rail of the actor becomes the largest rectangle of free tiles with its tile as a
// corner; the actor moves onto that corner
void fit_rail(LebRoom &room, size_t mi, FixedHash &points, FhEntry &rail) {
    const FhEntry *grid = room.fh.find("grid");
    if (!grid || !grid->sub) return;
    const FhEntry *data = nullptr, *info = nullptr;
    for (const FhEntry &e : grid->sub->entries) {
        if (e.name == "data") data = &e;
        else if (e.name == "info") info = &e;
    }
    if (!info || !data) throw std::runtime_error("las: grid without data / info");
    const int height = (int)read_le(info->data, 0, 2), width = (int)read_le(info->data, 2, 2);
    const double ts = read_f32(info->data, 4), gx = read_f32(info->data, 8), gz = read_f32(info->data, 0xC);
    if (height != 8) return;  // (a side view: 2, or not a grid the Python reads)
    LebActor &act = room.actors[mi];
    auto free = [&](int row, int col) {
        if (!(0 <= row && row < height && 0 <= col && col < width)) return false;
        size_t t = 0x10 * (size_t)(row * width + col);
        uint8_t f1 = (uint8_t)read_le(data->data, t, 1), f3 = (uint8_t)read_le(data->data, t + 2, 1);
        return !((f1 & 2) || (f1 & 1) || (f3 & 4));  // (collision, deep water / lava, water / lava)
    };
    const int c0 = (int)((act.pos[0] - gx) / ts), r0 = (int)((act.pos[2] - gz) / ts);
    int best = -1, bw = 0, bh = 0, bdx = 0, bdz = 0;
    for (int w = 7; w > 1; w--)
        for (int h = 5; h > 1; h--)
            for (const auto &dd : {std::make_pair(1, 1), std::make_pair(1, -1), std::make_pair(-1, 1), std::make_pair(-1, -1)}) {
                const int dx = dd.first, dz = dd.second;
                const int cmin = std::min(c0, c0 + dx * w), cmax = std::max(c0, c0 + dx * w);
                const int rmin = std::min(r0, r0 + dz * h), rmax = std::max(r0, r0 + dz * h);
                bool ok = true;
                for (int cc = cmin; cc <= cmax && ok; cc++)
                    ok = free(r0, cc) && free(r0 + dz * h, cc);
                for (int rr = rmin; rr <= rmax && ok; rr++)
                    ok = free(rr, c0) && free(rr, c0 + dx * w);
                if (ok && (best < 0 || w * h > best)) best = w * h, bw = w, bh = h, bdx = dx, bdz = dz;
            }
    if (best < 0) return;
    auto ctr = [&](int rr, int cc, double &x, double &z) {
        volatile double px = (cc + 0.5) * ts, pz = (rr + 0.5) * ts;  // (kept apart from the add: the Python rounds twice)
        x = gx + px, z = gz + pz;
    };
    double cx[4], cz[4];
    ctr(r0, c0, cx[0], cz[0]);
    ctr(r0 + bdz * bh, c0, cx[1], cz[1]);
    ctr(r0 + bdz * bh, c0 + bdx * bw, cx[2], cz[2]);
    ctr(r0, c0 + bdx * bw, cx[3], cz[3]);
    const size_t n = std::min<size_t>(4, get_u16(rail.data, 0x2C));
    for (size_t k = 0; k < n; k++) {
        size_t pi = get_u16(rail.data, 0x30 + 2 * k);
        Bytes &pd = points.entries.at(pi).data;
        set_f32(pd, 0, cx[k]), set_f32(pd, 4, act.pos[1] + 0.2), set_f32(pd, 8, cz[k]);
    }
    act.pos[0] = cx[0], act.pos[2] = cz[0];
}

// enemies.add_miniboss_kit: the kit actors of room.actors[mi]'s mini-boss copied from its arena (same offsets, linked
// as there), every kit actor's rails copied with their points
void add_miniboss_kit(LebRoom &room, size_t mi, const Kit &kit, LebRoom &arena, const std::string &room_name) {
    const uint16_t type = room.actors.at(mi).type;
    size_t src_i = arena.actors.size();
    for (size_t i = 0; i < arena.actors.size(); i++)
        if (arena.actors[i].type == type) { src_i = i; break; }
    if (src_i == arena.actors.size()) throw std::runtime_error("las: no mini-boss in its arena " + std::string(kit.arena));
    const LebActor src = arena.actors[src_i];
    std::vector<size_t> take = {src_i};
    for (size_t i = 0; i < arena.actors.size(); i++)
        for (uint16_t t : kit.with)
            if (arena.actors[i].type == t) { take.push_back(i); break; }
    std::map<size_t, size_t> newidx = {{src_i, mi}};
    const double ax = room.actors[mi].pos[0], ay = room.actors[mi].pos[1], az = room.actors[mi].pos[2];
    const uint32_t room_id = room.actors[mi].room_id;
    const uint64_t crc = crc32(room_name) & 0xFFFFFF;
    for (size_t t = 1; t < take.size(); t++) {
        LebActor a = arena.actors[take[t]];
        a.key = ((uint64_t)KIT_KEY_TAG << 56) | (crc << 32) | ((uint64_t)(room.actors.size() & 0xFFFF) << 16) | (a.key & 0xFFFF);
        char hex[18];
        snprintf(hex, sizeof hex, "-%016llX", (unsigned long long)a.key);
        a.name = a.name.substr(0, a.name.find('-')) + hex;
        a.pos[0] = ax + (a.pos[0] - src.pos[0]), a.pos[1] = ay + (a.pos[1] - src.pos[1]), a.pos[2] = az + (a.pos[2] - src.pos[2]);
        a.room_id = room_id;
        room.actors.push_back(a);
        newidx[take[t]] = room.actors.size() - 1;
    }
    FixedHash &sp = section(arena, "point"), &sr = section(arena, "rail");
    FixedHash &dp = section(room, "point"), &dr = section(room, "rail");
    std::map<uint32_t, uint32_t> rail_map, point_map;
    for (size_t i : take) {
        const LebLinks &srl = arena.actors[i].links;
        LebLinks &rl = room.actors[newidx[i]].links;
        std::vector<LebLinks::ActorLink> s1;
        for (const auto &s : srl.actors)
            if (newidx.count(s.actor)) { auto n = s; n.actor = (uint32_t)newidx[s.actor]; s1.push_back(n); }
        std::vector<uint32_t> s3;
        for (uint32_t j : srl.ids)
            if (newidx.count(j)) s3.push_back((uint32_t)newidx[j]);
        rl.actors = s1, rl.ids = s3;
        std::vector<LebLinks::RailLink> rails;
        for (const auto &s2 : srl.rails) {
            const uint32_t ri = s2.rail;
            if (!rail_map.count(ri)) {
                FhEntry rail = sr.entries.at(ri);
                Bytes &d = rail.data;
                for (size_t k = 0, n = get_u16(d, 0x2C); k < n; k++) {
                    const uint16_t pi = get_u16(d, 0x30 + 2 * k);
                    if (!point_map.count(pi)) {
                        FhEntry pt = sp.entries.at(pi);
                        const double x = read_f32(pt.data, 0), y = read_f32(pt.data, 4), z = read_f32(pt.data, 8);
                        set_f32(pt.data, 0, ax + (x - src.pos[0]));
                        set_f32(pt.data, 4, ay + (y - src.pos[1]));
                        set_f32(pt.data, 8, az + (z - src.pos[2]));
                        dp.entries.push_back(pt);
                        point_map[pi] = (uint32_t)dp.entries.size() - 1;
                    }
                    set_u16(d, 0x30 + 2 * k, (uint16_t)point_map[pi]);
                }
                for (size_t k = 0; k < 4; k++)  // (string params point into the names, which repack rebuilds: ints)
                    set_u32(d, 0xC + 8 * k, 0), set_u32(d, 0x10 + 8 * k, 3);
                dr.entries.push_back(rail);
                rail_map[ri] = (uint32_t)dr.entries.size() - 1;
            }
            LebLinks::RailLink n = s2;
            n.rail = rail_map[ri];
            if (point_map.count(s2.point)) n.point = point_map[s2.point];
            rails.push_back(n);
        }
        rl.rails = rails;
        rl.x = (uint8_t)rl.actors.size(), rl.y = (uint8_t)rl.ids.size(), rl.z = (uint8_t)rl.rails.size();
    }
    LebActor &act = room.actors[mi];
    if (act.type == RAIL_FIT && !act.links.rails.empty())
        fit_rail(room, mi, section(room, "point"), section(room, "rail").entries.at(act.links.rails[0].rail));
}

LebParam json_param(yyjson_val *v) {
    if (yyjson_is_str(v)) return str_param(yyjson_get_str(v));
    if (yyjson_is_real(v)) return LebParam::of_float(yyjson_get_real(v));
    if (yyjson_is_int(v)) return LebParam::of_int((uint32_t)yyjson_get_sint(v));
    throw std::runtime_error("las: bad enemy parameter in the decisions");
}

// ======== the roll: enemies.shuffleEnemyActors with randomizeEnemies' pools

bool has(const std::vector<int> &v, int x) { return std::find(v.begin(), v.end(), x) != v.end(); }
bool has(const std::vector<const char *> &v, const std::string &x) {
    for (const char *s : v)
        if (x == s) return true;
    return false;
}

const EnemyActor &actor_of(int id) {  // [v for v in ENEMY_DATA['Actors'].values() if v['id'] == id][0]
    for (const EnemyActor &a : ACTORS)
        if (a.id == id) return a;
    throw std::runtime_error("las: enemy " + std::to_string(id) + " isn't in ENEMY_DATA");
}

LebParam value_param(const EnemyValue &v) {
    if (v.kind == EnemyValue::Str) return str_param(v.s);
    if (v.kind == EnemyValue::Float) return LebParam::of_float(v.f);
    return LebParam::of_int((uint32_t)v.i);
}

// randomizeEnemies' enemy_ids and enemy_settings
struct Pools {
    std::vector<int> land, air, no_vire, water, water2d, water_shallow, tree, hole;
    std::set<int> total;  // (total_ids)
    bool types = false, sizes = false, minibosses = false, random = false;
    int freq = 0;  // (0 - 3: Normal, 2x, 3x, All)
};

Pools pools(SeedBuild &b) {
    const Decisions &d = b.decisions();
    Pools P;
    P.types = S(b, "randomize-enemies"), P.sizes = S(b, "randomize-enemy-sizes");
    P.minibosses = S(b, "enemy-minibosses");
    P.random = (setting_val(d, "enemy-mode") ? d.setting_str("enemy-mode") : "seeded") == "random";
    yyjson_val *f = setting_val(d, "miniboss-frequency");  // int(settings.get('miniboss-frequency', 0))
    long long freq = 0;
    if (yyjson_is_int(f) || yyjson_is_bool(f)) freq = yyjson_is_bool(f) ? yyjson_get_bool(f) : yyjson_get_sint(f);
    else if (yyjson_is_real(f)) freq = (long long)yyjson_get_real(f);
    else if (yyjson_is_str(f)) freq = atoll(yyjson_get_str(f));
    P.freq = (int)std::min(3LL, std::max(0LL, freq));
    for (const EnemyActor &a : ACTORS) {
        if (a.minibosses_only && !P.minibosses) continue;  // (Shadow Link and co. only with Include mini-bosses)
        const std::string t = a.type;
        std::vector<int> *to = t == "land" ? &P.land : t == "air" ? &P.air : t == "water" ? &P.water
                             : t == "water2D" ? &P.water2d : t == "water-shallow" ? &P.water_shallow
                             : t == "tree" ? &P.tree : t == "hole" ? &P.hole : nullptr;
        if (to) to->push_back(a.id), P.total.insert(a.id);
    }
    P.no_vire = P.air;
    auto vire = std::find(P.no_vire.begin(), P.no_vire.end(), 0x26);  // (list.remove: the first, which must be there)
    if (vire == P.no_vire.end()) throw std::runtime_error("las: no Vire in the air enemies");
    P.no_vire.erase(vire);
    return P;
}

// enemies.kill_linked: the enemies a kill-all trigger watches (and every one in a room with a shutter no switch drives)
std::set<size_t> kill_linked(const LebRoom &room, const std::set<int> &pool) {
    std::set<size_t> out;
    for (const LebActor &a : room.actors) {
        if (a.type != OBJ_CLOSED_DOOR) continue;
        bool driven = false;
        for (const auto &sw : a.switches) driven |= sw.first == 1 || sw.first == 2;
        if (!driven) {
            for (size_t i = 0; i < room.actors.size(); i++)
                if (pool.count(room.actors[i].type)) out.insert(i);
            break;
        }
    }
    for (const LebActor &a : room.actors) {
        if (pool.count(a.type)) continue;
        for (const auto &s : a.links.actors)
            if (s.actor < room.actors.size() && pool.count(room.actors[s.actor].type)) out.insert(s.actor);
    }
    return out;
}

// enemies.room_needed / roomy: whether a land spot has room for a mini-boss (the original room's grid and solid objects)
int room_needed(int mini) {
    if (has(SMALL_MINIBOSSES, mini)) return 25;
    for (const auto &n : NEEDS_ROOM)
        if (n.first == mini) return n.second;
    return BIG_ROOM;
}
bool roomy(const LebRoom &room, const LebActor &act, int most_blocked) {
    if (!room.has_grid) return true;  // (the Python's AttributeError: no grid, no limit)
    const LebGrid &g = room.grid;
    auto tile = [&](double x, double z) {
        return std::make_pair((long)floor((x - g.x) / g.tile_size), (long)floor((z - g.z) / g.tile_size));
    };
    const auto c = tile(act.pos[0], act.pos[2]);
    std::set<std::pair<long, long>> solid;
    for (const LebActor &a : room.actors)
        if (has(SOLID_OBJECTS, a.type)) solid.insert(tile(a.pos[0], a.pos[2]));
    int blocked = 0, seen = 0;
    for (long z = c.second - 2; z < c.second + 3; z++)
        for (long x = c.first - 2; x < c.first + 3; x++) {
            if (!(0 <= x && x < g.room_width && 0 <= z && z < g.room_height)) continue;
            const LebTile &t = g.tiles.at((size_t)(z * g.room_width + x));
            seen++;
            blocked += (t.flags[0] & 2) || (t.flags[0] & 1) || (t.flags[2] & 4) || solid.count({x, z});
        }
    return blocked * 25 <= most_blocked * seen;  // (near the screen's edge: the same share of the tiles on it)
}

// Python's == on two parameters (an int equals the same float; a string only a string)
bool same_param(const LebParam &a, const LebParam &b) {
    if (a.kind == LebParam::Str || b.kind == LebParam::Str) return a.kind == b.kind && a.s == b.s;
    const double x = a.kind == LebParam::Float ? a.f : (double)a.i, y = b.kind == LebParam::Float ? b.f : (double)b.i;
    return x == y;
}

std::string json_text(const std::string &s) {
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') o += '\\';
        o += c;
    }
    return o + "\"";
}
std::string param_json(const LebParam &p) {
    if (p.kind == LebParam::Str) return json_text(p.s);
    if (p.kind == LebParam::Float) return py_float_repr(p.f);
    return std::to_string(p.i);
}

// what Random mode re-rolls in a room (mod_generator.enemy_random's room entry)
struct Slot {
    uint64_t key;
    std::string cls;
    LebActor vanilla;  // (type, parameters, rotation y, scale, links' enemy byte as the roll found them)
    size_t name_len;
};
struct RoomRoll {
    bool edited = false;
    std::string acts;  // (the record's actor list, JSON)
    std::vector<int> restr_start;
    std::vector<Slot> slots;
};

const Kit *kit_of(uint16_t type) {
    for (const Kit &k : kits())
        if (k.type == type) return &k;
    return nullptr;
}
LebRoom &arena_of(SeedBuild &b, const Kit &kit, std::map<uint16_t, LebRoom> &arenas) {  // (the game's own room)
    auto ar = arenas.find(kit.type);
    if (ar == arenas.end()) {
        std::string bytes;
        if (!b.read_game(SeedBuild::path_of(std::string(kit.arena) + ".leb"), bytes))
            throw std::runtime_error(std::string("las: no arena room ") + kit.arena);
        ar = arenas.emplace(kit.type, LebRoom::parse(bytes)).first;
    }
    return ar->second;
}

// the actors shuffleEnemyActors leaves alone in a room (ENEMY_DATA's Excluded_Actors, kill_linked) and the
// restrictions it starts from
std::set<size_t> excluded_of(const Pools &P, const LebRoom &room, const std::string &name) {
    std::set<size_t> excluded;
    for (const auto &x : EXCLUDED_ACTORS)
        if (name == x.first)
            for (int i : x.second) excluded.insert((size_t)i);
    for (size_t i : kill_linked(room, P.total)) excluded.insert(i);
    return excluded;
}
std::vector<int> start_restr(const LebRoom &room, bool field, const std::string &name) {
    std::vector<int> restr = {-1, 0x3, 0x15, 0x16};  // (randomizeEnemies' restrictions)
    if (has(ROOMS_2D, name)) restr.insert(restr.end(), NO_2D.begin(), NO_2D.end());
    if (!field) {
        bool arena = false;
        for (const LebActor &a : room.actors) arena |= has(ARENA_TYPES, a.type);
        if (arena) restr.insert(restr.end(), ONE_PER_ROOM.begin(), ONE_PER_ROOM.end());
    }
    return restr;
}
// a visited actor's slot class: its enemy type ('air_field': an air enemy on the Field)
std::string slot_class(const LebActor &act, bool field) {
    const std::string t = actor_of(act.type).type;
    return t == "air" && field ? "air_field" : t;
}

// enemies.shuffleEnemyActors on a room (edited in place), the draws from rng; the record of it as
// tools/rando/record_las_world.py makes it
RoomRoll roll_room(SeedBuild &b, const Pools &P, LebRoom &room, const std::string &folder, const std::string &name,
                   PyRandom &rng, std::map<uint16_t, LebRoom> &arenas) {
    RoomRoll rr;
    const bool field = folder == "Field";
    const double chance = MINIBOSS_CHANCES.at((size_t)P.freq);
    const bool one_per_room = P.freq < 3;
    const std::vector<LebActor> before = room.actors;

    const std::set<size_t> excluded = excluded_of(P, room, name);
    std::vector<int> restr = start_restr(room, field, name);
    const bool random_room = P.random && P.types;
    rr.restr_start = restr;

    int stair_flag = -1;
    for (const auto &s : STAIR_GUARDS)
        if (name == s.first) stair_flag = s.second;
    bool room_has_miniboss = false;
    std::vector<std::pair<double, double>> placed;  // (this room's mini-boss spots, x z)
    bool have_grid_room = false;
    LebRoom grid_room;  // (the original room, for its grid: read once)
    std::vector<size_t> minis;  // (the record's "m")

    for (size_t e = 0; e < room.actors.size(); e++) {  // (the list grows with the kits: visited too)
        if (!P.total.count(room.actors[e].type) || excluded.count(e)) continue;
        const auto stairs = std::make_pair((uint8_t)1, (uint16_t)stair_flag);
        if (P.types && field && stair_flag >= 0 && room.actors[e].switches[0] == stairs) {
            room.actors[e].switches[0] = {4, 0};  // (STAIR_GUARDS: the stairs stay open)
            rr.edited = true;
        }
        if (P.types) {
            LebActor &act = room.actors[e];
            const std::string enemy_type = actor_of(act.type).type;
            const bool land = enemy_type == "land", air = enemy_type == "air";
            int new_enemy = -1;
            const LebActor vanilla = act;
            auto own = std::find(restr.begin(), restr.end(), (int)act.type);
            if (own != restr.end()) restr.erase(own);

            int mini = -1;  // (None)
            std::vector<int> fits;
            if (land)
                for (const auto &m : FIELD_MINIBOSSES) fits.push_back(m.first);
            else
                fits = ANY_TERRAIN_MINIBOSSES;
            if (has(SMALL_ONLY_SCREENS, name) && field)
                fits = land || (air && SMALL_ON_AIR) ? SMALL_MINIBOSSES : std::vector<int>();
            if (field && land) {
                if (!have_grid_room) {
                    std::string bytes;
                    const std::string path =
                        "region_common/level/" + name.substr(0, name.rfind('_')) + "/" + name + ".leb";
                    if (!b.read_game(path, bytes)) throw std::runtime_error("las: no game room " + path);
                    grid_room = LebRoom::parse(bytes, true), have_grid_room = true;
                }
                std::vector<int> ok;
                for (int m : fits)
                    if (room_needed(m) >= 25 || roomy(grid_room, act, room_needed(m))) ok.push_back(m);
                fits = ok;
            }
            bool near = false;
            for (const auto &p : placed) {
                volatile double dx = act.pos[0] - p.first, dz = act.pos[2] - p.second;
                volatile double dx2 = dx * dx, dz2 = dz * dz;  // (rounded apart, as the Python's ** 2)
                near |= dx2 + dz2 < MB_GAP * MB_GAP;
            }
            if (near) {  // (next to another mini-boss: only the small ones)
                std::vector<int> ok;
                for (int m : fits)
                    if (has(SMALL_MINIBOSSES, m)) ok.push_back(m);
                fits = ok;
            }
            if (P.minibosses && !fits.empty() && field && !(one_per_room && room_has_miniboss) && rng.random() < chance) {
                mini = fits[rng.choice_index(fits.size())];
                room_has_miniboss = true;
                new_enemy = mini;
            }
            while (mini < 0 && has(restr, new_enemy)) {  // (keep shuffling until it's a valid enemy)
                const std::vector<int> *pick;
                std::vector<int> waters;
                if (land) pick = &P.land;
                else if (air) pick = field ? &P.no_vire : &P.air;  // (no vires on the overworld)
                else if (enemy_type == "water") {
                    waters = P.water;
                    waters.insert(waters.end(), P.water_shallow.begin(), P.water_shallow.end());
                    pick = &waters;
                } else if (enemy_type == "water2D") pick = &P.water2d;
                else if (enemy_type == "water-shallow") pick = &P.water_shallow;
                else if (enemy_type == "tree") pick = &P.tree;
                else if (enemy_type == "hole") pick = &P.hole;
                else throw std::runtime_error("las: enemy type " + enemy_type + " has no pool");
                new_enemy = (*pick)[rng.choice_index(pick->size())];
            }
            // (one group per room)
            const std::vector<int> *group = has(FLY_BOMBS, new_enemy) ? &FLY_BOMBS
                                          : has(BLOCKING, new_enemy) ? &BLOCKING
                                          : has(ONE_PER_ROOM, new_enemy) && mini < 0 ? &ONE_PER_ROOM : nullptr;
            if (group) restr.insert(restr.end(), group->begin(), group->end());

            if (mini >= 0) {
                std::vector<EnemyValue> params;
                bool variant = false;
                for (const auto &v : MINIBOSS_VARIANTS)
                    if (v.first == mini) params = v.second[rng.choice_index(v.second.size())], variant = true;
                if (!variant)
                    for (const auto &m : FIELD_MINIBOSSES)
                        if (m.first == mini) params = m.second;
                for (size_t i = 0; i < 8; i++) act.params[i] = i < params.size() ? value_param(params[i]) : str_param("");
                act.type = (uint16_t)mini;
                act.links.e = 1;
                act.rot[1] = 0;
                act.scale[0] = act.scale[1] = act.scale[2] = 1.0;
                if (const Kit *kit = kit_of((uint16_t)mini)) add_miniboss_kit(room, e, *kit, arena_of(b, *kit, arenas), name);
                minis.push_back(e);
                placed.push_back({room.actors[e].pos[0], room.actors[e].pos[2]});  // (Cue Ball: his loop's corner)
                rr.edited = true;
                continue;  // (no slot, no size roll)
            }
            if (act.type != new_enemy) {
                act.type = (uint16_t)new_enemy;
                const EnemyActor &now = actor_of(new_enemy);
                for (size_t i = 0; i < 8; i++) {
                    if (!now.has_params || i >= now.params.size()) { act.params[i] = str_param(""); continue; }
                    const EnemyParam &p = now.params[i];
                    act.params[i] = value_param(p.pick ? p.values[rng.choice_index(p.values.size())] : p.values.at(0));
                }
                act.links.e = (uint8_t)now.enemy;
                if (act.type == 0x4A || act.type == 0x4D)  // (stretchy ghosts, color orbs: one of three colours)
                    act.type = (uint16_t)(act.type + rng.choice_index(3));
                act.rot[1] = 0;  // (facing the screen)
                act.scale[0] = act.scale[1] = act.scale[2] = 1.0;
                rr.edited = true;
            }
            if (random_room) rr.slots.push_back({act.key, slot_class(vanilla, field), vanilla, act.name.size()});
        }
        if (P.sizes) {
            const double s = rng.uniform(0.5, 1.5);
            LebActor &act = room.actors[e];
            act.scale[0] = act.scale[1] = act.scale[2] = s;
            rr.edited = true;
        }
    }
    if (!rr.slots.empty()) rr.edited = true;  // (the runtime re-rolls the seed's copy of the room)

    // the record (tools/rando/record_las_world.py)
    rr.acts = "[";
    for (size_t e = 0; e < before.size(); e++) {
        const LebActor &a = room.actors[e], &was = before[e];
        std::string rec;
        if (a.switches[0] != was.switches[0]) rec += ", \"sw\": 1";
        const bool mini = std::find(minis.begin(), minis.end(), e) != minis.end();
        bool params_same = true;
        for (size_t i = 0; i < 8; i++) params_same &= same_param(a.params[i], was.params[i]);
        double base[3] = {was.scale[0], was.scale[1], was.scale[2]};
        if (a.type != was.type || !params_same || a.links.e != was.links.e || a.rot[1] != was.rot[1] || mini) {
            rec += ", \"t\": " + std::to_string(a.type) + ", \"p\": [";
            for (size_t i = 0; i < 8; i++) rec += (i ? ", " : "") + param_json(a.params[i]);
            rec += "], \"re\": " + std::to_string(a.links.e);
            base[0] = base[1] = base[2] = 1.0;
        }
        if (mini) rec += ", \"m\": 1";
        if (a.scale[0] != base[0] || a.scale[1] != base[1] || a.scale[2] != base[2]) {
            if (a.scale[0] != a.scale[1] || a.scale[1] != a.scale[2])
                throw std::runtime_error("las: uneven enemy scale in " + name);
            rec += ", \"s\": " + py_float_repr(a.scale[0]);
        }
        if (!rec.empty())
            rr.acts += std::string(rr.acts.size() > 1 ? ", " : "") + "{\"e\": " + std::to_string(e) + rec + "}";
    }
    rr.acts += "]";
    return rr;
}

// mod_generator.enemy_random, as a later step reads it (SeedBuild::carry): per room "room <name> <restr...>", then its
// slots "slot <key> <name length> <vanilla type> <class> <vanilla e> <vanilla rotation y> <vanilla scale x y z>
// <vanilla parameters>" (tab-separated; the last three already as enemy_random.py writes them)
const char *const CARRY = "enemy_random";

std::string hex(long long v) {  // '%#x'
    char buf[24];
    snprintf(buf, sizeof buf, "%s0x%llx", v < 0 ? "-" : "", (unsigned long long)(v < 0 ? -v : v));
    return buf;
}

// enemy_random._params: 8 parameters as the table writes them; "" when one holds a string (the table's S)
std::string table_params(const LebParam *ps) {
    std::string out;
    for (size_t i = 0; i < 8; i++) {
        const LebParam &p = ps[i];
        std::string t;
        if (p.kind == LebParam::Str) {
            if (!p.s.empty()) return "";
            t = "-";
        } else if (p.kind == LebParam::Float) {
            t = "f:" + py_float_repr(p.f);
        } else {
            t = "i:" + std::to_string(p.i);
        }
        out += (i ? " " : "") + t;
    }
    return out;
}

std::string carry_room(const std::string &name, const RoomRoll &rr) {
    std::string o = "room\t" + name + "\t";
    for (size_t i = 0; i < rr.restr_start.size(); i++) o += (i ? " " : "") + std::to_string(rr.restr_start[i]);
    o += "\n";
    for (const Slot &s : rr.slots) {
        const LebActor &v = s.vanilla;
        const std::string ps = table_params(v.params);
        o += "slot\t" + std::to_string(s.key) + "\t" + std::to_string(s.name_len) + "\t" + hex(v.type) + "\t" + s.cls +
             "\t" + std::to_string(v.links.e) + "\t" + py_float_repr(v.rot[1]) + "\t" + py_float_repr(v.scale[0]) + " " +
             py_float_repr(v.scale[1]) + " " + py_float_repr(v.scale[2]) + "\t" + (ps.empty() ? "S" : ps) + "\n";
    }
    return o;
}

// apply mode, Random mode: the slots a room's roll made - every actor it visits but the mini-bosses, which the
// record marks "m" - from the room as the steps before left it, without a draw
RoomRoll room_slots(const Pools &P, const LebRoom &room, const std::string &folder, const std::string &name,
                    const std::set<size_t> &minis) {
    RoomRoll rr;
    const bool field = folder == "Field";
    const std::set<size_t> excluded = excluded_of(P, room, name);
    rr.restr_start = start_restr(room, field, name);
    for (size_t e = 0; e < room.actors.size(); e++) {
        const LebActor &act = room.actors[e];
        if (!P.total.count(act.type) || excluded.count(e) || minis.count(e)) continue;
        rr.slots.push_back({act.key, slot_class(act, field), act, act.name.size()});
    }
    return rr;
}

// randomizeEnemies' walk: every room of the included level folders in order; each room's roll (decide mode: rng,
// the rooms edited in place, not written; the record's room list as JSON) or, in apply mode (no rng), the slots the
// record's roll made. Random mode's slots in carry (CARRY)
void roll_all(SeedBuild &b, const Pools &P, PyRandom *rng, std::string &record, std::string &carry) {
    std::map<std::string, std::set<size_t>> minis;  // (apply mode: room -> the actors the roll made mini-bosses)
    if (!rng) {
        yyjson_val *rooms = b.decisions().raw("enemies"), *entry, *a;
        if (!yyjson_is_arr(rooms)) throw std::runtime_error("las: no enemies recorded (tools/rando/record_las_world.py)");
        yyjson_arr_iter it = yyjson_arr_iter_with(rooms);
        while ((entry = yyjson_arr_iter_next(&it))) {
            const char *name = yyjson_get_str(yyjson_arr_get(entry, 0));
            if (!name) throw std::runtime_error("las: bad enemies entry");
            std::set<size_t> &m = minis[name];
            yyjson_arr_iter ai = yyjson_arr_iter_with(yyjson_arr_get(entry, 1));
            while ((a = yyjson_arr_iter_next(&ai)))
                if (yyjson_obj_get(a, "m")) m.insert((size_t)yyjson_get_uint(yyjson_obj_get(a, "e")));
        }
    }
    std::map<uint16_t, LebRoom> arenas;
    size_t n = 0;
    record = "[";
    for (const std::string &folder : b.list_game("region_common/level")) {
        if (!has(INCLUDED_FOLDERS, folder)) continue;
        for (const std::string &file : b.list_game("region_common/level/" + folder)) {
            if (file.size() < 4 || file.compare(file.size() - 4, 4, ".leb")) continue;
            const std::string name = file.substr(0, file.size() - 4);
            LebRoom room = b.read_room(file);
            if (!rng) {
                auto m = minis.find(name);
                const RoomRoll rr = room_slots(P, room, folder, name, m == minis.end() ? std::set<size_t>() : m->second);
                if (!rr.slots.empty()) carry += carry_room(name, rr);
                continue;
            }
            const RoomRoll rr = roll_room(b, P, room, folder, name, *rng, arenas);
            if (rr.edited) record += (n++ ? ", " : "") + ("[" + json_text(name) + ", " + rr.acts + "]");
            if (!rr.slots.empty()) carry += carry_room(name, rr);
        }
    }
    record += "]";
}

// ======== the rooms edited from the record

// what randomizeEnemies' draws did (the decisions' "enemies", recorded or decided), done to the rooms
void apply_enemies(SeedBuild &b) {
    yyjson_val *rooms = b.decisions().raw("enemies"), *entry;
    if (!yyjson_is_arr(rooms)) throw std::runtime_error("las: no enemies recorded (tools/rando/record_las_world.py)");
    yyjson_arr_iter it = yyjson_arr_iter_with(rooms);
    std::map<uint16_t, LebRoom> arenas;
    while ((entry = yyjson_arr_iter_next(&it))) {
        const char *name = yyjson_get_str(yyjson_arr_get(entry, 0));
        if (!name) throw std::runtime_error("las: bad enemies entry");
        const std::string room = name, file = room + ".leb";
        LebRoom r = b.read_room(file);
        yyjson_val *acts = yyjson_arr_get(entry, 1), *a;
        yyjson_arr_iter ai = yyjson_arr_iter_with(acts);
        while ((a = yyjson_arr_iter_next(&ai))) {
            const size_t e = (size_t)yyjson_get_uint(yyjson_obj_get(a, "e"));
            LebActor &act = r.actors.at(e);
            if (yyjson_obj_get(a, "sw")) act.switches[0] = {4, 0};  // (a stair guard: the stairs stay open)
            if (yyjson_val *t = yyjson_obj_get(a, "t")) {
                act.type = (uint16_t)yyjson_get_uint(t);
                yyjson_val *p = yyjson_obj_get(a, "p");
                if (yyjson_arr_size(p) != 8) throw std::runtime_error("las: enemy parameters aren't 8 in " + room);
                for (size_t i = 0; i < 8; i++) act.params[i] = json_param(yyjson_arr_get(p, i));
                act.links.e = (uint8_t)yyjson_get_uint(yyjson_obj_get(a, "re"));
                act.rot[1] = 0;  // (facing the screen)
                act.scale[0] = act.scale[1] = act.scale[2] = 1.0;
            }
            if (yyjson_obj_get(a, "m")) {
                const Kit *kit = nullptr;
                for (const Kit &k : kits())
                    if (k.type == act.type) kit = &k;
                if (kit) {
                    auto ar = arenas.find(kit->type);
                    if (ar == arenas.end()) {  // (the game's own room, whatever this seed made of it)
                        std::string bytes;
                        if (!b.read_game(SeedBuild::path_of(std::string(kit->arena) + ".leb"), bytes))
                            throw std::runtime_error(std::string("las: no arena room ") + kit->arena);
                        ar = arenas.emplace(kit->type, LebRoom::parse(bytes)).first;
                    }
                    add_miniboss_kit(r, e, *kit, ar->second, room);
                }
            }
            if (yyjson_val *s = yyjson_obj_get(a, "s")) {
                LebActor &ac = r.actors.at(e);  // (the kit may have moved the list)
                ac.scale[0] = ac.scale[1] = ac.scale[2] = yyjson_get_num(s);
            }
        }
        b.write_room(file, r);
    }
}

}  // namespace

void randomize_enemies(SeedBuild &b) {
    if (!S(b, "randomize-enemies") && !S(b, "randomize-enemy-sizes")) return;
    const Pools P = pools(b);
    std::string record, carry;
    if (Decider *dc = b.decider()) {
        dc->reseed_shared();  // (random.seed(self.seed): the same enemies whatever the rest of the seed drew)
        roll_all(b, P, &dc->shared(), record, carry);
        if (record != "[]") dc->set("enemies", record);  // (the record has the field only when a room was edited)
    } else if (P.random && P.types) {  // (apply mode: Random mode's slots, from the record)
        roll_all(b, P, nullptr, record, carry);
    }
    if (P.random) b.carry()[CARRY] = carry;  // (mods.enemy_random: {} even when nothing has slots)
    if (b.decider() && record == "[]") return;
    apply_enemies(b);
}

int draw_chest_enemy(PyRandom &shared) { return CHEST_ENEMIES[shared.choice_index(CHEST_ENEMIES.size())]; }

// ======== roam.write: the Field screens' walls for roaming mini-bosses (the seed's rooms, else the game's)

void write_roam(SeedBuild &b) {
    if (!S(b, "enemy-minibosses")) return;
    std::string out;
    for (const std::string &f : b.list_game("region_common/level/Field")) {
        if (f.size() < 4 || f.compare(f.size() - 4, 4, ".leb")) continue;
        const std::string path = "region_common/level/Field/" + f;
        std::string bytes;
        if (!b.read_bytes(path, bytes)) throw std::runtime_error("roam: no file " + path);
        const LebRoom r = LebRoom::parse(bytes, true);
        if (!r.has_grid) throw std::runtime_error("roam: no grid in " + f);
        const LebGrid &g = r.grid;
        char head[128];
        snprintf(head, sizeof head, "%g\t%g\t%g\t%u\t%u\t", g.x, g.z, g.tile_size, (unsigned)g.room_width,
                 (unsigned)g.room_height);
        out += head;
        for (const LebTile &t : g.tiles) out += (t.flags[0] & 2) || (t.flags[0] & 1) || (t.flags[2] & 4) ? '#' : '.';
        out += '\n';
    }
    b.table_write("roam.tsv", out);
}

// ======== enemy_random.write: Random mode's table (after every other room edit: the actors found by their keys)

namespace {

// enemy_random._param on an ENEMY_DATA parameter ("" for a string: the Python's ValueError)
std::string data_param(const EnemyParam &p) {
    if (p.pick) {
        std::string o = "c:";
        for (size_t i = 0; i < p.values.size(); i++) {
            const EnemyValue &v = p.values[i];
            if (v.kind == EnemyValue::Str && *v.s) throw std::runtime_error("enemy random: a string in a choice");
            o += (i ? "," : "") + (v.kind == EnemyValue::Str ? std::string("-")
                                   : std::to_string(v.kind == EnemyValue::Float ? (long long)v.f : v.i));
        }
        return o;
    }
    const EnemyValue &v = p.values.at(0);
    if (v.kind == EnemyValue::Str) return *v.s ? "" : "-";
    if (v.kind == EnemyValue::Float) return "f:" + py_float_repr(v.f);
    return "i:" + std::to_string(v.i);
}

std::string ids_line(const std::vector<int> &ids) {
    std::string o;
    for (size_t i = 0; i < ids.size(); i++) o += (i ? " " : "") + hex(ids[i]);
    return o;
}

std::vector<std::string> split(const std::string &s, char c) {
    std::vector<std::string> out;
    size_t at = 0;
    for (size_t k; (k = s.find(c, at)) != std::string::npos; at = k + 1) out.push_back(s.substr(at, k - at));
    out.push_back(s.substr(at));
    return out;
}

}  // namespace

void write_enemy_random(SeedBuild &b) {
    auto carried = b.carry().find(CARRY);
    if (carried == b.carry().end()) return;  // (mods.enemy_random is None: not Random mode, or no enemy randomizer)
    const bool minibosses = S(b, "enemy-minibosses"), sizes = S(b, "randomize-enemy-sizes");
    int marker = -1;  // (settings['miniboss-marker-flag']: miniboss_guard made the marker flag)
    if (minibosses)
        for (const GStruct &v : b.read_sheet("GlobalFlags.gsheet").values)
            if (at(v, "symbol").s == "OverworldMinibossMarker") marker = at(v, "index").i;

    std::map<std::string, std::vector<int>> ids;
    std::vector<std::string> table;
    for (const EnemyActor &a : ACTORS) {
        if (minibosses || !a.minibosses_only) ids[a.type].push_back(a.id);
        std::string ps;
        for (size_t i = 0; i < 8; i++) {
            const std::string p = a.has_params && i < a.params.size() ? data_param(a.params[i]) : "-";
            if (p.empty()) throw std::runtime_error("enemy random: string parameters in " + std::string(a.name));
            ps += (i ? " " : "") + p;
        }
        table.push_back(hex(a.id) + "\t" + std::to_string(a.enemy ? 1 : 0) + "\t" + ps);
    }
    std::vector<int> no_vire, water = ids["water"];
    for (int x : ids["air"])
        if (x != 0x26) no_vire.push_back(x);
    water.insert(water.end(), ids["water-shallow"].begin(), ids["water-shallow"].end());
    const std::vector<std::pair<const char *, std::vector<int>>> picks = {
        {"land", ids["land"]}, {"air", ids["air"]}, {"air_field", no_vire}, {"water", water}, {"water2D", ids["water2D"]},
        {"water-shallow", ids["water-shallow"]}, {"tree", ids["tree"]}, {"hole", ids["hole"]}};
    std::vector<std::string> lines = {
        "# LAS Recomp: enemy randomizer, mode Random (tools/rando/enemy_random.py -> runtime/hle/enemy_random.c)"};
    for (const auto &p : picks) lines.push_back(std::string("pick\t") + p.first + "\t" + ids_line(p.second));
    lines.push_back("no2d\t" + ids_line(NO_2D));
    lines.push_back("group\tfly\t" + ids_line(FLY_BOMBS));
    lines.push_back("group\tblock\t" + ids_line(BLOCKING));
    if (minibosses) lines.push_back("group\tone\t" + ids_line(ONE_PER_ROOM));
    if (minibosses && marker >= 0) lines.push_back("mark\t" + std::to_string(marker) + "\t" + ids_line(MARK_ANYWHERE));
    for (const std::string &t : table) lines.push_back("enemy\t" + t);
    if (sizes) lines.push_back("sizes\t1");

    // the rooms' slots (CARRY), by key (a later room's actor with the same key wins, as the dict)
    struct Room { std::string name, restr; };
    std::vector<Room> rooms;
    struct Carried { size_t room; uint64_t key; size_t name_len; std::string vtype, cls, ve, vrot, vscale, params; };
    std::map<uint64_t, Carried> by_key;
    for (const std::string &line : split(carried->second, '\n')) {
        const std::vector<std::string> f = split(line, '\t');
        if (f[0] == "room") {
            std::string restr;
            for (const std::string &x : split(f.at(2), ' '))
                if (!x.empty() && atoi(x.c_str()) >= 0) restr += (restr.empty() ? "" : " ") + hex(atoi(x.c_str()));
            rooms.push_back({f.at(1), restr.empty() ? "-" : restr});
        } else if (f[0] == "slot") {
            const uint64_t key = strtoull(f.at(1).c_str(), nullptr, 10);
            by_key[key] = {rooms.size() - 1, key, (size_t)atol(f.at(2).c_str()), f.at(3), f.at(4), f.at(5), f.at(6),
                           f.at(7), f.at(8)};
        }
    }
    const std::string lvl = "region_common/level";
    for (const std::string &level : b.list_written(lvl)) {
        const std::string dir = lvl + "/" + level;
        if (b.files().count(dir)) continue;  // (a file, not a level folder)
        for (const std::string &f : b.list_written(dir)) {
            const std::string path = dir + "/" + f;
            if (f.size() < 4 || f.compare(f.size() - 4, 4, ".leb") || !b.files().count(path)) continue;
            const std::string &data = b.files().at(path);
            std::map<uint64_t, uint16_t> types;
            for (const LebActor &a : LebRoom::parse(data).actors) types[a.key] = a.type;
            std::vector<std::pair<size_t, const Carried *>> found;
            for (const auto &t : types) {
                auto s = by_key.find(t.first);
                if (s == by_key.end()) continue;
                std::string kb(8, '\0');
                for (int i = 0; i < 8; i++) kb[i] = (char)(t.first >> (8 * i));
                size_t at = data.find(kb);  // (the record: the key, then its type at +0xC)
                while (at != std::string::npos && (at + 0xE > data.size() || read_le(data, at + 0xC, 2) != t.second))
                    at = data.find(kb, at + 1);
                if (at == std::string::npos) throw std::runtime_error("enemy random: " + f + ": an actor has no record");
                found.push_back({at, &s->second});
            }
            if (found.empty()) continue;
            std::sort(found.begin(), found.end(), [](const auto &x, const auto &y) { return x.first < y.first; });
            for (const auto &x : found)
                if (x.second->room != found[0].second->room)
                    throw std::runtime_error("enemy random: " + f + ": actors from several rooms");
            lines.push_back("file\t" + path + "\t" + rooms.at(found[0].second->room).restr);
            for (const auto &x : found) {
                const Carried &s = *x.second;
                const uint64_t name_off = read_le(data, x.first + 0x8, 4);
                const long long stype = (long long)read_le(data, x.first + 0xC, 2);
                lines.push_back("slot\t" + hex((long long)x.first) + "\t" + hex(stype) + "\t" + s.vtype + "\t" + s.cls +
                                "\t" + std::to_string(name_off + s.name_len) + "\t" + s.ve + "\t" + s.vrot + "\t" +
                                s.vscale + "\t" + s.params);
            }
        }
    }
    std::string out;
    for (const std::string &l : lines) out += l + "\n";
    b.table_write("enemies_random.tsv", out);
}

}  // namespace las
}  // namespace rando
