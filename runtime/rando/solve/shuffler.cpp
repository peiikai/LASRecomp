// The item fill, natively (shuffler.h). Ported from LAS-Randomizer's shuffler.py (MIT, Owen-Splat and contributors),
// with the LAS Recomp changes made there (marked "LAS Recomp" in the Python), function for function in the same
// order. Every random draw is the Python's: the same method, the same arguments, in the same order.
//
// Python behaviour kept because the seeds depend on it (each is the Python's, quirks included):
//   - vanilla_locations and excluded-locations are Python sets: iterated in CPython's order (PyStrSet), which sets
//     the order of placements['indexes'];
//   - the zol-trap / stalfos-note chests and the items dropped when the pool is over by a few leave the access dict
//     as it was (the Python never removes them), so the fill goes on assuming them in hand;
//   - an undo takes back the oldest placement (placement_tracker.pop(0)), Tarin's included;
//   - verifySeashellsAttainable is asked for 5, 15, 30, 40 and 50 shells with the same state: one search, the answer
//     for 50 (the others follow from it);
//   - settings_access holds every setting == True, so a setting set to 1 or 1.0 counts as on;
//   - sum() of floats in the pacing is CPython 3.12's compensated sum (Neumaier), and ** is correctly rounded
//     (crpow.h): this platform's pow() may differ in the last bit.
// Two things the Python would never finish are errors here: Tarin's loop when no item before the first seashell can
// open anything, and a dungeon / main fill with no locations left.
#include "shuffler.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <deque>
#include <unordered_map>

#include "crpow.h"
#include "logic.h"
#include "pyset.h"

#include "../pyrandom.h"

namespace rando {
namespace solve {

const char *const GIVE_UP_TEXT = "These settings leave too few places for the items. Try other settings.";

bool fill_gave_up(const std::string &e) {  // (tools/rando/las_headless.py GIVE_UP)
    size_t d = e.find("can't place ");
    return e.find("can't place the items of D") != std::string::npos || e.find("fill stuck: ") != std::string::npos ||
           e.find("no item for Tarin") != std::string::npos ||
           (d != std::string::npos && e.find(" in D", d) != std::string::npos && e.find(": none of ", d) != std::string::npos);
}

namespace {

const int PL_ABSENT = -2, PL_NONE = -1;  // placements: no such key / None

enum ItemType : int8_t { T_UNKNOWN = -2, T_MISSING = -1, T_IMPORTANT, T_TRADE, T_SEASHELL, T_TRIFORCE, T_GOOD,
                         T_IMPORTANT_HEALTH, T_JUNK, T_OTHER };

bool starts_with(const std::string &s, const char *p) { return s.compare(0, strlen(p), p) == 0; }
bool starts_with_any(const std::string &s, const std::vector<std::string> &ps) {
    for (const std::string &p : ps)
        if (s.compare(0, p.size(), p) == 0) return true;
    return false;
}
// s[-2:] == 'D<i>'
bool dungeon_suffix(const std::string &s, int i) {
    return s.size() >= 2 && s[s.size() - 2] == 'D' && s[s.size() - 1] == '0' + i;
}
// re.search(r'D[0-8]$', s) / re.match(r"D[0-8]", s[-2:])
bool dungeon_item(const std::string &s) {
    return s.size() >= 2 && s[s.size() - 2] == 'D' && s[s.size() - 1] >= '0' && s[s.size() - 1] <= '8';
}
// s[-2:-1] == 'D' and s[-1].isdigit()
bool dungeon_digit(const std::string &s) {
    return s.size() >= 2 && s[s.size() - 2] == 'D' && s[s.size() - 1] >= '0' && s[s.size() - 1] <= '9';
}
bool in_list(const Value &list_or_none, const std::string &s) {  // s in settings.get(key, ())
    if (list_or_none.is_none()) return false;
    return list_or_none.contains(Value(s));
}
std::vector<std::string> str_tuple(const Value &v) {
    std::vector<std::string> out;
    for (const Value &x : v.list()) out.push_back(x.s());
    return out;
}

// sum(xs) of floats as CPython 3.12 adds them: 0 + xs[0], then Neumaier's compensated sum (Python/bltinmodule.c)
double py_fsum(const double *xs, size_t n) {
    if (!n) return 0.0;
    double f = 0.0 + xs[0], c = 0.0;
    for (size_t k = 1; k < n; k++) {
        double x = xs[k];
        volatile double t = f + x;  // (no fused or reordered sums: the compensation relies on each rounding)
        if (fabs(f) >= fabs(x)) c += (f - t) + x;
        else c += (x - t) + f;
        f = t;
    }
    if (c != 0.0 && isfinite(c)) f += c;
    return f;
}

PyError index_error(const char *what) { return PyError("IndexError", what); }

}  // namespace

struct ItemShuffler::Impl {
    ItemShuffler &S;
    PyRandom rng;
    PyStrSet vanilla;
    Names names;
    std::string logic;
    std::unique_ptr<Logic> L;
    bool compare_libm = false;
    std::atomic<bool> stopped{false};
    void active() {  // (the Python's thread_active checks)
        if (stopped) throw PyError("RuntimeError", "stopped");
    }

    // fill state
    std::vector<int> pl;          // placements by location name id (PL_ABSENT / PL_NONE / item name id)
    std::vector<int> pl_order;    // location keys in the dict's order
    std::vector<uint8_t> force_junk, absent, junk;  // by name id
    std::vector<int8_t> types;    // item_defs[name]['type'] by name id (T_UNKNOWN: not looked up yet)
    std::vector<int8_t> paces_;   // _paces(name): -1 not looked up yet
    Value absent_v, junk_v;
    Value indexes;                // placements['indexes']

    // pacing cache (_pace_cache): sphere per node, the placements it was made from, the access it started with
    bool cache_ok = false;
    std::vector<int8_t> cache_sphere;
    std::vector<std::pair<int, int>> cache_pl;
    Access cache_start;

    Impl(ItemShuffler &s, const std::string &seed) : S(s) { rng.seed(seed); }

    template <class C> void shuffle(C &x) {  // random.shuffle
        for (size_t i = x.size(); i-- > 1;) {
            size_t j = (size_t)rng.randbelow(i + 1);
            std::swap(x[i], x[j]);
        }
    }
    template <class T> const T &choice(const std::vector<T> &seq) {  // random.choice
        if (seq.empty()) throw index_error("Cannot choose from an empty sequence");
        return seq[(size_t)rng.randbelow(seq.size())];
    }
    void say(const std::string &s) {
        if (S.log) S.log(s);
    }
    int id(const std::string &s) { return names.id(s); }
    const std::string &nm(int i) const { return names.name(i); }

