// tools/rando/dungeon_gen.py's Coupled / Decoupled planning (plan_all_chaos and what it calls), ported (rooms.h).
#include <math.h>
#include <stdlib.h>

#include <algorithm>
#include <functional>
#include <map>
#include <regex>
#include <set>
#include <tuple>

#include "../pyrandom.h"
#include "rooms.h"

namespace rando {
namespace solve {
namespace rooms {

namespace {

const std::set<std::string> STAY = {"Lv02BottleGrotto_07G", "Lv07EagleTower_02F", "Lv07EagleTower_03G",
                                    "Lv07EagleTower_02G",   "Lv07EagleTower_02B", "Lv03KeyCavern_03D",
                                    "Lv08TurtleRock_03E",   "Lv05CatfishsMaw_04F", "Lv04AnglersTunnel_03G",
                                    "Lv07EagleTower_04F",   "Lv07EagleTower_02A"};
const char *const NOT_MOVABLE[] = {"Stairs",   "Downstairs", "Upstairs", "Warp",       "Hole",         "PlayerStart",
                                   "Npc",      "Shop",       "Panel",    "LevelOpen",  "FullMoon",     "Instrument",
                                   "ConchHorn", "SeaLily",   "SurfHarp", "WindMari",   "CoralTri",     "EveningCalm",
                                   "ThunderD", "HeartContainer", "Owl",  "Return",     "BossDoor"};
const char *const LINK_ACTORS[] = {"Stairs", "Downstairs", "Upstairs", "Hole", "PlayerStart"};
const char *const TOWER = "Lv07EagleTower";
const char *const DEAD[] = {"05F", "05G", "06E", "06F", "06G", "06H", "07E", "07F", "07G", "07H", "08F", "08G"};
const char *const COPY_UNFIT[] = {"Item",     "TreasureBox", "DungeonPot", "DungeonOw", "LockDoor", "LockBlock",
                                  "CrystalSw", "SwitchChe",  "stairs",     "Npc",       "HoleLowe", "BossDoor",
                                  "Boss",     "DungeonWa",   "DungeonSt",  "LevelOpen"};
const std::set<std::string> CHAOS_DOORS = {"shutter", "lock", "cracked", "potdoor"};
const int TEXTURE_BUDGET_DEFAULT = 330;
const double MAP_CELL_W = 81.5, MAP_CELL_H = 64.75, MAP_TOL = 20.0;

using Pos = std::pair<int, int>;

bool contains(const std::string &hay, const char *needle) { return hay.find(needle) != std::string::npos; }
bool starts(const std::string &s, const std::string &p) { return !s.compare(0, p.size(), p); }

int texture_budget() {
    const char *e = getenv("LARECOMP_TEXTURE_BUDGET");
    return e ? atoi(e) : TEXTURE_BUDGET_DEFAULT;
}

// TIED / PINNED: Face Shrine 07G's south side only joins its vanilla partner 08G
const std::pair<std::string, int> TIED_ROOM = {"Lv06FaceShrine_07G", S};
bool pinned(const std::string &room, int side) {
    static const std::string nb = neighbour(TIED_ROOM.first, TIED_ROOM.second);
    return (room == TIED_ROOM.first && side == TIED_ROOM.second) || (room == nb && side == OPP[TIED_ROOM.second]);
}
bool pinned_ok(const std::string &room, int s, const std::string &other) {
    if (pinned(room, s) && other != neighbour(room, s)) return false;
    return !(pinned(other, OPP[s]) && room != neighbour(other, OPP[s]));
}

bool movable(const Room &v, const std::string &room = "") {
    for (const std::string &u : v.unit)
        if (STAY.count(u)) return false;
    for (int64_t t : v.types)
        if ((t >= 0x51 && t < 0x70) || t == 0x220) return false;
    bool skip_link = !room.empty() && level_of(room) == TOWER;
    for (const std::string &a : v.actors)
        for (const char *k : NOT_MOVABLE) {
            if (skip_link && std::find_if(std::begin(LINK_ACTORS), std::end(LINK_ACTORS),
                                          [&](const char *x) { return !strcmp(x, k); }) != std::end(LINK_ACTORS))
                continue;
            if (contains(a, k)) return false;
        }
    return true;
}

bool chaos_free(const Room &room, int side, const Slot &slot) {
    if (slot.wide() || pinned(room.name, side)) return false;
    for (const Door &d : room.doors)
        if (d.side == side && std::abs(d.half - slot.v) <= 1 && !(d.mirror || CHAOS_DOORS.count(d.kind))) return false;
    return true;
}

bool heights_ok(const Side &a, const Side &b) {
    for (size_t i = 0; i < a.size() && i < b.size(); i++)
        if (a[i].has_h && b[i].has_h && !(std::fabs(a[i].h - b[i].h) < 0.3)) return false;
    return true;
}
bool same_slots(const Side &a, const Side &b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++)
        if (a[i].slot != b[i].slot) return false;
    return true;
}

bool openings_match(const Side &a, const Side &b) {
    if (a.size() == b.size()) {
        const Side *ps[2][2] = {{&a, &b}, {&b, &a}};
        for (auto &pq : ps)
            for (size_t i = 0; i < pq[0]->size(); i++) {
                const Open &x = (*pq[0])[i], &y = (*pq[1])[i];
                if (!x.has_h && !x.own && !(y.own && x.slot == y.slot)) return false;
            }
    }
    if (same_slots(a, b)) return heights_ok(a, b);
    if (a.size() != b.size()) return false;
    for (const Side *p : {&a, &b})
        for (const Open &x : *p)
            if (x.slot.wide() || x.own) return false;
    for (const Side *p : {&a, &b})
        for (const Open &x : *p)
            if (x.has_h && std::fabs(x.h) > 0.3) return false;
    return heights_ok(a, b);
}

std::set<int> tiles_of(int sl) {
    if (sl % 2 == 0) return {sl / 2 - 1, sl / 2};
    return {sl / 2};
}
// (Python floor division: slots are non-negative)

bool wall_behind(const Room &room, int side, const Room &other) {
    std::set<int> theirs;
    for (const Open &y : other.op[OPP[side]])
        if (!y.slot.wide())
            for (int t : tiles_of(y.slot.v)) theirs.insert(t);
    std::set<int> stray;
    for (int t : other.tiles[OPP[side]])
        if (!theirs.count(t)) stray.insert(t);
    for (const Open &x : room.op[side])
        if (!x.slot.wide())
            for (int t : tiles_of(x.slot.v))
                if (stray.count(t)) return false;
    return true;
}

bool walls_off(const Room &room, int side, const Room &other) {
    std::set<int> across(other.tiles[OPP[side]].begin(), other.tiles[OPP[side]].end());
    for (const Open &x : room.op[side])
        if (!x.slot.wide())
            for (int t : tiles_of(x.slot.v))
                if (across.count(t)) return false;
    return true;
}

std::set<int> exit_sides(const Room &room) {
    std::set<int> out;
    const Data &D = data();
    for (int s = 0; s < 4; s++) {
        std::string nb = neighbour(room.name, s);
        if (!room.op[s].empty() && (nb.empty() || !D.room(nb))) out.insert(s);
    }
    return out;
}

const std::set<std::string> &anchors_of(const std::string &level) {
    static std::map<std::string, std::set<std::string>> cache;
    auto it = cache.find(level);
    if (it != cache.end()) return it->second;
    std::set<std::string> out;
    for (const Room &r : data().rooms) {
        if (!starts(r.name, level + "_")) continue;
        if (!movable(r, r.name)) out.insert(r.unit.begin(), r.unit.end());
        if (r.unit.size() > 1) out.insert(r.unit.begin(), r.unit.end());
        if (!r.any_open) out.insert(r.name);
    }
    return cache[level] = out;
}

std::set<Pos> dead_shadow(const std::string &level) {
    std::set<Pos> dead, out;
    if (level != TOWER) return out;
    for (const char *c : DEAD) dead.insert(cell_pos(std::string(TOWER) + "_" + c));
    for (const Pos &p : dead)
        for (int dc = -1; dc <= 1; dc++)
            for (int dr = -1; dr <= 1; dr++) out.insert({p.first + dc, p.second + dr});
    for (const Pos &p : dead) out.erase(p);
    return out;
}

std::string floor_room(const std::string &level, const std::string &cell, const std::string &room) {
    if (!room.empty() && level == TOWER && level_of(room) == level && room != cell) return room;
    return cell;
}

std::pair<double, double> map_shift(const std::string &level, const std::string &cell) {
    const Data &D = data();
    auto it = D.map_anims.find(cell);
    int64_t anim = 0;
    if (it != D.map_anims.end()) anim = it->second;
    else {
        auto e = D.map_anims.find(entrance_of(level));
        if (e != D.map_anims.end()) anim = e->second;
    }
    auto s = D.map_shifts.find(anim);
    return s == D.map_shifts.end() ? std::make_pair(0.0, 0.0) : s->second;
}

bool on_map(const std::string &level, int c, int r, const std::string &room = "") {
    std::string name = cell_name(level, c, r);
    auto sh = map_shift(level, floor_room(level, name, room));
    int cc = name.back() - 'A', rr = std::stoi(name.substr(name.size() - 3, 2));
    if (!(0 <= cc && cc < 8 && 1 <= rr && rr <= 8)) return false;
    volatile double x0 = cc * MAP_CELL_W - 326 + sh.first;  // (no fused multiply-add: the Python's two roundings)
    volatile double y0 = 259 - rr * MAP_CELL_H + sh.second;
    return x0 >= -326 - MAP_TOL && x0 + MAP_CELL_W <= 326 + MAP_TOL && y0 >= -259 - MAP_TOL &&
           y0 + MAP_CELL_H <= 259 + MAP_TOL;
}

std::set<Pos> offgrid_taken(const std::string &level) {
    static const std::regex grid_name(".*_\\d\\d[A-Z]$");
    std::set<Pos> out;
    auto it = data().offgrid.find(level);
    if (it == data().offgrid.end()) return out;
    for (const std::string &n : it->second)
        if (std::regex_match(n, grid_name)) out.insert(cell_pos(n));
    return out;
}

// textures: a bitset over the name table
struct Tex {
    std::vector<uint64_t> w;
    Tex() : w((data().tex_names + 63) / 64 + 1, 0) {}
    void add(const std::vector<int> &ids) {
        for (int i : ids) w[(size_t)i / 64] |= 1ULL << (i % 64);
    }
    size_t count_with(const std::vector<int> &ids) const {
        size_t n = 0;
        for (uint64_t x : w) n += (size_t)__builtin_popcountll(x);
        for (int i : ids)
            if (!(w[(size_t)i / 64] >> (i % 64) & 1)) n++;  // (ids are distinct)
        return n;
    }
    std::vector<int> missing(const std::vector<int> &ids) const {
        std::vector<int> out;
        for (int i : ids)
            if (!(w[(size_t)i / 64] >> (i % 64) & 1)) out.push_back(i);
        return out;
    }
    void remove(const std::vector<int> &ids) {
        for (int i : ids) w[(size_t)i / 64] &= ~(1ULL << (i % 64));
    }
    size_t count() const {
        size_t n = 0;
        for (uint64_t x : w) n += (size_t)__builtin_popcountll(x);
        return n;
    }
};

// LINK_LEVELS (Eagle's Tower): the rooms its stairs join, each with a CPython set of partners (dungeon_gen._partners)
struct Partners {
    std::vector<std::string> order;
    std::map<std::string, PyStrSet> sets;
};
const Partners &partners_of(const std::string &level) {
    static std::map<std::string, Partners> cache;
    auto it = cache.find(level);
    if (it != cache.end()) return it->second;
    Partners P;
    auto add = [&](const std::string &a, const std::string &b) {
        if (!P.sets.count(a)) P.order.push_back(a);
        P.sets[a].add(b);
    };
    auto st = data().stairs.find(level);
    if (st != data().stairs.end())
        for (const auto &ab : st->second) add(ab.first, ab.second), add(ab.second, ab.first);
    return cache[level] = P;
}

std::set<std::string> link_targets(const std::string &level) {
    std::set<std::string> out;
    if (level != TOWER) return out;
    for (const auto &kv : partners_of(level).sets) out.insert(kv.first);
    auto h = data().holes.find(level);
    if (h != data().holes.end())
        for (const auto &ab : h->second) out.insert(ab.second);
    return out;
}

bool linked_all(const std::string &level, const Value &layout) {
    if (level != TOWER) return true;
    std::set<std::string> have;
    for (const auto &e : layout.dict()) have.insert(e.value.s());
    for (const std::string &t : link_targets(level))
        if (!have.count(t)) return false;
    return true;
}

bool has_stairs(const Room &r) {
    for (const std::string &a : r.actors)
        if (contains(a, "Downstairs") || contains(a, "Upstairs")) return true;
    return false;
}

// ---------------------------------------------------------------- plan()
struct Planner {
    std::string level, entrance, seed;
    int size = 0, tries = 0;
    std::set<std::string> fixed, exclude, must;
    std::vector<const Room *> pool;
    Tex base_tex;
    std::vector<std::string> offgrid;
    std::map<std::string, std::array<const Side *, 4>> so;  // side_open (exit sides: none)
    std::map<std::string, std::array<std::string, 4>> orig_nb;
    std::map<std::string, std::array<bool, 4>> chaos_side;
    std::map<std::string, std::set<int>> exits;
    std::map<Pos, std::string> names;
    std::set<Pos> taken, near_dead, stairs;
    const std::set<std::string> *own_cells = nullptr;
    const Partners *partners = nullptr;
    int cols = 8, rows = 8;
    int budget_left = 0;
    // one attempt's state
    std::vector<std::pair<Pos, std::string>> placed_v;  // (insertion order; erased entries removed)
    std::map<Pos, std::string> placed;
    std::set<std::string> used;
    Tex tex;
    PyRandom rng;
    const Data &D = data();
    std::map<std::tuple<int, int, std::string>, bool> inside_cache;

