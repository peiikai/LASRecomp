// Shared pieces of the native dungeon room shuffle (rooms_lib.h).
#include "rooms_lib.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <mutex>

namespace rando {
namespace solve {

std::string room_data_json();  // (room_data.cpp: tools/rando/export_room_data.py)

namespace rooms {

const char SIDES[4] = {'N', 'S', 'W', 'E'};
const int OPP[4] = {S, N, E, W};
const int STEP_C[4] = {0, 0, -1, 1}, STEP_R[4] = {-1, 1, 0, 0};

int side_index(char c) {
    switch (c) {
    case 'N': return N;
    case 'S': return S;
    case 'W': return W;
    default: return E;
    }
}

const std::vector<std::string> LEVEL_ORDER = {"Lv01TailCave",    "Lv02BottleGrotto", "Lv03KeyCavern",
                                              "Lv04AnglersTunnel", "Lv05CatfishsMaw", "Lv06FaceShrine",
                                              "Lv08TurtleRock",  "Lv10ClothesDungeon", "Lv07EagleTower"};
static const std::map<std::string, std::string> ENTRANCES = {
    {"Lv01TailCave", "Lv01TailCave_08D"},         {"Lv02BottleGrotto", "Lv02BottleGrotto_08C"},
    {"Lv03KeyCavern", "Lv03KeyCavern_08B"},       {"Lv04AnglersTunnel", "Lv04AnglersTunnel_08D"},
    {"Lv05CatfishsMaw", "Lv05CatfishsMaw_08H"},   {"Lv06FaceShrine", "Lv06FaceShrine_08D"},
    {"Lv08TurtleRock", "Lv08TurtleRock_08D"},     {"Lv10ClothesDungeon", "Lv10ClothesDungeon_08C"},
    {"Lv07EagleTower", "Lv07EagleTower_08B"}};

const std::string &entrance_of(const std::string &level) {
    static const std::string none;
    auto it = ENTRANCES.find(level);
    return it == ENTRANCES.end() ? none : it->second;
}

// ---- cells (rooms.py): Eagle's Tower's 10 x 10 grid and names that don't match positions
static const std::map<std::string, std::pair<int, int>> &tower_positions() {
    static std::map<std::string, std::pair<int, int>> pos;
    static std::once_flag once;
    std::call_once(once, [] {
        char b[8];
        for (int r = 1; r < 9; r++)
            for (int c = 0; c < 8; c++) {
                snprintf(b, sizeof b, "%02d%c", r, 'A' + c);
                pos[b] = {c < 4 ? c : c + 2, r <= 4 ? r : r + 2};
            }
        for (int r = 1; r < 11; r++)
            for (int c = 4; c < 6; c++) {
                snprintf(b, sizeof b, "%02d%c", r, "IJ"[c - 4]);
                pos[b] = {c, r};
            }
        for (int r = 5; r < 7; r++)
            for (int c = 0; c < 10; c++)
                if (c != 4 && c != 5) {
                    snprintf(b, sizeof b, "%02d%c", r + 4, 'A' + (c < 4 ? c : c - 2));
                    pos[b] = {c, r};
                }
    });
    return pos;
}
static const std::map<std::pair<int, int>, std::string> &tower_names() {
    static std::map<std::pair<int, int>, std::string> names;
    static std::once_flag once;
    std::call_once(once, [] {
        for (const auto &kv : tower_positions()) names[kv.second] = kv.first;  // (positions are unique)
    });
    return names;
}
static const char *TOWER = "Lv07EagleTower";

std::string level_of(const std::string &room) { return room.substr(0, room.rfind('_')); }
std::string short_cell(const std::string &room) { return room.substr(room.rfind('_') + 1); }

std::pair<int, int> cell_pos(const std::string &room) {
    std::string lv = level_of(room), rc = short_cell(room);
    if (lv == TOWER) {
        auto it = tower_positions().find(rc.substr(0, 3));
        if (it != tower_positions().end()) return it->second;
    }
    return {rc[2] - 'A', std::stoi(rc.substr(0, 2))};
}

std::string cell_name(const std::string &level, int col, int row) {
    if (level == TOWER) {
        auto it = tower_names().find({col, row});
        if (it != tower_names().end()) return level + "_" + it->second;
    }
    char b[16];
    snprintf(b, sizeof b, "%02d%c", row, (char)('A' + col));
    return level + "_" + b;
}

std::pair<int, int> grid_size(const std::string &level) { return level == TOWER ? std::make_pair(10, 10) : std::make_pair(8, 8); }

std::string neighbour(const std::string &room, int side) {
    std::string lv = level_of(room), rc = short_cell(room);
    if (lv == TOWER && !tower_positions().count(rc.substr(0, 3))) return "";
    auto p = cell_pos(room);
    int col = p.first + STEP_C[side], row = p.second + STEP_R[side];
    if (lv == TOWER && !(0 <= col && col < 10 && 1 <= row && row <= 10)) return "";
    if (!(0 <= col && col < 26 && 1 <= row && row <= 99)) return "";
    return cell_name(lv, col, row);
}

// ---- the data
static Atoms str_atoms(const Value &l) {
    Atoms a;
    for (const Value &x : l.list()) a.push_back(x.s());
    std::sort(a.begin(), a.end());
    return a;
}

static Slot slot_of(const Value &v) {
    Slot s;
    if (v.is_str()) s.w = v.s();
    else s.v = (int)v.i();
    return s;
}

const Room &Data::at(const std::string &name) const {
    const Room *r = room(name);
    if (!r) throw PyError("KeyError", py_repr(name));
    return *r;
}

static void load(Data &D) {
    std::string js = room_data_json();
    Value doc = Value::parse_json(js.data(), js.size());
    const Value &cat = doc.at("catalog");
    for (const auto &e : cat.dict()) {
        Room r;
        r.name = e.key, r.level = level_of(e.key), r.cell = short_cell(e.key);
        auto p = cell_pos(e.key);
        r.col = p.first, r.row = p.second;
        const Value &v = e.value;
        for (const Value &d : v.at("doors").list()) {
            const List &l = d.list();
            Door x;
            x.side = side_index(l[0].s()[0]), x.idx = (int)l[1].i(), x.kind = l[2].s(), x.half = (int)l[3].i();
            x.mirror = x.kind.size() > 8 && !x.kind.compare(x.kind.size() - 8, 8, "(mirror)");
            r.doors.push_back(x);
        }
        for (int s = 0; s < 4; s++) {
            std::string k(1, SIDES[s]);
            const List &ops = v.at("openings").at(k).list();
            const List &hs = v.at("heights").at(k).list();
            for (size_t i = 0; i < ops.size(); i++) {
                Open o;
                o.slot = slot_of(ops[i]);
                o.has_h = !hs[i].is_none();
                o.h = o.has_h ? hs[i].f() : 0.0;
                // has_own: a door actor of the room's own (not a mirror; blockers too) within a half tile
                if (!o.slot.wide())
                    for (const Door &d : r.doors)
                        if (!d.mirror && d.side == s && std::abs(d.half - o.slot.v) <= 1) o.own = true;
                r.op[s].push_back(o);
            }
            for (const Value &t : v.at("tiles").at(k).list()) r.tiles[s].push_back((int)t.i());
            r.any_open |= !r.op[s].empty();
        }
        for (const Value &u : v.at("unit").list()) r.unit.push_back(u.s());
        for (const Value &a : v.at("actors").list()) r.actors.push_back(a.s());
        for (const Value &t : v.at("types").list()) r.types.push_back(t.i());
        D.index[r.name] = (int)D.rooms.size();
        D.rooms.push_back(std::move(r));
    }
    D.location_rooms = doc.at("location_rooms");
    D.door_overrides = doc.at("door_overrides");
    for (const auto &e : doc.at("exit_req").dict()) D.exit_req[e.key] = str_atoms(e.value);
    for (const auto &e : D.location_rooms.dict())
        if (e.value.get("room").truthy()) D.check_rooms.insert(e.value.at("room").s());
    for (const Room &r : D.rooms)
        if (std::find(r.actors.begin(), r.actors.end(), "ObjDungeonPot") != r.actors.end()) D.check_rooms.insert(r.name);

    D.room_fixes = doc.at("room_fixes");
    for (const Value &p : doc.at("passages").list()) {
        const List &l = p.list();
        std::vector<Atoms> alts;
        for (const Value &a : l[2].list()) alts.push_back(str_atoms(a));
        D.passages[{l[0].s(), l[1].s()}] = alts;
    }
    for (const auto &e : doc.at("kills").dict()) {
        std::vector<std::string> ks;
        for (const Value &k : e.value.list()) ks.push_back(k.s());
        D.kills[std::stoi(e.key)] = ks;
    }
    for (const Value &x : doc.at("inward_reviewed").list()) {
        const List &l = x.list();
        D.inward_reviewed[{l[0].s(), side_index(l[1].s()[0]), (int)l[2].i()}] = l[3].s();
    }
    D.ports[0] = Value::new_dict(), D.ports[1] = Value::new_dict();
    D.hole_spots[0] = Value::new_dict(), D.hole_spots[1] = Value::new_dict();
}

static const Data &base_data() {
    static Data D;
    static std::once_flag once;
    std::call_once(once, [] { load(D); });
    return D;
}

static thread_local const Data *g_current = nullptr;

const Data &data() { return g_current ? *g_current : base_data(); }

GameScope::GameScope(const GameFiles &game, bool hook) : own_(base_data()), prev_(g_current) {
    static std::atomic<uint64_t> gen{0};
    own_.generation = ++gen;
    read_game_facts(own_, game, hook);
    g_current = &own_;
}

GameScope::~GameScope() { g_current = prev_; }

const Atoms *Ctx::exit_of(const std::string &room) const {
    if (!exit_req) return nullptr;
    const auto &m = data().exit_req;
    auto it = m.find(room);
    return it == m.end() || it->second.empty() ? nullptr : &it->second;
}


// Objects/setobject.c frozenset_hash: the members' hashes shuffled and xor'ed, then the size and a final mix
int64_t frozenset_hash(const Atoms &a) {
    auto shuffle = [](uint64_t h) { return ((h ^ 89869747ULL) ^ (h << 16)) * 3644798167ULL; };
    uint64_t h = 0;
    for (const std::string &s : a) h ^= shuffle((uint64_t)py_str_hash(s));
    h ^= ((uint64_t)a.size() + 1) * 1927868237ULL;
    h ^= (h >> 11) ^ (h >> 25);
    h = h * 69069U + 907133923ULL;
    if (h == (uint64_t)-1) h = 590923713ULL;
    return (int64_t)h;
}

// ---- terms
bool subset(const Atoms &a, const Atoms &b) { return std::includes(b.begin(), b.end(), a.begin(), a.end()); }

Atoms unite(const Atoms &a, const Atoms &b) {
    Atoms out;
    out.reserve(a.size() + b.size());
    std::set_union(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(out));
    return out;
}

bool term_less(const Atoms &a, const Atoms &b) {
    if (a.size() != b.size()) return a.size() < b.size();
    return a < b;
}

std::string dnf_str(const std::set<Atoms> &terms) {
    std::vector<Atoms> ts(terms.begin(), terms.end());
    std::sort(ts.begin(), ts.end(), term_less);
    std::vector<std::string> out;
    for (const Atoms &t : ts) {
        std::string s;
        for (size_t i = 0; i < t.size(); i++) {
            if (i) s += " & ";
            s += t[i].find('|') != std::string::npos ? "(" + t[i] + ")" : t[i];
        }
        if (!s.empty()) out.push_back(s);
    }
    if (out.empty()) return "";
    if (out.size() == 1) return out[0];
    std::string r;
    for (size_t i = 0; i < out.size(); i++) r += (i ? " | (" : "(") + out[i] + ")";
    return r;
}

}  // namespace rooms
}  // namespace solve
}  // namespace rando