    Value &qty(const std::string &item) { return S.item_defs.at(item).at("quantity"); }
    void add_qty(const std::string &item, int64_t d) {
        Value &q = qty(item);
        q = Value((long long)(q.i() + d));
    }

    // ---- run() and the steps before the fill

    void logicSettingsChanges() {
        Value &defs = S.logic_defs;
        for (const char *kind : {"overworld", "dungeon"}) {
            std::string key = std::string("owl-") + kind + "-gifts";
            if (!S.settings.at(key).truthy()) {
                std::vector<std::string> owls;
                for (const auto &e : defs.dict())
                    if (e.value.at("type") == Value("item") && e.value.at("subtype") == Value(std::string(kind) + "-statue"))
                        owls.push_back(e.key);
                for (const std::string &o : owls) defs.dict().erase(o);
            } else {
                add_qty("rupee-20", kind[0] == 'o' ? 9 : 24);  // (33 owl statues: 9 in the overworld, 24 in dungeons)
            }
        }
        if (S.settings.at("shuffle-bombs").truthy()) S.item_defs.at("bomb").set("type", "important");
        if (S.settings.at("shuffle-powder").truthy()) S.item_defs.at("powder").set("type", "important");
    }

    void vanillaLocations() {
        static const char *const randomized[] = {"chest", "boss", "enemy", "drop", "npc", "standing",
                                                 "overworld-statue", "dungeon-statue", "underwater"};
        for (const auto &e : S.logic_defs.dict()) {
            const Value &v = e.value;
            if (v.at("type") != Value("item")) continue;
            const Value &st = v.at("subtype");
            bool skip = false;
            for (const char *r : randomized) skip = skip || st == Value(r);
            if (skip) continue;
            if (S.settings.get("shellsanity").truthy() &&
                (st == Value("dig") || st == Value("hidden") || st == Value("bomb-hole")))
                continue;
            vanilla.add(e.key);
        }
        if (!S.settings.get("trendsanity").truthy()) {
            for (int i = 1; i <= 6; i++) vanilla.add("trendy-prize-" + std::to_string(i));
            vanilla.add("trendy-prize-final");
        }
        if (S.settings.get("shopsanity", "off") != Value("off")) {
            PyStrSet shops;
            for (const auto &e : S.logic_defs.dict())
                if (e.value.get("subtype") == Value("shop")) shops.add(e.key);
            vanilla.difference_update(shops);
        }
        if (!S.settings.at("blupsanity").truthy())
            for (int i = 0; i < 28; i++) vanilla.add("D0-rupee-" + std::to_string(i + 1));
    }

    // RandomizerCore.Randomizers.data.start_dungeon_items: item name prefixes
    std::vector<std::string> start_dungeon_item_kinds() {
        if (S.settings.has("dungeon-items-start")) return str_tuple(S.settings.at("dungeon-items-start"));
        const Value &d = S.settings.at("dungeon-items");
        if (!d.is_str()) return {};
        if (d.s() == "mc") return {"map", "compass"};
        if (d.s() == "mcb") return {"map", "compass", "stone-beak"};
        if (d.s() == "stone-beak") return {"stone-beak"};
        return {};
    }

    static Value start_location(const Value &content) {
        Dict d;
        d.set("type", "item"), d.set("subtype", "npc"), d.set("content", content), d.set("region", "mabe");
        d.set("spoiler-region", "mabe-village");
        return Value(std::move(d));
    }

    void addStartingItems() {
        std::vector<std::string> instruments = {"full-moon-cello", "conch-horn", "sea-lilys-bell", "surf-harp",
                                                "wind-marimba", "coral-triangle", "evening-calm-organ", "thunder-drum"};
        List &start = S.settings.at("starting-items").list();
        std::vector<Value> start_instruments, picked;
        for (const Value &x : start)
            if (x.is_str() && std::find(instruments.begin(), instruments.end(), x.s()) != instruments.end()) picked.push_back(x);
        for (const Value &i : picked) {
            start.erase(std::find(start.begin(), start.end(), i));
            start_instruments.push_back(i);
            auto at = std::find(instruments.begin(), instruments.end(), i.s());
            if (at == instruments.end()) throw PyError("ValueError", "list.remove(x): x not in list");
            instruments.erase(at);
        }
        std::vector<std::string> instrument_locations;
        for (const auto &e : S.logic_defs.dict()) {
            const Value &v = e.value;
            if (v.at("type") == Value("item") && v.at("subtype") == Value("standing")) {
                const Value &c = v.at("content");
                if (c.is_str() && std::find(instruments.begin(), instruments.end(), c.s()) != instruments.end())
                    instrument_locations.push_back(e.key);
            }
        }
        shuffle(instrument_locations);
        int64_t num = S.settings.at("starting-instruments").i() - (int64_t)start_instruments.size();
        if (num <= 0) num = 0;
        for (int64_t i = 0; i < num; i++) {
            if (instrument_locations.empty()) throw index_error("pop from empty list");
            std::string inst = instrument_locations.front();
            instrument_locations.erase(instrument_locations.begin());
            start_instruments.push_back(S.logic_defs.at(inst).at("content"));
        }
        if (!S.settings.at("shuffle-instruments").truthy())
            for (const std::string &inst : instrument_locations) vanilla.add(inst);

        std::vector<std::string> to_check = start_dungeon_item_kinds();
        if (!to_check.empty()) {
            std::vector<std::string> items;
            for (const auto &e : S.item_defs.dict())
                if (starts_with_any(e.key, to_check)) items.push_back(e.key);
            for (size_t e = 0; e < items.size(); e++) {
                std::string loc = "starting-dungeon-item-" + std::to_string(e + 1);
                S.logic_defs.set(loc, start_location(Value(items[e])));
                vanilla.add(loc);
                add_qty("rupee-50", 1);  // (a location for each, so a 50-rupee more in the pool for each)
            }
        }
        for (const Value &v : start_instruments) start.push_back(v);
        for (size_t e = 0; e < start.size(); e++) {
            std::string loc = "starting-item-" + std::to_string(e + 1);
            S.logic_defs.set(loc, start_location(start[e]));
            vanilla.add(loc);
            add_qty("rupee-50", 1);
        }
    }