    const Side &side_open(const std::string &room, int s) { return *so.at(room)[s]; }
    bool can_seal(const std::string &room, int s) { return chaos_side.at(room)[s] && !exits[room].count(s); }

    bool joined(const std::string &a, const std::string &b, int s, bool home_pair) {
        const Side &x = side_open(a, s), &y = side_open(b, OPP[s]);
        return !x.empty() && !y.empty() && (home_pair || (same_slots(x, y) && openings_match(x, y)));
    }
    bool inside(int c, int r) {
        if (!(0 <= c && c < cols && 1 <= r && r <= rows)) return false;
        if (taken.count({c, r}) || near_dead.count({c, r})) return false;
        if (own_cells && !own_cells->count(names.at({c, r}))) return false;
        return on_map(level, c, r);
    }
    void place(const Pos &p, const std::string &room) { placed[p] = room, placed_v.push_back({p, room}); }
    void unplace(const Pos &p) {
        placed.erase(p);
        for (size_t i = 0; i < placed_v.size(); i++)
            if (placed_v[i].first == p) { placed_v.erase(placed_v.begin() + (long)i); break; }
    }

    // frontier(): {cell: doorways facing it}, in the order placed rooms list them
    std::vector<Pos> frontier() {
        std::vector<Pos> out;
        std::set<Pos> seen;
        for (const auto &pr : placed_v)
            for (int s = 0; s < 4; s++) {
                if (side_open(pr.second, s).empty()) continue;
                Pos n = {pr.first.first + STEP_C[s], pr.first.second + STEP_R[s]};
                if (!placed.count(n) && seen.insert(n).second) out.push_back(n);
            }
        return out;
    }
    bool any_required(const std::set<Pos> &fr) {
        for (const auto &pr : placed_v)
            for (int s = 0; s < 4; s++) {
                Pos n = {pr.first.first + STEP_C[s], pr.first.second + STEP_R[s]};
                if (fr.count(n) && !side_open(pr.second, s).empty() && !can_seal(pr.second, s)) return true;
            }
        return false;
    }

