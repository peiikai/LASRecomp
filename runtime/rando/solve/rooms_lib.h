// Shared pieces of the native dungeon room shuffle (rooms.h): the room data, CPython sets of frozensets, logic terms.
#pragma once

#include <stdint.h>

#include <array>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "prepare.h"
#include "pyset.h"
#include "value.h"

namespace rando {
namespace solve {
namespace rooms {

// ---- sides: 'NSWE', in that order everywhere (the Python's dict / string order)
enum { N = 0, S = 1, W = 2, E = 3 };
extern const char SIDES[4];
extern const int OPP[4];
extern const int STEP_C[4], STEP_R[4];
int side_index(char c);

// a doorway centre: half tiles (int) or a wide opening 'wA-B'
struct Slot {
    int v = 0;
    std::string w;  // (non-empty: wide)
    bool wide() const { return !w.empty(); }
    bool operator==(const Slot &o) const { return w == o.w && (wide() || v == o.v); }
    bool operator!=(const Slot &o) const { return !(*this == o); }
    Value value() const { return wide() ? Value(w) : Value((long long)v); }
};

// dungeon_gen.openings: (slot, floor height or None, a door actor of the room's own stands in it)
struct Open {
    Slot slot;
    bool has_h = false;
    double h = 0;
    bool own = false;
};
using Side = std::vector<Open>;

struct Door {  // catalog 'doors': [side, tile index, kind, centre in half tiles]
    int side = 0, idx = 0, half = 0;
    std::string kind;  // ('lock', 'shutter(mirror)', 'blocked:water' ...)
    bool mirror = false;
};

using Atoms = std::vector<std::string>;  // a frozenset of atoms, sorted

struct Move {  // room_areas' moves: area a -> b, alternatives
    int a = 0, b = 0;
    std::vector<Atoms> alts;
};
struct Areas {  // room_areas: area of each tile (row * 10 + col; -1: none), the area count, the moves
    std::vector<int> tiles;
    int n = 1;
    std::vector<Move> moves;
};

struct Land {  // landing_depth
    double depth = 2.2, height = 0.0, lateral = 0.0;
};

struct Room {
    std::string name, level, cell;  // 'Lv01TailCave_05A', 'Lv01TailCave', '05A'
    int col = 0, row = 0;           // cell_pos
    std::array<Side, 4> op;         // openings(cat, room)
    std::array<std::vector<int>, 4> tiles;
    std::vector<Door> doors;
    std::vector<std::string> unit, actors;
    std::vector<int64_t> types;
    bool any_open = false;          // any(openings(cat, room).values())
    // what the game's rooms say (read_game_facts, from the player's files)
    Areas areas[2];                                  // [hookshot-anything]
    std::vector<std::pair<int, Value>> locks;        // lock_flag's (side, flag or None), in actor order
    std::vector<Value> blocks;                       // lock_blocks
    bool throw_ok = false;
    Atoms throw_req;                                 // (throw_ok: the pot door's requirement)
    std::array<Atoms, 4> shutter;
    std::vector<int> tex;                            // room_textures (indexes into the texture name table)
    std::map<std::tuple<int, int, int>, Land> land;  // (side, slot, min tile)
};

struct Data {
    std::vector<Room> rooms;                     // catalog order
    std::map<std::string, int> index;            // name -> rooms[i]
    Value location_rooms;                        // location_rooms.json
    Value door_overrides;                        // {room: [[side, slot, in, out]]}
    std::map<std::string, Atoms> exit_req;       // souls.exit_req
    std::map<std::string, std::vector<std::string>> offgrid;  // level -> offgrid_rooms
    std::map<std::string, std::vector<int>> offgrid_tex_all;  // level -> union of its offgrid rooms' textures
    std::map<std::string, std::vector<int>> base_tex;         // level -> _base_tex
    std::map<std::string, std::set<std::string>> level_cells;
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> stairs, holes;  // level -> links
    Value ports[2], hole_spots[2];               // [hook]: level -> [[a, areas, b, areas, alts]]
    std::set<std::tuple<std::string, int, int>> inward_set;
    std::map<std::tuple<std::string, int, int>, std::string> inward;  // (room, side, slot) -> 'ledge' / 'arrival'
    std::map<std::string, int64_t> map_anims;
    std::map<int64_t, std::pair<double, double>> map_shifts;
    size_t tex_names = 0;
    std::set<std::string> check_rooms;           // dungeon_gen._check_rooms
    // our own tables (room_data.cpp): room_overrides.ROOMS' tile / move fixes, PASSAGES, the shutters' kill rules,
    // dungeon_gen.INWARD_REVIEWED
    Value room_fixes;
    std::map<std::pair<std::string, std::string>, std::vector<Atoms>> passages;  // (sorted pair) -> alternatives
    std::map<int, std::vector<std::string>> kills;
    std::map<std::tuple<std::string, int, int>, std::string> inward_reviewed;
    uint64_t generation = 0;  // (which generation's game facts: the planner's caches follow it)
    const Room *room(const std::string &name) const {
        auto it = index.find(name);
        return it == index.end() ? nullptr : &rooms[(size_t)it->second];
    }
    const Room &at(const std::string &name) const;  // (KeyError)
};

// The room data the planner works with: the generation's own while a GameScope is alive on this thread (the facts
// read from the player's files), else our own tables only (no game facts: PyError if a reader's result is asked for)
const Data &data();

// What the game's rooms say, read from the player's files (rooms_game.cpp): areas (for hookshot-anything or not),
// locks, keyhole blocks, pot doors, shutters, landing spots, drop-down-only doorways, stairs, holes, textures, level
// cells and the pause-menu map. Every file read once.
void read_game_facts(Data &D, const GameFiles &game, bool hook);

// For one generation (plan_rooms): our tables plus the game facts, current on this thread until it ends
class GameScope {
public:
    GameScope(const GameFiles &game, bool hook);
    ~GameScope();
    GameScope(const GameScope &) = delete;
    GameScope &operator=(const GameScope &) = delete;

private:
    Data own_;
    const Data *prev_;
};

// what the seed's other settings make of the room logic: hookshot-anything, mini-boss souls (room_logic.EXIT_REQ)
struct Ctx {
    bool hook = false;
    bool exit_req = false;
    const Atoms *exit_of(const std::string &room) const;  // EXIT_REQ.get(room) (nullptr: none)
};

// ---- cells (rooms.py, dungeon_gen.py): Eagle's Tower's grid doesn't follow its names
std::pair<int, int> cell_pos(const std::string &room);        // 'Lv01TailCave_06C' -> (2, 6)
std::string cell_name(const std::string &level, int col, int row);
std::string level_of(const std::string &room);
std::string short_cell(const std::string &room);              // 'Lv01TailCave_06C' -> '06C'
std::string neighbour(const std::string &room, int side);     // ('' when None)
std::pair<int, int> grid_size(const std::string &level);
extern const std::vector<std::string> LEVEL_ORDER;           // dungeon_gen.ENTRANCES order
const std::string &entrance_of(const std::string &level);    // dungeon_gen.ENTRANCES

// ---- CPython sets (pyset.h) of other things: the same table, any key with a CPython hash
template <class K, class H> class PySet {
public:
    PySet() : table_(8) {}
    bool contains(const K &k) const { return find(k, H()(k)) >= 0; }
    void add(const K &k);
    size_t size() const { return used_; }
    bool empty() const { return used_ == 0; }
    std::vector<K> items() const {
        std::vector<K> out;
        for (const E &e : table_)
            if (e.state == 1) out.push_back(e.key);
        return out;
    }

private:
    struct E {
        int state = 0;
        int64_t hash = 0;
        K key{};
    };
    long find(const K &k, int64_t h) const;
    void resize(size_t minused);
    std::vector<E> table_;
    size_t fill_ = 0, used_ = 0;
};

// ---- CPython sets (pyset.cpp's table, any key)
static const size_t PYSET_PROBES = 9, PYSET_SHIFT = 5;

template <class K, class H> inline long PySet<K, H>::find(const K &k, int64_t h) const {
    size_t mask = table_.size() - 1, perturb = (size_t)h, i = (size_t)h & mask;
    for (;;) {
        size_t probes = i + PYSET_PROBES <= mask ? PYSET_PROBES : 0;
        for (size_t j = 0; j <= probes; j++) {
            const E &e = table_[i + j];
            if (e.state == 0) return -1;
            if (e.state == 1 && e.hash == h && e.key == k) return (long)(i + j);
        }
        perturb >>= PYSET_SHIFT;
        i = (i * 5 + 1 + perturb) & mask;
    }
}

template <class K, class H> inline void PySet<K, H>::add(const K &k) {
    int64_t h = H()(k);
    size_t mask = table_.size() - 1, perturb = (size_t)h, i = (size_t)h & mask;
    E *freeslot = nullptr;
    for (;;) {
        size_t probes = i + PYSET_PROBES <= mask ? PYSET_PROBES : 0;
        for (size_t j = 0; j <= probes; j++) {
            E &e = table_[i + j];
            if (e.state == 0) {
                if (freeslot) {
                    freeslot->state = 1, freeslot->hash = h, freeslot->key = k;
                    used_++;
                    return;
                }
                e.state = 1, e.hash = h, e.key = k;
                fill_++, used_++;
                if (fill_ * 5 >= mask * 3) resize(used_ > 50000 ? used_ * 2 : used_ * 4);
                return;
            }
            if (e.state == 1 && e.hash == h && e.key == k) return;
            if (e.state == 2) freeslot = &e;
        }
        perturb >>= PYSET_SHIFT;
        i = (i * 5 + 1 + perturb) & mask;
    }
}

template <class K, class H> inline void PySet<K, H>::resize(size_t minused) {
    size_t newsize = 8;
    while (newsize <= minused) newsize <<= 1;
    std::vector<E> old;
    old.swap(table_);
    table_.assign(newsize, E());
    size_t mask = newsize - 1;
    for (E &e : old) {
        if (e.state != 1) continue;
        size_t perturb = (size_t)e.hash, i = (size_t)e.hash & mask;
        for (;;) {
            size_t probes = i + PYSET_PROBES <= mask ? PYSET_PROBES : 0;
            size_t j = 0;
            for (; j <= probes && table_[i + j].state != 0; j++) {}
            if (j <= probes) {
                table_[i + j] = std::move(e);
                break;
            }
            perturb >>= PYSET_SHIFT;
            i = (i * 5 + 1 + perturb) & mask;
        }
    }
    fill_ = used_;
}

int64_t frozenset_hash(const Atoms &a);  // hash(frozenset(a)) under PYTHONHASHSEED=0
struct FrozenHash {
    int64_t operator()(const Atoms &a) const { return frozenset_hash(a); }
};
using TermSet = PySet<Atoms, FrozenHash>;  // a set of frozensets, in CPython's order

// ---- logic terms (minimal sums of products)
bool subset(const Atoms &a, const Atoms &b);   // a <= b
Atoms unite(const Atoms &a, const Atoms &b);   // a | b
bool term_less(const Atoms &a, const Atoms &b);  // the cut's order: (len(t), sorted(t))
std::string dnf_str(const std::set<Atoms> &terms);  // (no terms: "")

}  // namespace rooms
}  // namespace solve
}  // namespace rando