    void addTraps() {
        const Value &level = S.settings.at("traps");
        if (level == Value("none")) return;
        std::vector<std::string> traps;
        for (const auto &e : S.item_defs.dict()) {
            const std::string &k = e.key;
            if (k.size() >= 4 && k.compare(k.size() - 4, 4, "trap") == 0 && k.compare(0, 3, "zol") != 0) traps.push_back(k);
        }
        int64_t num_traps;
        if (level == Value("few")) num_traps = 3;
        else if (level == Value("several")) num_traps = 9;
        else if (level == Value("many")) num_traps = 17;
        else if (level == Value("trapsanity")) num_traps = 19;
        else throw PyError("KeyError", level.repr());
        if (num_traps == 19) {  // (every 5- (Blupsanity), 20- and 50-rupee and medicine becomes a trap)
            if (S.settings.at("blupsanity").truthy()) {
                int64_t blues = qty("rupee-5").i();
                qty("rupee-5") = Value(0);
                for (int64_t i = 0; i < blues; i++) add_qty(choice(traps), 1);
            }
            for (const char *key : {"rupee-20", "medicine"}) {
                int64_t q = qty(key).i();
                qty(key) = Value(0);
                for (int64_t i = 0; i < q; i++) add_qty(choice(traps), 1);
            }
            num_traps = qty("rupee-50").i();
        }
        add_qty("zol-trap", -3);
        add_qty("rupee-100", 2);
        add_qty("rupee-300", 1);
        for (int64_t i = 0; i < num_traps; i++) {
            std::string src = "rupee-50";
            if (qty("rupee-50").i() <= 0) {  // max((20, 100, 300), key=quantity): the first of the largest
                src = "rupee-20";
                for (const char *k : {"rupee-100", "rupee-300"})
                    if (qty(k).i() > qty(src).i()) src = k;
            }
            if (qty(src).i() <= 0) break;
            add_qty(src, -1);
            add_qty(choice(traps), 1);
        }
    }

    void shuffleDungeons() {
        static const char *const dungeons[] = {"tail-cave", "bottle-grotto", "key-cavern", "angler-tunnel", "catfish-maw",
                                               "face-shrine", "eagle-tower", "turtle-rock", "color-dungeon"};
        S.dungeon_entrances = Value::new_dict();
        if (S.settings.at("shuffle-dungeons").truthy()) {
            std::vector<std::string> target(std::begin(dungeons), std::end(dungeons));
            Dict conditions;
            shuffle(target);
            for (const char *dungeon : dungeons) {
                std::string dun = target.front();
                target.erase(target.begin());
                S.dungeon_entrances.set(dungeon, dun);
                conditions.set(dun, S.logic_defs.at(dungeon).at("condition-basic"));
            }
            for (const auto &c : conditions) S.logic_defs.at(c.key).set("condition-basic", c.value);
        } else if (S.settings.get("mixed-dungeon-ways").truthy()) {  // (LAS Recomp: dungeon doors mixed with houses)
            S.dungeon_entrances = S.settings.at("mixed-dungeon-ways").copy();
        } else {
            for (const char *dungeon : dungeons) S.dungeon_entrances.set(dungeon, dungeon);
        }
    }

    // ---- the fill's helpers

    int &P(int loc) {
        if ((size_t)loc >= pl.size()) pl.resize((size_t)loc + 1, PL_ABSENT);
        return pl[(size_t)loc];
    }
    int pget(int loc) const {  // placements.get(loc) (None: PL_NONE)
        int v = (size_t)loc < pl.size() ? pl[(size_t)loc] : PL_ABSENT;
        return v == PL_ABSENT ? PL_NONE : v;
    }
    int pat(int loc) const {  // placements[loc]
        int v = (size_t)loc < pl.size() ? pl[(size_t)loc] : PL_ABSENT;
        if (v == PL_ABSENT) throw PyError("KeyError", py_repr(nm(loc)));
        return v;
    }
    void set_place(int loc, int item) {
        if (P(loc) == PL_ABSENT) pl_order.push_back(loc);
        P(loc) = item;
    }
    bool flag(const std::vector<uint8_t> &f, int i) const { return (size_t)i < f.size() && f[(size_t)i]; }

    int type_of(int item) {  // item_defs[item]['type']
        if ((size_t)item >= types.size()) types.resize((size_t)item + 1, T_UNKNOWN);
        int8_t &t = types[(size_t)item];
        if (t == T_UNKNOWN) {
            const Value *d = S.item_defs.dict().find(nm(item));
            const Value *ty = d && d->is_dict() ? d->dict().find("type") : nullptr;
            if (!ty) t = T_MISSING;
            else if (*ty == Value("important")) t = T_IMPORTANT;
            else if (*ty == Value("trade")) t = T_TRADE;
            else if (*ty == Value("seashell")) t = T_SEASHELL;
            else if (*ty == Value("triforce")) t = T_TRIFORCE;
            else if (*ty == Value("good")) t = T_GOOD;
            else if (*ty == Value("important-health")) t = T_IMPORTANT_HEALTH;
            else if (*ty == Value("junk")) t = T_JUNK;
            else t = T_OTHER;
        }
        if (t == T_MISSING) throw PyError("KeyError", py_repr(S.item_defs.has(nm(item)) ? "type" : nm(item)));
        return t;
    }
    // _paces: an important item that isn't a dungeon item
    bool paces(int item) {
        if ((size_t)item >= paces_.size()) paces_.resize((size_t)item + 1, -1);
        int8_t &p = paces_[(size_t)item];
        if (p < 0) {
            const Value *d = S.item_defs.dict().find(nm(item));
            bool imp = d && d->is_dict() && d->get("type") == Value("important");
            p = imp && !dungeon_item(nm(item));
        }
        return p;
    }

    template <class C> static void remove_first(C &c, int v) {  // list.remove(v)
        auto it = std::find(c.begin(), c.end(), v);
        if (it == c.end()) throw PyError("ValueError", "list.remove(x): x not in list");
        c.erase(it);
    }

    // ---- searches (canReachLocation and the LAS Recomp variants)

    // the item a reached item / follower location holds: placements[key] (a KeyError if the key isn't there)
    template <class F> void reached_item(Search &s, size_t k, F &&get) {
        const LogicNode &n = L->node(k);
        if (n.kind == NODE_ITEM || n.kind == NODE_FOLLOWER) {
            int i = get(n.id);
            if (i >= 0) s.add(i);
        }
    }

    bool canReachLocation(int to_reach, const Access &start) {
        if (S.tick) S.tick();
        if (flag(force_junk, to_reach)) return false;
        if (logic == "none") return true;
        S.stats.searches++;
        Search s(*L, start);
        bool found = false;
        s.run([&](size_t k) {
            if (L->node(k).id == to_reach) return found = true;
            reached_item(s, k, [&](int l) { return pat(l); });
            return false;
        });
        return found;
    }

