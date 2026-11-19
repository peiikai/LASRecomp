// The entrance shuffle, natively (entrances.h): tools/rando/las_headless.py's entrance section and
// tools/rando/entrance_logic.py, function for function, with every random draw the Python's.
//
// Python behaviour kept because the seeds depend on it:
//   - the doors' layout is re-rolled while reach_all() loses a location; reach_all is entrance_logic's own reachability
//     (not LAS's check_access): name:0 always holds, an item / follower needs its region's count to be non-zero, a
//     condition is eval()ed after turning '!' into ' not ' - a condition starting with '!' (or a space) is an
//     IndentationError there, which a non-enemy node takes as "not reached" and an enemy node raises;
//   - lv_door maps a level to the LAST house door leading into it (dict comprehension), so pots follow that door;
//   - logic_defs is rebuilt from a deep copy per try and replaces the caller's dict in place (clear + update).
#include "entrances.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../fmt/leb.h"
#include "logic.h"

#include "../pyrandom.h"

namespace rando {
namespace solve {

namespace {

using Log = std::function<void(const std::string &)>;

// ---- Python helpers

Value new_dict() { return Value::new_dict(); }

// str(x)
std::string py_str(const Value &v) { return v.is_str() ? v.s() : v.repr(); }

// d.get(key) for a dict Value; a key that isn't a str is never in the solver's dicts
Value get(const Value &d, const char *key) { return d.get(key); }

[[noreturn]] void raise(const char *type, const std::string &msg) { throw PyError(type, msg); }

// entrance_logic._and: the truthy parts, each in parentheses, joined by ' & '; None when none
Value and_(std::initializer_list<Value> parts) {
    std::string s;
    bool any = false;
    for (const Value &p : parts) {
        if (!p.truthy()) continue;
        if (any) s += " & ";
        s += "(" + py_str(p) + ")";
        any = true;
    }
    return any ? Value(s) : Value();
}

// x in d, with a key that may be None (never in a str-keyed dict)
bool in(const Value &d, const Value &key) { return key.is_str() && d.has(key.s()); }

// d[key] (KeyError), the key a Value
Value &at(Value &d, const Value &key) {
    if (!key.is_str()) raise("KeyError", key.repr());
    return d.at(key.s());
}

void pop(Value &d, const char *key) { d.dict().erase(key); }

void pop_levels(Value &node, bool trick) {
    pop(node, "condition-advanced"), pop(node, "condition-glitched"), pop(node, "condition-hell");
    if (trick) pop(node, "condition-trick");
}

Value region_node(Value cond) {
    Value n = new_dict();
    n.set("type", "region");
    n.set("condition-basic", std::move(cond));
    return n;
}

// int(s, 0)
int64_t int_base0(const std::string &t) {
    size_t a = 0, b = t.size();
    while (a < b && isspace((unsigned char)t[a])) a++;
    while (b > a && isspace((unsigned char)t[b - 1])) b--;
    bool neg = false;
    if (a < b && (t[a] == '+' || t[a] == '-')) neg = t[a++] == '-';
    int base = 10;
    if (b - a > 2 && t[a] == '0' && strchr("xXoObB", t[a + 1])) {
        base = strchr("xX", t[a + 1]) ? 16 : strchr("oO", t[a + 1]) ? 8 : 2;
        a += 2;
        if (a < b && t[a] == '_') a++;
    }
    if (a >= b) raise("ValueError", "invalid literal for int() with base 0: " + py_repr(t));
    if (base == 10 && t[a] == '0' && t.find_first_not_of("0_", a) < b)
        raise("ValueError", "invalid literal for int() with base 0: " + py_repr(t));
    int64_t v = 0;
    for (size_t i = a; i < b; i++) {
        char c = t[i];
        if (c == '_' && i > a && i + 1 < b && t[i + 1] != '_') continue;
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'z' ? c - 'a' + 10 : c >= 'A' && c <= 'Z' ? c - 'A' + 10 : 99;
        if (d >= base) raise("ValueError", "invalid literal for int() with base 0: " + py_repr(t));
        v = v * base + d;
    }
    return neg ? -v : v;
}

// Python's float // (float_floor_div)
double py_floordiv(double vx, double wx) {
    double mod = fmod(vx, wx), div = (vx - mod) / wx;
    if (mod != 0) {
        if ((wx < 0) != (mod < 0)) div -= 1.0;
    }
    double fl;
    if (div != 0) {
        fl = floor(div);
        if (div - fl > 0.5) fl += 1.0;
    } else fl = copysign(0.0, vx / wx);
    return fl;
}

// ---- the tables (entrances_data.cpp)

const Value &tables() {
    static const Value T = [] {
        std::string s = entrance_tables_json();
        return Value::parse_json(s.data(), s.size());
    }();
    return T;
}

// ---- entrance_logic.py

struct Ctx {
    const Value &T;
    Value ed;  // entrance_data.yml (this run's copy: its door dicts go into 'ent')
    Value dungeon_doors, pockets, pocket_tiles;
    std::string raft_dock, d6_entry, d6_rest;
};

// pool(ed, closed)
List pool(Ctx &C, const std::vector<std::string> &closed) {
    List out;
    std::set<std::string> keep;
    for (const Value &k : C.T.at("keep").list()) keep.insert(k.s());
    for (const Value &d : C.ed.at("doors").list()) {
        Value outer = get(d, "outer_region");
        if (d.at("type") != Value("single") || (d.at("id").is_str() && keep.count(d.at("id").s())) ||
            (outer.is_str() && std::find(closed.begin(), closed.end(), outer.s()) != closed.end()))
            continue;
        Value rec = get(d, "shuffle_recommended");
        if (get(d, "water").truthy() || get(d, "pocket").truthy() || get(d, "outer_extra").truthy() ||
            (rec.is_bool() && !rec.b()))
            continue;
        out.push_back(d);
    }
    return out;
}

// dungeon_pool(ed): the nine front doors in DUNGEON_DOORS order
List dungeon_pool(Ctx &C) {
    Value by_id = new_dict();
    for (const Value &d : C.ed.at("doors").list()) by_id.set(d.at("id").s(), d);
    List out;
    for (const auto &e : C.dungeon_doors.dict()) out.push_back(by_id.at(e.key));
    return out;
}

// shuffle(doors, decoupled, rng, coupled) -> (entry, exit)
std::pair<Value, Value> shuffle(const List &doors, bool decoupled, PyRandom &rng, const Value *coupled) {
    std::vector<std::string> ids;
    for (const Value &d : doors) ids.push_back(d.at("id").s());
    std::vector<std::string> targets = ids;
    rng.shuffle(targets);
    Value entry = new_dict(), exit_ = new_dict();
    for (size_t i = 0; i < ids.size(); i++) entry.set(ids[i], targets[i]);
    auto is_coupled = [&](const std::string &d) { return coupled && coupled->has(d); };
    if (decoupled) {
        Value fixed = new_dict();
        for (const std::string &d : ids)
            if (is_coupled(d)) fixed.set(entry.at(d).s(), d);
        std::vector<std::string> outs, rest;
        for (const std::string &d : ids)
            if (!is_coupled(d)) outs.push_back(d);
        rng.shuffle(outs);
        for (const std::string &t : ids)
            if (!fixed.has(t)) rest.push_back(t);
        for (size_t i = 0; i < rest.size() && i < outs.size(); i++) exit_.set(rest[i], outs[i]);
        for (const auto &e : fixed.dict()) exit_.set(e.key, e.value);
    } else {
        for (const auto &e : entry.dict()) exit_.set(e.value.s(), e.key);
    }
    return {entry, exit_};
}

// _or_into(node, extra)
void or_into(Value &node, const Value &extra) {
    Value c = get(node, "condition-basic");
    if (c.is_none()) return;
    node.set("condition-basic", "(" + py_str(c) + ") | (" + py_str(extra) + ")");
}

Value outer_of(Ctx &C, const Value &d) {
    return d.at("id") == Value("raft-shop") ? Value(C.raft_dock) : d.at("outer_region");
}

std::string dungeon_node(Ctx &C, const std::string &t) {
    return t == "d6-face-shrine" ? C.d6_entry : C.dungeon_doors.at(t).s();
}

Value pocket_name(const Value &d) {
    Value p = get(d, "pocket");
    return p.truthy() ? get(p, "name") : Value();
}

// _move(logic, location, region, cond)
int move(Value &logic, const Value &location, const std::string &region, const Value &cond) {
    if (!in(logic, location)) return 0;
    Value loc = at(logic, location);
    if (!loc.truthy()) return 0;
    loc.set("region", region);
    pop_levels(loc, false);  // basic-only is never looser
    if (cond.truthy()) loc.set("condition-basic", py_str(cond));
    else pop(loc, "condition-basic");
    return 1;
}

// extra_inside lists: [[location, cond], ...]
using Extra = Value;  // {key: [[location, cond]]}

// apply_logic(logic, doors, entry, exit_, extra_inside)
int apply_logic(Ctx &C, Value &logic, const List &doors, const Value &entry, const Value &exit_, const Extra &extra) {
    Value by_id = new_dict();
    for (const Value &d : doors) by_id.set(d.at("id").s(), d);
    if (by_id.has("raft-shop")) {
        for (const Value &p : C.T.at("raft_patches").list()) {
            const List &pl = p.list();
            Value node = logic.at(pl[0].s());
            if (get(node, "condition-basic") != pl[1])
                raise("AssertionError", "(" + pl[0].repr() + ", " + get(node, "condition-basic").repr() + ")");
            if (pl[0].s() == "rapids") pop_levels(node, true);
            node.set("condition-basic", pl[2]);
        }
        logic.set(C.raft_dock, region_node("(heights & hookshot) | rapids"));
    }
    for (const Value &d : doors) {
        // _doorstep(logic, d)
        Value p = pocket_name(d), cond;
        if (in(C.pockets, p) && in(logic, C.pockets.at(p.s()).list()[0]))
            cond = and_({C.pockets.at(p.s()).list()[0], get(d, "door_req")});
        else
            cond = and_({outer_of(C, d), get(d, "outer_extra"), get(d, "door_req")});
        logic.set("door-" + py_str(d.at("id")), region_node(cond));
    }
    Value into = new_dict();
    for (const auto &e : entry.dict()) {
        const std::string &t = e.value.s();
        if (!into.has(t)) into.set(t, Value::new_list());
        into.at(t).list().push_back(e.key);
    }
    for (const auto &e : into.dict()) {
        const std::string &t = e.key;
        std::string ways;
        for (const Value &d : e.value.list()) ways += (ways.empty() ? "door-" : " | door-") + d.s();
        if (C.dungeon_doors.has(t)) {  // (a dungeon: its own node, its locations stay where they are)
            Value node = logic.at(dungeon_node(C, t));
            pop_levels(node, true);
            if (t == "d6-face-shrine") {
                bool ok = false;
                for (const Value &f : C.T.at("d6_front").list())
                    if (node.at("condition-basic") == Value(f.s() + " | " + C.d6_rest)) ok = true;
                if (!ok) raise("AssertionError", node.at("condition-basic").repr());
                ways = "(" + ways + ") | " + C.d6_rest;
            }
            node.set("condition-basic", ways);
            continue;
        }
        logic.set("inside-" + t, region_node(ways));
    }
    int moved = 0;
    for (const auto &e : into.dict()) {
        const std::string &t = e.key;
        if (C.dungeon_doors.has(t)) continue;
        Value door = by_id.at(t);
        List its;
        Value inside = get(door, "inside");
        if (inside.truthy())
            for (const Value &it : inside.list()) its.push_back(it);
        if (extra.has(t))
            for (const Value &x : extra.at(t).list()) {
                Value it = new_dict();
                it.set("location", x.list()[0]);
                it.set("cond", x.list()[1]);
                its.push_back(it);
            }
        for (const Value &it : its) {
            Value l = it.at("location");
            if (!in(logic, l)) continue;
            Value loc = at(logic, l);
            if (!loc.truthy()) continue;
            loc.set("region", "inside-" + t);
            pop_levels(loc, false);  // those were written against vanilla outer access; basic-only is never looser
            Value c = get(it, "cond");
            if (c.truthy()) loc.set("condition-basic", py_str(c));
            else pop(loc, "condition-basic");
            moved++;
        }
        Value fos = get(door, "followers");
        if (fos.truthy())
            for (const Value &fo : fos.list()) {  // (BowWow, the Rooster: freed inside, they follow from there)
                Value f = fo.at("follower");
                if (!in(logic, f)) continue;
                Value loc = at(logic, f);
                if (!loc.truthy()) continue;
                loc.set("region", "inside-" + t);
                pop_levels(loc, false);
                loc.set("condition-basic", py_str(fo.at("cond")));
                moved++;
            }
    }
    for (const Value &tok : C.T.at("inside_tokens").list()) {
        const List &tl = tok.list();
        const std::string &loc = tl[0].s(), &token = tl[1].s(), &door = tl[2].s();
        if (logic.has(loc) && by_id.has(door) && logic.has("inside-" + door)) {
            Value c = logic.at(loc).get("condition-basic", "");
            if (!c.is_str()) raise("AttributeError", "object has no attribute 'split'");
            std::string out, w;
            const std::string &s = c.s();
            bool first = true;
            for (size_t i = 0; i <= s.size(); i++) {
                if (i == s.size() || isspace((unsigned char)s[i])) {
                    if (!w.empty()) {
                        out += (first ? "" : " ") + (w == token ? "inside-" + door : w);
                        first = false;
                        w.clear();
                    }
                } else w += s[i];
            }
            logic.at(loc).set("condition-basic", out);
        }
    }
    // leaving interior I through door c puts Link outside c, past what blocks that doorstep (exit_req)
    for (const auto &e : exit_.dict()) {
        const std::string &t = e.key, &c = e.value.s();
        if (C.dungeon_doors.has(c)) continue;
        Value dc = by_id.at(c);
        Value region = outer_of(C, dc);
        std::string inside = C.dungeon_doors.has(t) ? dungeon_node(C, t) : "inside-" + t;
        if (in(logic, region) && logic.has(inside)) or_into(at(logic, region), and_({inside, get(dc, "exit_req")}));
    }
    return moved;
}

// dungeon_ways(doors, entry, exit_)
std::pair<Value, Value> dungeon_ways(Ctx &C, const List &doors, const Value &entry, const Value &exit_) {
    Value by_id = new_dict();
    for (const Value &d : doors) by_id.set(d.at("id").s(), d);
    Value ways = new_dict(), exits = new_dict();
    for (const auto &e : entry.dict()) {
        const std::string &t = e.value.s();
        if (C.dungeon_doors.has(t))
            ways.set(C.dungeon_doors.has(e.key) ? C.dungeon_doors.at(e.key).s() : e.key, C.dungeon_doors.at(t));
    }
    for (const auto &e : exit_.dict())
        if (C.dungeon_doors.has(e.key))
            exits.set(C.dungeon_doors.at(e.key).s(), by_id.at(e.value.s()).at("return_zone").at("target"));
    return {ways, exits};
}

// connector_pool(ed)
List connector_pool(Ctx &C) {
    Value by_id = new_dict();
    for (const Value &d : C.ed.at("doors").list()) by_id.set(d.at("id").s(), d);
    List out;
    for (const Value &name : C.T.at("connectors").list()) {
        Value c = C.ed.at("connectors").at(name.s());
        if (get(c, "special").truthy()) raise("AssertionError", name.s());
        for (const Value &did : c.at("doors").list()) {
            Value d = by_id.at(did.s()).copy();
            d.set("connector", name);
            d.set("node", c.at("door_nodes").at(did.s()));
            Value p = pocket_name(d);
            d.set("pocket_region", p.truthy() ? at(C.pockets, p).list()[0] : Value());
            out.push_back(d);
        }
    }
    return out;
}

std::string node_region(const Value &d) { return "conn-" + py_str(d.at("connector")) + "-" + py_str(d.at("node")); }

// pocket_of(cdoors): pocket_screens + POCKET_TILES
struct PocketOf {
    std::map<std::string, Value> screens;
    std::map<std::pair<std::string, int64_t>, Value> tiles;
    Value operator()(const std::string &room, const Value &tile) const {
        if (tile.is_int()) {
            auto it = tiles.find({room, tile.i()});
            if (it != tiles.end() && it->second.truthy()) return it->second;
        }
        auto s = screens.find(room);
        return s == screens.end() ? Value() : s->second;
    }
};

PocketOf pocket_of(Ctx &C, const List &cdoors) {
    PocketOf P;
    for (const Value &d : cdoors) {
        if (!d.at("pocket_region").truthy()) continue;
        Value own = C.pocket_tiles.get(py_str(d.at("pocket").at("name")), new_dict());
        Value gs = get(d, "gb_screen");
        int64_t g = int_base0(gs.truthy() ? py_str(gs) : "-1");
        std::vector<std::string> rooms{py_str(d.at("field_room"))};
        if (g >= 0) {
            char b[32];
            snprintf(b, sizeof b, "Field_%02lld%c", (long long)(g / 16 + 1), (char)(65 + g % 16));
            rooms.push_back(b);
        }
        for (const std::string &room : rooms)
            if (!own.has(room)) P.screens[room] = d.at("pocket_region");
    }
    for (const Value &d : cdoors) {
        Value p = pocket_name(d);
        if (!in(C.pocket_tiles, p)) continue;
        for (const auto &e : C.pocket_tiles.at(p.s()).dict())
            for (const Value &t : e.value.list()) P.tiles[{e.key, t.i()}] = d.at("pocket_region");
    }
    return P;
}

// connector_levels(cdoors): level -> the one node region of a cave level
Value connector_levels(const List &cdoors) {
    Value nodes = new_dict();  // level -> [distinct node regions]
    for (const Value &d : cdoors) {
        std::string lv = py_str(d.at("dest_level")), n = node_region(d);
        if (!nodes.has(lv)) nodes.set(lv, Value::new_list());
        List &l = nodes.at(lv).list();
        if (std::find(l.begin(), l.end(), Value(n)) == l.end()) l.push_back(n);
    }
    Value out = new_dict();
    for (const auto &e : nodes.dict())
        if (e.value.list().size() == 1) out.set(e.key, e.value.list()[0]);
    return out;
}

// apply_connector_logic(logic, ed, cdoors, entry, exit_, extra_inside)
int apply_connector_logic(Ctx &C, Value &logic, const List &cdoors, const Value &entry, const Value &exit_,
                          const Extra &extra) {
    Value by_id = new_dict();
    for (const Value &d : cdoors) by_id.set(d.at("id").s(), d);
    std::vector<std::string> names;
    for (const Value &d : cdoors) names.push_back(d.at("connector").s());
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    Value terms = new_dict();  // region -> extra OR terms
    std::set<std::string> built;  // regions whose condition is only those terms
    auto add = [&](const Value &region, const Value &term) {
        if (!region.is_str()) raise("AttributeError", "'NoneType' object has no attribute 'startswith'");
        const std::string &r = region.s();
        if (!terms.has(r)) terms.set(r, Value::new_list());
        terms.at(r).list().push_back(term);
        if (r.compare(0, 5, "conn-") == 0 || r.compare(0, 7, "pocket-") == 0) built.insert(r);
    };
    const Value &patches = C.T.at("region_patches");
    for (const std::string &name : names) {
        if (!patches.has(name)) continue;
        for (const Value &p : patches.at(name).list()) {
            const List &pl = p.list();
            Value node = logic.at(pl[0].s());
            if (get(node, "condition-basic") != pl[1])
                raise("AssertionError", "(" + pl[0].repr() + ", " + get(node, "condition-basic").repr() + ")");
            pop_levels(node, true);
            if (pl[2].is_none()) {
                if (!node.has("condition-basic")) raise("KeyError", "'condition-basic'");
                pop(node, "condition-basic");
                built.insert(pl[0].s());
            } else node.set("condition-basic", pl[2]);
        }
    }
    for (const Value &d : cdoors) {
        Value outer = d.at("pocket_region");
        if (!outer.truthy()) outer = and_({d.at("outer_region"), get(d, "outer_extra")});
        logic.set("door-" + py_str(d.at("id")), region_node(and_({outer, get(d, "door_req")})));
    }
    for (const auto &e : entry.dict())  // door d leads into the cave spot of door t
        add(node_region(by_id.at(e.value.s())), "door-" + e.key);
    int moved = 0;
    for (const std::string &name : names) {
        Value c = C.ed.at("connectors").at(name);
        for (const Value &ed : c.at("edges").list()) {
            std::vector<std::pair<Value, Value>> pairs{{ed.at("from"), ed.at("to")}};
            if (get(ed, "bidirectional").truthy()) pairs.push_back({ed.at("to"), ed.at("from")});
            for (const auto &ab : pairs) {
                std::string src = "conn-" + name + "-" + py_str(ab.first);
                Value cond = get(ed, "cond");
                add("conn-" + name + "-" + py_str(ab.second), cond.truthy() ? and_({src, cond}) : Value(src));
            }
        }
        for (const Value &it : c.at("items").list())
            moved += move(logic, it.at("location"), "conn-" + name + "-" + py_str(it.at("node")), get(it, "cond"));
    }
    for (const auto &e : extra.dict())
        for (const Value &x : e.value.list()) moved += move(logic, x.list()[0], e.key, x.list()[1]);
    // pockets: their outdoor locations, outdoor ways in, drops
    for (const Value &d : cdoors) {
        Value p = get(d, "pocket");
        if (!p.truthy()) continue;
        const List &pk = at(C.pockets, p.at("name")).list();
        const std::string &region = pk[0].s();
        if (!logic.has(region)) {
            Value n = new_dict();
            n.set("type", "region");
            logic.set(region, n);
            if (pk[1].truthy()) add(region, pk[1]);
            if (pk[2].truthy()) add(pk[2].list()[0], and_({region, pk[2].list()[1]}));
        }
        Value its = get(p, "items");
        if (its.truthy())
            for (const Value &it : its.list()) {
                Value l = it.at("location");
                Value cur = in(logic, l) ? get(at(logic, l), "region") : Value();
                if (cur != Value(region)) moved += move(logic, l, region, get(it, "cond"));
            }
    }
    // leaving the cave through the zone of door t puts Link outside door exit_[t]
    for (const auto &e : exit_.dict()) {
        Value dc = by_id.at(e.value.s());
        std::string from = node_region(by_id.at(e.key));
        if (dc.at("pocket_region").truthy()) add(dc.at("pocket_region"), and_({from, get(dc, "exit_req")}));
        else add(dc.at("outer_region"), and_({from, get(dc, "outer_extra"), get(dc, "exit_req")}));
    }
    for (const auto &e : terms.dict()) {
        if (!logic.has(e.key)) logic.set(e.key, region_node(Value())), pop(logic.at(e.key), "condition-basic");
        Value nd = logic.at(e.key);
        Value c = get(nd, "condition-basic");
        if (c.is_none() && !built.count(e.key)) continue;  // a LAS region without a condition: always reachable
        std::string s;
        bool first = true;
        if (c.truthy()) s = "(" + py_str(c) + ")", first = false;
        for (const Value &x : e.value.list()) s += (first ? "(" : " | (") + py_str(x) + ")", first = false;
        nd.set("condition-basic", s);
    }
    return moved;
}

// hookshot_logic.apply(logic_defs)
void hookshot_apply(Ctx &C, Value &logic) {
    for (const auto &e : C.T.at("hookshot_routes").dict()) {
        if (!logic.has(e.key)) continue;
        Value node = logic.at(e.key);
        if (!node.truthy() || !node.has("condition-basic") || node.at("condition-basic").contains(e.value)) continue;
        node.set("condition-basic", "(" + py_str(node.at("condition-basic")) + ") | (" + py_str(e.value) + ")");
    }
}

// ---- reach_all(logic, item_defs, settings): entrance_logic's own reachability (playthrough.spheres, no placements)

class Reach {
public:
    Names names;
    Conditions conds;

