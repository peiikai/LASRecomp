// What dungeon_gen.py / room_logic.py read from the game's own rooms, read here from the player's files (rooms_lib.h:
// read_game_facts): each room's tile grid and actors (room_tiles / room_areas, lock_flag, lock_blocks, throw_req,
// shutter_req, landing_depth, inward_only), the stairs and holes of each level (stairs_links, hole_links,
// stairs_ports), the map textures a room needs (room_textures, _base_tex), the level files' camera zones
// (level_cells) and where each floor sits on the pause-menu map (map_shifts, map_anims). Ported from tools/rando
// (dungeon_gen.py, room_logic.py, rooms.py); each read once per generation, as the Python's caches do.
#include <math.h>
#include <string.h>

#include <algorithm>
#include <functional>
#include <map>
#include <regex>
#include <set>
#include <stdexcept>

#include "../fmt/bntx.h"
#include "../fmt/fixed_hash.h"
#include "../fmt/gsheet.h"
#include "../fmt/leb.h"
#include "../fmt/sarc.h"
#include "rooms_lib.h"

namespace rando {
namespace solve {
namespace rooms {

namespace {

const char *const TOWER = "Lv07EagleTower";
const char *const HOOK_TARGETS[] = {"ObjTreasureBox", "ObjSquareBlock", "ObjDungeonSt", "ObjDungeonOw", "ObjHookBridge"};
const char *const INWARD_ARRIVALS[] = {"AreaUpstairs", "AreaDownstairs", "TagPlayerStart", "ObjDungeonWarp"};
const int HOOK_REACH = 8;
const double STEP_UP = 1.0, LEDGE = 1.0;
enum Kind { FLOOR, SOLID, PIT, WATER };

bool starts(const std::string &s, const char *p) { return !s.compare(0, strlen(p), p); }
bool ends(const std::string &s, const char *e) {
    size_t n = strlen(e);
    return s.size() >= n && !s.compare(s.size() - n, n, e);
}
float f32(const std::string &b, size_t o) {
    float f;
    memcpy(&f, b.data() + o, 4);
    return f;
}
uint32_t u32(const std::string &b, size_t o) { return (uint32_t)(uint8_t)b[o] | (uint32_t)(uint8_t)b[o + 1] << 8 | (uint32_t)(uint8_t)b[o + 2] << 16 | (uint32_t)(uint8_t)b[o + 3] << 24; }
uint16_t u16(const std::string &b, size_t o) { return (uint16_t)((uint8_t)b[o] | (uint8_t)b[o + 1] << 8); }
std::string name_of(const LebActor &a) { return a.name; }  // (actor names are ASCII)
std::pair<double, double> origin(const std::string &room) {
    auto p = cell_pos(room);
    return {p.first * 15.0, (p.second - 1) * 12.0};
}

// a room file as the readers need it: the actors and the tile grid ('grid' info / data), read once
struct LebInfo {
    bool ok = false;           // (the file is there and parses)
    std::vector<LebActor> actors;
    bool grid = false;         // (rooms._grid: a 'grid' entry with 'info' and 'data')
    uint16_t h = 0, w = 0;
    double ox = 0, oz = 0;
    std::string raw;           // (grid data)
};

struct Reader {
    const GameFiles &game;
    std::map<std::string, LebInfo> lebs;              // romfs path -> room
    std::map<std::string, std::vector<std::string>> lists;
    explicit Reader(const GameFiles &g) : game(g) {}