    // _reachable: what canReachLocation's search reaches (logic 'none': every node)
    struct Reach {
        bool all_nodes = false;
        Access a;
    };
    Reach reachable(const Access &start) {
        Reach r;
        if (logic == "none") return r.all_nodes = true, r;
        S.stats.searches++;
        Search s(*L, start);
        s.run([&](size_t k) {
            reached_item(s, k, [&](int l) { return pat(l); });
            return false;
        });
        r.a = std::move(s.access);
        return r;
    }
    bool in_reach(const Reach &r, int loc) const { return r.all_nodes ? L->index_of(loc) >= 0 : r.a.in(loc); }

    int verifySeashellsCount(const Access &start) {  // len(locations) + vanilla_seashells
        S.stats.searches++;
        Search s(*L, start);
        int locations = 0, shells = 0, seashell = names.find("seashell");
        s.run([&](size_t k) {
            const LogicNode &n = L->node(k);
            if (n.kind == NODE_ITEM || n.kind == NODE_FOLLOWER) {
                int i = pat(n.id);
                if (i >= 0) {
                    if (i == seashell) shells++;
                    else s.add(i);
                }
            }
            if (n.kind == NODE_ITEM && pat(n.id) == PL_NONE) locations++;
            return false;
        });
        return locations + shells;
    }

    bool verifyFreeLocations(const Access &start, int64_t goal) {
        S.stats.searches++;
        Search s(*L, start);
        int64_t locations = 0;
        s.run([&](size_t k) {
            const LogicNode &n = L->node(k);
            reached_item(s, k, [&](int l) { return pget(l); });
            if (n.kind == NODE_ITEM && pget(n.id) == PL_NONE && !flag(force_junk, n.id) && !flag(absent, n.id) &&
                !flag(junk, n.id))
                locations++;
            return false;
        });
        return locations >= goal;
    }

    int pace_reach(const Access &start) {  // _pace_reach
        S.stats.searches++;
        Search s(*L, start);
        int n = 0;
        s.run([&](size_t k) {
            if (L->node(k).kind == NODE_ITEM) n++;
            reached_item(s, k, [&](int l) { return pget(l); });
            return false;
        });
        return n;
    }

    static constexpr int LATE = 6;  // len(PACE_CURVE[...])

    // _pace_spheres: sphere per item / follower node (-1: not reached), cached as the Python caches it
    const std::vector<int8_t> &pace_spheres(const Access &start) {
        if (cache_ok && cache_start == start) {
            bool same = true;
            for (const auto &li : cache_pl)
                if (pget(li.first) != li.second) {
                    same = false;
                    break;
                }
            if (same) return cache_sphere;
        }
        S.stats.searches++;
        std::vector<int8_t> sphere(L->size(), -1);
        std::vector<int> order;  // (the reached item / follower nodes, in the dict's order)
        Search s(*L, start);
        int sp = 0;
        for (;;) {
            std::vector<int> found;
            s.run([&](size_t k) {
                const LogicNode &n = L->node(k);
                if (n.kind == NODE_ITEM || n.kind == NODE_FOLLOWER) {
                    sphere[k] = (int8_t)sp;
                    order.push_back((int)k);
                    if (pget(n.id) >= 0) found.push_back(pget(n.id));
                }
                return false;
            });
            if (sp == LATE - 1) break;  // (what this sphere holds would only open a later one)
            sp++;
            if (found.empty()) break;
            for (int i : found) s.add(i);
        }
        cache_pl.clear();
        for (int k : order)
            if (sphere[(size_t)k] < sp) cache_pl.emplace_back(L->node((size_t)k).id, pget(L->node((size_t)k).id));
        cache_sphere = std::move(sphere);
        cache_start = start;
        cache_ok = true;
        return cache_sphere;
    }

    bool is_major(int item) const {
        static const char *const major[] = {"sword", "bracelet", "feather", "boots", "hookshot", "flippers", "rod", "bow",
                                            "shovel", "boomerang", "song-ballad", "song-soul", "ocarina", "powder", "bomb"};
        for (const char *m : major)
            if (nm(item) == m) return true;
        return false;
    }

    // _paced_order: the locations in a weighted random order (Efraimidis-Spirakis keys random() ** (top / weight))
    std::deque<int> paced_order(const std::deque<int> &locations, const Access &settings_access, const std::deque<int> &items) {
        static const double CURVE[2][LATE] = {{0.3, 0.93, 1.48, 1.75, 1.38, 2.08}, {0.3, 3.42, 1.88, 1.57, 1.05, 0.95}};
        bool major = is_major(items[0]);
        const double *curve = CURVE[major];
        Access assumed = settings_access;
        for (int i : items)
            if (!paces(i) && !dungeon_item(nm(i))) assumed.add(i);
        const std::vector<int8_t> &sphere = pace_spheres(assumed);
        int64_t free_[LATE + 1] = {}, have[LATE] = {};
        for (size_t k = 0; k < L->size(); k++) {
            int t = sphere[k];
            if (t < 0) continue;
            int l = L->node(k).id, i = pget(l);
            if (t < LATE && i >= 0 && paces(i) && is_major(i) == major && !starts_with(nm(l), "starting-")) have[t]++;
        }
        std::vector<int> tier;
        tier.reserve(locations.size());
        for (int l : locations) {
            int k = L->index_of(l), t = k >= 0 ? sphere[(size_t)k] : -1;
            tier.push_back(t < 0 || t > LATE ? LATE : t);
        }
        for (size_t n = 0; n < locations.size(); n++) {
            int l = locations[n];
            if (!flag(force_junk, l) && !flag(absent, l) && !flag(junk, l)) free_[tier[n]]++;
        }
        int64_t left = 0;
        for (int i : items) left += paces(i) && is_major(i) == major;
        double lack[LATE], share[LATE + 1], weight[LATE + 1];
        for (int t = 0; t < LATE; t++) {
            int64_t h = 0;
            for (int u = 0; u <= t; u++) h += have[u];
            volatile double x = py_fsum(curve, (size_t)t + 1) - (double)h;  // (two roundings, as Python's)
            double y = x - py_fsum(lack, (size_t)t);
            lack[t] = y > 0.0 ? y : 0.0;
        }
        for (int t = 0; t < LATE; t++) share[t] = free_[t] ? lack[t] / (double)left : 0.0;
        double total = py_fsum(share, LATE);
        if (total > 1)
            for (int t = 0; t < LATE; t++) share[t] = share[t] / total;
        double rest = 1 - py_fsum(share, LATE);
        share[LATE] = rest > 0.0 ? rest : 0.0;
        for (int t = 0; t <= LATE; t++) {
            double w = free_[t] ? share[t] / (double)free_[t] : 0.0;
            weight[t] = 1e-6 > w ? 1e-6 : w;
        }
        double top = weight[0];
        for (int t = 1; t <= LATE; t++)
            if (weight[t] > top) top = weight[t];
        std::vector<double> keys(locations.size());
        std::vector<double> libm;
        if (compare_libm) libm.resize(locations.size());
        for (size_t n = 0; n < locations.size(); n++) {
            double r = rng.random(), e = top / weight[tier[n]];
            keys[n] = py_pow(r, e);
            if (e != 1.0) {
                S.stats.pow_calls++;
                if (compare_libm && keys[n] != pow(r, e)) S.stats.pow_libm_differs++;
            }
            if (compare_libm) libm[n] = r == 0 ? 0.0 : pow(r, e);
        }
        std::vector<size_t> idx(locations.size());
        for (size_t n = 0; n < idx.size(); n++) idx[n] = n;
        std::stable_sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return -keys[a] < -keys[b]; });
        if (compare_libm) {
            std::vector<size_t> idx2(locations.size());
            for (size_t n = 0; n < idx2.size(); n++) idx2[n] = n;
            std::stable_sort(idx2.begin(), idx2.end(), [&](size_t a, size_t b) { return -libm[a] < -libm[b]; });
            if (idx2 != idx) S.stats.pow_order_changes++;
        }
        std::deque<int> out;
        for (size_t n : idx) out.push_back(locations[n]);
        return out;
    }