    // ev(cond, acc): the condition as entrance_logic.reach_all evaluates it
    bool ev(const Value &cond, const Access &acc) {
        if (!cond.is_str()) raise("TypeError", "expected string or bytes-like object, got " + cond.repr());
        auto it = cache_.find(cond.s());
        int c = it != cache_.end() ? it->second : (cache_[cond.s()] = compile(cond.s()));
        if (c == INDENT) raise("IndentationError", "unexpected indent");
        return conds.eval(c, acc);
    }

private:
    enum { INDENT = -2 };
    // reach_all writes name[:N] as h("name", N) (acc.get(name, 0) >= N), '|' ' or ', '&' ' and ', '!' ' not '.
    // Conditions reads LAS's translation ('or', 'and', 'not '; hasAccess wants the name present): the same once
    // the operators have spaces round them and a name:0 (always true here) is the always-present name 'true'.
    int compile(const std::string &s) {
        if (!s.empty() && (s[0] == ' ' || s[0] == '\t' || s[0] == '!')) return INDENT;  // (leading whitespace)
        std::string t;
        for (size_t i = 0; i < s.size();) {
            char ch = s[i];
            bool namec = isalnum((unsigned char)ch) || ch == '-' || ch == '[' || ch == ']';
            if (namec) {
                size_t a = i;
                while (i < s.size() && (isalnum((unsigned char)s[i]) || s[i] == '-' || s[i] == '[' || s[i] == ']')) i++;
                size_t e = i;
                if (i + 1 < s.size() && s[i] == ':' && isdigit((unsigned char)s[i + 1])) {
                    size_t d = ++i;
                    while (i < s.size() && isdigit((unsigned char)s[i])) i++;
                    if (s.find_first_not_of('0', d) >= i) {  // (name:0, name:00: h(name, 0))
                        t += "true";
                        continue;
                    }
                    e = i;
                }
                t.append(s, a, e - a);
                continue;
            }
            if (ch == '|') t += " | ";
            else if (ch == '&') t += " & ";
            else if (ch == '!') t += " !";
            else t += ch;
            i++;
        }
        return conds.compile(t, names);
    }
    std::unordered_map<std::string, int> cache_;
};

// the set of reached nodes, as their logic order positions
std::vector<uint8_t> reach_all(Reach &R, const Value &logic, const Value &item_defs, const Value &settings) {
    Names &N = R.names;
    Access acc;
    for (const auto &e : settings.dict())
        if (e.value.is_bool() && e.value.b()) acc.set(N.id(e.key), 1);
    for (const auto &e : item_defs.dict()) {
        Value q = get(e.value, "quantity");
        if (q.truthy()) acc.set(N.id(e.key), (int32_t)q.to_int());
    }
    acc.set(N.id("bow-wow"), 1), acc.set(N.id("rooster"), 1);
    acc.set(N.id("true"), 1);

    struct Node {
        std::string key;
        Value v;
        int id, pit = -1, heavy = -1;
        bool enemy;
    };
    std::vector<Node> nodes;
    for (const auto &e : logic.dict()) {
        Node n{e.key, e.value, N.id(e.key), -1, -1, get(e.value, "type") == Value("enemy")};
        if (n.enemy) n.pit = N.id(e.key + "[pit]"), n.heavy = N.id(e.key + "[heavy]");
        nodes.push_back(std::move(n));
    }
    // check(key, acc)
    auto check = [&](const std::string &key, const Access &a) -> bool {
        size_t lb = key.rfind('[');
        if (key.size() >= 3 && key.back() == ']' && lb != std::string::npos && lb >= 1 && lb + 2 < key.size()) {
            bool word = true;
            for (size_t i = lb + 1; i + 1 < key.size(); i++)
                if (!(isalnum((unsigned char)key[i]) || key[i] == '_')) word = false;
            if (word) {  // (.+)\[(\w+)\]$
                const Value &node = logic.at(key.substr(0, lb));
                Value cb = get(node, "condition-basic");
                if (R.ev(cb.truthy() ? cb : Value("true"), a)) return true;
                return R.ev(node.at("condition-" + key.substr(lb + 1, key.size() - lb - 2)), a);
            }
        }
        const Value &node = logic.at(key);
        Value type = get(node, "type");
        if (type == Value("item") || type == Value("follower")) {
            Value region = get(node, "region");
            int rid = region.is_str() ? N.find(region.s()) : -1;
            if (rid < 0 || a.count(rid) == 0) return false;
        }
        if (!node.has("condition-basic") || node.at("condition-basic").is_none()) return true;
        return R.ev(node.at("condition-basic"), a);
    };
    std::vector<uint8_t> seen(nodes.size(), 0);
    bool grew = true;
    while (grew) {  // (placements are empty: the first sphere is everything)
        grew = false;
        for (size_t i = 0; i < nodes.size(); i++) {
            if (seen[i] || nodes[i].enemy) continue;
            bool ok;
            try {
                ok = check(nodes[i].key, acc);
            } catch (const PyError &) {
                ok = false;
            }
            if (!ok) continue;
            seen[i] = 1;
            acc.add(nodes[i].id);
            grew = true;
        }
        for (size_t i = 0; i < nodes.size(); i++) {  // enemies, as LAS does: killable -> also [pit] / [heavy]
            Node &n = nodes[i];
            if (!n.enemy || seen[i]) continue;
            if (check(n.key, acc)) {
                seen[i] = 1;
                acc.set(n.id, 1), acc.set(n.pit, 1), acc.set(n.heavy, 1);
                grew = true;
                continue;
            }
            for (int v = 0; v < 2; v++) {
                const char *param = v ? "heavy" : "pit";
                int k = v ? n.heavy : n.pit;
                if (n.v.has(std::string("condition-") + param) && acc.count(k) == 0 &&
                    check(n.key + "[" + param + "]", acc))
                    acc.set(k, 1), grew = true;
            }
        }
    }
    return seen;
}

// items(lg): the reached item locations
std::set<std::string> reach_items(Reach &R, const Value &logic, const Value &item_defs, const Value &settings) {
    std::vector<uint8_t> seen = reach_all(R, logic, item_defs, settings);
    std::set<std::string> out;
    size_t i = 0;
    for (const auto &e : logic.dict()) {
        if (seen[i++] && get(e.value, "type") == Value("item")) out.insert(e.key);
    }
    return out;
}

std::vector<std::string> minus(const std::set<std::string> &a, const std::set<std::string> &b) {
    std::vector<std::string> out;
    for (const std::string &x : a)
        if (!b.count(x)) out.push_back(x);
    return out;  // (sorted)
}

std::string first6(const std::vector<std::string> &v) {
    std::string s;
    for (size_t i = 0; i < v.size() && i < 6; i++) s += (i ? ", " : "") + v[i];
    return s;
}

// ---- the Field checks a pocket can hold (bushes, digs, rocks, apple trees), from their location names

struct FieldCheck {
    std::string name, room;
    Value tile;
};

bool match_room_n(const std::string &name, const char *prefix, std::string &room, int64_t &n) {
    // <prefix>-<NN><A-P>-<digits>
    size_t pl = strlen(prefix);
    if (name.compare(0, pl, prefix) != 0 || name.size() < pl + 6 || name[pl] != '-') return false;
    const char *r = name.c_str() + pl + 1;
    if (!isdigit((unsigned char)r[0]) || !isdigit((unsigned char)r[1]) || r[2] < 'A' || r[2] > 'P' || r[3] != '-')
        return false;
    const char *d = r + 4;
    if (!*d) return false;
    for (const char *q = d; *q; q++)
        if (!isdigit((unsigned char)*q)) return false;
    room = std::string("Field_") + std::string(r, 3);
    n = atoll(d);
    return true;
}

class FieldTiles {
public:
    explicit FieldTiles(const GameRead &read) : read_(read) {}
    // bushes._tile(room, the n-th actor of that type in the room)
    Value tile(const std::string &room, int type, int64_t n) {
        const LebRoom &r = load(room);
        int64_t k = 0;
        for (const LebActor &a : r.actors) {
            if (a.type != type) continue;
            if (k++ != n) continue;
            int row = atoi(room.substr(6, 2).c_str()), col = room[8] - 'A';
            int64_t tx = (int64_t)py_floordiv(a.pos[0] - col * 15.0, 1.5);
            int64_t tz = (int64_t)py_floordiv(a.pos[2] - (row - 1) * 12.0, 1.5);
            return 0 <= tx && tx < 10 && 0 <= tz && tz < 8 ? Value((long long)(tz * 10 + tx)) : Value();
        }
        raise("RuntimeError", room + ": no actor " + std::to_string(n) + " of type " + std::to_string(type));
    }

private:
    const LebRoom &load(const std::string &room) {
        auto it = rooms_.find(room);
        if (it != rooms_.end()) return it->second;
        std::string path = "region_common/level/Field/" + room + ".leb", bytes;
        if (!read_ || !read_(path, bytes)) raise("FileNotFoundError", path);
        return rooms_[room] = LebRoom::parse(bytes);
    }
    const GameRead &read_;
    std::map<std::string, LebRoom> rooms_;
};

}  // namespace

Value shuffle_entrances(Value state, const GameRead &read, const Log &log) {
    auto say = [&](const std::string &s) {
        if (log) log(s);
    };
    Ctx C{tables(), Value(), Value(), Value(), Value(), "", "", ""};
    const Value &T = C.T;
    Value settings = state.at("settings"), item_defs = state.at("item_defs"), logic_defs = state.at("logic_defs");

    int64_t tier = std::min<int64_t>(settings.get("entrance-tier", 0).to_int(), 2);
    settings.set("entrance-tier", (long long)tier);
    Value ent;
    if (tier == 1) settings.set("shuffle-dungeons", true);
    bool mix = tier >= 2 && settings.get("shuffle-dungeons").truthy() && settings.get("mix-dungeon-entrances").truthy();
    if (mix) settings.set("shuffle-dungeons", false);
    else pop(settings, "mix-dungeon-entrances");

    if (tier >= 2) {
        C.ed = T.at("entrance_data").deepcopy();
        C.dungeon_doors = T.at("dungeon_doors");
        C.pockets = T.at("pockets");
        C.pocket_tiles = T.at("pocket_tiles");
        C.raft_dock = T.at("raft_dock").s();
        C.d6_entry = T.at("d6_entry").s();
        C.d6_rest = T.at("d6_rest").s();
        std::string seed = py_str(settings.at("seed"));
        bool decoupled = settings.get("entrance-decoupled", false).truthy();
        // (decoupled with the desert shut by the sleeping Walrus: its door, the Lanmola cave, stays vanilla)
        std::vector<std::string> closed;
        if (decoupled && !settings.get("open-desert", true).truthy()) closed.push_back("desert");
        List ent_doors = pool(C, closed);
        const List house_doors = ent_doors;
        if (mix)
            for (const Value &d : dungeon_pool(C)) ent_doors.push_back(d);

        // the shopsanity / trendsanity locations inside shuffled interiors (sanity_inside)
        Extra extra = new_dict();
        Value shop = settings.get("shopsanity", "off");
        if (shop == Value("vanilla") || shop == Value("random"))
            for (const auto &e : T.at("shop_inside").dict()) extra.set(e.key, e.value.deepcopy());
        if (settings.get("trendsanity").truthy())
            for (const auto &e : T.at("trendy_inside").dict()) extra.set(e.key, e.value.deepcopy());
        Value lv_door = new_dict();
        for (const Value &d : house_doors)
            if (d.at("dest_level").is_str()) lv_door.set(d.at("dest_level").s(), d.at("id"));
        // pots (pots.location_name: pot-<level>-<room>-<n> outside the dungeons; the dungeons' never match a door or
        // cave level): pots inside shuffled interiors follow their interior
        std::vector<std::pair<std::string, std::string>> pots;  // (location, level)
        if (settings.get("pots").truthy())
            for (const auto &e : logic_defs.dict()) {
                const std::string &k = e.key;
                if (k.compare(0, 4, "pot-") != 0) continue;
                size_t b = k.rfind('-'), a = b == std::string::npos ? b : k.rfind('-', b - 1);
                if (a == std::string::npos || a <= 4 || b + 1 >= k.size() ||
                    k.find_first_not_of("0123456789", b + 1) != std::string::npos || a + 1 == b)
                    continue;
                pots.push_back({k, k.substr(4, a - 4)});
            }
        auto append = [](Value &x, const std::string &key, const std::string &loc, const char *cond) {
            if (!x.has(key)) x.set(key, Value::new_list());
            x.at(key).list().push_back(Value(List{loc, cond}));
        };
        for (const auto &p : pots)
            if (lv_door.has(p.second)) append(extra, lv_door.at(p.second).s(), p.first, "break-pot");
        // connector caves: their own pool, logic first (house exits are ORed into regions it rewrites)
        List cdoors = connector_pool(C);
        Value lv_node = connector_levels(cdoors);
        Extra cextra = new_dict();
        for (const auto &p : pots)
            if (lv_node.has(p.second)) append(cextra, lv_node.at(p.second).s(), p.first, "break-pot");
        PocketOf pocket_at = pocket_of(C, cdoors);

        // the Field checks a pocket may hold: bush_locs, dig_locs, rock_locs, apple_locs (room, tile)
        FieldTiles ft(read);
        std::set<std::string> pocket_rooms;
        for (const auto &e : C.pocket_tiles.dict())
            for (const auto &r : e.value.dict()) pocket_rooms.insert(r.key);
        std::vector<FieldCheck> field;
        bool bushes = settings.get("bushes").truthy(), digs = settings.get("digs").truthy(),
             rocks = settings.get("rocks").truthy(), apples = settings.get("apples").truthy();
        for (const auto &e : logic_defs.dict()) {
            std::string room;
            int64_t n;
            const std::string &k = e.key;
            if (bushes && match_room_n(k, "bush", room, n))  // (a tile only matters where a pocket shares the screen)
                field.push_back({k, room, pocket_rooms.count(room) ? ft.tile(room, (int)T.at("bush_type").i(), n - 1) : Value()});
            else if (rocks && match_room_n(k, "rock", room, n))
                field.push_back({k, room, pocket_rooms.count(room) ? ft.tile(room, (int)T.at("rock_type").i(), n - 1) : Value()});
            else if (digs && match_room_n(k, "dig", room, n))
                field.push_back({k, room, Value((long long)n)});
            else if (apples && match_room_n(k, "apple-tree", room, n))
                field.push_back({k, room, T.at("apple_stand").at(room)});
        }

        Reach R;
        auto with_caves = [&](const List *caves, const Value &centry, const Value &cexit, const Value &hentry,
                              const Value &hexit, int *moved) {
            Value lg = logic_defs.deepcopy();
            int n = caves ? apply_connector_logic(C, lg, *caves, centry, cexit, cextra) : 0;
            // a bush / dig / apple tree / rock tile in a pocket (its door's screen, or its tiles on the screens
            // beside it) needs it
            if (caves)
                for (const FieldCheck &f : field) {
                    if (!lg.has(f.name)) continue;
                    Value pk = pocket_at(f.room, f.tile);
                    if (pk.truthy())
                        lg.at(f.name).set("condition-basic", and_({get(lg.at(f.name), "condition-basic"), pk}));
                }
            n += apply_logic(C, lg, ent_doors, hentry, hexit, extra);
            if (settings.get("hookshot-anything").truthy()) hookshot_apply(C, lg);  // (on what the caves rewrote)
            if (moved) *moved = n;
            return lg;
        };
        auto items = [&](const Value &lg) { return reach_items(R, lg, item_defs, settings); };
        Value none = new_dict();
        std::set<std::string> whole;
        if (mix) {  // (every location the unshuffled doors reach: a door layout that loses one is re-rolled)
            Value same = new_dict();
            for (const Value &d : ent_doors) same.set(d.at("id").s(), d.at("id"));
            whole = items(with_caves(nullptr, none, none, same, same, nullptr));
        }
        Value ent_entry, ent_exit, cave_entry, cave_exit, lg;
        int moved = 0;
        bool done = false;
        for (int house_try = 0; house_try < (mix ? 40 : 1) && !done; house_try++) {
            PyRandom hr(seed + "-entrances" + (house_try ? "-" + std::to_string(house_try) : ""));
            auto ee = shuffle(ent_doors, decoupled, hr, mix ? &C.dungeon_doors : nullptr);
            ent_entry = ee.first, ent_exit = ee.second;
            std::set<std::string> want = items(with_caves(nullptr, none, none, ent_entry, ent_exit, nullptr));
            if (mix && !minus(whole, want).empty()) {
                say("entrances: door layout " + std::to_string(house_try) + " loses " + first6(minus(whole, want)));
                continue;
            }
            bool found = false;
            for (int attempt = 0; attempt < 100; attempt++) {  // re-roll a cave layout that loses a location
                PyRandom cr(seed + "-connectors" + (attempt ? "-" + std::to_string(attempt) : ""));
                auto ce = shuffle(cdoors, decoupled, cr, nullptr);
                cave_entry = ce.first, cave_exit = ce.second;
                lg = with_caves(&cdoors, cave_entry, cave_exit, ent_entry, ent_exit, &moved);
                std::vector<std::string> lost = minus(want, items(lg));
                if (lost.empty()) {
                    found = true;
                    break;
                }
                say("caves: layout " + std::to_string(attempt) + " loses " + first6(lost));
            }
            if (!found) {
                if (mix) continue;
                raise("RuntimeError", "no connector cave layout keeps every location reachable");
            }
            done = true;
        }
        if (!done) raise("RuntimeError", "no door layout keeps every location reachable");
        logic_defs.dict().clear();
        for (const auto &e : lg.dict()) logic_defs.set(e.key, e.value);
        List doors = ent_doors;
        for (const Value &d : cdoors) doors.push_back(d);
        Value entry = new_dict(), exit_ = new_dict();
        for (const auto &e : ent_entry.dict()) entry.set(e.key, e.value);
        for (const auto &e : cave_entry.dict()) entry.set(e.key, e.value);
        for (const auto &e : ent_exit.dict()) exit_.set(e.key, e.value);
        for (const auto &e : cave_exit.dict()) exit_.set(e.key, e.value);
        ent = Value(List{Value(doors), entry, exit_});
        if (mix) {  // (LAS's dungeon-entrances: door -> dungeon; the instrument / Fairy Queen warps: outside the exit door)
            auto w = dungeon_ways(C, ent_doors, ent_entry, ent_exit);
            settings.set("mixed-dungeon-ways", w.first);
            settings.set("dungeon-exits", w.second);
            std::string s;
            for (const auto &e : w.first.dict()) {
                bool own = false;  // (a key that is LAS's own dungeon key: the dungeon's own door)
                for (const auto &dd : C.dungeon_doors.dict())
                    if (dd.value == Value(e.key)) own = true;
                if (!own) s += (s.empty() ? "" : ", ") + e.key + " -> " + py_str(e.value);
            }
            say("dungeon doors mixed in: " + (s.empty() ? std::string("none behind house doors") : s));
        }
        say("entrances: " + std::to_string(ent_entry.len()) + " doors + " + std::to_string(cave_entry.len()) +
            " cave doors shuffled, " + std::to_string(moved) + " locations moved");
    }
    state.set("ent", ent);
    return ent;
}

}  // namespace solve
}  // namespace rando