    // fits(): the new doorways it opens, or -1 when it doesn't fit
    int fits(const Room &room, int c, int r) {
        int nnew = 0;
        bool home = names.at({c, r}) == room.name;
        for (int s = 0; s < 4; s++) {
            Pos n = {c + STEP_C[s], r + STEP_R[s]};
            auto it = placed.find(n);
            if (it != placed.end()) {
                const std::string &other = it->second;
                if (!pinned_ok(room.name, s, other)) return -1;
                if (home && other == orig_nb.at(room.name)[s]) continue;
                const Side &a = room.op[s], &b = side_open(other, OPP[s]);
                bool exact = same_slots(a, b) && (a.empty() || heights_ok(a, b));
                bool m = openings_match(a, b);
                if (!a.empty() && !b.empty() && m && !doors_fit(room, s, D.at(other))) return -1;
                if (!m && !((a.empty() || can_seal(room.name, s)) && (b.empty() || can_seal(other, OPP[s])))) return -1;
                if (!exact && ((!a.empty() && !wall_behind(room, s, D.at(other))) ||
                               (!b.empty() && !wall_behind(D.at(other), OPP[s], room))))
                    return -1;
            } else if (!room.op[s].empty()) {
                if (!inside(n.first, n.second)) return -1;
                nnew++;
            }
        }
        return nnew;
    }
    static std::set<std::string> own_kinds(const Room &k, int s) {
        std::set<std::string> out;
        for (const Door &d : k.doors)
            if (d.side == s && !d.mirror) out.insert(d.kind);
        return out;
    }
    bool doors_fit(const Room &a, int s, const Room &b) {
        auto ka = own_kinds(a, s), kb = own_kinds(b, OPP[s]);
        if (ka.count("lock") || kb.count("lock")) return false;
        return !ka.count("oneway") && !kb.count("oneway");
    }

    bool chaos_ok() {
        std::map<std::string, Pos> pos_of;
        for (const auto &pr : placed_v) pos_of[pr.second] = pr.first;
        std::set<Pos> left;
        for (const auto &pr : placed_v) {
            bool any = false;
            for (int s = 0; s < 4; s++) any |= !side_open(pr.second, s).empty();
            if (any) left.insert(pr.first);
        }
        int ncomps = 0;
        std::vector<int> stubs;
        while (!left.empty()) {
            Pos start = *left.begin();
            left.erase(left.begin());
            std::vector<Pos> todo = {start};
            int nstub = 0;
            while (!todo.empty()) {
                Pos p = todo.back();
                todo.pop_back();
                const std::string &room = placed.at(p);
                std::vector<Pos> nbrs;
                if (partners) {
                    auto it = partners->sets.find(room);
                    if (it != partners->sets.end())
                        for (const std::string &q : it->second.items()) {
                            auto f = pos_of.find(q);
                            if (f != pos_of.end()) nbrs.push_back(f->second);
                        }
                } else if (stairs.count(p)) {
                    for (const Pos &st : stairs)
                        if (st != p) nbrs.push_back(st);
                }
                for (int s = 0; s < 4; s++) {
                    Pos n = {p.first + STEP_C[s], p.second + STEP_R[s]};
                    const Side &a = side_open(room, s);
                    auto it = placed.find(n);
                    bool home_pair = it != placed.end() && it->second == orig_nb.at(room)[s] && it->second == names.at(n) &&
                                     room == names.at(p);
                    if (it != placed.end() && joined(room, it->second, s, home_pair)) nbrs.push_back(n);
                    else if (!a.empty() && can_seal(room, s)) nstub++;
                }
                for (const Pos &n : nbrs)
                    if (left.erase(n)) todo.push_back(n);
            }
            ncomps++;
            stubs.push_back(nstub);
        }
        if (ncomps == 1) return true;
        int total = 0;
        for (int x : stubs) {
            if (!x) return false;
            total += x;
        }
        return total >= 2 * (ncomps - 1);
    }

    struct Cand {
        bool not_must;
        double key;
        double rnd;
        std::string room;
        bool operator<(const Cand &o) const {
            if (not_must != o.not_must) return !not_must;
            if (key != o.key) return key < o.key;
            if (rnd != o.rnd) return rnd < o.rnd;
            return room < o.room;
        }
    };

    std::vector<Cand> candidates(int c, int r) {
        std::vector<Cand> out;
        bool want_close = (int)placed.size() >= size;
        const std::string &here = names.at({c, r});
        int budget = texture_budget();
        for (const Room *room : pool) {
            if (used.count(room->name)) continue;
            int n = fits(*room, c, r);
            if (n < 0) continue;
            if (floor_room(level, here, room->name) != here && !on_map(level, c, r, room->name)) continue;
            if ((int)tex.count_with(room->tex) > budget) continue;
            double key = want_close ? (double)n : std::fabs(n - 1.5);
            out.push_back({!must.count(room->name), key, rng.random(), room->name});
        }
        return out;
    }

    bool solve() {
        std::vector<Pos> fr = frontier();
        std::set<Pos> frs(fr.begin(), fr.end());
        bool done = true;
        for (const std::string &m : must) done &= used.count(m) != 0;
        if (fr.empty() || (done && !any_required(frs) && (int)placed.size() >= size)) return done && chaos_ok();
        if (--budget_left < 0) return false;
        // fewest candidates first (and give up at once when an open cell has none)
        std::vector<std::pair<double, Pos>> keyed;
        for (const Pos &p : fr) keyed.push_back({rng.random(), p});
        std::stable_sort(keyed.begin(), keyed.end(),
                         [](const std::pair<double, Pos> &a, const std::pair<double, Pos> &b) { return a.first < b.first; });
        bool have = false;
        Pos best_cell;
        std::vector<Cand> best;
        for (const auto &kp : keyed) {
            const Pos &cell = kp.second;
            if (!inside(cell.first, cell.second)) return false;
            std::vector<Cand> cs = candidates(cell.first, cell.second);
            if (cs.empty()) return false;
            if (!have || cs.size() < best.size()) have = true, best_cell = cell, best = std::move(cs);
        }
        std::sort(best.begin(), best.end());
        for (size_t i = 0; i < best.size() && i < 6; i++) {
            const std::string &room = best[i].room;
            place(best_cell, room);
            used.insert(room);
            std::vector<int> added = tex.missing(D.at(room).tex);
            tex.add(added);
            if ((int)placed.size() <= size * 3 && solve()) return true;
            unplace(best_cell);
            used.erase(room);
            tex.remove(added);
        }
        return false;
    }