    // _pace_first: the major items that open the most locations go first
    void pace_first(std::deque<int> &items, const Access &access) {
        std::vector<size_t> at;
        for (size_t k = 0; k < items.size(); k++)
            if (paces(items[k])) at.push_back(k);
        if (at.empty()) return;
        int full = pace_reach(access);
        std::unordered_map<int, int> opens;
        for (size_t k : at) {
            int i = items[k];
            if (opens.count(i)) continue;
            Access without = access;
            without.set(i, 0);
            opens[i] = full - pace_reach(without);
        }
        std::vector<int> order;
        for (size_t k : at) order.push_back(items[k]);
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return -opens[a] < -opens[b]; });
        for (size_t n = 0; n < at.size(); n++) items[at[n]] = order[n];
    }

    // ---- makeRandomizedPlacement

    Value makeRandomizedPlacement() {
        Value &cfg = S.settings;  // (self.settings)
        // excluded locations that are vanilla aren't excluded; the rest must be item locations (a set from here on)
        {
            Value excl = cfg.at("excluded-locations");
            bool disjoint = true;
            for (const Value &l : excl.list())
                if (l.is_str() && vanilla.contains(l.s())) disjoint = false;
            if (!disjoint) {
                say("Warning! Some locations set as disabled are unrandomized. These locations will not actually be "
                    "considered out of logic.");
                List kept;
                for (const Value &l : excl.list())
                    if (!(l.is_str() && vanilla.contains(l.s()))) kept.push_back(l);
                cfg.set("excluded-locations", Value(std::move(kept)));
            }
            PyStrSet fj;
            for (const Value &l : cfg.at("excluded-locations").list())
                if (l.is_str() && S.logic_defs.has(l.s()) && S.logic_defs.at(l.s()).at("type") == Value("item")) fj.add(l.s());
            List fjl;
            for (const std::string &k : fj.items()) fjl.push_back(Value(k));
            cfg.set("excluded-locations", Value(std::move(fjl)));
        }
        logic = cfg.at("logic").s();
        L.reset(new Logic(S.logic_defs, logic, names));
        for (const Value &l : cfg.at("excluded-locations").list()) {
            int i = id(l.s());
            if ((size_t)i >= force_junk.size()) force_junk.resize((size_t)i + 1, 0);
            force_junk[(size_t)i] = 1;
        }
        absent_v = cfg.get("absent-locations");
        junk_v = cfg.get("junk-locations");
        for (auto *fv : {&absent_v, &junk_v}) {
            std::vector<uint8_t> &f = fv == &absent_v ? absent : junk;
            if (fv->is_none()) continue;
            for (const Value &l : fv->list())
                if (l.is_str()) {
                    int i = id(l.s());
                    if ((size_t)i >= f.size()) f.resize((size_t)i + 1, 0);
                    f[(size_t)i] = 1;
                }
        }

        Access access;
        std::vector<int> important_items, seashell_items, triforce_items, good_items, junk_items, dungeon_items;
        std::deque<int> locations;
        indexes = Value::new_dict();

        std::unordered_map<int, std::vector<int64_t>> indexes_available;
        auto range = [](int64_t n) {
            std::vector<int64_t> v;
            for (int64_t i = 0; i < n; i++) v.push_back(i);
            return v;
        };
        indexes_available[id("seashell")] = range(50);
        indexes_available[id("heart-piece")] = range(32);
        indexes_available[id("heart-container")] = range(9);
        indexes_available[id("bottle")] = range(3);
        indexes_available[id("golden-leaf")] = range(5);
        indexes_available[id("chamber-stone")] = {3, 4, 8, 10, 11, 12, 13, 20, 21, 22, 23, 24, 25, 26};
        if (cfg.get("triforce-placed").truthy())  // (LAS Recomp Triforce hunt: each piece its own index)
            indexes_available[id("triforce-piece")] = range(cfg.at("triforce-placed").to_int());
        Value tn = cfg.get("triforce-needed", 0);
        int64_t tri_needed = (tn.truthy() ? tn : Value(0)).to_int();

        for (size_t k = 0; k < L->size(); k++)
            if (L->node(k).kind == NODE_ITEM) {
                locations.push_back(L->node(k).id);
                set_place(L->node(k).id, PL_NONE);
            }

        // everything starts in hand, and is taken away as it is placed
        for (const auto &e : S.item_defs.dict()) {
            int item = id(e.key);
            int64_t q = e.value.at("quantity").i();
            for (int64_t i = 0; i < q; i++) access.add(item);
            std::vector<int> *to = nullptr;
            if (dungeon_item(e.key)) to = &dungeon_items;
            else switch (type_of(item)) {
                case T_IMPORTANT: case T_TRADE: to = &important_items; break;
                case T_SEASHELL: to = &seashell_items; break;
                case T_TRIFORCE: to = &triforce_items; break;
                case T_GOOD: case T_IMPORTANT_HEALTH: to = &good_items; break;
                case T_JUNK: to = &junk_items; break;
                default: break;
            }
            if (to)
                for (int64_t i = 0; i < q; i++) to->push_back(item);
        }
        // the settings are in hand too (fast trendy, free fishing, ...): every setting == True
        Access settings_access;
        for (const auto &e : cfg.dict())
            if (e.value == Value(true)) settings_access.set(id(e.key), 1);
        for (size_t i = 0; i < settings_access.n.size(); i++)
            if (settings_access.n[i]) access.set((int)i, 1);

        set_place(id("moblin-cave"), id("bow-wow"));  // (the followers stay vanilla)
        if (S.logic_defs.at("rooster-statue").get("type") == Value("follower")) set_place(id("rooster-statue"), id("rooster"));

        shuffle(important_items);
        shuffle(seashell_items);
        shuffle(good_items);
        std::deque<int> items;
        for (auto *v : {&important_items, &seashell_items, &triforce_items, &good_items, &junk_items, &dungeon_items})
            items.insert(items.end(), v->begin(), v->end());

        // the vanilla locations get their own content (in the set's order: it orders placements['indexes'])
        for (const std::string &loc_name : vanilla.items()) {
            int loc = id(loc_name);
            if (std::find(locations.begin(), locations.end(), loc) == locations.end()) continue;
            const Value &content = S.logic_defs.at(loc_name).at("content");
            int item = content.is_str() ? id(content.s()) : -1;
            if (item < 0 || std::find(items.begin(), items.end(), item) == items.end())
                throw PyError("ValueError", "vanilla content " + content.repr() + " of " + loc_name + " is not in the item pool");
            P(loc) = item;
            remove_first(items, item);
            access.remove(item);
            remove_first(locations, loc);
            auto ia = indexes_available.find(item);
            if (ia != indexes_available.end()) {
                const Value &index = S.logic_defs.at(loc_name).at("index");
                indexes.set(loc_name, index);
                auto at = std::find_if(ia->second.begin(), ia->second.end(), [&](int64_t v) { return Value((long long)v) == index; });
                if (at == ia->second.end()) throw PyError("ValueError", "list.remove(x): x not in list");
                ia->second.erase(at);
            }
        }

        // dungeon items into their own dungeons (some may be there already: vanilla)
        Value dungeon_of = cfg.get("dungeon-of", Value::new_dict());
        for (int i = 0; i < 9; i++) {
            std::vector<std::string> to_check = start_dungeon_item_kinds();
            std::vector<int> item_pool;
            for (int s : items)
                if (dungeon_suffix(nm(s), i) && !(!to_check.empty() && starts_with_any(nm(s), to_check))) item_pool.push_back(s);
            // LARecomp keysanity: these go to the general pool instead
            std::vector<std::string> ks_free = keysanity_free();
            if (!ks_free.empty()) {
                std::vector<int> kept;
                for (int s : item_pool)
                    if (!starts_with_any(nm(s), ks_free)) kept.push_back(s);
                item_pool.swap(kept);
            }
            // LAS Recomp dungeon room shuffle: a check belongs to the dungeon its room is in now
            std::string di = "D" + std::to_string(i);
            std::deque<int> location_pool;
            for (int s : locations) {
                const std::string &n = nm(s);
                Value own = n.size() >= 2 ? Value(n.substr(0, 2)) : Value("");
                if (dungeon_of.get(n, own) == Value(di)) location_pool.push_back(s);
            }
            shuffle(location_pool);
            std::deque<int> tracker;
            int undos = 0;
            while (!item_pool.empty()) {
                active();
                int item = item_pool[0];
                if (location_pool.empty()) throw index_error("list index out of range");
                int first = location_pool[0];
                bool valid = false;
                while (!valid) {
                    active();
                    int l0 = location_pool[0];
                    P(l0) = item;
                    access.remove(item);
                    valid = canReachLocation(l0, access);
                    if (!valid) {
                        access.add(item);
                        P(l0) = PL_NONE;
                        location_pool.push_back(location_pool.front());
                        location_pool.pop_front();
                        if (location_pool[0] == first) {
                            // nowhere: undo the oldest placement and try again in a new order
                            if (tracker.empty()) {
                                std::vector<std::string> pool;
                                for (int l : location_pool) pool.push_back(nm(l));
                                std::sort(pool.begin(), pool.end());
                                std::string list = "[";
                                for (size_t n = 0; n < pool.size(); n++) list += (n ? ", " : "") + py_repr(pool[n]);
                                throw PyError("RuntimeError", "can't place " + nm(item) + " in D" + std::to_string(i) +
                                                                  ": none of " + list + "] is reachable without it");
                            }
                            undos++;
                            S.stats.undo_dungeon++;
                            if (undos > (cfg.get("dungeon-of").truthy() ? 60 : DUNGEON_GIVE_UP))
                                throw PyError("RuntimeError", "can't place the items of D" + std::to_string(i) + ": " +
                                                                  std::to_string(undos) + " tries");
                            int undo = tracker.front();
                            tracker.pop_front();
                            location_pool.push_back(undo);
                            locations.push_back(undo);
                            shuffle(location_pool);
                            items.push_front(pat(undo));
                            item_pool.insert(item_pool.begin(), pat(undo));
                            access.add(pat(undo));
                            P(undo) = PL_NONE;
                            break;
                        }
                    }
                }
                if (valid) {
                    remove_first(items, item);
                    remove_first(item_pool, item);
                    remove_first(locations, location_pool[0]);
                    tracker.push_back(location_pool.front());
                    location_pool.pop_front();
                }
            }
        }

        // LAS Recomp Triforce hunt (tools/rando/triforce.py): the pieces take the place of filler
        int64_t tri_placed = cfg.get("triforce-placed", 0).truthy() ? cfg.at("triforce-placed").to_int() : 0;
        if (tri_placed) {
            auto tier = [&](int k) -> int {
                if (type_of(k) != T_JUNK) return -1;
                const std::string &key = nm(k);
                std::string base = starts_with(key, "pot-") || starts_with(key, "dig-") ? key.substr(4) : key;
                if (starts_with(base, "rupee-") && base != "rupee-5" && base != "rupee-1") return 0;
                if (key.size() >= 5 && key.compare(key.size() - 5, 5, "-trap") == 0 && key != "zol-trap") return 1;
                if (base == "rupee-5" || base == "rupee-1" || key == "pot-heart" || key == "pot-arrows" ||
                    key == "pot-bombs" || key == "pot-powder" || key == "bomb" || key == "powder" || key == "arrow")
                    return 2;
                if (key == "chamber-stone") return 3;
                return -1;
            };
            std::vector<int> fill;
            for (int k : items)
                if (tier(k) >= 0) fill.push_back(k);
            say("triforce: " + std::to_string(fill.size()) + " filler items in the pool for " + std::to_string(tri_placed) + " pieces");
            if ((int64_t)fill.size() < tri_placed)
                throw PyError("RuntimeError", "Triforce hunt: " + std::to_string(tri_placed) + " pieces need " +
                                                  std::to_string(tri_placed) + " filler items to replace, but this seed has only " +
                                                  std::to_string(fill.size()) + ". Turn on Potsanity, Bushsanity or Digsanity, or "
                                                  "choose fewer pieces.");
            for (int64_t n = 0; n < tri_placed; n++) {
                // min(sorted(set(fill)), key=(tier, -count)): the first of the smallest in name order
                std::vector<int> uniq(fill);
                std::sort(uniq.begin(), uniq.end(), [&](int a, int b) { return nm(a) < nm(b); });
                uniq.erase(std::unique(uniq.begin(), uniq.end()), uniq.end());
                int best = -1, bt = 0;
                long bc = 0;
                for (int k : uniq) {
                    int t = tier(k);
                    long c = (long)std::count(fill.begin(), fill.end(), k);
                    if (best < 0 || t < bt || (t == bt && -c < -bc)) best = k, bt = t, bc = c;
                }
                remove_first(fill, best);
                for (size_t m = items.size(); m-- > 0;)  // (the last one in the pool)
                    if (items[m] == best) {
                        items.erase(items.begin() + (long)m);
                        break;
                    }
                access.remove(best);
            }
            size_t at = items.size();
            for (size_t m = 0; m < items.size(); m++) {
                int t = type_of(items[m]);
                if (t != T_IMPORTANT && t != T_TRADE && t != T_SEASHELL) {
                    at = m;
                    break;
                }
            }
            int piece = id("triforce-piece");
            items.insert(items.begin() + (long)at, (size_t)tri_placed, piece);
            for (int64_t n = 0; n < tri_placed; n++) access.add(piece);
        }

        // LAS Recomp keysanity: the freed dungeon items are placed with the important items
        std::vector<std::string> ks_free = keysanity_free();
        if (!ks_free.empty()) {
            std::vector<int> freed, rest;
            for (int s : items)
                if (starts_with_any(nm(s), ks_free) && dungeon_digit(nm(s))) freed.push_back(s);
            for (int s : items)
                if (std::find(freed.begin(), freed.end(), s) == freed.end()) rest.push_back(s);
            size_t n_imp = rest.size();
            for (size_t k = 0; k < rest.size(); k++)
                if (type_of(rest[k]) != T_IMPORTANT) {
                    n_imp = k;
                    break;
                }
            items.clear();
            items.insert(items.end(), rest.begin(), rest.begin() + (long)n_imp);
            items.insert(items.end(), freed.begin(), freed.end());
            items.insert(items.end(), rest.begin() + (long)n_imp, rest.end());
        }

        shuffle(locations);

        // the traps and the Master Stalfos note have to go in chests: first
        int zol = id("zol-trap"), note = id("stalfos-note");
        std::vector<int> to_place;
        for (int s : items)
            if (s == zol || s == note) to_place.push_back(s);
        std::deque<int> chests;
        for (int s : locations) {
            if (S.logic_defs.at(nm(s)).at("subtype") != Value("chest")) continue;
            if (in_list(absent_v, nm(s)) || in_list(junk_v, nm(s))) continue;
            chests.push_back(s);
        }
        for (int item : to_place) {
            if (chests.empty()) throw index_error("pop from empty list");
            int chest = chests.front();
            chests.pop_front();
            P(chest) = item;
            remove_first(items, item);
            remove_first(locations, chest);
        }

        std::deque<int> tracker;

        // Tarin is the only check with no items: something that opens more, never the boots
        int tarin = id("tarin");
        if (pat(tarin) == PL_NONE) {
            bool success = false;
            Value tail = S.dungeon_entrances.get("tail-cave", "door-d1-tail-cave");
            int tail_id = tail.is_str() ? id(tail.s()) : -1;
            size_t tries = 0;
            while (!success) {
                active();
                if (items.empty()) throw index_error("list index out of range");
                P(tarin) = items[0];
                S.stats.tarin_tries++;
                success = canReachLocation(id("can-shop"), settings_access) ||
                          (tail_id >= 0 && canReachLocation(tail_id, settings_access)) ||
                          canReachLocation(id("beach"), settings_access) ||
                          canReachLocation(id("ciao-ciao"), settings_access) ||
                          canReachLocation(id("marin"), settings_access) ||
                          canReachLocation(id("trendy"), settings_access);
                if (nm(items[0]) == "boots") success = false;
                if (!success) {
                    auto sh = std::find(items.begin(), items.end(), id("seashell"));
                    if (sh == items.end()) throw PyError("ValueError", "'seashell' is not in list");
                    if (sh == items.begin() || ++tries > items.size())
                        throw PyError("RuntimeError", "no item for Tarin opens a check");
                    items.insert(sh, items[0]);
                    items.pop_front();
                }
            }
            tracker.push_back(tarin);
            access.remove(items[0]);
            items.pop_front();
            remove_first(locations, tarin);
        }

        // LAS Recomp: one item per location
        long short_ = (long)locations.size() - (long)items.size();
        if (short_ > 0) {
            say("pool short by " + std::to_string(short_) + ": rupee-20 added");
            items.insert(items.end(), (size_t)short_, id("rupee-20"));
        }
        long extra = (long)items.size() - (long)locations.size();
        for (size_t k = items.size(); k-- > 0;) {
            if (extra <= 0) break;
            if (type_of(items[k]) == T_JUNK) {
                items.erase(items.begin() + (long)k);
                extra--;
            }
        }

        // everything else
        pace_first(items, access);
        int stuck = 0;
        bool paced = false;
        int seashell = id("seashell"), piece = id("triforce-piece"), red = id("red-tunic"), blue = id("blue-tunic"),
            taltal5 = id("taltal-5-chest-puzzle");
        bool room_shuffle = cfg.get("dungeon-room-shuffle", "off") != Value("off");
        auto undo_oldest = [&]() {
            if (tracker.empty()) throw index_error("pop from empty list");
            int undo = tracker.front();
            tracker.pop_front();
            locations.push_back(undo);
            shuffle(locations);
            items.push_front(pat(undo));
            access.add(pat(undo));
            P(undo) = PL_NONE;
        };
        while (!items.empty()) {
            active();
            int item = items[0];
            if (paces(item)) {
                locations = paced_order(locations, settings_access, items);
                paced = true;
            } else if (paced) {
                shuffle(locations);
                paced = false;
            }
            if (locations.empty()) throw index_error("list index out of range");
            int first = locations[0];
            bool have_reach = false;
            Reach reach;
            bool valid = false;
            int t = type_of(item);
            while (!valid) {
                active();
                int l0 = locations[0];
                P(l0) = item;
                access.remove(item);
                const LogicNode &node = L->node((size_t)L->index_of(l0));
                const std::string &sub = node.subtype;
                if ((item == red || item == blue) &&
                    (sub == "standing" || sub == "hidden" || sub == "dig" || sub == "bomb-hole" || sub == "drop" ||
                     sub == "underwater" || sub == "shop" || sub == "enemy"))
                    valid = false;
                else if ((item == zol || item == note) && sub != "chest")
                    valid = false;
                else if (item == zol && l0 == taltal5)  // (a zol-trap there: nothing happens with the 5th chest)
                    valid = false;
                else if ((flag(absent, l0) || flag(junk, l0)) && t != T_JUNK)
                    valid = false;
                else if (t == T_IMPORTANT || t == T_TRADE || t == T_SEASHELL ||
                         (t == T_TRIFORCE && [&] {
                             int64_t c = 0;
                             for (int l : tracker) c += pat(l) == item;
                             return c < tri_needed;
                         }())) {
                    // reachable? one search per item, from the items not placed yet (LAS Recomp)
                    if (!have_reach) {
                        P(l0) = PL_NONE;
                        reach = reachable(access);
                        P(l0) = item;
                        have_reach = true;
                    }
                    valid = !flag(force_junk, l0) && in_reach(reach, l0);
                } else {
                    valid = true;
                }
                if (!valid) {
                    access.add(item);
                    P(l0) = PL_NONE;
                    locations.push_back(locations.front());
                    locations.pop_front();
                    if (locations[0] == first) {
                        // nowhere: undo the oldest placement (LAS Recomp: a room-shuffle layout gives up after
                        // FILL_STUCK of these and las_headless plans the next)
                        stuck++;
                        if (stuck > (room_shuffle ? 150 : FILL_GIVE_UP))
                            throw PyError("RuntimeError", "fill stuck: " + nm(item) + " found no place " + std::to_string(stuck) + " times");
                        S.stats.undo_fill++;
                        undo_oldest();
                        break;
                    }
                }
            }
            if (valid) {
                items.pop_front();
                tracker.push_back(locations.front());
                locations.pop_front();
                // LAS Recomp Triforce hunt: room for the needed pieces before the first one
                if (tri_needed && !items.empty() && items[0] == piece && item != piece)
                    if (!verifyFreeLocations(settings_access, tri_needed)) S.stats.undo_triforce++, undo_oldest();
                // before the seashells: enough reachable locations for the 5 .. 50 shell rewards
                if (item != seashell && !items.empty() && items[0] == seashell)
                    if (verifySeashellsCount(settings_access) < 50) S.stats.undo_shells++, undo_oldest();
            }
        }

        // the indexes of what wasn't vanilla, in placement order
        for (int loc : tracker) {
            auto ia = indexes_available.find(pat(loc));
            if (ia == indexes_available.end()) continue;
            if (ia->second.empty()) throw index_error("pop from empty list");
            indexes.set(nm(loc), Value((long long)ia->second.front()));
            ia->second.erase(ia->second.begin());
        }
        // LARecomp keysanity: a dungeon item outside its own dungeon carries its dungeon (0-7 = D1-D8, 9 = Color Dungeon)
        std::vector<int> ks_keys;
        for (int loc : pl_order) {
            int item = pget(loc);
            if (item < 0) continue;
            const std::string &n = nm(item);
            if ((starts_with(n, "compass") || starts_with(n, "map") || starts_with(n, "stone-beak") || starts_with(n, "key-") ||
                 starts_with(n, "nightmare-key")) && dungeon_digit(n))
                ks_keys.push_back(loc);
        }
        for (int loc : ks_keys) {
            const std::string &key = nm(loc), &item = nm(pget(loc));
            int n = item.back() - '0';
            if (starts_with(key, "starting-")) continue;  // (given on entering their dungeon)
            Value own = Value(key.substr(0, 2));
            if (dungeon_of.get(key, own) == Value("D" + std::to_string(n))) continue;
            indexes.set(key, Value(n == 0 ? 9 : n - 1));
        }
        cfg.at("create-spoiler");  // (the Python reads it here to write the spoiler log: the caller's job)

        Value out = Value::new_dict();
        out.set("settings", cfg);
        out.set("force-junk", cfg.at("excluded-locations"));
        S.vanilla_locations = Value::new_list();
        for (const std::string &k : vanilla.items()) S.vanilla_locations.list().push_back(Value(k));
        out.set("force-vanilla", S.vanilla_locations);
        out.set("starting-items", cfg.at("starting-items"));
        out.set("dungeon-entrances", S.dungeon_entrances);
        out.set("indexes", indexes);
        for (int loc : pl_order) {
            int item = pl[(size_t)loc];
            out.set(nm(loc), item >= 0 ? Value(nm(item)) : Value());
        }
        return out;
    }

    std::vector<std::string> keysanity_free() {
        Value ks = S.settings.get("keysanity", "off");
        if (ks == Value("keys")) return {"key-"};
        if (ks == Value("keys+nk")) return {"key-", "nightmare-key-"};
        if (ks == Value("all")) return {"key-", "nightmare-key-", "map", "compass", "stone-beak"};
        return {};
    }
};

