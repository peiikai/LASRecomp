// tools/rando/room_logic.py, ported (rooms.h): the item logic of shuffled dungeons, from their room graphs.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <regex>
#include <set>

#include "rooms.h"

namespace rando {
namespace solve {
namespace rooms {

namespace {

const std::map<std::string, std::pair<int, std::string>> LEVELS = {
    {"Lv10ClothesDungeon", {0, "color-dungeon"}}, {"Lv01TailCave", {1, "tail-cave"}},
    {"Lv02BottleGrotto", {2, "bottle-grotto"}},   {"Lv03KeyCavern", {3, "key-cavern"}},
    {"Lv04AnglersTunnel", {4, "angler-tunnel"}}, {"Lv05CatfishsMaw", {5, "catfish-maw"}},
    {"Lv06FaceShrine", {6, "face-shrine"}},       {"Lv07EagleTower", {7, "eagle-tower"}},
    {"Lv08TurtleRock", {8, "turtle-rock"}}};
const char *const SHUTTER = "sword | shoot-arrows | bombs | rod | boomerang";
const char *const SWITCH_SHUTTER = "sword | shoot-arrows | bombs | rod | boomerang | hookshot";
const char *const JUMP = "feather | (rooster & bad-pets)";
const char *const LONG_JUMP = "(feather & boots) | (rooster & bad-pets)";
const size_t MAX_TERMS = 48;
const int64_t ONEWAY_OUT = 0x1A3, ONEWAY_IN = 0x1A4;
const std::map<std::string, std::string> LEDGE_VIA = {{"D6-wizzrobes-ledge", "Lv06FaceShrine_08G"},
                                                      {"D3-east-ledge", "Lv03KeyCavern_03D"},
                                                      {"D8-northern-ledge", "Lv08TurtleRock_03E"}};
const char *const INSTRUMENTS[] = {"full-moon-cello", "conch-horn", "sea-lilys-bell", "surf-harp",
                                   "wind-marimba", "coral-triangle", "evening-calm-organ", "thunder-drum"};
const char *const TOWER = "Lv07EagleTower";

std::string level_with(int n) {
    for (const auto &kv : LEVELS)
        if (kv.second.first == n) return kv.first;
    return "";
}
bool starts(const std::string &s, const std::string &p) { return !s.compare(0, p.size(), p); }
bool ends(const std::string &s, const std::string &e) {
    return s.size() >= e.size() && !s.compare(s.size() - e.size(), e.size(), e);
}

// '06C' (a level's own cell names) <-> grid positions
std::pair<int, int> lpos(const std::string &level, const std::string &cell) { return cell_pos(level + "_" + cell); }
std::string lname(const std::string &level, int col, int row) { return short_cell(cell_name(level, col, row)); }

// ---------------------------------------------------------------- conditions: parse, strip, show
struct Node;
using NodeP = std::shared_ptr<Node>;
struct Node {
    char kind;  // 'a' atom, '!', '&', '|'
    std::string atom;
    std::vector<NodeP> kids;
};

std::vector<std::string> tokens(const std::string &s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && isspace((unsigned char)s[i])) i++;
        if (i >= s.size()) break;
        char c = s[i];
        if (c == '(' || c == ')' || c == '&' || c == '|' || c == '!') {
            out.push_back(std::string(1, c)), i++;
            continue;
        }
        size_t j = i;
        while (j < s.size() && !isspace((unsigned char)s[j]) && !strchr("()&|!", s[j])) j++;
        out.push_back(s.substr(i, j - i)), i = j;
    }
    return out;
}

NodeP parse(const std::string &s) {
    std::vector<std::string> toks = tokens(s);
    if (toks.empty()) return nullptr;
    size_t pos = 0;
    auto peek = [&]() -> std::string { return pos < toks.size() ? toks[pos] : std::string(); };
    auto take = [&]() -> std::string {
        if (pos >= toks.size()) throw PyError("IndexError", "list index out of range");
        return toks[pos++];
    };
    std::function<NodeP()> expr, term, factor;
    factor = [&]() -> NodeP {
        std::string t = take();
        if (t == "!") return std::make_shared<Node>(Node{'!', "", {factor()}});
        if (t == "(") {
            NodeP e = expr();
            take();
            return e;
        }
        return std::make_shared<Node>(Node{'a', t, {}});
    };
    term = [&]() -> NodeP {
        std::vector<NodeP> n = {factor()};
        while (pos < toks.size() && peek() == "&") take(), n.push_back(factor());
        return n.size() == 1 ? n[0] : std::make_shared<Node>(Node{'&', "", n});
    };
    expr = [&]() -> NodeP {
        std::vector<NodeP> n = {term()};
        while (pos < toks.size() && peek() == "|") take(), n.push_back(term());
        return n.size() == 1 ? n[0] : std::make_shared<Node>(Node{'|', "", n});
    };
    return expr();
}

// strip: atoms drop() says are satisfied taken out; an OR that lost something becomes an AND
std::pair<NodeP, bool> strip(const NodeP &node, const std::function<bool(const std::string &)> &drop) {
    if (!node) return {nullptr, false};
    if (node->kind == 'a') return drop(node->atom) ? std::make_pair(NodeP(), true) : std::make_pair(node, false);
    if (node->kind == '!') {
        auto sub = strip(node->kids[0], drop);
        return {sub.first ? std::make_shared<Node>(Node{'!', "", {sub.first}}) : NodeP(), sub.second};
    }
    std::vector<NodeP> parts;
    bool any = false;
    for (const NodeP &c : node->kids) {
        auto sub = strip(c, drop);
        any |= sub.second;
        if (sub.first) parts.push_back(sub.first);
    }
    char kind = node->kind == '|' && any ? '&' : node->kind;
    if (parts.empty()) return {nullptr, any};
    return {parts.size() == 1 ? parts[0] : std::make_shared<Node>(Node{kind, "", parts}), any};
}

NodeP falsify(const NodeP &node, const std::function<bool(const std::string &)> &bad) {
    if (!node) return nullptr;
    if (node->kind == 'a') return bad(node->atom) ? nullptr : node;
    if (node->kind == '!') return node;
    std::vector<NodeP> parts;
    for (const NodeP &c : node->kids) parts.push_back(falsify(c, bad));
    if (node->kind == '&') {
        for (const NodeP &p : parts)
            if (!p) return nullptr;
        return std::make_shared<Node>(Node{'&', "", parts});
    }
    std::vector<NodeP> keep;
    for (const NodeP &p : parts)
        if (p) keep.push_back(p);
    if (keep.empty()) return nullptr;
    return keep.size() == 1 ? keep[0] : std::make_shared<Node>(Node{'|', "", keep});
}

std::string show(const NodeP &node);
std::string paren(const NodeP &n) { return n->kind == 'a' || n->kind == '!' ? show(n) : "(" + show(n) + ")"; }
std::string show(const NodeP &node) {
    if (!node) return "";
    if (node->kind == 'a') return node->atom;
    if (node->kind == '!') return "!" + paren(node->kids[0]);
    std::string sep = std::string(" ") + node->kind + " ", out;
    for (size_t i = 0; i < node->kids.size(); i++) {
        const NodeP &c = node->kids[i];
        if (i) out += sep;
        out += (c->kind == '|' || c->kind == '&') && c->kind != node->kind ? paren(c) : show(c);
    }
    return out;
}

std::vector<std::string> top_level_or(const std::string &s) {
    std::vector<std::string> out;
    int depth = 0;
    std::string cur;
    for (char ch : s) {
        depth += ch == '(';
        depth -= ch == ')';
        if (ch == '|' && depth == 0) out.push_back(cur), cur.clear();
        else cur += ch;
    }
    bool blank = true;
    for (char ch : cur) blank = blank && isspace((unsigned char)ch);
    if (!blank) out.push_back(cur);
    return out;
}

std::string pystrip(const std::string &s, const char *chars = " \t\n\r\f\v") {
    size_t a = s.find_first_not_of(chars), b = s.find_last_not_of(chars);
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

// re.sub with a function
std::string re_sub(const std::string &s, const std::regex &re, const std::function<std::string(const std::smatch &)> &f) {
    std::string out;
    auto begin = std::sregex_iterator(s.begin(), s.end(), re);
    size_t last = 0;
    for (auto it = begin; it != std::sregex_iterator(); ++it) {
        out += s.substr(last, (size_t)it->position() - last);
        out += f(*it);
        last = (size_t)(it->position() + it->length());
    }
    return out + s.substr(last);
}

// ---------------------------------------------------------------- the room graph
Atoms atoms_of(std::initializer_list<std::string> l) {
    Atoms a(l);
    std::sort(a.begin(), a.end());
    a.erase(std::unique(a.begin(), a.end()), a.end());
    return a;
}
void add_atom(Atoms &a, const std::string &x) {
    auto it = std::lower_bound(a.begin(), a.end(), x);
    if (it == a.end() || *it != x) a.insert(it, x);
}

std::string node_name(const std::string &cell, int a, int n) { return n == 1 ? cell : cell + "." + std::to_string(a); }
std::string cell_of(const std::string &nd) { return nd.substr(0, nd.find('.')); }

struct Graph {
    std::vector<std::string> names;
    std::map<std::string, int> idx;
    std::vector<std::vector<std::pair<int, Atoms>>> out;
    int locks = 0;
    int node(const std::string &n) {
        auto it = idx.find(n);
        if (it != idx.end()) return it->second;
        idx[n] = (int)names.size();
        names.push_back(n);
        out.emplace_back();
        return (int)names.size() - 1;
    }
    int at(const std::string &n) const {
        auto it = idx.find(n);
        if (it == idx.end()) throw PyError("KeyError", py_repr(n));
        return it->second;
    }
};

using Layout = std::vector<std::pair<std::string, std::string>>;  // (cell '08D', room), the layout's order
std::string layout_get(const Layout &L, const std::string &cell) {
    for (const auto &e : L)
        if (e.first == cell) return e.second;
    return "";
}

const Areas &areas_of(const std::string &room, const Ctx &ctx) { return data().at(room).areas[ctx.hook ? 1 : 0]; }

std::vector<std::pair<int, int>> doorway_tiles(int side, const Slot &slot, int &dr, int &dc) {
    int n = side == N || side == S ? 10 : 8;
    std::set<int> along;
    if (slot.wide()) {
        int a = std::stoi(slot.w.substr(1, slot.w.find('-') - 1)), b = std::stoi(slot.w.substr(slot.w.find('-') + 1));
        for (int i = a; i <= b; i++) along.insert(i);
    } else if (slot.v % 2 == 0) {
        along = {slot.v / 2 - 1, slot.v / 2};
    } else {
        along = {slot.v / 2};
    }
    static const int IN_R[4] = {1, -1, 0, 0}, IN_C[4] = {0, 0, 1, -1};
    dr = IN_R[side], dc = IN_C[side];
    std::vector<std::pair<int, int>> out;
    for (int i : along) {
        if (!(0 <= i && i < n)) continue;
        switch (side) {
        case N: out.push_back({0, i}); break;
        case S: out.push_back({7, i}); break;
        case W: out.push_back({i, 0}); break;
        default: out.push_back({i, 9}); break;
        }
    }
    return out;
}

std::set<int> doorway_areas(const std::string &room, int side, const Slot &slot, const Ctx &ctx) {
    const Areas &A = areas_of(room, ctx);
    if (A.n == 1) return {0};
    int dr, dc;
    std::set<int> out;
    for (const auto &t : doorway_tiles(side, slot, dr, dc))
        for (int i = 0; i < 3; i++) {
            int r = t.first + dr * i, c = t.second + dc * i;
            if (r < 0 || r > 7 || c < 0 || c > 9) continue;
            int a = A.tiles[(size_t)(r * 10 + c)];
            if (a >= 0) {
                out.insert(a);
                break;
            }
        }
    if (out.empty())
        for (int i = 0; i < A.n; i++) out.insert(i);
    return out;
}

std::vector<std::string> own_doors(const Room &room, int side, const Slot &slot) {
    std::vector<std::string> out;
    if (slot.wide()) return out;
    for (const Door &d : room.doors) {
        if (d.side != side || std::abs(d.half - slot.v) > 1 || d.mirror) continue;
        // a water door over shallow water (read_game_facts: shallow_water): Link walks through it - Angler's Tunnel's
        // entrance 08D -> 07D (user's play-test 2026-11-23), Catfish's Maw 03F / 04F
        if (d.kind == "blocked:water" && room.shallow_water.count({d.side, d.idx})) continue;
        out.push_back(d.kind);
    }
    return out;
}

// lock_flag: the first lock of that side's flag (None: a Value None)
Value lock_flag(const Room &room, int side) {
    for (const auto &l : room.locks)
        if (l.first == side) return l.second;
    return Value();
}
std::string py_str(const Value &v) { return v.is_str() ? v.s() : v.is_none() ? "None" : std::to_string(v.i()); }

// door_terms: false = None (impassable)
bool door_terms(const std::vector<std::string> &types, int n, bool entering, const Room &room, const std::string &lock_id,
                const Atoms &shutter, const Room &thrower, Atoms &req) {
    req.clear();
    for (const std::string &t : types) {
        if (t == "shutter" && !shutter.empty()) {
            for (const std::string &a : shutter) add_atom(req, a);
            continue;
        }
        if (t == "potdoor") {
            if (!thrower.throw_ok) return false;
            for (const std::string &a : thrower.throw_req) add_atom(req, a);
            continue;
        }
        if (t == "oneway") {
            if (std::find(room.types.begin(), room.types.end(), entering ? ONEWAY_IN : ONEWAY_OUT) != room.types.end()) continue;
            return false;
        }
        if (t == "blocked:statue" || t == "blocked:chest") return false;
        if (t == "lock") add_atom(req, "LOCK|" + std::to_string(n) + "|" + lock_id);
        else if (t == "bossdoor") add_atom(req, "nightmare-key-D" + std::to_string(n));
        else if (t == "cracked") add_atom(req, "bombs");
        else if (t == "blocked:water") add_atom(req, "flippers");
        else if (t == "blocked:crystal") add_atom(req, "break-crystal");
        else if (t == "shutter") add_atom(req, "rs-shutter");
    }
    return true;
}

// room_overrides.door: false = None (never)
bool door_fix(const std::string &room, int side, const Slot &slot, const char *way, Atoms &out) {
    out.clear();
    const Value &ov = data().door_overrides;
    if (!ov.has(room)) return true;
    for (const Value &row : ov.at(room).list()) {
        const List &r = row.list();
        // (a wide opening's row names it: ['N', 'w0-9', ...] - Angler's Tunnel's railed walkway)
        if (side_index(r[0].s()[0]) != side || r[1].is_str() != slot.wide()) continue;
        if (slot.wide() ? r[1].s() != slot.w : r[1].i() != slot.v) continue;
        const Value &v = r[!strcmp(way, "in") ? 2 : 3];
        if (v.is_none()) return false;
        for (const Value &a : v.list()) out.push_back(a.s());
        std::sort(out.begin(), out.end());
        return true;
    }
    return true;
}

std::string side_str(int s) { return std::string(1, SIDES[s]); }
std::string slot_str(const Slot &s) { return s.wide() ? s.w : std::to_string(s.v); }

struct SlotHash {
    int64_t operator()(const Slot &s) const {
        if (s.wide()) return py_str_hash(s.w);
        return s.v == -1 ? -2 : s.v;
    }
};

Graph level_graph(const std::string &level, const Layout &layout, const Value &jump_rows, const Ctx &ctx) {
    const Data &D = data();
    int n = LEVELS.at(level).first;
    std::map<std::pair<int, int>, std::string> pos;
    for (const auto &e : layout) pos[lpos(level, e.first)] = e.first;
    std::map<std::string, const Areas *> info;
    for (const auto &e : layout) info[e.first] = &areas_of(e.second, ctx);
    Graph G;
    for (const auto &e : layout)
        for (int a = 0; a < info[e.first]->n; a++) G.node(node_name(e.first, a, info[e.first]->n));
    auto link = [&](const std::string &src, const std::set<int> &sa, const std::string &dst, const std::set<int> &da,
                    const std::vector<Atoms> &alts) {
        for (int a : sa)
            for (int b : da) {
                int from = G.at(node_name(src, a, info[src]->n)), to = G.at(node_name(dst, b, info[dst]->n));
                for (const Atoms &r : alts) G.out[(size_t)from].push_back({to, r});
            }
    };
    for (const auto &e : layout) {
        const Areas &A = *info[e.first];
        for (const Move &m : A.moves) {
            int from = G.at(node_name(e.first, m.a, A.n)), to = G.at(node_name(e.first, m.b, A.n));
            for (const Atoms &r : m.alts) G.out[(size_t)from].push_back({to, r});
        }
    }
    std::set<std::string> flags;
    for (const auto &e : layout) {
        const Room &room = D.at(e.second);
        for (const Door &d : room.doors)
            if (d.kind == "lock") {
                Value f = lock_flag(room, d.side);
                flags.insert(f.truthy() ? "f:" + py_str(f) : "t:" + room.name + ":" + side_str(d.side));
            }
    }
    std::set<std::string> rooms_in;
    for (const auto &e : layout) rooms_in.insert(e.second);
    for (const std::string &r : rooms_in)
        for (const Value &b : D.at(r).blocks) flags.insert("b:" + (b.is_str() ? "s" + b.s() : "i" + py_str(b)));
    G.locks = (int)flags.size();

    struct Via {
        std::string cell, target;
        std::set<int> dst;
        Atoms req;
    };
    std::vector<Via> via_stairs;
    std::set<std::tuple<std::string, int, int>> jumped;
    for (const Value &rowv : jump_rows.list()) {
        const List &row = rowv.list();
        if (row[0].s() != level) continue;
        std::string cell = row[1].s();
        int side = side_index(row[2].s()[0]), own = (int)row[3].i();
        std::string target = row[5].s();
        int tside = side_index(row[6].s()[0]), tslot = (int)row[4].i();
        jumped.insert({cell, side, own});
        std::string troom = layout_get(layout, target);
        if (troom.empty()) continue;
        const Room &room = D.at(layout_get(layout, cell));
        Slot os;
        os.v = own;
        Slot ts;
        ts.v = tslot;
        auto kind = D.inward.find({room.name, side, own});
        Value lf = lock_flag(room, side);
        std::string lock_id = lf.truthy() ? py_str(lf) : cell + side_str(side) + std::to_string(own);
        Atoms req, f0, f1;
        bool ok = door_terms(own_doors(room, side, os), n, false, room, lock_id, room.shutter[side], room, req);
        bool ok0 = door_fix(room.name, side, os, "out", f0), ok1 = door_fix(troom, tside, ts, "in", f1);
        if (!ok || !ok0 || !ok1) continue;
        req = unite(unite(req, f0), f1);
        std::set<int> dst = doorway_areas(troom, tside, ts, ctx);
        if (kind != D.inward.end() && kind->second == "arrival") via_stairs.push_back({cell, target, dst, req});
        else if (kind == D.inward.end()) link(cell, doorway_areas(room.name, side, os, ctx), target, dst, {req});
    }
    for (const auto &e : layout) {
        const std::string &cell = e.first;
        const Room &room = D.at(e.second);
        auto p = lpos(level, cell);
        for (int side = 0; side < 4; side++) {
            auto nbi = pos.find({p.first + STEP_C[side], p.second + STEP_R[side]});
            if (nbi == pos.end()) continue;
            const std::string &nb = nbi->second;
            const Room &other = D.at(layout_get(layout, nb));
            PySet<Slot, SlotHash> theirs;
            for (const Open &x : other.op[OPP[side]]) theirs.add(x.slot);
            bool home_pair = room.name == level + "_" + cell && other.name == level + "_" + nb &&
                             (neighbour(room.name, side) == other.name || neighbour(other.name, OPP[side]) == room.name);
            std::vector<Slot> slots;
            for (const Open &x : room.op[side]) slots.push_back(x.slot);
            if (home_pair)
                for (const Slot &t : theirs.items())
                    if (std::find(slots.begin(), slots.end(), t) == slots.end()) slots.push_back(t);
            for (const Slot &slot : slots) {
                if ((!slot.wide() && jumped.count({cell, side, slot.v})) || (!theirs.contains(slot) && !home_pair)) continue;
                Value lf = lock_flag(room, side), lf2 = lock_flag(other, OPP[side]);
                std::string id1 = lf.truthy() ? py_str(lf) : cell + side_str(side) + slot_str(slot);
                std::string id2 = lf2.truthy() ? py_str(lf2) : nb + side_str(OPP[side]) + slot_str(slot);
                Atoms req_out, req_in, f0, f1;
                bool o1 = door_terms(own_doors(room, side, slot), n, false, room, id1, room.shutter[side], room, req_out);
                bool o2 = door_terms(own_doors(other, OPP[side], slot), n, true, other, id2, other.shutter[OPP[side]], room, req_in);
                bool o3 = door_fix(room.name, side, slot, "out", f0), o4 = door_fix(other.name, OPP[side], slot, "in", f1);
                if (!o1 || !o2 || !o3 || !o4) continue;
                req_out = unite(req_out, f0), req_in = unite(req_in, f1);
                auto kind = slot.wide() ? D.inward.end() : D.inward.find({room.name, side, slot.v});
                std::set<int> dst = doorway_areas(other.name, OPP[side], slot, ctx);
                if (kind != D.inward.end() && kind->second == "arrival") via_stairs.push_back({cell, nb, dst, unite(req_out, req_in)});
                else if (kind == D.inward.end())
                    link(cell, doorway_areas(room.name, side, slot, ctx), nb, dst, {unite(req_out, req_in)});
            }
        }
    }
    for (const auto &e : layout) {  // keyhole blocks: they may guard the way out
        const Room &room = D.at(e.second);
        if (room.blocks.empty()) continue;
        Atoms bl;
        for (const Value &b : room.blocks) add_atom(bl, "LOCK|" + std::to_string(n) + "|block" + py_str(b));
        for (int a = 0; a < info[e.first]->n; a++) {
            int nd = G.at(node_name(e.first, a, info[e.first]->n));
            for (auto &tr : G.out[(size_t)nd])
                if (cell_of(G.names[(size_t)tr.first]) != e.first) tr.second = unite(tr.second, bl);
        }
    }
    std::map<std::string, std::string> where;
    for (const auto &e : layout) where[e.second] = e.first;
    std::map<std::string, std::vector<std::tuple<std::string, std::set<int>, std::vector<Atoms>>>> stairs_from;
    auto alts_of = [](const Value &l) {
        std::vector<Atoms> out;
        for (const Value &a : l.list()) {
            Atoms x;
            for (const Value &s : a.list()) x.push_back(s.s());
            std::sort(x.begin(), x.end());
            out.push_back(x);
        }
        return out;
    };
    auto int_set = [](const Value &l) {
        std::set<int> out;
        for (const Value &x : l.list()) out.insert((int)x.i());
        return out;
    };
    const Value &ports = D.ports[ctx.hook ? 1 : 0];
    if (ports.has(level))
        for (const Value &pv : ports.at(level).list()) {
            const List &p = pv.list();
            std::string a = p[0].s(), b = p[2].s(), ca, cb;
            if (level == TOWER) {
                ca = where.count(a) ? where[a] : "", cb = where.count(b) ? where[b] : "";
            } else {
                ca = short_cell(a), cb = short_cell(b);
                if (layout_get(layout, ca) != a || layout_get(layout, cb) != b) continue;
            }
            if (!ca.empty() && !cb.empty()) {
                std::set<int> sa = int_set(p[1]);
                std::vector<Atoms> alts = alts_of(p[4]);
                link(ca, sa, cb, int_set(p[3]), alts);
                stairs_from[cb].push_back({ca, sa, alts});
            }
        }
    for (const Via &v : via_stairs) {
        auto it = stairs_from.find(v.cell);
        if (it == stairs_from.end()) continue;
        for (const auto &sf : it->second) {
            std::vector<Atoms> alts;
            for (const Atoms &x : std::get<2>(sf)) alts.push_back(unite(v.req, x));
            link(std::get<0>(sf), std::get<1>(sf), v.target, v.dst, alts);
        }
    }
    if (level == TOWER) {
        const Value &holes = D.hole_spots[ctx.hook ? 1 : 0];
        if (holes.has(level))
            for (const Value &hv : holes.at(level).list()) {
                const List &h = hv.list();
                std::string a = h[0].s(), b = h[2].s();
                if (where.count(a) && where.count(b)) link(where[a], int_set(h[1]), where[b], int_set(h[3]), {Atoms()});
            }
    }
    for (const auto &e : layout) {  // leaving a mini-boss room: its soul
        const Atoms *ex = ctx.exit_of(e.second);
        if (!ex) continue;
        for (int a = 0; a < info[e.first]->n; a++) {
            int nd = G.at(node_name(e.first, a, info[e.first]->n));
            for (auto &tr : G.out[(size_t)nd]) {
                std::string tc = cell_of(G.names[(size_t)tr.first]);
                const Atoms *ex2 = ctx.exit_of(layout_get(layout, tc));
                if (tc == e.first || (ex2 && *ex2 == *ex)) continue;
                tr.second = unite(tr.second, *ex);
            }
        }
    }
    return G;
}

std::string entrance_node(const std::string &level, const Layout &layout, const Ctx &ctx) {
    const std::string &ent = entrance_of(level);
    std::string cell = short_cell(ent), room = layout_get(layout, cell);
    if (room.empty()) room = ent;
    const Areas &A = areas_of(room, ctx);
    if (A.n == 1) return cell;
    std::set<int> got;
    const Room *r = data().room(room);
    if (r)
        for (int s = 0; s < 4; s++) {
            std::string nb = neighbour(r->name, s);
            if (r->op[s].empty() || (!nb.empty() && data().room(nb))) continue;  // (_exit_sides)
            for (const Open &o : r->op[s])
                for (int a : doorway_areas(room, s, o.slot, ctx)) got.insert(a);
        }
    return node_name(cell, got.empty() ? 0 : *got.begin(), A.n);
}

std::map<std::string, int> key_counts(const Graph &G, int start) {
    std::set<std::string> locks;
    for (const auto &outs : G.out)
        for (const auto &tr : outs)
            for (const std::string &a : tr.second)
                if (starts(a, "LOCK|")) locks.insert(a);
    std::map<std::string, int> out;
    for (const std::string &L : locks) {
        std::set<int> seen = {start};
        std::vector<int> todo = {start};
        std::set<std::string> met;
        while (!todo.empty()) {
            int a = todo.back();
            todo.pop_back();
            for (const auto &tr : G.out[(size_t)a]) {
                if (std::binary_search(tr.second.begin(), tr.second.end(), L)) continue;
                for (const std::string &x : tr.second)
                    if (starts(x, "LOCK|")) met.insert(x);
                if (seen.insert(tr.first).second) todo.push_back(tr.first);
            }
        }
        out[L] = (int)met.size() + 1;
    }
    return out;
}

int kc_get(const std::map<std::string, int> &kc, const std::string &k, int dflt) {
    auto it = kc.find(k);
    return it == kc.end() ? dflt : it->second;
}

std::set<Atoms> resolve_keys(const std::set<Atoms> &terms, const std::map<std::string, int> &kc, int n, int total) {
    std::set<Atoms> out;
    for (const Atoms &t : terms) {
        std::vector<int> ks;
        Atoms rest;
        for (const std::string &a : t)
            if (starts(a, "LOCK|")) ks.push_back(kc_get(kc, a, total));
            else rest.push_back(a);
        if (!ks.empty()) add_atom(rest, "key-D" + std::to_string(n) + ":" + std::to_string(std::min(*std::max_element(ks.begin(), ks.end()), total)));
        out.insert(rest);
    }
    return out;
}

// reach_terms, its sets in CPython's order: the MAX_TERMS cut can make the result depend on it
std::vector<TermSet> reach_terms(const Graph &G, int start, const std::string &base) {
    std::vector<TermSet> req(G.names.size());
    req[(size_t)start].add(Atoms{base});
    bool changed = true;
    int passes = 0;
    while (changed && passes < 400) {
        changed = false, passes++;
        for (size_t a = 0; a < G.out.size(); a++)
            for (const auto &tr : G.out[a]) {
                size_t b = (size_t)tr.first;
                std::vector<Atoms> snap = req[a].items();
                for (const Atoms &t : snap) {
                    Atoms nt = unite(t, tr.second);
                    bool dominated = false;
                    for (const Atoms &o : req[b].items())
                        if (subset(o, nt)) { dominated = true; break; }
                    if (dominated) continue;
                    TermSet fresh;
                    for (const Atoms &o : req[b].items())
                        if (!subset(nt, o)) fresh.add(o);
                    fresh.add(nt);
                    if (fresh.size() > MAX_TERMS) {
                        std::vector<Atoms> all = fresh.items();
                        std::sort(all.begin(), all.end(), term_less);
                        TermSet cut;
                        for (size_t i = 0; i < MAX_TERMS; i++) cut.add(all[i]);
                        fresh = cut;
                    }
                    req[b] = fresh;
                    changed = true;
                }
            }
    }
    return req;
}

std::set<Atoms> and_terms(const std::set<Atoms> &a, const std::set<Atoms> &b) {
    std::set<Atoms> out;
    for (const Atoms &x : a)
        for (const Atoms &y : b) {
            Atoms t = unite(x, y);
            bool dom = false;
            for (const Atoms &o : out)
                if (subset(o, t)) { dom = true; break; }
            if (dom) continue;
            for (auto it = out.begin(); it != out.end();)
                it = subset(t, *it) ? out.erase(it) : std::next(it);
            out.insert(t);
        }
    if (out.size() > MAX_TERMS) {
        std::vector<Atoms> all(out.begin(), out.end());
        std::sort(all.begin(), all.end(), term_less);
        out = std::set<Atoms>(all.begin(), all.begin() + (long)MAX_TERMS);
    }
    return out;
}

// cell_terms: {cell: terms}, in the order the graph's nodes first name each cell
std::vector<std::pair<std::string, std::set<Atoms>>> cell_terms(const Graph &G, const std::vector<TermSet> &req) {
    std::vector<std::pair<std::string, std::set<Atoms>>> out;
    std::map<std::string, size_t> at;
    for (size_t i = 0; i < G.names.size(); i++) {
        std::string c = cell_of(G.names[i]);
        std::vector<Atoms> items = req[i].items();
        std::set<Atoms> t(items.begin(), items.end());
        auto it = at.find(c);
        if (t.empty()) {
            if (it == at.end()) at[c] = out.size(), out.push_back({c, {}});
            continue;
        }
        if (it == at.end()) {
            at[c] = out.size(), out.push_back({c, t});
            continue;
        }
        std::set<Atoms> &cur = out[it->second].second;
        cur = cur.empty() ? t : and_terms(cur, t);
    }
    return out;
}

bool any_nightmare(const Atoms &t) {
    for (const std::string &a : t)
        if (starts(a, "nightmare-key")) return true;
    return false;
}

Layout short_layout(const Value &layout) {
    Layout L;
    for (const auto &e : layout.dict()) L.push_back({short_cell(e.key), e.value.s()});
    return L;
}

// the rest of room_logic's helpers
bool truthy_setting(const Value &settings, const char *k) { return settings.get(k).truthy(); }

bool vanilla_check(const std::string &loc, const Value &v, const Value &settings) {
    Value content = v.get("content");
    if (content.is_str())
        for (const char *i : INSTRUMENTS)
            if (content.s() == i && !truthy_setting(settings, "shuffle-instruments")) return true;
    if (starts(loc, "D0-rupee-") && !truthy_setting(settings, "blupsanity")) return true;
    static const char *const R[] = {"chest", "boss", "enemy", "drop", "npc", "standing", "overworld-statue",
                                    "dungeon-statue", "underwater"};
    Value sub = v.get("subtype");
    for (const char *x : R)
        if (sub.is_str() && sub.s() == x) return false;
    return true;
}

int dungeon_items(const Value &settings) {
    size_t n = 0;
    if (settings.has("dungeon-items-start")) n = settings.at("dungeon-items-start").len();
    else {
        Value di = settings.get("dungeon-items", "none");
        std::string s = di.is_str() ? di.s() : "";
        n = s == "mc" ? 2 : s == "mcb" ? 3 : s == "stone-beak" ? 1 : 0;
    }
    return 4 - (int)n;
}

bool lr_vanilla(const std::string &k, const Value &v, const Value &settings) {
    if (ends(k, "-instrument") && !truthy_setting(settings, "shuffle-instruments")) return true;
    if (v.get("subtype") == Value("dungeon-statue") && !truthy_setting(settings, "owl-dungeon-gifts")) return true;
    return starts(k, "D0-rupee-") && !truthy_setting(settings, "blupsanity");
}

std::map<int, int> roomless_checks(const Value &settings, const std::function<bool(const std::string &)> &excluded) {
    static const std::regex dn("^D(\\d)-");
    std::map<int, int> out;
    for (const auto &e : data().location_rooms.dict()) {
        std::smatch m;
        bool hit = std::regex_search(e.key, m, dn);
        if (lr_vanilla(e.key, e.value, settings)) continue;
        if (hit && !e.value.get("room").truthy() && e.value.get("subtype") != Value("boss") && !excluded(e.key))
            out[std::stoi(m[1].str())]++;
    }
    return out;
}

std::function<bool(const std::string &)> excluded_in(const Value &settings) {
    Value ex = settings.get("excluded-locations");
    std::set<std::string> s;
    if (ex.is_list())
        for (const Value &x : ex.list()) s.insert(x.s());
    return [s](const std::string &k) { return s.count(k) != 0; };
}

}  // namespace

// ---------------------------------------------------------------- the generator's checks
bool boss_door_ok(const std::string &level, const Value &layout, const Value &rows, const Ctx &ctx) {
    const Data &D = data();
    Layout L = short_layout(layout);
    Graph G = level_graph(level, L, rows, ctx);
    int start = G.at(entrance_node(level, L, ctx));
    auto reach = [&](bool boss_doors) {
        std::set<int> seen = {start};
        std::vector<int> todo = {start};
        while (!todo.empty()) {
            int a = todo.back();
            todo.pop_back();
            for (const auto &tr : G.out[(size_t)a])
                if (!seen.count(tr.first) && (boss_doors || !any_nightmare(tr.second))) seen.insert(tr.first), todo.push_back(tr.first);
        }
        return seen;
    };
    std::set<int> every = reach(true);
    for (const auto &e : L) {
        if (!D.at(e.second).any_open) continue;
        bool hit = false;
        for (int x : every) hit = hit || cell_of(G.names[(size_t)x]) == e.first;
        if (!hit) return false;
    }
    std::set<int> without = reach(false), behind;
    for (int x : every)
        if (!without.count(x)) behind.insert(x);
    std::set<std::string> allowed_cells;
    int n = LEVELS.at(level).first;
    Value inst = D.location_rooms.get("D" + std::to_string(n) + "-instrument");
    std::string inst_room = inst.is_dict() && inst.get("room").is_str() ? inst.at("room").s() : "";
    for (const auto &e : L) {
        if (e.second == inst_room) allowed_cells.insert(e.first);
        for (const Door &d : D.at(e.second).doors)
            if (d.kind == "bossdoor") {
                auto p = lpos(level, e.first);
                allowed_cells.insert(lname(level, p.first + STEP_C[d.side], p.second + STEP_R[d.side]));
            }
    }
    std::set<int> allowed;
    for (size_t i = 0; i < G.names.size(); i++)
        if (allowed_cells.count(cell_of(G.names[i]))) allowed.insert((int)i);
    std::vector<int> todo;
    for (int c : allowed)
        if (behind.count(c)) todo.push_back(c);
    while (!todo.empty()) {
        int a = todo.back();
        todo.pop_back();
        for (const auto &tr : G.out[(size_t)a])
            if (behind.count(tr.first) && allowed.insert(tr.first).second) todo.push_back(tr.first);
    }
    for (int x : behind)
        if (!allowed.count(x)) return false;
    return true;
}

bool fill_ok(const std::string &level, const Value &layout, const Value &rows, const Value &settings, const Ctx &ctx) {
    const Data &D = data();
    Layout L = short_layout(layout);
    Graph G = level_graph(level, L, rows, ctx);
    int start = G.at(entrance_node(level, L, ctx));
    std::map<std::string, int> kc = key_counts(G, start);
    int total = std::max(G.locks, (int)kc.size());
    int n = LEVELS.at(level).first;
    std::map<std::string, int> cell_k;
    for (const auto &ct : cell_terms(G, reach_terms(G, start, "in"))) {
        std::vector<int> ks;
        for (const Atoms &t : ct.second) {
            if (any_nightmare(t)) continue;
            int mx = 0;
            bool any = false;
            for (const std::string &a : t)
                if (starts(a, "LOCK|")) {
                    int v = kc_get(kc, a, total);
                    mx = any ? std::max(mx, v) : v, any = true;
                }
            ks.push_back(any ? mx : 0);
        }
        if (!ks.empty()) cell_k[ct.first] = *std::min_element(ks.begin(), ks.end());
    }
    std::vector<std::pair<std::string, int>> per_room;  // (dict order)
    for (const auto &e : D.location_rooms.dict()) {
        Value room = e.value.get("room");
        if (!room.truthy() || e.value.get("subtype") == Value("boss") || lr_vanilla(e.key, e.value, settings)) continue;
        bool found = false;
        for (auto &pr : per_room)
            if (pr.first == room.s()) pr.second++, found = true;
        if (!found) per_room.push_back({room.s(), 1});
    }
    std::vector<int> ks;
    for (const auto &pr : per_room) {
        std::vector<int> cells;
        for (const auto &e : L)
            if (e.second == pr.first && cell_k.count(e.first)) cells.push_back(cell_k[e.first]);
        if (cells.empty()) continue;
        int k = *std::min_element(cells.begin(), cells.end());
        const Room *r = D.room(pr.first);
        if (r && !r->blocks.empty())
            for (const Value &b : r->blocks) k = std::max(k, kc_get(kc, "LOCK|" + std::to_string(n) + "|block" + py_str(b), 1));
        for (int i = 0; i < pr.second; i++) ks.push_back(k);
    }
    auto rc = roomless_checks(settings, excluded_in(settings));
    for (int i = 0; i < (rc.count(n) ? rc[n] : 0); i++) ks.push_back(total);
    for (int j = 1; j <= total; j++) {
        int c = 0;
        for (int k : ks) c += k <= j - 1;
        if (c < j) return false;
    }
    return (int)ks.size() >= total + dungeon_items(settings);
}

}  // namespace rooms

// ---------------------------------------------------------------- room_logic.apply
std::string room_logic_apply(const RoomPlan &plan, Value &logic_defs, Value &item_defs, Value &settings,
                             PyStrSet &excluded, PyStrSet &absent, const Value &extra, const rooms::Ctx &ctx) {
    using namespace rooms;
    const Data &D = data();
    std::vector<std::pair<std::string, Layout>> layouts;
    for (const auto &e : plan.layouts.dict()) layouts.push_back({e.key, short_layout(e.value)});
    const Value &lr = D.location_rooms;
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> rooms_at;  // room -> [(level, cell)]
    std::map<std::pair<std::string, std::string>, std::string> cell_expr;             // ('' = None)
    std::map<int, int> lock_count;
    auto def = [&](const char *k, const char *cond) {
        Value d = Value::new_dict();
        d.set("type", "interaction"), d.set("condition-basic", cond);
        logic_defs.set(k, d);
    };
    def("rs-shutter", SHUTTER), def("rs-switch-shutter", SWITCH_SHUTTER), def("rs-jump", JUMP), def("rs-long-jump", LONG_JUMP);
    std::map<std::string, std::string> boss_cell;
    long junk_delta = 0;
    std::map<std::string, std::pair<std::map<std::string, int>, int>> kcs;
    std::vector<std::string> kcs_order;
    std::map<std::pair<std::string, std::string>, int> cell_keys;
    std::map<std::string, int> check_keys;
    auto ce = [&](const std::pair<std::string, std::string> &p) -> std::string {
        auto it = cell_expr.find(p);
        return it == cell_expr.end() ? "" : it->second;
    };
    for (const auto &lv_layout : layouts) {
        const std::string &level = lv_layout.first;
        const Layout &layout = lv_layout.second;
        int n = LEVELS.at(level).first;
        const std::string &region = LEVELS.at(level).second;
        std::string rc;
        if (logic_defs.has(region)) {
            Value c = logic_defs.at(region).get("condition-basic", "");
            rc = c.is_str() ? c.s() : "";
        }
        std::regex dn_cell("\\bD" + std::to_string(n) + "-\\d");
        if (std::regex_search(rc, dn_cell)) {
            std::string start0 = short_cell(entrance_of(level));
            std::string st = start0.substr(start0.find_first_not_of('0'));
            std::string ent;
            std::string key = "D" + std::to_string(n) + "-" + st;
            if (logic_defs.has(key)) {
                Value c = logic_defs.at(key).get("condition-basic", "");
                ent = c.is_str() ? c.s() : "";
            }
            std::vector<std::string> outside;
            for (const std::string &a : top_level_or(ent))
                if (!std::regex_search(a, dn_cell)) outside.push_back(pystrip(a));
            if (!outside.empty()) {
                std::string j;
                for (size_t i = 0; i < outside.size(); i++) j += (i ? " | (" : "(") + outside[i] + ")";
                logic_defs.at(region).set("condition-basic", j);
            }
        }
        for (const auto &e : layout) {
            rooms_at[e.second].push_back({level, e.first});
            for (const Door &d : D.at(e.second).doors)
                if (d.kind == "bossdoor") {
                    auto p = lpos(level, e.first);
                    boss_cell[level] = lname(level, p.first + STEP_C[d.side], p.second + STEP_R[d.side]);
                }
        }
        Graph G = level_graph(level, layout, plan.jumps, ctx);
        int start = G.at(entrance_node(level, layout, ctx));
        auto terms = cell_terms(G, reach_terms(G, start, region));
        std::map<std::string, int> kc = key_counts(G, start);
        kcs[level] = {kc, n};
        kcs_order.push_back(level);
        int locks = std::max(G.locks, (int)kc.size());
        lock_count[n] = locks;
        for (const auto &ct : terms) {
            cell_expr[{level, ct.first}] = dnf_str(resolve_keys(ct.second, kc, n, locks));
            std::vector<int> ks;
            for (const Atoms &x : ct.second) {
                if (any_nightmare(x)) continue;
                int mx = 0;
                bool any = false;
                for (const std::string &a : x)
                    if (starts(a, "LOCK|")) {
                        int v = kc_get(kc, a, locks);
                        mx = any ? std::max(mx, v) : v, any = true;
                    }
                ks.push_back(std::min(any ? mx : 0, locks));
            }
            if (!ks.empty()) cell_keys[{level, ct.first}] = *std::min_element(ks.begin(), ks.end());
        }
        std::regex room_re("^D" + std::to_string(n) + "-\\d\\d?[A-P]$");
        for (auto &e : logic_defs.dict()) {
            if (e.value.get("type") != Value("room") || !std::regex_search(e.key, room_re)) continue;
            std::string cell = e.key.substr(e.key.find('-') + 1);
            while (cell.size() < 3) cell = "0" + cell;
            std::string x = ce({level, cell});
            e.value.set("condition-basic", !x.empty() ? x : "nightmare-key-D" + std::to_string(n) + " & !nightmare-key-D" + std::to_string(n));
            for (const char *k : {"condition-advanced", "condition-glitched", "condition-hell"}) e.value.dict().erase(k);
        }
        std::string kk = "key-D" + std::to_string(n);
        if (item_defs.has(kk)) {
            long delta = locks - item_defs.at(kk).at("quantity").i();
            item_defs.at(kk).set("quantity", (long long)locks);
            junk_delta += delta;
        }
    }
    std::map<std::string, long> claimed;
    for (const auto &e : logic_defs.dict())
        if (e.value.get("type") == Value("item") && e.value.get("content").truthy()) claimed[e.value.at("content").s()]++;
    long left = junk_delta;
    auto cl = [&](const std::string &k) { auto it = claimed.find(k); return it == claimed.end() ? 0L : it->second; };
    while (left > 0) {
        std::vector<std::string> spare;
        for (const auto &e : item_defs.dict())
            if ((starts(e.key, "rupee-") || starts(e.key, "chamber-stone")) && e.value.get("quantity", 0).i() - cl(e.key) > 0)
                spare.push_back(e.key);
        std::stable_sort(spare.begin(), spare.end(), [&](const std::string &a, const std::string &b) {
            return cl(a) - item_defs.at(a).at("quantity").i() < cl(b) - item_defs.at(b).at("quantity").i();
        });
        if (spare.empty()) break;
        item_defs.at(spare[0]).set("quantity", (long long)(item_defs.at(spare[0]).at("quantity").i() - 1));
        left--;
    }
    if (left < 0 && item_defs.has("rupee-20"))
        item_defs.at("rupee-20").set("quantity", (long long)(item_defs.at("rupee-20").at("quantity").i() - left));
    std::set<int> shuffled;
    for (const auto &l : layouts) shuffled.insert(LEVELS.at(l.first).first);
    static const std::regex cell_tok("^D(\\d)-\\d\\d?[A-P]$"), clear_re("^D(\\d)-clear$"),
        key_re("^key-D\\d(:\\d+)?$"), cell_find("\\bD(\\d)-\\d\\d?[A-P]\\b"), key_count_re("key-D(\\d):\\d+"),
        cell_ref("\\bD(\\d)-(\\d\\d?[A-P])\\b"), amp_amp("&\\s*&");
    auto is_cell_tok = [&](const std::string &a, int *num) {
        std::smatch m;
        if (!std::regex_search(a, m, cell_tok)) return false;
        if (num) *num = std::stoi(m[1].str());
        return true;
    };
    auto names_shuffled = [&](const std::string &c) {
        for (auto it = std::sregex_iterator(c.begin(), c.end(), cell_find); it != std::sregex_iterator(); ++it)
            if (shuffled.count(std::stoi((*it)[1].str()))) return true;
        return false;
    };
    for (auto &e : logic_defs.dict()) {
        Value t = e.value.get("type");
        if (t == Value("item") || t == Value("room") || e.key == "face-shrine") continue;
        std::smatch m;
        if (std::regex_search(e.key, m, clear_re) && shuffled.count(std::stoi(m[1].str()))) {
            std::string lv = level_with(std::stoi(m[1].str()));
            std::string arena = boss_cell.count(lv) ? ce({lv, boss_cell[lv]}) : "";
            if (!arena.empty()) {
                Value c = e.value.get("condition-basic", "");
                auto rest = strip(parse(c.is_str() ? c.s() : ""), [&](const std::string &a) {
                    std::smatch mm;
                    return is_cell_tok(a, nullptr) || std::regex_search(a, mm, key_re);
                });
                e.value.set("condition-basic", rest.first ? "(" + arena + ") & (" + show(rest.first) + ")" : arena);
            }
            continue;
        }
        for (const char *key : {"condition-basic", "condition-advanced", "condition-glitched", "condition-hell"}) {
            Value c = e.value.get(key);
            if (!c.truthy() || !names_shuffled(c.s())) continue;
            NodeP n2 = falsify(parse(c.s()), [&](const std::string &a) {
                int num;
                return is_cell_tok(a, &num) && shuffled.count(num);
            });
            e.value.set(key, n2 ? show(n2) : "nightmare-key-D0 & !nightmare-key-D0");
        }
    }
    for (auto &e : logic_defs.dict()) {
        Value t = e.value.get("type");
        if (t == Value("item") || t == Value("room")) continue;
        for (const char *key : {"condition-basic", "condition-advanced", "condition-glitched", "condition-hell"}) {
            Value c = e.value.get(key);
            if (!c.truthy()) continue;
            std::smatch m;
            std::string cs = c.s();
            if (!std::regex_search(cs, m, key_count_re)) continue;
            e.value.set(key, re_sub(cs, key_count_re, [&](const std::smatch &mm) {
                int x = std::stoi(mm[1].str());
                if (shuffled.count(x) && lock_count.count(x) && lock_count[x])
                    return "key-D" + mm[1].str() + ":" + std::to_string(lock_count[x]);
                return mm[0].str();
            }));
        }
    }
    std::set<std::string> region_tok;
    for (const auto &e : logic_defs.dict())
        if (e.value.get("type") == Value("room") || e.value.get("type") == Value("region")) region_tok.insert(e.key);
    auto drop = [&](const std::string &a) {
        std::smatch m;
        return region_tok.count(a) || std::regex_search(a, m, key_re);
    };
    auto remap_cells = [&](const std::string &cond, bool &gone) {
        gone = false;
        return re_sub(cond, cell_ref, [&](const std::smatch &m) {
            int x = std::stoi(m[1].str());
            if (!shuffled.count(x)) return m[0].str();
            std::string xl = level_with(x), c = m[2].str();
            while (c.size() < 3) c = "0" + c;
            std::vector<std::string> ps;
            auto it = rooms_at.find(xl + "_" + c);
            if (it != rooms_at.end())
                for (const auto &p : it->second)
                    if (!ce(p).empty()) ps.push_back(ce(p));
            gone = gone || ps.empty();
            if (ps.empty()) return m[0].str();
            std::string j = "(";
            for (size_t i = 0; i < ps.size(); i++) j += (i ? " | (" : "(") + ps[i] + ")";
            return j + ")";
        });
    };
    Value dungeon_of;
    if (!settings.has("dungeon-of")) settings.set("dungeon-of", Value::new_dict());
    dungeon_of = settings.at("dungeon-of");
    if (!settings.has("excluded-locations")) settings.set("excluded-locations", Value::new_list());  // (the caller's set)
    if (!settings.has("absent-locations")) settings.set("absent-locations", Value::new_list());
    long rewritten = 0, unused = 0, fallback = 0;
    std::map<std::string, bool> crystal;
    for (const auto &l : layouts) {
        bool any = false;
        for (const auto &e : l.second) {
            const Room &r = D.at(e.second);
            any = any || std::find(r.actors.begin(), r.actors.end(), "ObjCrystalSw") != r.actors.end();
        }
        crystal[l.first] = any;
    }
    std::map<std::string, std::pair<Value, Value>> targets;  // location -> (room, level)
    for (const auto &e : lr.dict()) targets[e.key] = {e.value.get("room"), e.value.get("level")};
    if (extra.is_dict())
        for (const auto &e : extra.dict()) targets[e.key] = {e.value, Value(level_of(e.value.s()))};
    auto layout_of = [&](const std::string &lv) -> const Layout & {
        for (const auto &l : layouts)
            if (l.first == lv) return l.second;
        throw PyError("KeyError", py_repr(lv));
    };
    std::vector<std::string> locs = logic_defs.dict().keys();
    static const std::regex dn_prefix("^D(\\d)-");
    for (const std::string &loc : locs) {
        Value v = logic_defs.at(loc);
        if (v.get("type") != Value("item") || !targets.count(loc)) continue;
        Value roomv = targets[loc].first, levelv = targets[loc].second;
        std::smatch m;
        int home = -1;
        if (std::regex_search(loc, m, dn_prefix)) home = std::stoi(m[1].str());
        else if (levelv.is_str() && LEVELS.count(levelv.s())) home = LEVELS.at(levelv.s()).first;
        if (home < 0 || !shuffled.count(home)) continue;
        std::string room = roomv.is_str() ? roomv.s() : "";
        if (room.empty() && v.get("subtype") == Value("boss")) {
            std::string lv = level_with(home);
            if (boss_cell.count(lv)) room = layout_get(layout_of(lv), boss_cell[lv]);
        }
        Value cv = v.get("condition-basic", "");
        std::string cond = cv.is_str() ? cv.s() : "";
        if (room.empty()) {
            fallback++;
            int nn = home;
            std::string hl = level_with(nn);
            bool gone;
            std::string c2 = remap_cells(cond, gone);
            if (gone) {
                excluded.add(loc), absent.add(loc);
                unused++;
                continue;
            }
            std::regex kn("key-D" + std::to_string(nn) + ":\\d+");
            std::string repl = lock_count.count(nn) && lock_count[nn] ? "key-D" + std::to_string(nn) + ":" + std::to_string(lock_count[nn]) : "";
            c2 = re_sub(c2, kn, [&](const std::smatch &) { return repl; });
            c2 = pystrip(re_sub(c2, amp_amp, [](const std::smatch &) { return std::string("&"); }), " &");
            v.set("condition-basic", !c2.empty() ? c2 : LEVELS.at(hl).second);
            for (const char *k : {"condition-advanced", "condition-glitched", "condition-hell"}) v.dict().erase(k);
            continue;
        }
        std::vector<std::pair<std::string, std::string>> places;
        if (rooms_at.count(room)) places = rooms_at[room];
        const Room *rr = D.room(room);
        auto with_blocks = [&](const std::pair<std::string, std::string> &p) {
            std::string e = ce(p);
            if (e.empty() || !rr || rr->blocks.empty() || !kcs.count(p.first)) return e;
            const auto &kc = kcs[p.first].first;
            int nn = kcs[p.first].second, k = 0;
            bool any = false;
            for (const Value &b : rr->blocks) {
                int x = kc_get(kc, "LOCK|" + std::to_string(nn) + "|block" + py_str(b), 1);
                k = any ? std::max(k, x) : x, any = true;
            }
            return "(" + e + ") & key-D" + std::to_string(nn) + ":" + std::to_string(k);
        };
        std::vector<std::string> reach;
        for (const auto &p : places)
            if (!ce(p).empty()) reach.push_back(with_blocks(p));
        std::vector<int> ck;
        for (const auto &p : places)
            if (cell_keys.count(p)) ck.push_back(cell_keys[p]);
        auto via = LEDGE_VIA.find(loc);
        if (via != LEDGE_VIA.end() && !reach.empty()) {
            std::vector<std::string> ve;
            std::vector<int> vk;
            if (rooms_at.count(via->second))
                for (const auto &p : rooms_at[via->second]) {
                    if (!ce(p).empty()) ve.push_back(ce(p));
                    if (cell_keys.count(p)) vk.push_back(cell_keys[p]);
                }
            if (!ve.empty()) {
                std::string vj;
                for (size_t i = 0; i < ve.size(); i++) vj += (i ? " | (" : "(") + ve[i] + ")";
                for (std::string &e : reach) e = "(" + e + ") & (" + vj + ")";
            } else {
                reach.clear();
            }
            if (!ck.empty() && !vk.empty())
                ck = {std::max(*std::min_element(ck.begin(), ck.end()), *std::min_element(vk.begin(), vk.end()))};
        }
        if (!ck.empty()) {
            int k = *std::min_element(ck.begin(), ck.end());
            const std::pair<std::string, std::string> *p0 = nullptr;
            for (const auto &p : places)
                if (cell_keys.count(p) && cell_keys[p] == k) { p0 = &p; break; }
            if (rr && !rr->blocks.empty() && p0 && kcs.count(p0->first)) {
                const auto &kc0 = kcs[p0->first].first;
                int n0 = kcs[p0->first].second;
                for (const Value &b : rr->blocks) k = std::max(k, kc_get(kc0, "LOCK|" + std::to_string(n0) + "|block" + py_str(b), 1));
            }
            check_keys[loc] = k;
        }
        if (reach.empty()) {
            bool at_home = false;
            for (const auto &p : places) at_home = at_home || (p.second == short_cell(room) && p.first == level_of(room));
            if (at_home) {
                fallback++;
                continue;
            }
            excluded.add(loc), absent.add(loc);
            unused++;
            continue;
        }
        if (v.has("room-local")) {
            Value rl = v.at("room-local");
            cond = rl.is_str() ? rl.s() : "";
        }
        NodeP local = strip(parse(cond), drop).first;
        std::string lvl_now = places[0].first;
        for (const auto &p : places)
            if (LEVELS.at(p.first).first == home) { lvl_now = p.first; break; }
        if (cond.find("hit-crystal-switch") != std::string::npos && !crystal[lvl_now]) {
            excluded.add(loc), absent.add(loc);
            unused++;
            continue;
        }
        std::string r;
        if (reach.size() == 1) r = reach[0];
        else
            for (size_t i = 0; i < reach.size(); i++) r += (i ? " | (" : "(") + reach[i] + ")";
        v.set("condition-basic", local ? "(" + r + ") & (" + show(local) + ")" : r);
        for (const char *k : {"condition-advanced", "condition-glitched", "condition-hell"}) v.dict().erase(k);
        dungeon_of.set(loc, "D" + std::to_string(LEVELS.at(lvl_now).first));
        rewritten++;
    }
    for (auto &e : logic_defs.dict()) {
        if (e.value.get("type") != Value("item") || dungeon_of.has(e.key) || excluded.contains(e.key)) continue;
        Value cv = e.value.get("condition-basic", "");
        std::string cond = cv.is_str() ? cv.s() : "";
        if (!names_shuffled(cond)) continue;
        bool gone;
        std::string c2 = remap_cells(cond, gone);
        if (gone) {
            excluded.add(e.key), absent.add(e.key);
            unused++;
            continue;
        }
        e.value.set("condition-basic", c2);
        for (const char *k : {"condition-advanced", "condition-glitched", "condition-hell"}) e.value.dict().erase(k);
        rewritten++;
    }
    if (settings.get("keysanity", "off") == Value("off")) {
        std::map<std::string, std::vector<int>> need_k;
        for (const auto &e : dungeon_of.dict()) {
            Value v = logic_defs.get(e.key, Value::new_dict());
            if (excluded.contains(e.key) || v.get("type") != Value("item") || !check_keys.count(e.key) ||
                vanilla_check(e.key, v, settings))
                continue;
            need_k[e.value.s()].push_back(check_keys[e.key]);
        }
        auto rcl = roomless_checks(settings, [&](const std::string &k) { return excluded.contains(k); });
        for (const std::string &level : kcs_order) {
            int nn = kcs[level].second, total = lock_count[nn];
            std::vector<int> ks = need_k["D" + std::to_string(nn)];
            for (int i = 0; i < (rcl.count(nn) ? rcl[nn] : 0); i++) ks.push_back(total);
            std::sort(ks.begin(), ks.end());
            bool broke = false;
            for (int j = 1; j <= total; j++) {
                int c = 0;
                for (int k : ks) c += k <= j - 1;
                if (c < j) {
                    settings.set("room-logic-unfillable", level + ": key " + std::to_string(j) + " of " + std::to_string(total) +
                                                              " has " + std::to_string(c) + " checks to go in");
                    broke = true;
                    break;
                }
            }
            if (!broke && (int)ks.size() < total + dungeon_items(settings))
                settings.set("room-logic-unfillable", level + ": " + std::to_string(ks.size()) + " checks for " +
                                                          std::to_string(total + dungeon_items(settings)) + " dungeon items");
        }
    }
    std::vector<std::string> lost;
    static const std::regex inst_re("^D\\d-instrument$");
    for (const std::string &k : excluded.items()) {
        std::smatch m;
        if (std::regex_search(k, m, inst_re)) lost.push_back(k);
    }
    std::sort(lost.begin(), lost.end());
    if (!lost.empty() && !settings.get("room-logic-unfillable").truthy()) {
        std::string j = "unreachable ";
        for (size_t i = 0; i < lost.size(); i++) j += (i ? ", " : "") + lost[i];
        settings.set("room-logic-unfillable", j);
    }
    bool tower_in = false;
    for (const auto &l : layouts) tower_in = tower_in || l.first == TOWER;
    std::string tower = tower_in ? ce({TOWER, "03F"}) : "";
    if (!tower.empty()) {
        std::string roof = "(" + tower + ") & nightmare-key-D7 & (hookshot | (rooster & bad-pets))";
        for (const char *k : {"D7-instrument", "D7-eagle"})
            if (logic_defs.has(k)) {
                Value v = logic_defs.at(k);
                v.set("condition-basic", roof + " & hookshot & kill-evil-eagle");
                for (const char *x : {"condition-advanced", "condition-glitched", "condition-hell"}) v.dict().erase(x);
            }
    }
    std::string locks = "{";
    bool first = true;
    for (const auto &kv : lock_count) {
        locks += (first ? "" : ", ") + std::to_string(kv.first) + ": " + std::to_string(kv.second);
        first = false;
    }
    locks += "}";
    return std::to_string(layouts.size()) + " dungeons, " + std::to_string(rewritten) + " checks rewritten, " +
           std::to_string(unused) + " in unused rooms (excluded), " + std::to_string(fallback) +
           " without a room (vanilla condition), locks " + locks;
}

}  // namespace solve
}  // namespace rando