    bool run(Value &out) {
        auto g = grid_size(level);
        cols = g.first, rows = g.second;
        for (int c = -1; c <= cols; c++)
            for (int r = 0; r <= rows + 1; r++) names[{c, r}] = cell_name(level, c, r);
        for (const Room &k : D.rooms)
            if (k.unit.size() == 1 && movable(k, k.name) && !fixed.count(k.name) && !exclude.count(k.name) &&
                (!starts(k.name, TOWER) || level == TOWER))
                pool.push_back(&k);  // (catalog order is sorted: the Python sorts the names)
        std::sort(pool.begin(), pool.end(), [](const Room *a, const Room *b) { return a->name < b->name; });
        if (D.base_tex.count(level)) base_tex.add(D.base_tex.at(level));
        if (D.offgrid.count(level)) offgrid = D.offgrid.at(level);
        std::vector<std::string> fixed_sorted(fixed.begin(), fixed.end());
        std::vector<const Room *> keys = pool;
        for (const std::string &f : fixed_sorted) keys.push_back(&D.at(f));
        for (const std::string &f : fixed_sorted) exits[f] = exit_sides(D.at(f));
        for (const Room *k : keys) {
            std::array<const Side *, 4> sides;
            std::array<std::string, 4> nb;
            std::array<bool, 4> cs;
            static const Side empty;
            for (int s = 0; s < 4; s++) {
                sides[s] = exits.count(k->name) && exits[k->name].count(s) ? &empty : &k->op[s];
                nb[s] = neighbour(k->name, s);
                bool all = !k->op[s].empty();
                for (const Open &x : k->op[s]) all = all && chaos_free(*k, s, x.slot);
                cs[s] = all;
            }
            so[k->name] = sides, orig_nb[k->name] = nb, chaos_side[k->name] = cs;
        }
        taken = offgrid_taken(level);
        near_dead = dead_shadow(level);
        if (level == TOWER) own_cells = &D.level_cells.at(level);
        for (const std::string &f : fixed)
            if (has_stairs(D.at(f))) stairs.insert(cell_pos(f));
        if (level == TOWER) {
            partners = &partners_of(level);
            for (const std::string &t : link_targets(level))
                if (!fixed.count(t)) must.insert(t);
        }
        Tex fixed_tex = base_tex;
        for (const std::string &f : fixed_sorted) fixed_tex.add(D.at(f).tex);
        if (D.offgrid_tex_all.count(level)) fixed_tex.add(D.offgrid_tex_all.at(level));
        for (int attempt = 0; attempt < tries; attempt++) {
            rng = PyRandom(seed + "-" + level + "-" + std::to_string(attempt));
            placed.clear(), placed_v.clear(), used = fixed, tex = fixed_tex;
            for (const std::string &f : fixed_sorted) place(cell_pos(f), f);
            budget_left = 1500;
            if (solve()) {
                out = Value::new_dict();
                for (const auto &pr : placed_v) out.set(cell_name(level, pr.first.first, pr.first.second), pr.second);
                return true;
            }
        }
        return false;
    }
};

bool plan_chaos(const std::string &level, const std::string &seed, int size, const std::set<std::string> &exclude,
                int tries, Value &out) {
    Planner P;
    P.level = level, P.entrance = entrance_of(level), P.seed = seed, P.size = size, P.tries = tries;
    P.fixed = anchors_of(level);
    P.fixed.insert(P.entrance);
    P.exclude = exclude;
    return P.run(out);
}

// ---------------------------------------------------------------- pieces and pairs
struct Stub {
    Pos p;
    std::string cell;
    int side = 0;
    int slot = 0;
    bool operator==(const Stub &o) const { return p == o.p && cell == o.cell && side == o.side && slot == o.slot; }
    bool operator<(const Stub &o) const {  // (Python tuple order: the side by its letter)
        if (p != o.p) return p < o.p;
        if (cell != o.cell) return cell < o.cell;
        if (side != o.side) return SIDES[side] < SIDES[o.side];
        return slot < o.slot;
    }
};

std::map<Pos, std::string> positions(const Value &layout) {
    std::map<Pos, std::string> pos;
    for (const auto &e : layout.dict()) pos[cell_pos(e.key)] = e.key;
    return pos;
}

const Room &room_at(const Value &layout, const std::string &cell) { return data().at(layout.at(cell).s()); }

struct SideEntry {
    bool join;
    Pos p;
    std::string cell;
    int side;
    Pos to;                  // (join)
    std::vector<int> free;  // (stub)
};

std::vector<SideEntry> chaos_sides(const Value &layout, const std::string &entrance) {
    std::map<Pos, std::string> pos = positions(layout);
    std::set<int> ex = exit_sides(data().at(entrance));
    std::vector<SideEntry> out;
    for (const auto &pc : pos) {  // (sorted by position)
        const std::string &cell = pc.second;
        const Room &room = room_at(layout, cell);
        for (int s = 0; s < 4; s++) {
            if (cell == entrance && ex.count(s)) continue;
            Pos to = {pc.first.first + STEP_C[s], pc.first.second + STEP_R[s]};
            const Side &mine = room.op[s];
            if (mine.empty()) continue;
            auto n = pos.find(to);
            if (n != pos.end()) {
                const Room &other = room_at(layout, n->second);
                const Side &theirs = other.op[OPP[s]];
                bool home = room.name == cell && other.name == n->second && neighbour(room.name, s) == n->second;
                if (home || (!theirs.empty() && same_slots(mine, theirs) && openings_match(mine, theirs))) {
                    out.push_back({true, pc.first, cell, s, to, {}});
                    continue;
                }
            }
            std::vector<int> fr;
            for (const Open &x : mine)
                if (chaos_free(room, s, x.slot)) fr.push_back(x.slot.v);
            if (!fr.empty()) out.push_back({false, pc.first, cell, s, {}, fr});
        }
    }
    return out;
}

int chaos_stub_count(const std::string &level, const Value &layout) {
    int n = 0;
    for (const SideEntry &e : chaos_sides(layout, entrance_of(level))) n += !e.join;
    return n;
}

bool dead_ends(const std::string &level, const Value &layout) {
    std::map<Pos, std::string> pos = positions(layout);
    const std::string &ent = entrance_of(level);
    std::set<int> ex = exit_sides(data().at(ent));
    for (const auto &e : layout.dict()) {
        const std::string &cell = e.key;
        const Room &room = data().at(e.value.s());
        Pos p = cell_pos(cell);
        for (int s = 0; s < 4; s++) {
            if (cell == ent && ex.count(s)) continue;
            std::string nb_home = neighbour(room.name, s);
            if (cell == room.name && !nb_home.empty() && !data().room(nb_home)) continue;
            auto n = pos.find({p.first + STEP_C[s], p.second + STEP_R[s]});
            if (n != pos.end() && cell == room.name && layout.at(n->second).s() == n->second && n->second == nb_home) continue;
            std::vector<Slot> theirs;
            if (n != pos.end())
                for (const Open &y : room_at(layout, n->second).op[OPP[s]]) theirs.push_back(y.slot);
            for (const Open &x : room.op[s]) {
                if (std::find(theirs.begin(), theirs.end(), x.slot) != theirs.end() || chaos_free(room, s, x.slot)) continue;
                return true;
            }
        }
    }
    return false;
}

// own_room: a multi-cell room's cells are one room; a copy is the same room
std::string own_room(const Value &layout, const std::string &cell) {
    const Room &r = room_at(layout, cell);
    if (r.unit.size() > 1) {
        std::vector<std::string> u = r.unit;
        std::sort(u.begin(), u.end());
        std::string s = "(unit)";
        for (const std::string &x : u) s += "|" + x;
        return s;
    }
    return r.name;
}

Value jump_row(const std::string &level, const Value &layout, const Stub &x, const Stub &y) {
    const Room &dst = room_at(layout, y.cell);
    bool own = false;
    for (const Door &d : dst.doors)
        if (!d.mirror && d.side == y.side && std::abs(d.half - y.slot) <= 1) own = true;
    Land ld;
    auto it = dst.land.find({y.side, y.slot, own ? 1 : 0});
    if (it != dst.land.end()) ld = it->second;
    List row = {Value(level), Value(short_cell(x.cell)), Value(std::string(1, SIDES[x.side])), Value((long long)x.slot),
                Value((long long)y.slot), Value(short_cell(y.cell)), Value(std::string(1, SIDES[y.side])),
                Value(ld.depth), Value(ld.height)};
    if (ld.lateral != 0.0) row.push_back(Value(ld.lateral));
    return Value(std::move(row));
}

struct Pieces {
    std::vector<Pos> comp_keys;             // comps' roots, in insertion order
    std::map<Pos, std::vector<Pos>> comps;
    std::vector<Pos> stub_keys;             // by_comp's roots, in insertion order
    std::map<Pos, std::vector<Stub>> by_comp;
    std::map<std::pair<std::string, int>, std::vector<Stub>> follow;  // (cell, side)
};

Pieces chaos_pieces(const std::string &level, const Value &layout, const std::string &entrance, PyRandom &rng) {
    std::map<Pos, std::string> pos = positions(layout);
    std::vector<Pos> pos_order;  // (the Python's dict: layout order)
    for (const auto &e : layout.dict()) pos_order.push_back(cell_pos(e.key));
    std::map<Pos, Pos> parent;
    for (const Pos &p : pos_order) parent[p] = p;
    std::function<Pos(Pos)> find = [&](Pos p) {
        while (parent[p] != p) {
            parent[p] = parent[parent[p]];
            p = parent[p];
        }
        return p;
    };
    std::set<Pos> stairs;
    for (const auto &e : layout.dict())
        if (e.key == e.value.s() && has_stairs(data().at(e.value.s()))) stairs.insert(cell_pos(e.key));
    if (level == TOWER) {
        std::map<std::string, Pos> pos_of;
        for (const auto &e : layout.dict()) pos_of[e.value.s()] = cell_pos(e.key);
        const Partners &P = partners_of(level);
        for (const std::string &a : P.order)
            for (const std::string &b : P.sets.at(a).items())
                if (pos_of.count(a) && pos_of.count(b)) parent[find(pos_of[a])] = find(pos_of[b]);
    } else {
        std::vector<Pos> st(stairs.begin(), stairs.end());
        for (size_t i = 1; i < st.size(); i++) parent[find(st[i])] = find(st[0]);
    }
    Pieces out;
    std::vector<Stub> stubs;
    for (const SideEntry &e : chaos_sides(layout, entrance)) {
        if (e.join) {
            parent[find(e.p)] = find(e.to);
            continue;
        }
        std::vector<int> fr = e.free;
        rng.shuffle(fr);
        stubs.push_back({e.p, e.cell, e.side, fr[0]});
        std::vector<Stub> f;
        for (size_t i = 1; i < fr.size(); i++) f.push_back({e.p, e.cell, e.side, fr[i]});
        out.follow[{e.cell, e.side}] = f;
    }
    for (const Pos &p : pos_order) {
        if (!room_at(layout, pos.at(p)).any_open) continue;
        Pos r = find(p);
        if (!out.comps.count(r)) out.comp_keys.push_back(r);
        out.comps[r].push_back(p);
    }
    for (const Stub &st : stubs) {
        Pos r = find(st.p);
        if (!out.by_comp.count(r)) out.stub_keys.push_back(r);
        out.by_comp[r].push_back(st);
    }
    return out;
}

bool oneway(const Value &layout, const Stub &x) {
    return data().inward.count({layout.at(x.cell).s(), x.side, x.slot}) != 0;
}

// _side_target: where a cell side's first jump row leads (pos, cell, side, slot)
bool side_target(const std::string &level, const Value &rows, const std::string &cell, int side, Stub &out) {
    std::string sh = short_cell(cell);
    for (const Value &row : rows.list()) {
        const List &r = row.list();
        if (r[0].s() == level && r[1].s() == sh && r[2].s()[0] == SIDES[side] && r.size() > 6) {
            std::string t = level + "_" + r[5].s();
            out = {cell_pos(t), t, side_index(r[6].s()[0]), (int)r[4].i()};
            return true;
        }
    }
    return false;
}

Value with_followers(const std::string &level, const Value &layout, const Pieces &pc, const Value &rows) {
    Value out = rows.copy();
    std::vector<std::pair<std::pair<std::string, int>, std::vector<Stub>>> items(pc.follow.begin(), pc.follow.end());
    std::sort(items.begin(), items.end(), [](const auto &a, const auto &b) {  // (cell, side letter), then the lists
        if (a.first.first != b.first.first) return a.first.first < b.first.first;
        if (a.first.second != b.first.second) return SIDES[a.first.second] < SIDES[b.first.second];
        return std::lexicographical_compare(a.second.begin(), a.second.end(), b.second.begin(), b.second.end());
    });
    for (const auto &it : items) {
        if (it.second.empty()) continue;
        Stub tgt;
        if (!side_target(level, rows, it.first.first, it.first.second, tgt)) continue;
        for (const Stub &x : it.second) {
            Stub t = tgt;
            if (t.cell == it.first.first && t.side == it.first.second) t = x;  // (leading back into itself: so does it)
            out.list().push_back(jump_row(level, layout, x, t));
        }
    }
    return out;
}

bool madness_rows(const std::string &level, const Value &layout, const std::vector<Pos> &order, Pieces &pc,
                  PyRandom &rng, Value &rows) {
    std::vector<Stub> stubs;
    for (const Pos &k : order)
        if (pc.by_comp.count(k))
            for (const Stub &x : pc.by_comp[k]) stubs.push_back(x);
    rows = Value::new_list();
    if (stubs.empty()) return true;
    std::vector<Pos> ring;
    for (const Pos &k : order)
        if (pc.by_comp.count(k) && !pc.by_comp[k].empty()) ring.push_back(k);
    std::map<Pos, Stub> exits, arrivals;
    for (const Pos &k : ring) {
        std::vector<Stub> v = pc.by_comp[k];
        rng.shuffle(v);
        std::stable_sort(v.begin(), v.end(), [&](const Stub &a, const Stub &b) { return !oneway(layout, a) && oneway(layout, b); });
        exits[k] = v.front(), arrivals[k] = v.back();
    }
    std::vector<std::pair<Stub, Stub>> link;  // (dict: insertion order, keys unique)
    auto link_has = [&](const Stub &x) {
        for (const auto &l : link)
            if (l.first == x) return true;
        return false;
    };
    if (ring.size() > 1)
        for (size_t i = 0; i < ring.size(); i++) link.push_back({exits[ring[i]], arrivals[ring[(i + 1) % ring.size()]]});
    std::vector<Stub> free_out, free_in;
    for (const Stub &x : stubs)
        if (!link_has(x)) free_out.push_back(x);
    for (const Stub &x : stubs) {
        bool in_vals = false;
        for (const auto &l : link) in_vals |= l.second == x;
        if (!in_vals) free_in.push_back(x);
    }
    std::map<std::string, std::string> own_cache;
    auto own = [&](const Stub &x) { return own_room(layout, x.cell); };
    bool ok = false;
    for (int k = 0; k < 50 && !ok; k++) {
        rng.shuffle(free_in);
        ok = true;
        for (size_t i = 0; i < free_out.size() && i < free_in.size(); i++)
            if (own(free_out[i]) == own(free_in[i])) ok = false;
    }
    if (!ok)
        for (size_t i = 0; i < free_out.size(); i++)
            if (own(free_in[i]) == own(free_out[i]))
                for (size_t j = 0; j < free_in.size(); j++)
                    if (own(free_in[j]) != own(free_out[i]) && own(free_in[i]) != own(free_out[j])) {
                        std::swap(free_in[i], free_in[j]);
                        break;
                    }
    for (size_t i = 0; i < free_out.size() && i < free_in.size(); i++) {
        bool found = false;
        for (auto &l : link)
            if (l.first == free_out[i]) l.second = free_in[i], found = true;
        if (!found) link.push_back({free_out[i], free_in[i]});
    }
    for (const auto &l : link)
        if (own(l.first) == own(l.second)) return false;
    std::sort(link.begin(), link.end(), [](const auto &a, const auto &b) {
        if (!(a.first == b.first)) return a.first < b.first;
        return a.second < b.second;
    });
    for (const auto &l : link) rows.list().push_back(jump_row(level, layout, l.first, l.second));
    return true;
}

// chaos_pairs: the jump rows, or None (false)
bool chaos_pairs(const std::string &level, const Value &layout, const std::string &entrance, PyRandom &rng, bool madness,
                 Value &out) {
    Pieces pc = chaos_pieces(level, layout, entrance, rng);
    for (const Pos &k : pc.stub_keys) {
        std::vector<Stub> &v = pc.by_comp[k];
        rng.shuffle(v);
        std::stable_sort(v.begin(), v.end(), [&](const Stub &a, const Stub &b) { return oneway(layout, a) && !oneway(layout, b); });
    }
    std::vector<Pos> order = pc.comp_keys;
    std::sort(order.begin(), order.end());
    rng.shuffle(order);
    auto nstubs = [&](const Pos &k) -> long { auto it = pc.by_comp.find(k); return it == pc.by_comp.end() ? 0 : (long)it->second.size(); };
    std::stable_sort(order.begin(), order.end(), [&](const Pos &a, const Pos &b) { return -nstubs(a) < -nstubs(b); });
    if (order.size() > 1)
        for (const Pos &k : order)
            if (!pc.by_comp.count(k)) return false;
    if (order.size() > 1)
        for (const Pos &k : order) {
            bool all = true;
            for (const Stub &x : pc.by_comp[k]) all = all && oneway(layout, x);
            if (all) return false;
        }
    if (madness) {
        Value rows;
        if (!madness_rows(level, layout, order, pc, rng, rows)) return false;
        out = with_followers(level, layout, pc, rows);
        return true;
    }
    std::vector<std::pair<Stub, Stub>> pairs;
    std::vector<Pos> linked;
    if (!order.empty()) linked.push_back(order[0]);
    for (size_t i = 1; i < order.size(); i++) {
        const Pos &k = order[i];
        std::vector<Pos> cand;
        for (const Pos &q : linked)
            if (nstubs(q)) cand.push_back(q);
        if (cand.empty() || !nstubs(k)) return false;
        long most = 0;
        for (const Pos &q : cand) most = std::max(most, nstubs(q));
        std::vector<Pos> best;
        for (const Pos &q : cand)
            if (nstubs(q) == most) best.push_back(q);
        Pos q = rng.choice(best);
        Stub a = pc.by_comp[q].back();
        pc.by_comp[q].pop_back();
        Stub b = pc.by_comp[k].back();
        pc.by_comp[k].pop_back();
        pairs.push_back({a, b});
        linked.push_back(k);
    }
    std::vector<Stub> rest;
    for (const Pos &k : pc.stub_keys)
        for (const Stub &x : pc.by_comp[k]) rest.push_back(x);
    rng.shuffle(rest);
    size_t tree = pairs.size();
    while (rest.size() >= 2) {
        Stub a = rest.back();
        rest.pop_back();
        Stub b = rest.back();
        rest.pop_back();
        pairs.push_back({a, b});
    }
    auto own = [&](const Stub &x) { return own_room(layout, x.cell); };
    for (size_t i = tree; i < pairs.size(); i++) {
        Stub a = pairs[i].first, b = pairs[i].second;
        if (own(a) != own(b)) continue;
        for (size_t j = tree; j < pairs.size(); j++) {
            Stub c = pairs[j].first, d = pairs[j].second;
            if (j != i && own(c) != own(b) && own(d) != own(a)) {
                pairs[i] = {a, d}, pairs[j] = {c, b};
                break;
            }
        }
    }
    if (!rest.empty()) return false;
    for (const auto &ab : pairs)
        if (own(ab.first) == own(ab.second)) return false;
    Value rows = Value::new_list();
    for (const auto &ab : pairs) {
        rows.list().push_back(jump_row(level, layout, ab.first, ab.second));
        if (!(ab.first == ab.second)) rows.list().push_back(jump_row(level, layout, ab.second, ab.first));
    }
    out = with_followers(level, layout, pc, rows);
    return true;
}

// ---------------------------------------------------------------- leftovers
const Tex &level_tex_extra(const std::string &lv) {  // _base_tex + the offgrid rooms' textures
    static std::map<std::string, Tex> cache;
    auto it = cache.find(lv);
    if (it != cache.end()) return it->second;
    Tex t;
    if (data().base_tex.count(lv)) t.add(data().base_tex.at(lv));
    if (data().offgrid_tex_all.count(lv)) t.add(data().offgrid_tex_all.at(lv));
    return cache[lv] = t;
}

// _place_room: a group [(room, (dc, dr))] into a random empty cell nobody's doorway faces; the cells, or empty
std::vector<std::string> place_room(const std::string &lv, Value &L, const std::vector<std::pair<std::string, Pos>> &group,
                                    PyRandom &rng) {
    const Data &D = data();
    const std::string &room = group[0].first;
    Tex tex = level_tex_extra(lv);
    for (const auto &e : L.dict()) tex.add(D.at(e.value.s()).tex);
    Tex all = tex;
    for (const auto &m : group) all.add(D.at(m.first).tex);
    if ((int)all.count() > texture_budget()) return {};
    std::set<Pos> taken;
    for (const auto &e : L.dict()) taken.insert(cell_pos(e.key));
    std::set<Pos> reserved = offgrid_taken(lv), shadow = dead_shadow(lv);
    auto g = grid_size(lv);
    std::set<Pos> free_set;
    for (int c = 0; c < g.first; c++)
        for (int r = 1; r <= g.second; r++)
            if (!taken.count({c, r}) && !reserved.count({c, r}) && !shadow.count({c, r}) && on_map(lv, c, r) &&
                (lv != TOWER || D.level_cells.at(lv).count(cell_name(lv, c, r))))
                free_set.insert({c, r});
    std::vector<Pos> free_cells;
    for (int c = 0; c < g.first; c++)
        for (int r = 1; r <= g.second; r++)
            if (free_set.count({c, r}) && on_map(lv, c, r, room)) free_cells.push_back({c, r});
    rng.shuffle(free_cells);
    for (const Pos &fc : free_cells) {
        std::vector<std::pair<std::string, Pos>> at;
        for (const auto &m : group) at.push_back({m.first, {fc.first + m.second.first, fc.second + m.second.second}});
        bool ok = true;
        for (const auto &mp : at) ok = ok && free_set.count(mp.second) && on_map(lv, mp.second.first, mp.second.second, mp.first);
        std::map<Pos, std::string> mine_at;
        for (const auto &mp : at) mine_at[mp.second] = mp.first;
        if (ok)
            for (const auto &mp : at) {
                const Room &m = D.at(mp.first);
                for (int s = 0; s < 4; s++) {
                    Pos n = {mp.second.first + STEP_C[s], mp.second.second + STEP_R[s]};
                    if (mine_at.count(n)) continue;
                    if (!taken.count(n)) continue;
                    const Room &other = D.at(L.at(cell_name(lv, n.first, n.second)).s());
                    const Side &theirs = other.op[OPP[s]], &mineop = m.op[s];
                    if (!theirs.empty() && !mineop.empty() && same_slots(mineop, theirs)) ok = false;
                    if ((!mineop.empty() && !walls_off(m, s, other)) || (!theirs.empty() && !walls_off(other, OPP[s], m))) ok = false;
                    if (!theirs.empty()) {
                        bool all = true;
                        for (const Open &x : theirs) all = all && chaos_free(other, OPP[s], x.slot);
                        if (!all) ok = false;
                    }
                }
            }
        if (ok) {
            std::vector<std::string> cells;
            for (const auto &mp : at) {
                std::string c = cell_name(lv, mp.second.first, mp.second.second);
                L.set(c, mp.first);
                cells.push_back(c);
            }
            return cells;
        }
    }
    return {};
}

bool jumpable(const Room &room) {
    static const std::string tied_nb = neighbour(TIED_ROOM.first, TIED_ROOM.second);
    if (room.name == TIED_ROOM.first || room.name == tied_nb) return false;
    if (!room.any_open) return false;
    for (int s = 0; s < 4; s++) {
        if (pinned(room.name, s)) continue;
        for (const Open &x : room.op[s])
            if (!chaos_free(room, s, x.slot)) return false;
    }
    return true;
}

std::string lower(std::string s) {
    for (char &c : s) c = (char)tolower((unsigned char)c);
    return s;
}

bool copy_fit(const Room &v, const std::set<std::string> &fixed, const Ctx &ctx) {
    if (!starts(v.name, "Lv") || v.unit.size() != 1 || !movable(v, v.name) || fixed.count(v.name) ||
        data().check_rooms.count(v.name) || ctx.exit_of(v.name))
        return false;
    for (const std::string &a : v.actors)
        for (const char *b : COPY_UNFIT)
            if (contains(lower(a), lower(b).c_str())) return false;
    for (const Door &d : v.doors)
        if (d.kind == "lock" || d.kind == "bossdoor" || contains(d.kind, "oneway")) return false;
    return jumpable(v);
}

struct Fix {
    std::string kind, room, level, cell;
};

// _even_stubs: Coupled dungeons with an odd number of free doorway sides evened out; false: one stays odd
bool even_stubs(const std::vector<std::string> &levels, std::map<std::string, Value> &out, const std::set<std::string> &pool,
                std::set<std::string> &used, std::map<std::string, std::vector<std::string>> &extra, PyRandom &rng,
                const Ctx &ctx, std::vector<Fix> &fixes) {
    const Data &D = data();
    auto odd = [&](const std::string &lv) { return chaos_stub_count(lv, out[lv]) % 2 != 0; };
    auto may_go = [](const std::string &room, const std::string &lv) { return !starts(room, TOWER) || lv == TOWER; };
    auto try_place = [&](const std::string &room, const std::string &lv) -> std::string {
        std::vector<std::string> cells = place_room(lv, out[lv], {{room, {0, 0}}}, rng);
        if (!cells.empty() && odd(lv)) {
            out[lv].dict().erase(cells[0]);
            return "";
        }
        return cells.empty() ? "" : cells[0];
    };
    auto remove_first = [](std::vector<std::string> &v, const std::string &x) {
        auto it = std::find(v.begin(), v.end(), x);
        if (it != v.end()) v.erase(it);
    };
    std::vector<std::string> sorted_levels = levels;
    std::sort(sorted_levels.begin(), sorted_levels.end());
    for (const std::string &lv : sorted_levels) {
        if (!odd(lv)) continue;
        std::vector<std::string> cands;
        for (const std::string &r : pool)  // (sorted)
            if (!used.count(r) && may_go(r, lv) && jumpable(D.at(r))) cands.push_back(r);
        rng.shuffle(cands);
        std::string cell;
        for (const std::string &room : cands) {
            cell = try_place(room, lv);
            if (!cell.empty()) {
                used.insert(room);
                extra[lv].push_back(cell);
                fixes.push_back({"room", room, lv, cell});
                break;
            }
        }
        if (cell.empty())
            for (const std::string &lv2 : sorted_levels) {
                if (lv2 == lv || !odd(lv2)) continue;
                std::vector<std::string> ex = extra.count(lv2) ? extra[lv2] : std::vector<std::string>();
                for (const std::string &c2 : ex) {
                    std::string room = out[lv2].at(c2).s();
                    if (!may_go(room, lv)) continue;
                    out[lv2].dict().erase(c2);
                    if (!odd(lv2)) {
                        cell = try_place(room, lv);
                        if (!cell.empty()) {
                            remove_first(extra[lv2], c2);
                            extra[lv].push_back(cell);
                            fixes.push_back({"move", room, lv, cell});
                            break;
                        }
                    }
                    out[lv2].set(c2, room);
                }
                if (!cell.empty()) break;
            }
        if (cell.empty()) {
            std::vector<std::string> ex = extra.count(lv) ? extra[lv] : std::vector<std::string>();
            for (const std::string &c2 : ex) {
                std::string room = out[lv].at(c2).s();
                out[lv].dict().erase(c2);
                if (!odd(lv))
                    for (const std::string &lv2 : sorted_levels)
                        if (lv2 != lv && odd(lv2) && may_go(room, lv2)) {
                            std::string c3 = try_place(room, lv2);
                            if (!c3.empty()) {
                                remove_first(extra[lv], c2);
                                extra[lv2].push_back(c3);
                                fixes.push_back({"move", room, lv2, c3});
                                cell = c2;
                                break;
                            }
                        }
                if (!cell.empty()) break;
                out[lv].set(c2, room);
            }
        }
        if (cell.empty()) {
            std::vector<std::string> ex = extra.count(lv) ? extra[lv] : std::vector<std::string>();
            for (const std::string &c2 : ex) {
                std::string room = out[lv].at(c2).s();
                out[lv].dict().erase(c2);
                std::vector<std::string> others;
                for (const std::string &r : cands)
                    if (!used.count(r)) others.push_back(r);
                for (const std::string &other : others) {
                    cell = try_place(other, lv);
                    if (!cell.empty()) {
                        remove_first(extra[lv], c2);
                        extra[lv].push_back(cell);
                        used.erase(room);
                        used.insert(other);
                        fixes.push_back({"swap", other, lv, cell});
                        break;
                    }
                }
                if (!cell.empty()) break;
                out[lv].set(c2, room);
            }
        }
        if (cell.empty()) {
            std::vector<std::string> ex = extra.count(lv) ? extra[lv] : std::vector<std::string>();
            for (const std::string &c2 : ex) {
                std::string room = out[lv].at(c2).s();
                if (D.check_rooms.count(room)) continue;
                out[lv].dict().erase(c2);
                if (!odd(lv)) {
                    remove_first(extra[lv], c2);
                    used.erase(room);
                    cell = c2;
                    fixes.push_back({"drop", room, lv, c2});
                    break;
                }
                out[lv].set(c2, room);
            }
        }
        if (cell.empty()) {
            std::set<std::string> fixed;
            for (const std::string &x : levels) {
                const auto &a = anchors_of(x);
                fixed.insert(a.begin(), a.end());
                fixed.insert(entrance_of(x));
            }
            std::vector<std::string> cs;
            for (const Room &r : D.rooms) {  // (sorted: the catalog's order is the names')
                std::string head = r.name.substr(0, r.name.find('_'));
                if (std::find(levels.begin(), levels.end(), head) == levels.end()) continue;
                if (!(used.count(r.name) || !pool.count(r.name))) continue;
                if (!may_go(r.name, lv) || !copy_fit(r, fixed, ctx)) continue;
                int sides = 0;
                for (int s = 0; s < 4; s++) sides += !r.op[s].empty();
                if (sides % 2) cs.push_back(r.name);
            }
            std::sort(cs.begin(), cs.end());
            rng.shuffle(cs);
            std::stable_sort(cs.begin(), cs.end(), [&](const std::string &a, const std::string &b) {
                auto copied = [&](const std::string &r) {
                    for (const Fix &f : fixes)
                        if (f.kind == "copy" && f.room == r) return true;
                    return false;
                };
                return !copied(a) && copied(b);
            });
            for (const std::string &room : cs) {
                cell = try_place(room, lv);
                if (!cell.empty()) {
                    fixes.push_back({"copy", room, lv, cell});
                    break;
                }
            }
        }
        if (cell.empty()) return false;
    }
    return true;
}

// chaos_place: false when a dungeon can't be built or a layout has a dead end
bool chaos_place(const std::string &sub, const std::vector<std::string> &levels, PyRandom &rng,
                 const std::set<std::string> &pool, bool shuffle, bool even, const Ctx &ctx,
                 std::map<std::string, Value> &out, std::vector<std::string> &out_order, std::set<std::string> &used,
                 std::vector<Fix> &fixes) {
    std::vector<std::string> order = levels;
    std::stable_sort(order.begin(), order.end(), [](const std::string &a, const std::string &b) {
        return -(long)anchors_of(a).size() < -(long)anchors_of(b).size();
    });
    if (shuffle) rng.shuffle(order);
    bool failed = false;
    for (const std::string &lv : order) {
        std::set<std::string> fx = anchors_of(lv);
        fx.insert(entrance_of(lv));
        int n = (int)fx.size();
        Value L;
        bool got = false;
        for (int add : {6, 3, 0})
            if (!got) got = plan_chaos(lv, sub, n + add, used, 12, L);
        if (!got) {
            failed = true;
            continue;
        }
        for (const auto &e : L.dict())
            if (pool.count(e.value.s())) used.insert(e.value.s());
        out[lv] = L;
        out_order.push_back(lv);
    }
    if (failed) return false;
    const Data &D = data();
    std::vector<std::string> left;
    for (const std::string &r : pool)
        if (!used.count(r)) left.push_back(r);
    rng.shuffle(left);
    std::map<std::string, std::vector<std::string>> extra;
    std::map<std::string, long> share;
    for (const std::string &lv : levels)
        for (const std::string &k : pool) share[lv] += starts(k, lv + "_");
    static const std::string tied_nb = neighbour(TIED_ROOM.first, TIED_ROOM.second);
    for (const std::string &room : left) {
        if (room == tied_nb) continue;  // (placed with its head)
        std::vector<std::pair<std::string, Pos>> group = {{room, {0, 0}}};
        if (room == TIED_ROOM.first) group.push_back({tied_nb, {STEP_C[TIED_ROOM.second], STEP_R[TIED_ROOM.second]}});
        bool skip = false;
        for (const auto &m : group) skip |= !pool.count(m.first) || used.count(m.first);
        if (skip) continue;
        bool all_free = true;
        for (const auto &m : group) {
            const Room &r = D.at(m.first);
            for (int s = 0; s < 4; s++) {
                if (pinned(m.first, s)) continue;
                for (const Open &x : r.op[s]) all_free = all_free && chaos_free(r, s, x.slot);
            }
        }
        if (!all_free) continue;
        std::vector<std::pair<std::pair<long, double>, std::string>> keyed;
        for (const std::string &lv : levels) {
            long have = 0;
            for (const auto &e : out[lv].dict()) have += pool.count(e.value.s());
            keyed.push_back({{have - share[lv], rng.random()}, lv});
        }
        std::stable_sort(keyed.begin(), keyed.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
        for (const auto &kl : keyed) {
            const std::string &lv = kl.second;
            if (starts(room, TOWER) && lv != TOWER) continue;
            std::vector<std::string> cells = place_room(lv, out[lv], group, rng);
            if (cells.empty()) continue;
            for (const auto &m : group) used.insert(m.first);
            if (group.size() == 1) extra[lv].insert(extra[lv].end(), cells.begin(), cells.end());
            break;
        }
    }
    if (even) {
        PyRandom erng(sub + "-even");
        if (!even_stubs(levels, out, pool, used, extra, erng, ctx, fixes)) return false;
    }
    for (const std::string &lv : levels)
        if (dead_ends(lv, out[lv])) return false;
    for (const std::string &lv : levels)
        if (!linked_all(lv, out[lv])) return false;
    return true;
}

std::string py_list_str(const std::vector<Fix> &fixes) {
    std::string s;
    for (size_t i = 0; i < fixes.size(); i++) {
        const Fix &f = fixes[i];
        s += (i ? ", " : "") + f.kind + " " + f.room + " " + (f.kind != "drop" ? "->" : "from") + " " + f.level + " " + f.cell;
    }
    return s;
}

}  // namespace

}  // namespace rooms

RoomPlan plan_all_chaos(const std::string &seed, bool madness, const Value &settings, const rooms::Ctx &ctx,
                        std::vector<std::string> &said) {
    using namespace rooms;
    const Data &D = data();
    const std::vector<std::string> &levels = LEVEL_ORDER;
    int rounds = 8;
    if (const char *e = getenv("LARECOMP_GEN_ROUNDS")) rounds = atoi(e);
    std::set<std::string> fixed;
    for (const std::string &lv : levels) {
        const auto &a = anchors_of(lv);
        fixed.insert(a.begin(), a.end());
        fixed.insert(entrance_of(lv));
    }
    std::set<std::string> pool;
    for (const Room &k : D.rooms) {
        std::string head = k.name.substr(0, k.name.find('_'));
        if (k.unit.size() == 1 && movable(k, k.name) && !fixed.count(k.name) &&
            std::find(levels.begin(), levels.end(), head) != levels.end())
            pool.insert(k.name);
    }
    bool have = false;
    std::pair<long, long> best_score;
    std::map<std::string, Value> best_out;
    std::vector<std::string> best_order;
    Value best_jumps;
    std::vector<Fix> best_fixes;
    bool ks_off = settings.get("keysanity", "off") == Value("off");
    for (int rnd = 0; rnd < rounds; rnd++) {
        std::string sub = rnd == 0 ? seed : seed + "#" + std::to_string(rnd);
        PyRandom rng(sub + "-chaos");
        std::map<std::string, Value> out;
        std::vector<std::string> out_order;
        std::set<std::string> used;
        std::vector<Fix> fixes;
        if (!chaos_place(sub, levels, rng, pool, rnd > 0, !madness, ctx, out, out_order, used, fixes)) continue;
        Value jumps = Value::new_list();
        bool ok = true;
        for (const std::string &lv : levels) {
            Value j;
            bool got = false;
            int k = 0;
            for (; k < 60; k++) {
                got = chaos_pairs(lv, out[lv], entrance_of(lv), rng, madness, j);
                if (!got || (boss_door_ok(lv, out[lv], j, ctx) && (!ks_off || fill_ok(lv, out[lv], j, settings, ctx))))
                    break;
            }
            if (k == 60) got = false;  // (60 draws, none kept: None)
            if (!got) {
                ok = false;
                break;
            }
            for (const Value &row : j.list()) jumps.list().push_back(row);
        }
        if (!ok) continue;
        long copies = 0;
        for (const Fix &f : fixes) copies += f.kind == "copy";
        long unused = 0;
        for (const std::string &p : pool) unused += !used.count(p);
        std::pair<long, long> score = {copies, unused};
        if (!have || score < best_score) {
            have = true, best_score = score, best_out = out, best_order = out_order, best_jumps = jumps, best_fixes = fixes;
        }
        if (score == std::make_pair(0L, 0L)) break;
    }
    if (!have) throw PyError("RuntimeError", "no Chaos layout");
    RoomPlan p;
    p.jumps = best_jumps;
    long copies = best_score.first, unused = best_score.second;
    said.push_back(std::string("chaos: ") + (copies ? std::to_string(copies) + " room copy" : "no duplicates") + "; " +
                   std::to_string(unused) + " room(s) unused; " + std::to_string(best_jumps.len()) + " jump rows");
    if (!best_fixes.empty()) said.push_back("chaos: even doorways - " + py_list_str(best_fixes));
    p.layouts = Value::new_dict();  // (chaos_place's dict: the dungeons in the order they were planned)
    for (const std::string &lv : best_order) p.layouts.set(lv, best_out[lv]);
    return p;
}

}  // namespace solve
}  // namespace rando