ItemShuffler::ItemShuffler(const std::string &seed, Value settings_, Value item_defs_, Value logic_defs_)
    : settings(std::move(settings_)), item_defs(std::move(item_defs_)), logic_defs(std::move(logic_defs_)),
      d_(new Impl(*this, seed)) {}

ItemShuffler::~ItemShuffler() {}

void ItemShuffler::compare_libm_pow(bool on) { d_->compare_libm = on; }

void ItemShuffler::stop() { d_->stopped = true; }

void ItemShuffler::placed(size_t &done, size_t &total) const {
    done = total = 0;
    for (int loc : d_->pl_order) {
        const Value *def = logic_defs.dict().find(d_->nm(loc));
        if (!def || !def->is_dict() || def->get("type") != Value("item")) continue;
        total++;
        done += d_->pl[(size_t)loc] >= 0;
    }
}

Value ItemShuffler::run() {
    d_->logicSettingsChanges();
    d_->vanillaLocations();
    d_->addStartingItems();
    d_->addTraps();
    d_->shuffleDungeons();
    return d_->makeRandomizedPlacement();
}

std::vector<int64_t> ItemShuffler::random_state() const {
    PyRandom::State s = d_->rng.getstate();
    std::vector<int64_t> v(s.mt, s.mt + 624);
    v.push_back(s.mti);
    return v;
}

}  // namespace solve
}  // namespace rando