    const std::vector<std::string> &list(const std::string &dir) {
        auto it = lists.find(dir);
        if (it != lists.end()) return it->second;
        std::vector<std::string> names;
        if (!game.list || !game.list(dir, names)) names.clear();
        std::sort(names.begin(), names.end());  // (sorted(os.listdir()))
        return lists[dir] = names;
    }
    const LebInfo &leb_at(const std::string &path) {
        auto it = lebs.find(path);
        if (it != lebs.end()) return it->second;
        LebInfo L;
        std::string data;
        if (game.read && game.read(path, data)) {
            try {
                LebRoom r = LebRoom::parse(data, false);
                L.actors = std::move(r.actors);
                L.ok = true;
                const FhEntry *g = r.fh.find("grid");
                if (g && g->sub) {
                    const FhEntry *info = nullptr, *dat = nullptr;
                    for (const FhEntry &e : g->sub->entries) {
                        if (e.name == "info") info = &e;
                        if (e.name == "data") dat = &e;
                    }
                    if (info && info->data.size() >= 16) {
                        L.grid = true;
                        L.h = u16(info->data, 0), L.w = u16(info->data, 2);
                        L.ox = f32(info->data, 8), L.oz = f32(info->data, 12);
                        if (dat) L.raw = dat->data;
                    }
                }
            } catch (const std::exception &) {
                L.ok = false;
            }
        }
        return lebs[path] = L;
    }
    const LebInfo &room(const std::string &room) {
        return leb_at("region_common/level/" + level_of(room) + "/" + room + ".leb");
    }
    bool read(const std::string &path, std::string &out) { return game.read && game.read(path, out); }
};

// ---------------------------------------------------------------- room_logic.room_tiles / room_areas
struct Tile {
    Kind kind;
    bool has_h;
    double h;
};

// room_tiles: the grid's tiles (row * 10 + col) or none; solid: every tile with collision (_hook_tiles)
bool room_tiles(Reader &R, const Data &D, const std::string &room, std::vector<Tile> &out, std::set<int> &solid) {
    const LebInfo &L = R.room(room);
    if (!L.ok || !L.grid || L.raw.size() < 0x10 * 80) return false;
    if (!(L.h == 8 && L.w == 10)) return false;
    out.assign(80, Tile{FLOOR, true, 0.0});
    bool tr = starts(room, "Lv08TurtleRock");
    for (int row = 0; row < 8; row++)
        for (int col = 0; col < 10; col++) {
            size_t o = 0x10 * (row * 10 + col);
            uint8_t b0 = (uint8_t)L.raw[o], b2 = (uint8_t)L.raw[o + 2];
            double ht = f32(L.raw, o + 12);
            Tile t{FLOOR, true, ht};
            if ((b2 & 4) && ((b0 & 1) || ht < -0.3)) t.kind = tr ? PIT : WATER;
            else if ((b0 & 2) && !(b2 & 8)) t.kind = SOLID;
            else if ((b0 & 2) && (b2 & 8)) t.kind = FLOOR, t.has_h = false;
            else if (((b0 & 1) && !(b2 & 0xC)) || (ht < -1.0 && !(b2 & 4))) t.kind = PIT;
            out[(size_t)(row * 10 + col)] = t;
            if ((b0 & 2) && !(b2 & 8)) solid.insert(row * 10 + col);
        }
    for (int i = 0; i < 80; i++) {  // (collision no higher than the floor beside it: walkable)
        if (out[(size_t)i].kind != SOLID) continue;
        int r = i / 10, c = i % 10;
        bool any = false;
        double mx = 0;
        const int dr[4] = {1, -1, 0, 0}, dc[4] = {0, 0, 1, -1};
        for (int k = 0; k < 4; k++) {
            int rr = r + dr[k], cc = c + dc[k];
            if (rr < 0 || rr > 7 || cc < 0 || cc > 9) continue;
            const Tile &u = out[(size_t)(rr * 10 + cc)];
            if (u.kind == FLOOR && u.has_h) mx = any ? std::max(mx, u.h) : u.h, any = true;
        }
        if (any && out[(size_t)i].h <= mx + 0.3) out[(size_t)i].kind = FLOOR;
    }
    if (D.room_fixes.has(room)) {
        const Value &fix = D.room_fixes.at(room);
        for (const Value &t : fix.get("solid", Value::new_list()).list())
            out[(size_t)(t.list()[0].i() * 10 + t.list()[1].i())] = Tile{SOLID, true, 3.0};
        for (const Value &t : fix.get("floor", Value::new_list()).list())
            out[(size_t)(t.list()[0].i() * 10 + t.list()[1].i())] = Tile{FLOOR, true, 0.0};
    }
    return true;
}

double height_gap(const Tile &a, const Tile &b) { return !a.has_h || !b.has_h ? 0.0 : fabs(a.h - b.h); }

std::vector<Atoms> alts_of(const Value &l) {
    std::vector<Atoms> out;
    for (const Value &a : l.list()) {
        Atoms x;
        for (const Value &s : a.list()) x.push_back(s.s());
        std::sort(x.begin(), x.end());
        out.push_back(x);
    }
    return out;
}

std::vector<std::pair<int, int>> doorway_edge(int side, int slot) {  // _doorway_tiles(side, slot)[0], numeric slots
    int n = side == N || side == S ? 10 : 8;
    std::set<int> along;
    if (slot % 2 == 0) along = {slot / 2 - 1, slot / 2};
    else along = {slot / 2};
    std::vector<std::pair<int, int>> out;
    for (int i : along) {
        if (!(0 <= i && i < n)) continue;
        if (side == N) out.push_back({0, i});
        else if (side == S) out.push_back({7, i});
        else if (side == W) out.push_back({i, 0});
        else out.push_back({i, 9});
    }
    return out;
}

int pydiv_int(double a, double b) { return (int)floor(a / b); }  // int(a // b) on floats

Areas room_areas(Reader &R, const Data &D, const std::string &room, bool hook_anything) {
    Areas A;
    std::vector<Tile> g;
    std::set<int> solid;
    if (!room_tiles(R, D, room, g, solid)) {
        A.tiles.assign(80, 0), A.n = 1;
        return A;
    }
    std::set<std::pair<int, int>> hook;  // (row, col): what the shot latches onto (may lie off the grid)
    std::set<std::pair<int, int>> solid2;
    for (int i : solid) solid2.insert({i / 10, i % 10});
    auto o = origin(room);
    const LebInfo &L = R.room(room);
    for (const LebActor &x : L.actors) {
        std::string nm = name_of(x);
        bool tgt = false;
        for (const char *h : HOOK_TARGETS) tgt = tgt || starts(nm, h);
        if (!tgt) continue;
        std::pair<int, int> t = {pydiv_int(x.pos[2] - o.second, 1.5), pydiv_int(x.pos[0] - o.first, 1.5)};
        hook.insert(t), solid2.insert(t);
    }
    if (hook_anything) hook.insert(solid2.begin(), solid2.end());
    std::vector<std::pair<int, int>> blockers;  // (side, slot)
    const Room *cr = D.room(room);
    if (cr) {
        for (const Door &d : cr->doors)
            if (starts(d.kind, "blocked:") && !d.mirror) blockers.push_back({d.side, d.half});
        auto p0 = cell_pos(room);
        for (const std::string &u : cr->unit) {
            auto pu = cell_pos(u);
            int side = -1;
            for (int s = 0; s < 4; s++)
                if (p0.first + STEP_C[s] == pu.first && p0.second + STEP_R[s] == pu.second) { side = s; break; }
            if (side < 0 || u == room) continue;
            const Room *ur = D.room(u);
            if (ur)
                for (const Door &d : ur->doors)
                    if (d.side == OPP[side] && starts(d.kind, "blocked:") && !d.mirror) blockers.push_back({side, d.half});
        }
    }
    for (const auto &b : blockers)
        for (const auto &t : doorway_edge(b.first, b.second)) {
            std::pair<int, int> x = {t.first + STEP_R[b.first], t.second + STEP_C[b.first]};
            hook.insert(x), solid2.insert(x);
        }
    auto in_g = [](int r, int c) { return r >= 0 && r < 8 && c >= 0 && c < 10; };
    std::vector<int> area(80, -1);
    int n = 0;
    for (int i = 0; i < 80; i++) {  // (sorted tiles)
        Kind k = g[(size_t)i].kind;
        if ((k != FLOOR && k != WATER) || area[(size_t)i] >= 0) continue;
        area[(size_t)i] = n;
        std::vector<int> todo = {i};
        while (!todo.empty()) {
            int t = todo.back();
            todo.pop_back();
            const int dr[4] = {0, 1, 0, -1}, dc[4] = {1, 0, -1, 0};
            for (int q = 0; q < 4; q++) {
                int r = t / 10 + dr[q], c = t % 10 + dc[q];
                if (!in_g(r, c)) continue;
                int u = r * 10 + c;
                if (area[(size_t)u] >= 0 || g[(size_t)u].kind != k) continue;
                if (k == FLOOR && height_gap(g[(size_t)u], g[(size_t)t]) > STEP_UP) continue;
                area[(size_t)u] = n;
                todo.push_back(u);
            }
        }
        n++;
    }
    std::vector<std::pair<std::pair<int, int>, std::set<Atoms>>> moves;  // (dict: insertion order)
    auto find_move = [&](int a, int b) -> std::set<Atoms> * {
        for (auto &m : moves)
            if (m.first == std::make_pair(a, b)) return &m.second;
        return nullptr;
    };
    auto add = [&](int a, int b, const std::vector<Atoms> &al) {
        if (a == b) return;
        std::set<Atoms> *s = find_move(a, b);
        if (!s) moves.push_back({{a, b}, {}}), s = &moves.back().second;
        s->insert(al.begin(), al.end());
    };
    const Atoms none;
    const int dr[4] = {0, 1, 0, -1}, dc[4] = {1, 0, -1, 0};
    for (int i = 0; i < 80; i++) {
        if (area[(size_t)i] < 0) continue;
        int r = i / 10, c = i % 10, a = area[(size_t)i];
        const Tile &me = g[(size_t)i];
        for (int q = 0; q < 4; q++) {
            int ur = r + dr[q], uc = c + dc[q];
            if (!in_g(ur, uc)) continue;
            int u = ur * 10 + uc;
            const Tile &t2 = g[(size_t)u];
            if (area[(size_t)u] >= 0 && area[(size_t)u] != a) {
                if (me.kind == WATER || t2.kind == WATER)
                    add(a, area[(size_t)u], {t2.kind == WATER ? Atoms{"flippers"} : Atoms()});
                else if (me.has_h && t2.has_h && me.h > t2.h && (r == 0 || r == 7 || c == 0 || c == 9))
                    add(a, area[(size_t)u], {Atoms()});
            }
            if (me.kind != FLOOR || !me.has_h) continue;
            int gap = 0;
            while (gap < 3 && in_g(r + dr[q] * (gap + 1), c + dc[q] * (gap + 1)) &&
                   g[(size_t)((r + dr[q] * (gap + 1)) * 10 + c + dc[q] * (gap + 1))].kind == PIT)
                gap++;
            int lr = r + dr[q] * (gap + 1), lc = c + dc[q] * (gap + 1);
            if ((gap == 1 || gap == 2) && in_g(lr, lc) && area[(size_t)(lr * 10 + lc)] >= 0 &&
                g[(size_t)(lr * 10 + lc)].kind == FLOOR && (g[(size_t)(lr * 10 + lc)].has_h ? g[(size_t)(lr * 10 + lc)].h : 0.0) <= me.h + 0.3)
                add(a, area[(size_t)(lr * 10 + lc)], {gap == 1 ? Atoms{"rs-jump"} : Atoms{"rs-long-jump"}});
            bool over = false;
            int k = 1;
            while (k <= HOOK_REACH && !solid2.count({r + dr[q] * k, c + dc[q] * k})) {
                int tr = r + dr[q] * k, tc = c + dc[q] * k;
                if (!in_g(tr, tc)) break;
                Kind tk = g[(size_t)(tr * 10 + tc)].kind;
                over = over || tk == PIT || tk == WATER;
                k++;
            }
            int land_r = r + dr[q] * (k - 1), land_c = c + dc[q] * (k - 1);
            int hit_r = r + dr[q] * k, hit_c = c + dc[q] * k;
            if (in_g(hit_r, hit_c) && g[(size_t)(hit_r * 10 + hit_c)].kind == FLOOR) land_r = hit_r, land_c = hit_c;
            if (over && k <= HOOK_REACH && hook.count({hit_r, hit_c}) && in_g(land_r, land_c) &&
                area[(size_t)(land_r * 10 + land_c)] >= 0 && g[(size_t)(land_r * 10 + land_c)].kind == FLOOR)
                add(a, area[(size_t)(land_r * 10 + land_c)], {Atoms{"hookshot"}});
            if (t2.kind == SOLID) {  // a ledge: one tile of wall, then floor at least LEDGE lower
                int l2r = r + 2 * dr[q], l2c = c + 2 * dc[q];
                if (in_g(l2r, l2c)) {
                    int l2 = l2r * 10 + l2c;
                    if (area[(size_t)l2] >= 0 && g[(size_t)l2].kind == FLOOR && g[(size_t)l2].has_h && g[(size_t)l2].h <= me.h - LEDGE)
                        add(a, area[(size_t)l2], {Atoms()});
                }
            }
        }
    }
    // stairs on a raised spot: climbable from where it drops to
    std::set<int> raised;  // (-1: None)
    for (const LebActor &x : L.actors) {
        std::string nm = name_of(x);
        if (!starts(nm, "AreaDownstairs") && !starts(nm, "AreaUpstairs")) continue;
        int rr = std::min(7, std::max(0, (int)((x.pos[2] - o.second) / 1.5)));
        int cc = std::min(9, std::max(0, (int)((x.pos[0] - o.first) / 1.5)));
        raised.insert(area[(size_t)(rr * 10 + cc)]);
    }
    std::vector<std::pair<std::pair<int, int>, std::set<Atoms>>> snap = moves;
    for (const auto &m : snap) {
        int a = m.first.first, b = m.first.second;
        if (!raised.count(a) || !m.second.count(none)) continue;
        bool other = false;
        for (const auto &mm : moves)
            if (mm.first.second == a && mm.first.first != a) other = true;
        if (!other) add(b, a, {Atoms()});
    }
    if (D.room_fixes.has(room)) {
        const Value &fix = D.room_fixes.at(room);
        auto at = [&](const Value &t) {
            int i = (int)(t.list()[0].i() * 10 + t.list()[1].i());
            if (area[(size_t)i] < 0) throw PyError("KeyError", "tile");
            return area[(size_t)i];
        };
        for (const char *key : {"cut", "cut2"}) {
            bool both = !strcmp(key, "cut2");
            for (const Value &tu : fix.get(key, Value::new_list()).list()) {
                int x = at(tu.list()[0]), y = at(tu.list()[1]);
                std::vector<std::pair<int, int>> ps = {{x, y}};
                if (both) ps.push_back({y, x});
                for (const auto &p : ps)
                    for (size_t i = 0; i < moves.size(); i++)
                        if (moves[i].first == p) { moves.erase(moves.begin() + (long)i); break; }
            }
        }
        for (const char *key : {"add", "add2"}) {
            bool both = !strcmp(key, "add2");
            for (const Value &tu : fix.get(key, Value::new_list()).list()) {
                int x = at(tu.list()[0]), y = at(tu.list()[1]);
                std::vector<Atoms> al = alts_of(tu.list()[2]);
                add(x, y, al);
                if (both) add(y, x, al);
            }
        }
        if (fix.has("check")) {
            const List &c = fix.at("check").list();
            add(at(c[0]), n, alts_of(c[1]));
            n++;
        }
    }
    A.tiles = area, A.n = n;
    std::sort(moves.begin(), moves.end(), [](const auto &x, const auto &y) { return x.first < y.first; });
    for (const auto &m : moves) {
        Move mv;
        mv.a = m.first.first, mv.b = m.first.second;
        mv.alts.assign(m.second.begin(), m.second.end());  // (sorted(alts, key=sorted): Atoms are sorted lists)
        A.moves.push_back(mv);
    }
    return A;
}

std::set<int> spot_areas(const Areas &A, const std::string &room, double x, double z) {
    if (A.n == 1) return {0};
    auto o = origin(room);
    int r = std::min(7, std::max(0, (int)((z - o.second) / 1.5))), c = std::min(9, std::max(0, (int)((x - o.first) / 1.5)));
    if (A.tiles[(size_t)(r * 10 + c)] >= 0) return {A.tiles[(size_t)(r * 10 + c)]};
    std::set<int> near;
    const int dr[4] = {1, -1, 0, 0}, dc[4] = {0, 0, 1, -1};
    for (int k = 0; k < 4; k++) {
        int rr = r + dr[k], cc = c + dc[k];
        if (rr >= 0 && rr < 8 && cc >= 0 && cc < 10 && A.tiles[(size_t)(rr * 10 + cc)] >= 0) near.insert(A.tiles[(size_t)(rr * 10 + cc)]);
    }
    if (near.empty())
        for (int i = 0; i < A.n; i++) near.insert(i);
    return near;
}

// ---------------------------------------------------------------- doors: locks, keyhole blocks, pot doors, shutters
int nearest_side(const LebActor &a, const std::string &room) {  // min(d, key=d.get) over W, E, N, S
    auto o = origin(room);
    double lx = a.pos[0] - o.first, lz = a.pos[2] - o.second;
    double d[4] = {lx, 15.0 - lx, lz, 12.0 - lz};
    const int sides[4] = {W, E, N, S};
    int best = 0;
    for (int i = 1; i < 4; i++)
        if (d[i] < d[best]) best = i;
    return sides[best];
}
Value first_flag(const LebActor &a) {  // next((i for t, i in a.switches if t == 1), None)
    for (const auto &s : a.switches)
        if (s.first == 1) return Value((long long)s.second);
    return Value();
}

bool has_pits(const LebInfo &L) {
    const std::string &raw = L.grid ? L.raw : std::string();
    for (size_t o = 0; o + 15 < raw.size(); o += 16)
        if ((((uint8_t)raw[o] & 1) || f32(raw, o + 12) < -0.3) && !((uint8_t)raw[o + 2] & 4)) return true;
    return false;
}

Atoms shutter_req(Reader &R, const Data &D, const std::string &room, int side) {
    std::string level = level_of(room);
    std::string dir = "region_common/level/" + level;
    const LebInfo &L = R.room(room);
    if (!L.ok) return {"rs-shutter"};
    bool pits = has_pits(L);
    const std::vector<LebActor> &A = L.actors;
    static const std::set<int> NOT_COUNTED = {0x3d, 0xb7};
    static const std::set<std::string> PIT_KILLS = {"kill-hardhat-beetle", "kill-spiny", "kill-stalfos", "kill-wizzrobe",
                                                    "kill-zol-red"};
    static const std::set<int> KILL_TRIGGERS = {0xF0, 0x1B9, 0x247};
    auto atoms = [&](const std::vector<const LebActor *> &acts) {
        std::set<std::string> out;
        for (const LebActor *a : acts) {
            if (NOT_COUNTED.count(a->type)) continue;
            auto k = D.kills.find(a->type);
            std::vector<std::string> ks = k == D.kills.end() ? std::vector<std::string>{"rs-shutter"} : k->second;
            for (const std::string &x : ks) out.insert(pits && PIT_KILLS.count(x) ? x + "[pit]" : x);
        }
        return out;
    };
    auto watched = [](const LebActor &t, const std::vector<LebActor> &acts) {
        std::vector<const LebActor *> out;
        for (const auto &l : t.links.actors)
            if (l.actor < acts.size()) out.push_back(&acts[l.actor]);
        return out;
    };
    auto sets = [](const LebActor &b, int flag) {
        for (const auto &s : b.switches)
            if (s.first == 1 && s.second == flag) return true;
        return false;
    };
    std::set<std::string> req;
    for (size_t i = 0; i < A.size(); i++) {
        const LebActor &a = A[i];
        if (a.type != 0xEF || nearest_side(a, room) != side) continue;
        Value flag = first_flag(a);
        std::vector<const LebActor *> drivers;
        for (size_t j = 0; j < A.size(); j++) {
            if (j == i || A[j].type == 0xEF) continue;
            for (const auto &l : A[j].links.actors)
                if (l.actor == i) { drivers.push_back(&A[j]); break; }
        }
        bool have = false;
        std::set<std::string> got;
        if (!drivers.empty()) {
            got = atoms(drivers), have = true;
        } else if (flag.is_none()) {
            std::vector<const LebActor *> enemies;
            for (const LebActor &b : A)
                if (starts(name_of(b), "Enemy")) enemies.push_back(&b);
            if (!enemies.empty()) got = atoms(enemies), have = true;
        } else {
            int f = (int)flag.i();
            std::vector<const LebActor *> setters, trig;
            for (const LebActor &b : A)
                if (b.type != 0xEF && sets(b, f)) setters.push_back(&b);
            for (const LebActor *b : setters)
                if (KILL_TRIGGERS.count(b->type)) trig.push_back(b);
            const std::vector<LebActor> *A2 = &A;
            if (trig.empty() && setters.empty()) {
                for (const std::string &fn : R.list(dir)) {
                    if (!ends(fn, ".leb")) continue;
                    std::string other = fn.substr(0, fn.size() - 4);
                    if (other == room) continue;
                    const LebInfo &O = R.leb_at(dir + "/" + fn);
                    for (const LebActor &b : O.actors)
                        if (KILL_TRIGGERS.count(b.type) && sets(b, f)) trig.push_back(&b);
                    if (!trig.empty()) {
                        A2 = &O.actors;
                        break;
                    }
                }
            }
            if (!trig.empty()) {
                for (const LebActor *t : trig)
                    for (const std::string &x : atoms(watched(*t, *A2))) got.insert(x);
                have = true;
            } else if (!setters.empty()) {
                got = {"rs-switch-shutter"}, have = true;
            }
        }
        if (have && !got.empty()) req.insert(got.begin(), got.end());
        else req.insert("rs-shutter");
    }
    if (req.empty()) req.insert("rs-shutter");
    return Atoms(req.begin(), req.end());
}

// ---------------------------------------------------------------- stairs and holes (room_logic.stairs_links, ...)
struct Rect {
    bool ok = false;
    double x0, z0, x1, z1;
};
bool touch(const Rect &a, const Rect &b, double eps = 0.01) {
    bool side_by_side = (fabs(a.x1 - b.x0) < eps || fabs(b.x1 - a.x0) < eps) && std::min(a.z1, b.z1) - std::max(a.z0, b.z0) > eps;
    bool stacked = (fabs(a.z1 - b.z0) < eps || fabs(b.z1 - a.z0) < eps) && std::min(a.x1, b.x1) - std::max(a.x0, b.x0) > eps;
    return side_by_side || stacked;
}

std::vector<std::pair<std::string, std::string>> stairs_links(Reader &R, const Data &D, const std::string &level) {
    std::string dir = "region_common/level/" + level;
    std::map<std::string, std::set<std::string>> down, up;
    for (const std::string &f : R.list(dir)) {
        if (!ends(f, ".leb")) continue;
        std::string room = f.substr(0, f.size() - 4);
        const LebInfo &L = R.leb_at(dir + "/" + f);
        for (const LebActor &a : L.actors) {
            if (a.params[0].kind != LebParam::Str) continue;
            std::string n = name_of(a), target = a.params[0].s;
            if (starts(n, "AreaDownstairs")) down[room].insert(target);
            else if (starts(n, "AreaUpstairs")) up[room].insert(target);
        }
    }
    std::set<std::string> side;
    auto consider = [&](const std::string &r) { if (!D.room(r)) side.insert(r); };
    for (auto *m : {&down, &up})
        for (const auto &kv : *m) {
            consider(kv.first);
            for (const std::string &t : kv.second) consider(t);
        }
    std::map<std::string, std::string> parent;
    for (const std::string &s : side) parent[s] = s;
    std::function<std::string(std::string)> find = [&](std::string x) {
        while (parent[x] != x) parent[x] = parent[parent[x]], x = parent[x];
        return x;
    };
    std::map<std::string, Rect> rect;
    for (const std::string &s : side) {
        const LebInfo &L = R.leb_at(dir + "/" + s + ".leb");
        Rect r;
        if (L.ok && L.grid) r = {true, L.ox, L.oz, L.ox + 1.5 * L.w, L.oz + 1.5 * L.h};
        rect[s] = r;
    }
    for (const std::string &s : side)
        for (const std::string &t : side)
            if (s < t && rect[s].ok && rect[t].ok && touch(rect[s], rect[t])) parent[find(s)] = find(t);
    std::map<std::string, std::set<std::string>> comp;
    std::set<std::pair<std::string, std::string>> links;
    for (auto *m : {&down, &up})
        for (const auto &kv : *m)
            for (const std::string &t : kv.second) {
                const std::string &a = kv.first;
                if (parent.count(a) && !parent.count(t)) comp[find(a)].insert(t);
                else if (parent.count(t) && !parent.count(a)) comp[find(t)].insert(a);
                if (D.room(a) && D.room(t) && a != t) links.insert(a < t ? std::make_pair(a, t) : std::make_pair(t, a));
            }
    for (const auto &kv : comp) {
        std::vector<std::string> gs;
        for (const std::string &g : kv.second)
            if (!side.count(g)) gs.push_back(g);
        std::sort(gs.begin(), gs.end());
        for (size_t i = 0; i < gs.size(); i++)
            for (size_t j = i + 1; j < gs.size(); j++) links.insert({gs[i], gs[j]});
    }
    return std::vector<std::pair<std::string, std::string>>(links.begin(), links.end());
}

struct Hole {
    std::string room, target;
    double x, z;
};
std::vector<Hole> hole_links(Reader &R, const Data &D, const std::string &level) {
    std::string dir = "region_common/level/" + level;
    std::vector<Hole> out;
    for (const std::string &f : R.list(dir)) {
        std::string room = f.substr(0, f.size() >= 4 ? f.size() - 4 : 0);
        if (!ends(f, ".leb") || !D.room(room)) continue;
        for (const LebActor &a : R.leb_at(dir + "/" + f).actors) {
            std::vector<std::string> ps;
            for (const LebParam &p : a.params)
                if (p.kind == LebParam::Str && !p.s.empty()) ps.push_back(p.s);
            if (!starts(name_of(a), "AreaHoleLow") || ps.empty()) continue;
            std::string t = ps[0];
            if (!D.room(t)) t = D.room(level_of(t)) ? level_of(t) : "";
            if (!t.empty() && t != room) out.push_back({room, t, a.pos[0], a.pos[2]});
        }
    }
    return out;
}

// ---------------------------------------------------------------- dungeon_gen.landing_depth / inward_only
struct Grid {  // (row, col) -> (solid, hole, height)
    bool ok = false;
    std::vector<std::tuple<bool, bool, double>> t;
};
Grid grid_of(Reader &R, const std::string &room) {
    Grid G;
    const LebInfo &L = R.room(room);
    if (!L.ok || !L.grid || L.raw.size() < 0x10 * 80) return G;
    G.ok = true;
    for (int i = 0; i < 80; i++) {
        size_t o = 0x10 * (size_t)i;
        uint8_t b0 = (uint8_t)L.raw[o], b2 = (uint8_t)L.raw[o + 2];
        double h = f32(L.raw, o + 12);
        bool hole = ((b2 & 4) && h < -0.3) || h < -1.0 || (b0 & 1);
        G.t.push_back({(b0 & 2) && !(b2 & 8), hole, h});
    }
    return G;
}

double round2(double x) {  // round(x, 2): Python rounds the decimal value correctly
    char b[64];
    snprintf(b, sizeof b, "%.2f", x);  // (correctly rounded, halves to even on the exact binary value)
    return atof(b);
}

Land landing_depth(Reader &R, const std::string &room, int side, int slot, int min_tile) {
    const LebInfo &L = R.room(room);
    const std::string &raw = L.raw;
    int along = slot / 2;
    auto free = [&](int i, int a, bool pit_only, double &h) {
        int row, col;
        if (side == N) row = i, col = a;
        else if (side == S) row = 7 - i, col = a;
        else if (side == W) row = std::min(a, 7), col = i;
        else row = std::min(a, 7), col = 9 - i;
        row = std::min(row, 7), col = std::min(col, 9);
        size_t o = 0x10 * (size_t)(row * 10 + col);
        uint8_t t0 = (uint8_t)raw[o], t2 = (uint8_t)raw[o + 2];
        h = f32(raw, o + 12);
        bool solid = (t0 & 2) && !(t2 & 8);
        bool hazard = ((t2 & 4) && h < -0.3) || h < -1.0 || (t0 & 1);
        return !hazard && (pit_only || !solid);
    };
    bool even = slot % 2 == 0 && 0 < along && along < (side == N || side == S ? 10 : 8);
    auto centre = [&](int i) {
        double h;
        return !even || free(i, along - 1, true, h);
    };
    int first = std::max(min_tile, 1);
    for (bool walk_on : {true, false})
        for (int i = first; i < 5; i++) {
            double h, h2;
            bool ok = free(i, along, false, h);
            if (ok && (!walk_on || free(i + 1, along, false, h2))) {
                double lat = centre(i) && (!walk_on || centre(i + 1)) ? 0.0 : 0.75;
                return {round2(i * 1.5 + 0.75), round2(std::max(h, 0.0)), lat};
            }
        }
    if (even)
        for (bool walk_on : {true, false})
            for (int i = first; i < 5; i++) {
                double h, h2;
                bool ok = free(i, along - 1, false, h);
                if (ok && (!walk_on || free(i + 1, along - 1, false, h2))) return {round2(i * 1.5 + 0.75), round2(std::max(h, 0.0)), -0.75};
            }
    return {2.2, 0.0, 0.0};
}

void inward_only(Reader &R, Data &D) {
    std::map<std::string, Grid> grids;
    std::map<std::string, std::set<std::pair<int, int>>> arrivals;
    auto grid = [&](const std::string &room) -> bool {
        if (grids.count(room)) return grids[room].ok;
        Grid g = grid_of(R, room);
        grids[room] = g;
        if (!g.ok) return false;
        auto o = origin(room);
        std::set<std::pair<int, int>> arr;
        for (const LebActor &a : R.room(room).actors) {
            std::string nm = name_of(a);
            bool hit = false;
            for (const char *p : INWARD_ARRIVALS) hit = hit || starts(nm, p);
            if (hit)
                arr.insert({std::min(7, std::max(0, (int)((a.pos[2] - o.second) / 1.5))),
                            std::min(9, std::max(0, (int)((a.pos[0] - o.first) / 1.5)))});
        }
        arrivals[room] = arr;
        return true;
    };
    auto tiles = [](int side, int slot) {
        std::set<int> along = slot % 2 == 0 ? std::set<int>{slot / 2 - 1, slot / 2} : std::set<int>{slot / 2};
        int n = side == N || side == S ? 10 : 8;
        std::vector<std::pair<int, std::pair<int, int>>> out;
        for (int a : along) {
            if (!(0 <= a && a < n)) continue;
            std::pair<int, int> t = side == N ? std::make_pair(0, a) : side == S ? std::make_pair(7, a)
                                  : side == W ? std::make_pair(a, 0) : std::make_pair(a, 9);
            out.push_back({a, t});
        }
        return out;
    };
    auto line = [](int side, int a, int i) {
        return side == N ? std::make_pair(i, a) : side == S ? std::make_pair(7 - i, a)
               : side == W ? std::make_pair(a, i) : std::make_pair(a, 9 - i);
    };
    using T3 = std::tuple<std::string, int, int>;
    std::vector<const Room *> sorted_rooms;
    for (const Room &r : D.rooms) sorted_rooms.push_back(&r);
    std::sort(sorted_rooms.begin(), sorted_rooms.end(), [](const Room *a, const Room *b) { return a->name < b->name; });
    for (const Room *rp : sorted_rooms) {
        const Room &v = *rp;
        bool ok = true;
        for (const std::string &u : v.unit) ok = ok && D.room(u) && grid(u);  // (a unit cell without a grid: skipped)
        if (!ok) continue;
        std::map<std::pair<int, int>, std::string> pos;
        for (const std::string &u : v.unit) pos[cell_pos(u)] = u;
        auto p0 = cell_pos(v.name);
        std::set<T3> external;
        for (const std::string &u : v.unit) {
            auto pu = cell_pos(u);
            const Room &ur = D.at(u);
            for (int s = 0; s < 4; s++) {
                if (pos.count({pu.first + STEP_C[s], pu.second + STEP_R[s]})) continue;
                for (const Open &x : ur.op[s])
                    if (!x.slot.wide())
                        for (const auto &at : tiles(s, x.slot.v)) external.insert(T3{u, at.second.first, at.second.second});
            }
        }
        for (int s = 0; s < 4; s++) {
            if (pos.count({p0.first + STEP_C[s], p0.second + STEP_R[s]})) continue;
            for (const Open &x : v.op[s]) {
                if (x.slot.wide() || x.own) continue;
                const Grid &g = grids[v.name];
                auto ts = tiles(s, x.slot.v);
                auto G = [&](const std::string &room, int r, int c) -> const std::tuple<bool, bool, double> & {
                    return grids[room].t[(size_t)(r * 10 + c)];
                };
                if (ts.empty()) continue;
                bool bad = false;
                for (const auto &t : ts) bad = bad || std::get<0>(G(v.name, t.second.first, t.second.second)) || std::get<1>(G(v.name, t.second.first, t.second.second));
                if (bad) continue;
                double top = -1e300;
                for (const auto &t : ts) top = std::max(top, std::get<2>(G(v.name, t.second.first, t.second.second)));
                if (top < 1.0) continue;
                (void)g;
                bool ledge = false;
                for (const auto &t : ts) {
                    int a = t.first, i = 0;
                    while (i < 7) {
                        auto lc = line(s, a, i);
                        const auto &q = G(v.name, lc.first, lc.second);
                        if (std::get<0>(q) || fabs(std::get<2>(q) - top) > 0.3) break;
                        i++;
                    }
                    if (i < 7) {
                        auto lc = line(s, a, i);
                        ledge = ledge || std::get<0>(G(v.name, lc.first, lc.second));
                    }
                }
                if (!ledge) continue;
                std::set<T3> start;
                for (const auto &t : ts) start.insert(T3{v.name, t.second.first, t.second.second});
                std::set<T3> seen = start;
                std::vector<T3> todo(start.begin(), start.end());
                while (!todo.empty()) {
                    T3 cur = todo.back();
                    todo.pop_back();
                    const std::string &u = std::get<0>(cur);
                    int row = std::get<1>(cur), col = std::get<2>(cur);
                    auto pu = cell_pos(u);
                    const int dr[4] = {0, 1, 0, -1}, dc[4] = {1, 0, -1, 0};
                    for (int k = 0; k < 4; k++) {
                        int nr = row + dr[k], nc = col + dc[k];
                        std::string nu = u;
                        if (!(0 <= nr && nr < 8 && 0 <= nc && nc < 10)) {
                            std::pair<int, int> p = {pu.first + (nc > 9) - (nc < 0), pu.second + (nr > 7) - (nr < 0)};
                            if (!pos.count(p)) continue;
                            nu = pos[p], nr = (nr % 8 + 8) % 8, nc = (nc % 10 + 10) % 10;
                        }
                        const auto &q = G(nu, nr, nc);
                        if (!std::get<0>(q) && !std::get<1>(q) && fabs(std::get<2>(q) - std::get<2>(G(u, row, col))) <= 0.8 &&
                            !seen.count(T3{nu, nr, nc})) {
                            seen.insert(T3{nu, nr, nc});
                            todo.push_back(T3{nu, nr, nc});
                        }
                    }
                }
                bool walkway = false;
                for (const T3 &e : external)
                    if (!start.count(e) && seen.count(e)) walkway = true;
                if (walkway) continue;
                bool ramp = false;
                for (const T3 &e : seen) ramp = ramp || std::get<2>(G(std::get<0>(e), std::get<1>(e), std::get<2>(e))) < top - 0.8;
                if (ramp) continue;
                bool arrive = false;
                for (const T3 &e : seen)
                    for (int dr2 = -1; dr2 <= 1; dr2++)
                        for (int dc2 = -1; dc2 <= 1; dc2++)
                            for (const std::string &u : v.unit)
                                if (u == std::get<0>(e) && arrivals[u].count({std::get<1>(e) + dr2, std::get<2>(e) + dc2})) arrive = true;
                D.inward[{v.name, s, x.slot.v}] = arrive ? "arrival" : "ledge";
            }
        }
    }
    for (const auto &e : D.inward_reviewed) D.inward.insert(e);  // (setdefault: the grids' result first)
}

// ---------------------------------------------------------------- textures, level cells, the map
std::set<std::string> mi_refs(const std::string &b) {  // re.finditer(rb'MI_[A-Za-z0-9_.]+')
    std::set<std::string> out;
    size_t i = 0;
    while ((i = b.find("MI_", i)) != std::string::npos) {
        size_t j = i + 3;
        while (j < b.size() && (isalnum((unsigned char)b[j]) || b[j] == '_' || b[j] == '.')) j++;
        if (j == i + 3) {
            i++;
            continue;
        }
        out.insert(b.substr(i, j - i));
        i = j;
    }
    return out;
}

}  // namespace

void read_game_facts(Data &D, const GameFiles &game, bool hook) {
    Reader R(game);
    int h = hook ? 1 : 0;
    // rooms
    for (Room &r : D.rooms) {
        r.areas[h] = room_areas(R, D, r.name, hook);
        r.locks.clear(), r.blocks.clear();
        const LebInfo &L = R.room(r.name);
        for (const LebActor &a : L.actors) {
            if (a.type == 0xED) r.locks.push_back({nearest_side(a, r.name), first_flag(a)});
            if (a.type == 0x102) {
                Value f = first_flag(a);
                r.blocks.push_back(f.truthy() ? f : Value(r.name + "-" + std::to_string(r.blocks.size())));
            }
        }
        std::set<std::string> names;
        for (const LebActor &a : L.actors) names.insert(name_of(a).substr(0, name_of(a).find('-')));
        r.throw_ok = names.count("ObjDungeonPot") || names.count("ObjElephantS");
        r.throw_req = names.count("ObjDungeonPot") ? Atoms{"bracelet"} : names.count("ObjElephantS") ? Atoms{"bracelet:2"} : Atoms();
        for (int s = 0; s < 4; s++) r.shutter[s] = shutter_req(R, D, r.name, s);
        r.land.clear();
        if (L.ok && L.grid && L.raw.size() >= 0x10 * 80)
            for (int s = 0; s < 4; s++)
                for (const Open &x : r.op[s])
                    if (!x.slot.wide())
                        for (int m : {0, 1}) r.land[{s, x.slot.v, m}] = landing_depth(R, r.name, s, x.slot.v, m);
    }
    // textures: the level archives' names, each room's references among them
    std::map<std::string, int> ids;
    std::map<std::string, std::set<std::string>> level_names;
    for (const std::string &lv : LEVEL_ORDER) {
        std::string b;
        std::set<std::string> &names = level_names[lv];
        if (R.read("region_common/map/" + lv + ".bntx", b)) {
            Bntx bank = Bntx::parse(b);
            for (const BntxTexture &t : bank.textures) names.insert(t.name);
        }
    }
    auto tex_of = [&](const std::string &room) {
        std::string b;
        std::vector<int> out;
        const std::set<std::string> &names = level_names[level_of(room)];
        if (!R.read("region_common/map/" + room + ".bfres", b)) return out;
        for (const std::string &ref : mi_refs(b))
            if (names.count(ref)) {
                auto it = ids.find(ref);
                if (it == ids.end()) it = ids.insert({ref, (int)ids.size()}).first;
                out.push_back(it->second);
            }
        std::sort(out.begin(), out.end());
        return out;
    };
    for (Room &r : D.rooms) r.tex = tex_of(r.name);
    std::set<std::string> roof_refs;
    for (const std::string &f : R.list("region_common/map"))
        if (starts(f, "DungeonRoof") && ends(f, ".bfres")) {
            std::string b;
            if (R.read("region_common/map/" + f, b))
                for (const std::string &x : mi_refs(b)) roof_refs.insert(x);
        }
    for (const std::string &lv : LEVEL_ORDER) {
        std::string dir = "region_common/level/" + lv;
        std::vector<std::string> &off = D.offgrid[lv];
        off.clear();
        for (const std::string &f : R.list(dir))
            if (ends(f, ".leb") && !D.room(f.substr(0, f.size() - 4))) off.push_back(f.substr(0, f.size() - 4));
        std::set<int> all;
        for (const std::string &k : off)
            for (int i : tex_of(k)) all.insert(i);
        D.offgrid_tex_all[lv] = std::vector<int>(all.begin(), all.end());
        std::vector<int> base;
        for (const std::string &x : roof_refs)
            if (level_names[lv].count(x)) {
                auto it = ids.find(x);
                if (it == ids.end()) it = ids.insert({x, (int)ids.size()}).first;
                base.push_back(it->second);
            }
        D.base_tex[lv] = base;
        // level_cells: the level file's camera zones' areas
        std::set<std::string> &cells = D.level_cells[lv];
        cells.clear();
        std::string lvb;
        if (R.read(dir + "/" + lv + ".lvb", lvb)) {
            FixedHash fh = FixedHash::parse(lvb);
            const FhEntry *area = fh.find("area"), *zone = fh.find("zone");
            std::vector<std::string> names;
            if (area && area->sub)
                for (const FhEntry &e : area->sub->entries) names.push_back(e.name);
            if (zone && zone->sub)
                for (const FhEntry &z : zone->sub->entries) {
                    if (z.data.size() < 0x9c + 7) continue;
                    size_t n = (uint8_t)z.data[0x9c + 6];
                    for (size_t k = 0; k < n && 0x9c + 7 + k < z.data.size(); k++) {
                        size_t i = (uint8_t)z.data[0x9c + 7 + k];
                        if (i < names.size()) cells.insert(names[i]);
                    }
                }
        }
        D.stairs[lv] = stairs_links(R, D, lv);
        D.holes[lv].clear();
        Value holes = Value::new_list(), ports = Value::new_list();
        std::vector<Hole> hl = hole_links(R, D, lv);
        std::set<std::pair<std::string, std::string>> hs;
        for (const Hole &x : hl) hs.insert({x.room, x.target});
        D.holes[lv] = std::vector<std::pair<std::string, std::string>>(hs.begin(), hs.end());
        auto areas_v = [&](const std::set<int> &s) {
            Value l = Value::new_list();
            for (int i : s) l.list().push_back((long long)i);
            return l;
        };
        std::map<std::string, std::vector<std::pair<double, double>>> starts_of;
        auto player_starts = [&](const std::string &room) -> const std::vector<std::pair<double, double>> & {
            auto it = starts_of.find(room);
            if (it != starts_of.end()) return it->second;
            std::vector<std::pair<double, double>> out;
            for (const LebActor &a : R.room(room).actors)
                if (starts(name_of(a), "TagPlayerStart")) out.push_back({a.pos[0], a.pos[2]});
            return starts_of[room] = out;
        };
        for (const Hole &x : hl) {
            const Areas &Ab = D.at(x.target).areas[h];
            std::set<int> land;
            const auto &st = player_starts(x.target);
            if (!st.empty())
                for (const auto &p : st)
                    for (int i : spot_areas(Ab, x.target, p.first, p.second)) land.insert(i);
            else
                for (int i = 0; i < Ab.n; i++) land.insert(i);
            holes.list().push_back(Value(List{Value(x.room), areas_v(spot_areas(D.at(x.room).areas[h], x.room, x.x, x.z)),
                                              Value(x.target), areas_v(land)}));
        }
        // stairs_ports
        std::map<std::string, std::vector<std::pair<std::string, std::pair<double, double>>>> acts;
        for (const Room &r : D.rooms) {
            if (r.level != lv) continue;
            const LebInfo &L = R.room(r.name);
            if (!L.ok) continue;
            for (const LebActor &a : L.actors) {
                std::string nm = name_of(a);
                if ((starts(nm, "AreaDownstairs") || starts(nm, "AreaUpstairs")) && a.params[0].kind == LebParam::Str)
                    acts[r.name].push_back({a.params[0].s, {a.pos[0], a.pos[2]}});
            }
        }
        auto alts_v = [](const std::vector<Atoms> &al) {
            Value l = Value::new_list();
            for (const Atoms &a : al) {
                Value x = Value::new_list();
                for (const std::string &s : a) x.list().push_back(s);
                l.list().push_back(x);
            }
            return l;
        };
        auto port = [&](const std::string &a, std::pair<double, double> p, const std::string &b, std::pair<double, double> q,
                        const std::vector<Atoms> &req) {
            ports.list().push_back(Value(List{Value(a), areas_v(spot_areas(D.at(a).areas[h], a, p.first, p.second)), Value(b),
                                              areas_v(spot_areas(D.at(b).areas[h], b, q.first, q.second)), alts_v(req)}));
        };
        for (const auto &ab : D.stairs[lv]) {
            const std::string &a = ab.first, &b = ab.second;
            std::vector<std::pair<double, double>> dab, dba, pa, pb;
            for (const auto &x : acts[a]) {
                if (x.first == b) dab.push_back(x.second);
                if (!D.room(x.first)) pa.push_back(x.second);
            }
            for (const auto &x : acts[b]) {
                if (x.first == a) dba.push_back(x.second);
                if (!D.room(x.first)) pb.push_back(x.second);
            }
            if (!dab.empty() || !dba.empty()) {
                bool tower = (a == "Lv07EagleTower_04C" && b == "Lv07EagleTower_04G") || (a == "Lv07EagleTower_04G" && b == "Lv07EagleTower_04C");
                std::vector<Atoms> req = {tower ? Atoms{"break-crystal"} : Atoms()};
                struct Way { std::string src, dst; const std::vector<std::pair<double, double>> *here, *there; };
                Way ways[2] = {{a, b, &dab, &dba}, {b, a, &dba, &dab}};
                for (const Way &w : ways) {
                    const auto &here = !w.here->empty() ? *w.here : player_starts(w.src);
                    const auto there = !w.there->empty() ? *w.there : player_starts(w.dst);
                    for (const auto &p : here)
                        for (const auto &q : there) port(w.src, p, w.dst, q, req);
                }
                continue;
            }
            std::vector<Atoms> req = {Atoms{"feather"}};
            auto pz = D.passages.find({std::min(a, b), std::max(a, b)});
            if (pz != D.passages.end()) req = pz->second;
            for (const auto &p : pa)
                for (const auto &q : pb) port(a, p, b, q, req), port(b, q, a, p, req);
        }
        D.ports[h].set(lv, ports);
        D.hole_spots[h].set(lv, holes);
    }
    D.tex_names = ids.size();
    inward_only(R, D);
    // the pause-menu map: each floor page's shift (ui/Game.arc anim/Dg_DgnMap_Dgn_<NN>.bflan) and each room's page
    D.map_shifts.clear(), D.map_anims.clear();
    std::string arc;
    if (R.read("region_common/ui/Game.arc", arc)) {
        if (!arc.compare(0, 4, "Yaz0")) throw std::runtime_error("ui/Game.arc: compressed archives aren't read");
        Sarc s = Sarc::parse(arc);
        static const std::regex anim("^anim/Dg_DgnMap_Dgn_(\\d+)\\.bflan$");
        for (const auto &f : s.files) {
            std::smatch m;
            if (!std::regex_search(f.first, m, anim)) continue;
            const std::string &b = f.second;
            size_t p = b.find("N_MapPosition_00");
            size_t t = p == std::string::npos ? std::string::npos : b.find("FLPA", p);
            if (t == std::string::npos) continue;
            double xy[2] = {0.0, 0.0};
            for (int i = 0; i < (uint8_t)b[t + 4]; i++) {
                size_t io = t + u32(b, t + 8 + (size_t)i * 4);
                int target = (uint8_t)b[io + 1];
                uint16_t nkeys = u16(b, io + 4);
                uint32_t koff = u32(b, io + 8);
                if (target < 2 && nkeys) xy[target] = f32(b, io + koff + 4);
            }
            D.map_shifts[std::stoll(m[1].str())] = {xy[0], xy[1]};
        }
    }
    std::string sheet;
    if (R.read("region_common/datasheets/UiDungeonArea.gsheet", sheet)) {
        GSheet g = GSheet::parse(sheet);
        for (const GStruct &v : g.values) {
            const GValue *n = gfield(v, "mLeaName"), *a = gfield(v, "mAnimFloorIndex");
            if (n && a && starts(n->s, "Lv")) D.map_anims[n->s] = a->i;
        }
    }
}

}  // namespace rooms
}  // namespace solve
}  // namespace rando
