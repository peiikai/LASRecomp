// The seed's text outputs after the item fill (outputs.h): logic.json, the playthrough check, the Hint Sheet's steps,
// the Route, the hint picks, LAS's spoiler and settings.json; the tracker and the readable spoiler are in
// outputs_tracker.cpp / outputs_view.cpp.
//
// Python behaviour kept because the files depend on it (each is the Python's, quirks included):
//   - the reach fixpoints (hint_sheet / route) visit nodes from a stack fed by the readers of each token a node's
//     conditions name (the word regex, not the condition parser): with '!' the logic isn't monotonic, so the order is
//     theirs;
//   - the playthrough's enemy pass isn't guarded: a logic error there is the Python's exception;
//   - str() of the settings in LAS's spoiler: sets as CPython iterates them (the recorded order), dicts / lists as repr.
#if defined(__clang__)
#pragma clang fp contract(off)  // (float sums as Python rounds them: no fused multiply-add)
#endif
#include "outputs.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <fstream>
#include <iterator>

#include "../fmt/fixed_hash.h"
#include "outputs_lib.h"

#include "../pyrandom.h"

namespace rando {
namespace solve {

// ---------------------------------------------------------------- the tables' lookups
namespace odata {
const char *get(const StrMap &m, const std::string &key) {
    for (const auto &e : m)
        if (key == e.first) return e.second;
    return nullptr;
}
bool in(const StrList &l, const std::string &key) {
    for (const char *s : l)
        if (key == s) return true;
    return false;
}
const StrList *get(const ListMap &m, const std::string &key) {
    for (const auto &e : m)
        if (key == e.first) return &e.second;
    return nullptr;
}
}  // namespace odata

// ---------------------------------------------------------------- JSON as Python's json module writes it
static void json_escape(std::string &o, const std::string &s) {
    o += '"';
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        uint32_t cp = c;
        size_t n = 1;
        if (c >= 0x80) {  // (UTF-8: ensure_ascii writes the code point)
            n = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2;
            cp = c & (n == 4 ? 0x07 : n == 3 ? 0x0F : 0x1F);
            for (size_t k = 1; k < n && i + k < s.size(); k++) cp = cp << 6 | ((unsigned char)s[i + k] & 0x3F);
        }
        i += n;
        char b[16];
        if (cp == '"') o += "\\\"";
        else if (cp == '\\') o += "\\\\";
        else if (cp == '\n') o += "\\n";
        else if (cp == '\r') o += "\\r";
        else if (cp == '\t') o += "\\t";
        else if (cp == '\b') o += "\\b";
        else if (cp == '\f') o += "\\f";
        else if (cp >= 0x20 && cp < 0x7f) o += (char)cp;
        else if (cp >= 0x10000) {
            uint32_t v = cp - 0x10000;
            snprintf(b, sizeof b, "\\u%04x\\u%04x", 0xD800 + (v >> 10), 0xDC00 + (v & 0x3FF));
            o += b;
        } else {
            snprintf(b, sizeof b, "\\u%04x", cp);
            o += b;
        }
    }
    o += '"';
}

static void json_write(std::string &o, const Value &v, int indent, int level) {
    switch (v.type()) {
    case Value::NONE: o += "null"; return;
    case Value::BOOL: o += v.b() ? "true" : "false"; return;
    case Value::INT: o += std::to_string(v.i()); return;
    case Value::FLOAT: {
        double d = v.f();
        if (isnan(d)) o += "NaN";
        else if (isinf(d)) o += d < 0 ? "-Infinity" : "Infinity";
        else o += out::py_float_repr(d);
        return;
    }
    case Value::STR: json_escape(o, v.s()); return;
    case Value::LIST: {
        const List &l = v.list();
        if (l.empty()) { o += "[]"; return; }
        o += '[';
        std::string nl = indent >= 0 ? "\n" + std::string((size_t)(indent * (level + 1)), ' ') : "";
        for (size_t k = 0; k < l.size(); k++) {
            if (k) o += indent >= 0 ? "," : ", ";
            o += nl;
            json_write(o, l[k], indent, level + 1);
        }
        if (indent >= 0) o += "\n" + std::string((size_t)(indent * level), ' ');
        o += ']';
        return;
    }
    case Value::DICT: {
        const Dict &d = v.dict();
        if (d.empty()) { o += "{}"; return; }
        o += '{';
        std::string nl = indent >= 0 ? "\n" + std::string((size_t)(indent * (level + 1)), ' ') : "";
        bool first = true;
        for (const auto &e : d) {
            if (!first) o += indent >= 0 ? "," : ", ";
            first = false;
            o += nl;
            json_escape(o, e.key);
            o += ": ";
            json_write(o, e.value, indent, level + 1);
        }
        if (indent >= 0) o += "\n" + std::string((size_t)(indent * level), ' ');
        o += '}';
        return;
    }
    }
}

std::string py_json(const Value &v, int indent) {
    std::string o;
    json_write(o, v, indent, 0);
    return o;
}

namespace out {

// ---------------------------------------------------------------- small helpers
bool starts(const std::string &s, const std::string &p) { return s.compare(0, p.size(), p) == 0; }
bool ends(const std::string &s, const std::string &p) {
    return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}

std::string base_item(const std::string &item) {
    if (item.empty() || item.back() != ']') return item;
    size_t k = item.size() - 1;
    while (k > 0 && isdigit((unsigned char)item[k - 1])) k--;
    if (k == item.size() - 1 || k == 0 || item[k - 1] != '[') return item;
    return item.substr(0, k - 1);
}

std::vector<std::string> split(const std::string &s, char sep) {
    std::vector<std::string> out;
    size_t a = 0;
    for (;;) {
        size_t b = s.find(sep, a);
        out.push_back(s.substr(a, b == std::string::npos ? std::string::npos : b - a));
        if (b == std::string::npos) return out;
        a = b + 1;
    }
}

std::string fmt(const char *f, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, f);
    vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

std::string py_float_repr(double d) {
    std::string r = Value(d).repr();
    return r;
}

std::string py_g(double d) {  // ('%g': C's, as Python's % formatting)
    return fmt("%g", d);
}

std::string title_case(const std::string &s) {  // (str.title on ASCII: a letter after a non-letter is upper case)
    std::string o = s;
    bool prev = false;
    for (char &ch : o) {
        bool alpha = isalpha((unsigned char)ch);
        if (alpha) ch = prev ? (char)tolower((unsigned char)ch) : (char)toupper((unsigned char)ch);
        prev = alpha;
    }
    return o;
}

std::string set_repr(const std::vector<std::string> &items) {
    if (items.empty()) return "set()";
    std::string o = "{";
    for (size_t k = 0; k < items.size(); k++) o += (k ? ", " : "") + py_repr(items[k]);
    return o + "}";
}

std::string py_str(const Value &v) { return v.is_str() ? v.s() : v.repr(); }

bool Re::match(const std::string &s) const {
    auto it = memo_.find(s);
    if (it != memo_.end()) return it->second;
    bool r = std::regex_search(s, re_);
    memo_.emplace(s, r);
    return r;
}

std::vector<std::string> words(const std::string &s, bool brackets) {
    std::vector<std::string> out;
    size_t i = 0;
    auto head = [](char c) { return isalnum((unsigned char)c) != 0; };
    auto tail = [&](char c) { return isalnum((unsigned char)c) || c == '-' || (brackets && (c == '[' || c == ']')); };
    while (i < s.size()) {
        if (!head(s[i]) || (unsigned char)s[i] >= 0x80) { i++; continue; }
        size_t j = i + 1;
        while (j < s.size() && (unsigned char)s[j] < 0x80 && tail(s[j])) j++;
        out.push_back(s.substr(i, j - i));
        i = j;
    }
    return out;
}

std::set<std::string> str_set(const Value &v) {
    std::set<std::string> out;
    if (v.is_list())
        for (const Value &x : v.list()) out.insert(x.s());
    else if (v.is_dict())
        for (const auto &e : v.dict()) out.insert(e.key);
    return out;
}

std::vector<std::string> sorted_keys(const Value &d) {
    std::vector<std::string> k = d.dict().keys();
    std::sort(k.begin(), k.end());
    return k;
}

std::string door_name_of(const std::string &lv) {
    if (const char *n = odata::get(odata::DOOR_NAME, lv)) return n;
    static const std::regex tail("\\d+(_.*)?$");
    std::string cut = std::regex_replace(lv, tail, "", std::regex_constants::format_first_only);
    if (const char *n = odata::get(odata::DOOR_NAME, cut)) return n;
    return "a cave";
}

// ---------------------------------------------------------------- the logic
World::World(const Value &logic_defs, const Value &settings) : defs(logic_defs) {
    std::string level = settings.get("logic", Value("basic")).s();
    L.reset(new Logic(defs, level, names));
    for (const auto &e : defs.dict()) {
        index[e.key] = (int)keys.size();
        keys.push_back(e.key);
        type.push_back(e.value.get("type").is_str() ? e.value.get("type").s() : "");
    }
    for (const auto &e : settings.dict())
        if (e.value.is_bool() && e.value.b()) start.set(names.id(e.key), 1);
    std::set<std::string> j = str_set(settings.get("excluded-locations")), a = str_set(settings.get("absent-locations"));
    junk.assign(keys.size(), 0);
    for (size_t k = 0; k < keys.size(); k++) junk[k] = j.count(keys[k]) || a.count(keys[k]);
    // readers: every token of a node's conditions (the word regex) and its region
    std::vector<std::vector<int>> rd;
    for (size_t k = 0; k < keys.size(); k++) {
        std::set<int> toks;
        for (const auto &f : defs.dict().at(keys[k]).dict()) {
            if (starts(f.key, "condition-") && f.value.is_str())
                for (const std::string &t : words(f.value.s())) toks.insert(names.id(t));
            if (f.key == "region" && f.value.truthy() && f.value.is_str()) toks.insert(names.id(f.value.s()));
        }
        for (int t : toks) {
            if ((size_t)t >= rd.size()) rd.resize((size_t)t + 1);
            rd[(size_t)t].push_back((int)k);
        }
    }
    readers = std::move(rd);
}

int World::node(const std::string &key) const {
    auto it = index.find(key);
    return it == index.end() ? -1 : it->second;
}

bool World::check_safe(int i, int param, const Access &a) const {
    try {
        return check(i, param, a);
    } catch (const PyError &) {
        return false;
    }
}

bool Reach::has(int i) const { return std::binary_search(locs.begin(), locs.end(), i); }

Reacher::Reacher(World &w, const Value &settings, bool followers) : w_(w), followers_(followers) {
    const Value t = settings.get("triforce-needed", Value(0));
    tri_ = t.truthy() ? (int)t.to_int() : 0;
    tri_id_ = w.id(odata::TRIFORCE);
    for (const char *g : odata::INSTRUMENTS) goal_ids_.push_back(w.id(g));
    goal_ids_.push_back(w.id("kill-nightmare"));
}

int Reacher::base_id(int item) const {
    const std::string &s = w_.names.name(item);
    std::string b = base_item(s);
    return b == s ? item : w_.names.id(b);
}

Reach Reacher::operator()(const std::vector<int> &pl, const std::vector<std::pair<int, int>> &extra) const {
    const_cast<Reacher *>(this)->runs++;
    World &w = w_;
    const int n = (int)w.keys.size();
    Access access = w.start;
    for (const auto &e : extra) access.set(e.first, access.count(e.first) + e.second);
    std::vector<char> seen((size_t)n, 0), queued((size_t)n, 1);
    std::vector<int> todo;
    todo.reserve((size_t)n * 2);
    for (int k = n - 1; k >= 0; k--) todo.push_back(k);
    auto give = [&](int tok) {
        access.set(tok, access.count(tok) + 1);
        if ((size_t)tok < w.readers.size())
            for (int r : w.readers[(size_t)tok])
                if (!queued[(size_t)r] && !seen[(size_t)r]) queued[(size_t)r] = 1, todo.push_back(r);
    };
    const LogicNode *nodes = &w.L->node(0);
    while (!todo.empty()) {
        int k = todo.back();
        todo.pop_back();
        queued[(size_t)k] = 0;
        if (seen[(size_t)k]) continue;
        const LogicNode &nd = nodes[k];
        if (w.type[(size_t)k] == "enemy") {
            try {
                if (w.check(k, 0, access)) {
                    seen[(size_t)k] = 1;
                    for (int t : {nd.id, nd.pit, nd.heavy})
                        if (!access.count(t)) give(t);
                    continue;
                }
                if (nd.cond[C_PIT] >= 0 && !access.count(nd.pit) && w.check(k, 1, access)) give(nd.pit);
                if (nd.cond[C_HEAVY] >= 0 && !access.count(nd.heavy) && w.check(k, 2, access)) give(nd.heavy);
            } catch (const PyError &) {
            }
            continue;
        }
        if (!w.check_safe(k, 0, access)) continue;
        seen[(size_t)k] = 1;
        give(nd.id);
        const std::string &t = w.type[(size_t)k];
        if ((t == "item" || t == "follower") && pl[(size_t)k] >= 0) {
            int item = pl[(size_t)k], b = base_id(item);
            give(item);
            if (b != item) give(b);
        }
    }
    Reach r;
    for (int k = 0; k < n; k++) {
        const std::string &t = w.type[(size_t)k];
        if (seen[(size_t)k] && (t == "item" || (followers_ && t == "follower")) && !w.junk[(size_t)k]) r.locs.push_back(k);
    }
    if (tri_) r.goal = std::min(access.count(tri_id_), tri_);
    else
        for (int g : goal_ids_) r.goal += access.count(g) > 0;
    return r;
}

const std::string *Ctx::file(const std::string &name) const {
    for (const auto &f : res.files)
        if (f.first == name) return &f.second;
    return nullptr;
}

static bool read_file(const std::string &path, std::string &data) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    data.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

bool Ctx::romfs_file(const std::string &rel, std::string &data) const {
    return in.read_game ? in.read_game(rel, data) : read_file(in.romfs + "/" + rel, data);
}

bool Ctx::level_file(const std::string &rel, std::string &data) const {
    return seed_file("romfs/" + rel, data) || romfs_file(rel, data);
}

// ---------------------------------------------------------------- logic.json
static std::string logic_json(const Value &defs) {
    Value out = Value::new_dict();
    for (const auto &e : defs.dict()) {
        if (e.value.get("_check_only").truthy()) continue;
        Value n = Value::new_dict();
        for (const auto &f : e.value.dict())
            if (f.key == "type" || f.key == "subtype" || f.key == "region" || f.key == "content" ||
                starts(f.key, "condition-"))
                n.set(f.key, f.value);
        out.set(e.key, n);
    }
    return py_json(out);
}

// ---------------------------------------------------------------- playthrough.spheres
static Value spheres(Ctx &c, Access &access, std::vector<char> &seen) {
    World &w = *c.w;
    const int n = (int)w.keys.size();
    seen.assign((size_t)n, 0);
    Value out = Value::new_list();
    for (;;) {
        Value found = Value::new_list();
        bool grew = true;
        while (grew) {
            grew = false;
            for (int k = 0; k < n; k++) {
                if (seen[(size_t)k] || w.type[(size_t)k] == "enemy") continue;
                if (!w.check_safe(k, 0, access)) continue;
                seen[(size_t)k] = 1;
                const LogicNode &nd = w.L->node((size_t)k);
                access.add(nd.id);
                const std::string &t = w.type[(size_t)k];
                if (t == "item" || t == "follower") {
                    const Value v = c.in.placements.get(w.keys[(size_t)k]);
                    if (v.truthy()) found.list().push_back(Value(List{Value(w.keys[(size_t)k]), v}));
                }
                grew = true;
            }
            for (int k = 0; k < n; k++) {
                if (w.type[(size_t)k] != "enemy" || seen[(size_t)k]) continue;
                const LogicNode &nd = w.L->node((size_t)k);
                if (w.check(k, 0, access)) {  // (not guarded, as the Python)
                    seen[(size_t)k] = 1;
                    for (int t : {nd.id, nd.pit, nd.heavy}) access.set(t, 1);
                    grew = true;
                    continue;
                }
                if (nd.cond[C_PIT] >= 0 && !access.count(nd.pit) && w.check(k, 1, access)) access.set(nd.pit, 1), grew = true;
                if (nd.cond[C_HEAVY] >= 0 && !access.count(nd.heavy) && w.check(k, 2, access))
                    access.set(nd.heavy, 1), grew = true;
            }
        }
        if (found.list().empty()) break;
        out.list().push_back(found);
        for (const Value &f : found.list()) {
            const std::string &item = f.list()[1].s();
            access.add(w.id(item));
            std::string b = base_item(item);
            if (b != item) access.add(w.id(b));
        }
    }
    return out;
}

// ---------------------------------------------------------------- hint_sheet.steps
static std::vector<int> placements_of(Ctx &c) {
    World &w = *c.w;
    std::vector<int> pl(w.keys.size(), -1);
    for (size_t k = 0; k < w.keys.size(); k++) {
        if (w.type[k] != "item" && w.type[k] != "follower") continue;
        Value v = c.in.placements.get(w.keys[k]);
        if (v.is_str() && !v.s().empty()) pl[k] = w.id(v.s());
    }
    return pl;
}

static std::set<std::string> read_words(const Value &defs, bool brackets, bool strip) {
    std::set<std::string> read;
    for (const auto &e : defs.dict())
        for (const auto &f : e.value.dict())
            if (starts(f.key, "condition-") && f.value.is_str())
                for (const std::string &t : words(f.value.s(), brackets)) read.insert(strip ? base_item(t) : t);
    return read;
}

static std::string item_at(const Value &placements, const std::string &loc) {
    Value v = placements.get(loc);
    return v.is_str() ? v.s() : "";
}

static int hint_steps(Ctx &c) {
    World &w = *c.w;
    const Value &settings = c.in.settings;
    Reacher reach(w, settings, false);
    std::map<std::vector<int>, Reach> memo;
    auto without = [&](std::vector<int> locs) -> Reach {
        std::sort(locs.begin(), locs.end());
        auto it = memo.find(locs);
        if (it != memo.end()) return it->second;
        std::vector<int> pl = c.pl;
        for (int l : locs) pl[(size_t)l] = -1;
        Reach r = reach(pl);
        memo.emplace(locs, r);
        return r;
    };
    Reach full = without({});
    std::map<int, int> sphere_of;  // node -> sphere
    std::vector<std::pair<int, std::string>> order;
    {
        int i = 0;
        for (const Value &s : c.spheres.list()) {
            for (const Value &li : s.list()) {
                int k = w.node(li.list()[0].s());
                sphere_of[k] = i;
            }
            i++;
        }
        for (const Value &s : c.spheres.list())
            for (const Value &li : s.list())
                if (!starts(li.list()[0].s(), "starting-")) order.emplace_back(w.node(li.list()[0].s()), li.list()[1].s());
    }
    auto sphere = [&](int k) { auto it = sphere_of.find(k); return it == sphere_of.end() ? 999 : it->second; };
    std::vector<std::string> gkeys;
    std::map<std::string, std::vector<int>> groups;
    for (const auto &o : order) {
        if (c.hs_junk.match(base_item(o.second))) continue;
        std::string b = base_item(o.second);
        if (!groups.count(b)) gkeys.push_back(b);
        groups[b].push_back(o.first);
    }
    std::set<std::string> goal_items;
    for (const char *g : odata::INSTRUMENTS) goal_items.insert(g);
    goal_items.insert(odata::TRIFORCE);
    std::set<std::string> read = read_words(w.defs, true, true);
    std::map<int, Reach> effect;
    for (const std::string &b : gkeys) {
        const std::vector<int> &locs = groups[b];
        bool any = false;
        for (int l : locs) any = any || read.count(item_at(c.in.placements, w.keys[(size_t)l]));
        if (!read.count(b) && !goal_items.count(b) && !any) continue;
        if (without(locs) == full) continue;
        if (locs.size() == 1) {
            effect[locs[0]] = without(locs);
            continue;
        }
        if (locs.size() > 12 || b == odata::TRIFORCE) {
            size_t lo = 1, hi = locs.size();
            while (lo < hi) {
                size_t mid = (lo + hi) / 2;
                if (without(std::vector<int>(locs.begin() + (long)mid, locs.end())) == full) hi = mid;
                else lo = mid + 1;
            }
            for (size_t i = 0; i < lo; i++) effect[locs[i]] = without(std::vector<int>(locs.begin() + (long)i, locs.end()));
            continue;
        }
        std::vector<int> single;
        for (int l : locs)
            if (without({l}) != full) single.push_back(l);
        for (int l : single) effect[l] = without({l});
        if (single.empty()) effect[locs[0]] = without(locs);
    }
    struct Step {
        int loc;
        std::string item;
        int n, order;
        Value region;
        size_t opens;
        bool goal;
        std::vector<int> lost;
        std::vector<std::string> needs;
        size_t unlocks = 0;
        int first = 999;
        std::vector<int> example;
    };
    std::vector<Step> out;
    std::map<std::string, int> count;
    for (size_t i = 0; i < order.size(); i++) {
        std::string b = base_item(order[i].second);
        int nb = ++count[b];
        auto e = effect.find(order[i].first);
        if (e == effect.end()) continue;
        Step st;
        st.loc = order[i].first, st.item = b, st.n = nb, st.order = (int)i;
        const Value &node = w.defs.at(w.keys[(size_t)st.loc]);
        st.region = node.get("spoiler-region", Value(""));
        std::set_difference(full.locs.begin(), full.locs.end(), e->second.locs.begin(), e->second.locs.end(),
                            std::back_inserter(st.lost));
        st.opens = st.lost.size();
        st.goal = e->second.goal < full.goal;
        out.push_back(st);
    }
    auto gated = [](const std::string &it) { return odata::in(odata::GATED, it); };
    for (const char *g : odata::GATED) {
        auto gi = groups.find(g);
        if (gi == groups.end()) continue;
        const std::vector<int> &locs = gi->second;
        bool any = false;
        for (int l : locs) any = any || effect.count(l);
        if (!any) continue;
        std::vector<int> pl = c.pl;
        std::vector<std::pair<int, int>> others;  // (item id, count) in first-seen order
        std::map<int, size_t> at;
        auto bump = [&](int id) {
            auto it = at.find(id);
            if (it == at.end()) at[id] = others.size(), others.emplace_back(id, 1);
            else others[it->second].second++;
        };
        for (const auto &o : order)
            if (base_item(o.second) != g) {
                pl[(size_t)o.first] = -1;
                bump(w.id(o.second));
                if (base_item(o.second) != o.second) bump(w.id(base_item(o.second)));
            }
        for (int l : locs) pl[(size_t)l] = -1;
        Reach reach_g = reach(pl, others);
        for (Step &st : out) {
            if (gated(st.item) || !full.has(st.loc) || reach_g.has(st.loc)) continue;
            size_t lo = 1, hi = locs.size();
            int gid = w.id(g);
            while (lo < hi) {
                size_t mid = (lo + hi) / 2;
                std::vector<std::pair<int, int>> ex = others;
                ex.emplace_back(gid, (int)mid);  // (others never holds g itself)
                if (reach(pl, ex).has(st.loc)) hi = mid;
                else lo = mid + 1;
            }
            st.needs.push_back(std::string(g) + ":" + std::to_string(lo));
        }
    }
    std::set<int> keep;
    for (const Step &st : out) keep.insert(st.loc);
    for (;;) {
        std::set<std::string> needed;
        for (const Step &st : out)
            if (keep.count(st.loc) && !gated(st.item))
                for (const std::string &nd : st.needs) needed.insert(nd.substr(0, nd.find(':')));
        std::set<int> prog;
        for (const Step &st : out)
            if (keep.count(st.loc) && (!gated(st.item) || needed.count(st.item))) prog.insert(st.loc);
        std::set<int> nk;
        for (const Step &st : out) {
            if (!prog.count(st.loc)) continue;
            bool more = false;
            for (int l : st.lost) more = more || (l != st.loc && prog.count(l));
            if (st.goal || more) nk.insert(st.loc);
        }
        if (nk == keep) break;
        keep = nk;
    }
    std::vector<Step> kept;
    for (Step &st : out)
        if (keep.count(st.loc)) kept.push_back(st);
    for (Step &st : kept) {
        std::vector<int> behind;
        for (int l : st.lost)
            if (keep.count(l) && l != st.loc) behind.push_back(l);
        std::sort(behind.begin(), behind.end(), [&](int a, int b) {
            int sa = sphere(a), sb = sphere(b);
            return sa != sb ? sa < sb : w.keys[(size_t)a] < w.keys[(size_t)b];
        });
        st.unlocks = behind.size();
        st.first = behind.empty() ? 999 : sphere(behind[0]);
        st.example.assign(behind.begin(), behind.begin() + (long)std::min<size_t>(3, behind.size()));
    }
    std::stable_sort(kept.begin(), kept.end(), [&](const Step &a, const Step &b) {
        bool ga = gated(a.item), gb = gated(b.item);
        if (ga != gb) return !ga;
        bool ua = !a.unlocks, ub = !b.unlocks;
        if (ua != ub) return !ua;
        if (a.first != b.first) return a.first < b.first;
        if (a.unlocks != b.unlocks) return a.unlocks > b.unlocks;
        return a.order < b.order;
    });
    Value steps = Value::new_list();
    for (const Step &st : kept) {
        Value d = Value::new_dict();
        d.set("loc", w.keys[(size_t)st.loc]);
        d.set("item", st.item);
        d.set("n", st.n);
        d.set("region", st.region);
        d.set("opens", (long long)st.opens);
        d.set("goal", st.goal);
        d.set("order", st.order);
        if (!st.needs.empty()) {
            Value nl = Value::new_list();
            for (const std::string &s : st.needs) nl.list().push_back(s);
            d.set("needs", nl);
        }
        d.set("unlocks", (long long)st.unlocks);
        d.set("first", st.first);
        Value ex = Value::new_list();
        for (int l : st.example) ex.list().push_back(w.keys[(size_t)l]);
        d.set("unlocks-example", ex);
        steps.list().push_back(d);
    }
    c.steps = steps;
    Value doc = Value::new_dict();
    doc.set("steps", steps);
    Value de = c.in.placements.get("dungeon-entrances");
    doc.set("dungeon-entrances", de.truthy() ? de.copy() : Value::new_dict());
    c.write("hint_steps.json", py_json(doc, 1));
    size_t goal_only = 0;
    for (const Step &st : kept) goal_only += !st.unlocks;
    c.res.log.push_back(fmt("hint sheet: %zu steps (%zu for the goal only) from %zu items, %ld playthroughs", kept.size(),
                            goal_only, order.size(), reach.runs));
    return (int)kept.size();
}

// ---------------------------------------------------------------- route.build
static void route(Ctx &c) {
    World &w = *c.w;
    const Value &placements = c.in.placements;
    std::set<int> route;
    for (const Value &st : c.steps.list()) {
        const std::string &loc = st.at("loc").s();
        if (placements.get(loc).truthy()) route.insert(w.node(loc));
    }
    Reacher reach(w, c.in.settings, true);
    Reach full = reach(c.pl);
    std::vector<int> order;
    for (const Value &s : c.spheres.list())
        for (const Value &li : s.list())
            if (!starts(li.list()[0].s(), "starting-")) order.push_back(w.node(li.list()[0].s()));
    // only(locs): a str item stays where the route takes it (or a starting item); others become None
    auto only = [&](const std::set<int> &locs) {
        std::vector<int> pl = c.pl;
        for (size_t k = 0; k < pl.size(); k++)
            if (pl[k] >= 0 && !locs.count((int)k) && !starts(w.keys[k], "starting-")) pl[k] = -1;
        return pl;
    };
    std::set<std::string> read = read_words(w.defs, false, false);
    auto subset = [](const std::set<int> &a, const Reach &r) {
        for (int x : a)
            if (!r.has(x)) return false;
        return true;
    };
    auto complete = [&](const std::set<int> &locs) {
        Reach r = reach(only(locs));
        return r.goal >= full.goal && subset(locs, r);
    };
    std::vector<int> added;
    bool stuck = false;
    long last = -1;
    for (int it = 0; it < 300; it++) {
        Reach got = reach(only(route));
        if (got.goal >= full.goal && subset(route, got)) break;
        long base = (long)got.locs.size() + 1000L * got.goal;
        if (base > last) stuck = false;
        last = base;
        std::vector<int> cand;
        for (int loc : order)
            if (!route.count(loc) && got.has(loc) &&
                !c.hs_junk.match(base_item(item_at(placements, w.keys[(size_t)loc]))))
                cand.push_back(loc);
        int pick = -1;
        if (!stuck) {
            for (int loc : cand) {
                std::set<int> r2 = route;
                r2.insert(loc);
                Reach g2 = reach(only(r2));
                if ((long)g2.locs.size() + 1000L * g2.goal > base) {
                    pick = loc;
                    break;
                }
            }
            stuck = pick < 0;
        }
        if (pick < 0)
            for (int loc : cand)
                if (read.count(base_item(item_at(placements, w.keys[(size_t)loc])))) { pick = loc; break; }
        if (pick < 0) break;
        route.insert(pick);
        added.push_back(pick);
    }
    if (complete(route)) {
        std::vector<int> rev(added.rbegin(), added.rend());
        for (int loc : rev) {
            std::set<int> r2 = route;
            r2.erase(loc);
            if (complete(r2)) {
                route.erase(loc);
                added.erase(std::find(added.begin(), added.end(), loc));
            }
        }
    }
    std::map<int, int> sphere_of;
    {
        int i = 0;
        for (const Value &s : c.spheres.list()) {
            for (const Value &li : s.list()) sphere_of[w.node(li.list()[0].s())] = i;
            i++;
        }
    }
    auto sphere = [&](int k) { auto it = sphere_of.find(k); return it == sphere_of.end() ? 999 : it->second; };
    auto by_sphere = [&](int a, int b) {
        int sa = sphere(a), sb = sphere(b);
        return sa != sb ? sa < sb : w.keys[(size_t)a] < w.keys[(size_t)b];
    };
    Value dungeon_of = c.in.settings.get("dungeon-of");
    if (!dungeon_of.truthy()) dungeon_of = Value::new_dict();
    auto area = [&](int k) -> std::string {
        const std::string &loc = w.keys[(size_t)k];
        Value d = dungeon_of.get(loc);
        if (d.truthy()) return py_str(d);
        if (loc.size() >= 3 && loc[0] == 'D' && isdigit((unsigned char)loc[1]) && loc[2] == '-') return loc.substr(0, 2);
        Value r = w.defs.at(loc).get("spoiler-region", Value(""));
        return py_str(r);
    };
    std::set<int> got;
    bool have_here = false;
    std::string here;
    std::vector<std::pair<std::string, std::vector<int>>> outv;
    for (;;) {
        Reach r = reach(only(got));
        std::vector<int> can;
        for (int loc : r.locs)
            if (route.count(loc) && !got.count(loc)) can.push_back(loc);
        if (can.empty()) break;
        std::vector<int> now;
        for (int loc : can)
            if (have_here && area(loc) == here) now.push_back(loc);
        if (now.empty()) {
            int first = *std::min_element(can.begin(), can.end(), by_sphere);
            here = area(first), have_here = true;
            for (int loc : can)
                if (area(loc) == here) now.push_back(loc);
        }
        std::sort(now.begin(), now.end(), by_sphere);
        if (!outv.empty() && outv.back().first == here) outv.back().second.insert(outv.back().second.end(), now.begin(), now.end());
        else outv.emplace_back(here, now);
        got.insert(now.begin(), now.end());
    }
    std::vector<int> left;
    for (int loc : route)
        if (!got.count(loc)) left.push_back(loc);
    std::sort(left.begin(), left.end(), by_sphere);
    if (!left.empty()) {
        outv.emplace_back("", left);
        c.res.log.push_back("route: not reachable in order (listed last)");
    }
    Value sph = Value::new_list();
    size_t stops = 0;
    for (const auto &o : outv) {
        Value s = Value::new_list();
        for (int loc : o.second) s.list().push_back(Value(List{Value(w.keys[(size_t)loc]), placements.at(w.keys[(size_t)loc])}));
        stops += o.second.size();
        sph.list().push_back(s);
    }
    Value ad = Value::new_list();
    for (int loc : added) ad.list().push_back(w.keys[(size_t)loc]);
    Value doc = Value::new_dict();
    doc.set("spheres", sph);
    doc.set("added", ad);
    c.write("route.json", py_json(doc));
    c.res.log.push_back(fmt("route: %zu added, %zu stops", added.size(), stops));
}

// ---------------------------------------------------------------- seed_hints.pick
static bool re_dungeon_loc(const std::string &loc, std::string &dn, std::string &rest) {  // ^(D\d)-(.*)$
    if (loc.size() < 3 || loc[0] != 'D' || !isdigit((unsigned char)loc[1]) || loc[2] != '-') return false;
    dn = loc.substr(0, 2), rest = loc.substr(3);
    return rest.find('\n') == std::string::npos;
}

// hint_sheet.check_dungeon: 'D1'... when the check is in a dungeon (its room, its pots), else ""
static std::string check_dungeon(const std::string &loc, const Value &dungeon_of) {
    static const std::regex pot("^pot-([A-Za-z0-9]+)-\\d\\d[A-P]-\\d+$");
    std::smatch m;
    if (std::regex_search(loc, m, pot)) {
        if (const char *d = odata::get(odata::LEVEL_DUNGEON, m[1].str())) {
            Value v = dungeon_of.get(loc);
            return v.is_none() && !dungeon_of.has(loc) ? std::string(d) : py_str(v);
        }
    }
    if (loc.size() >= 3 && loc[0] == 'D' && isdigit((unsigned char)loc[1]) && loc[2] == '-' && !odata::in(odata::HS_PLACE, loc))
        return dungeon_of.has(loc) ? py_str(dungeon_of.at(loc)) : loc.substr(0, 2);
    return "";
}

// field_regions.tile_zones: {Field room: [80 zone ids]} from Field.lvb's grid
static std::map<std::string, std::vector<int64_t>> tile_zones(Ctx &c) {
    std::map<std::string, std::vector<int64_t>> out;
    std::string data;
    if (!c.romfs_file("region_common/level/Field/Field.lvb", data)) return out;
    FixedHash fh = FixedHash::parse(data);
    const FhEntry *ze = fh.find("zone"), *ge = fh.find("grid"), *ce = fh.find("gridConfig");
    std::vector<int64_t> zones;
    for (const FhEntry &z : ze->sub->entries) zones.push_back((int64_t)read_le(z.data, 0, 4));
    const std::string &grid = ge->data;
    int gh = (int)read_le(ce->data, 0, 2), gw = (int)read_le(ce->data, 2, 2);
    for (int row = 0; row < 16; row++)
        for (int col = 0; col < 16; col++) {
            std::vector<int64_t> zs;
            for (int z = 0; z < 8; z++)
                for (int x = 0; x < 10; x++) {
                    int gx = col * 10 + x, gz = row * 8 + z;
                    int zi = gz < gh && gx < gw ? (unsigned char)grid[(size_t)((gz * gw + gx) * 8 + 5)] : 255;
                    zs.push_back(zi < (int)zones.size() ? zones[(size_t)zi] : -1);
                }
            out[fmt("Field_%02d%c", row + 1, 65 + col)] = zs;
        }
    return out;
}

// hint_sheet.door_area
static std::string door_area(Ctx &c, const Value &door) {
    if (!c.zones_loaded) c.zones_cache = tile_zones(c), c.zones_loaded = true;
    const std::map<std::string, std::vector<int64_t>> &zones_cache = c.zones_cache;
    const std::string room = door.at("field_room").s();
    double x = door.at("pos_xz").list()[0].f(), z = door.at("pos_xz").list()[1].f();
    int col = room.back() - 65, row = atoi(room.substr(room.size() - 3, 2).c_str());
    long tx = std::min(9L, std::max(0L, (long)((x - col * 15) / 1.5)));
    long tz = std::min(7L, std::max(0L, (long)((z - (row - 1) * 12) / 1.5)));
    auto it = zones_cache.find(room);
    std::string region = door.get("outer_region").is_str() ? door.get("outer_region").s() : "";
    if (it != zones_cache.end()) {
        int64_t zone = it->second[(size_t)(tz * 10 + tx)];
        for (const auto &e : odata::ZONE_AREA)
            if (e.first == zone) return e.second;
    }
    const char *a = odata::get(odata::REGION_AREA, region);
    return a ? a : "mabe-village";
}

// hint_sheet.door_of: the door of a shuffled interior / connector cave holding the check (None: Value())
static Value door_of(Ctx &c, const std::string &loc) {
    const Value &ent = c.in.ent;
    if (!ent.truthy()) return Value();
    const List &e = ent.list();
    const Value &doors = e[0], &entry = e[1];
    Value node = c.w->defs.get(loc);
    std::string region = node.truthy() && node.get("region").is_str() ? node.get("region").s() : "";
    std::set<std::string> targets;
    if (starts(region, "inside-")) targets.insert(region.substr(7));
    else if (starts(region, "conn-")) {
        for (const Value &d : doors.list())
            if (d.get("connector").truthy() &&
                "conn-" + py_str(d.at("connector")) + "-" + py_str(d.at("node")) == region)
                targets.insert(d.at("id").s());
    } else
        return Value();
    std::vector<std::string> ways;
    for (const auto &kv : entry.dict())
        if (kv.value.is_str() && targets.count(kv.value.s())) ways.push_back(kv.key);
    std::sort(ways.begin(), ways.end());
    if (ways.empty() || targets.count(ways[0])) return Value();
    for (const Value &d : doors.list())
        if (d.at("id").s() == ways[0]) return d;
    throw PyError("KeyError", py_repr(ways[0]));
}

static std::string area_key(const std::string &r) { return starts(r, "taltal-mountains") ? "taltal-mountains" : r; }

static void seed_hints_pick(Ctx &c, const Value &layouts) {
    World &w = *c.w;
    const Value &settings = c.in.settings, &placements = c.in.placements;
    std::set<std::string> step_locs;
    for (const Value &s : c.steps.list()) step_locs.insert(s.at("loc").s());
    PyRandom rng(c.seed + "-hints");
    std::set<std::string> excluded = str_set(settings.get("excluded-locations")), absent = str_set(settings.get("absent-locations"));
    excluded.insert(absent.begin(), absent.end());
    Value tv = settings.get("triforce-needed", Value(0));
    int tri = tv.truthy() ? (int)tv.to_int() : 0;
    std::set<std::string> goal;
    if (tri) goal.insert(odata::TRIFORCE);
    else
        for (const char *g : odata::INSTRUMENTS) goal.insert(g);
    std::set<std::string> read = read_words(w.defs, true, true);
    std::set<std::string> gated;
    for (const Value &s : c.steps.list())
        if (odata::in(odata::GATED, s.at("item").s())) gated.insert(s.at("item").s());
    auto useful = [&](const std::string &item) {
        std::string b = base_item(item);
        if (c.hs_junk.match(b)) return false;
        for (const char *p : odata::SH_NOT_PROGRESS)
            if (starts(b, std::string(p) + "-")) return false;
        if (odata::in(odata::GATED, b)) return gated.count(b) != 0;
        return read.count(b) || goal.count(b) || b == odata::TRIFORCE;
    };
    Value dungeon_of = settings.get("dungeon-of", Value::new_dict());
    if (!dungeon_of.truthy()) dungeon_of = Value::new_dict();
    // _areas
    std::vector<std::pair<std::string, std::string>> area_of;  // loc -> area, in logic order
    std::map<std::string, std::string> area_map;
    for (size_t k = 0; k < w.keys.size(); k++) {
        const std::string &loc = w.keys[k];
        if (w.type[k] != "item" || starts(loc, "starting-") || absent.count(loc) || !placements.get(loc).truthy()) continue;
        std::string dn = check_dungeon(loc, dungeon_of);
        std::string a;
        if (!dn.empty()) a = dn;
        else {
            Value door = door_of(c, loc);
            if (door.truthy()) a = door_area(c, door);
            else {
                Value sr = w.defs.at(loc).get("spoiler-region");
                a = sr.truthy() ? py_str(sr) : "";
            }
            if (a.empty()) continue;
            a = area_key(a);
        }
        area_of.emplace_back(loc, a);
        area_map[loc] = a;
    }
    std::map<std::string, std::vector<std::string>> locs_in;  // (read sorted by area only)
    for (const auto &e : area_of) locs_in[e.second].push_back(e.first);
    std::map<std::string, size_t> real;
    for (const auto &e : locs_in) {
        size_t n = 0;
        for (const std::string &l : e.second) n += !excluded.count(l);
        real[e.first] = n;
    }
    Reacher reach(w, settings, false);
    Reach full = reach(c.pl);
    auto own_dungeon_item = [](const std::string &item, const std::string &area) {
        static const std::regex re("^(key|nightmare-key|compass|map|stone-beak)-(D\\d)$");
        std::smatch m;
        std::string b = base_item(item);
        return std::regex_search(b, m, re) && m[2].str() == area;
    };
    std::vector<std::string> barren, needed;
    for (const auto &e : locs_in) {  // (sorted)
        const std::string &a = e.first;
        if (real[a] < (size_t)odata::MIN_CHECKS) continue;
        bool any = false;
        for (const std::string &l : e.second) any = any || useful(item_at(placements, l));
        if (!any) { barren.push_back(a); continue; }
        std::vector<int> pl = c.pl;
        for (const std::string &l : e.second) {
            std::string it = item_at(placements, l);
            if (!goal.count(base_item(it)) && !own_dungeon_item(it, a)) pl[(size_t)w.node(l)] = -1;
        }
        if (reach(pl).goal < full.goal) needed.push_back(a);
    }
    std::vector<Value> quests;
    std::set<std::string> trade;
    for (const char *t : odata::TRADE_GIFT_LOCATIONS) trade.insert(t);
    for (const auto &q : odata::QUESTS) {
        std::string qn = q.first;
        std::vector<std::string> ls;
        for (const auto &e : area_map) {
            const std::string &l = e.first;
            if (excluded.count(l)) continue;
            bool hit = false;
            if (qn == "trade") hit = trade.count(l) != 0;
            else
                for (const char *p : q.second) hit = hit || (p[0] == '-' ? l.find(p) != std::string::npos : starts(l, p));
            if (hit) ls.push_back(l);
        }
        if (ls.empty()) continue;
        std::vector<std::string> good;
        for (const std::string &l : ls)
            if (odata::in(odata::GOOD, base_item(item_at(placements, l))) && !step_locs.count(l)) good.push_back(l);
        bool any = false;
        for (const std::string &l : ls) any = any || useful(item_at(placements, l));
        Value h = Value::new_dict();
        h.set("kind", "quest"), h.set("quest", qn);
        if (any) h.set("says", "need");
        else if (!good.empty()) {
            h.set("says", "item"), h.set("item", base_item(item_at(placements, good[0]))), h.set("loc", good[0]);
        } else
            h.set("says", "junk");
        quests.push_back(h);
    }
    std::vector<Value> goods;
    for (const auto &e : area_map) {
        const std::string &loc = e.first;
        std::string it = base_item(item_at(placements, loc));
        if (odata::in(odata::GOOD, it) && !step_locs.count(loc) && !excluded.count(loc) && !useful(it)) {
            Value door = door_of(c, loc);
            Value g = Value::new_dict();
            g.set("kind", "item"), g.set("item", it), g.set("area", e.second), g.set("loc", loc);
            g.set("door", door.truthy() ? door.at("id") : Value());
            goods.push_back(g);
        }
    }
    std::map<std::string, int> dungeon_steps;
    for (const Value &s : c.steps.list()) {
        std::string dn = check_dungeon(s.at("loc").s(), dungeon_of);
        if (!dn.empty()) dungeon_steps[dn]++;
    }
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> statues;
    std::map<std::string, std::string> where_now;
    if (layouts.truthy())
        for (const auto &L : layouts.dict())
            for (const auto &cr : L.value.dict()) where_now[cr.value.s()] = cr.key;
    for (const auto &e : odata::DUNGEON_OWLS) {
        std::string room = e.second, level = room.substr(0, room.rfind('_'));
        std::string cell;
        if (layouts.truthy() && layouts.has(level)) {
            auto it = where_now.find(room);
            if (it != where_now.end()) cell = it->second;
        } else
            cell = room;
        if (!cell.empty()) {
            const char *dn = odata::get(odata::LEVEL_DUNGEON, cell.substr(0, cell.rfind('_')));
            if (!dn) throw PyError("KeyError", py_repr(cell.substr(0, cell.rfind('_'))));
            statues[dn].emplace_back(e.first, cell);
        }
    }
    std::map<std::string, Value> bosses;
    for (const auto &e : area_map) {  // (sorted)
        std::string dn, rest;
        if (!re_dungeon_loc(e.first, dn, rest) || !odata::in(odata::BOSSES, rest)) continue;
        auto st = statues.find(e.second);
        if (st == statues.end() || st->second.size() <= 1) continue;
        std::string it = base_item(item_at(placements, e.first));
        std::string says = useful(it) ? "need" : odata::in(odata::GOOD, it) && !step_locs.count(e.first) ? "item" : "junk";
        Value b = Value::new_dict();
        b.set("kind", "boss"), b.set("dungeon", e.second), b.set("says", says), b.set("loc", e.first);
        if (says == "item") b.set("item", it);
        bosses[e.second] = b;
    }
    std::set<std::string> named;
    for (const Value &h : quests)
        if (h.get("says") == Value("item")) named.insert(h.at("loc").s());
    for (const auto &b : bosses)
        if (b.second.get("says") == Value("item")) named.insert(b.second.at("loc").s());
    {
        std::vector<Value> g2;
        for (const Value &g : goods)
            if (!named.count(g.at("loc").s())) g2.push_back(g);
        goods = g2;
    }
    rng.shuffle(barren);
    rng.shuffle(needed);
    rng.shuffle(quests);
    std::map<std::string, std::vector<Value>> by_item;
    for (const Value &g : goods) by_item[g.at("item").s()].push_back(g);
    std::vector<std::string> kinds;
    for (const auto &e : by_item) kinds.push_back(e.first);
    rng.shuffle(kinds);
    for (const std::string &k : kinds) rng.shuffle(by_item[k]);
    size_t most = 0;
    for (const auto &e : by_item) most = std::max(most, e.second.size());
    goods.clear();
    for (size_t i = 0; i < most; i++)
        for (const std::string &k : kinds)
            if (i < by_item[k].size()) goods.push_back(by_item[k][i]);
    std::map<std::string, std::vector<Value>> pools;
    for (const std::string &a : barren) {
        Value h = Value::new_dict();
        h.set("kind", "barren"), h.set("area", a);
        pools["barren"].push_back(h);
    }
    for (size_t i = 0; i < needed.size() && i < 3; i++) {
        Value h = Value::new_dict();
        h.set("kind", "needed"), h.set("area", needed[i]);
        pools["needed"].push_back(h);
    }
    pools["quest"] = quests;
    pools["item"] = goods;
    auto draw = [&](const std::vector<std::string> &ks) -> Value {
        for (const std::string &k : ks) {
            std::vector<Value> &p = pools[k];
            if (!p.empty()) {
                Value h = p.front();
                p.erase(p.begin());
                return h;
            }
        }
        return Value();
    };
    auto with = [](const Value &h, const std::vector<std::pair<std::string, Value>> &kv) {
        Value d = h.copy();
        for (const auto &e : kv) d.set(e.first, e.second);
        return d;
    };
    Value out = Value::new_list();
    const std::vector<std::string> order = {"barren", "quest", "needed", "item"};
    for (size_t i = 0; i < odata::FIELD_OWLS.size(); i++) {
        std::string want = std::vector<std::string>{"barren", "quest", i % 2 ? "needed" : "item"}[i % 3];
        std::vector<std::string> ks = {want};
        for (const std::string &k : order)
            if (k != want) ks.push_back(k);
        Value h = draw(ks);
        if (h.truthy()) out.list().push_back(with(h, {{"where", std::string("field:") + odata::FIELD_OWLS[i]}}));
    }
    for (int i = 0; i < odata::PHONES; i++) {
        std::vector<std::string> ks(order.begin() + i % 4, order.end());
        ks.insert(ks.end(), order.begin(), order.begin() + i % 4);
        Value h = draw(ks);
        if (h.truthy()) out.list().push_back(with(h, {{"where", "phone:" + std::to_string(i)}}));
    }
    for (const auto &st : statues) {
        const std::string &dn = st.first;
        for (size_t i = 0; i < st.second.size(); i++) {
            std::vector<std::pair<std::string, Value>> at = {{"where", "dungeon:" + st.second[i].first},
                                                             {"room", st.second[i].second}};
            if (i == 0) {
                Value h = Value::new_dict();
                h.set("kind", "count"), h.set("dungeon", dn), h.set("n", dungeon_steps.count(dn) ? dungeon_steps[dn] : 0);
                out.list().push_back(with(h, at));
            } else if (bosses.count(dn)) {
                out.list().push_back(with(bosses[dn], at));
                bosses.erase(dn);
            } else {
                Value h = draw(order);
                if (h.truthy()) out.list().push_back(with(h, at));
            }
        }
    }
    Value entered = Value::new_dict();
    Value de = placements.get("dungeon-entrances");
    if (de.truthy())
        for (const auto &e : de.dict()) entered.set(py_str(e.value), e.key);
    Value areas = Value::new_dict();
    for (const auto &e : locs_in) {
        std::vector<std::string> ls = e.second;
        std::sort(ls.begin(), ls.end());
        Value l = Value::new_list();
        for (const std::string &s : ls) l.list().push_back(s);
        areas.set(e.first, l);
    }
    Value doc = Value::new_dict();
    doc.set("hints", out);
    doc.set("entered-via", entered);
    doc.set("areas", areas);
    c.write("seed_hints.json", py_json(doc, 1));
    c.res.log.push_back(fmt("hints: %zu hints", out.list().size()));
}

// ---------------------------------------------------------------- LAS's spoiler (RandomizerCore/spoiler.py)
static const char *const kSpoilerRegions[] = {
    "mabe-village", "toronbo-shores", "mysterious-woods", "koholint-prairie", "tabahl-wasteland", "ukuku-prairie",
    "sign-maze", "goponga-swamp", "taltal-heights", "marthas-bay", "kanalet-castle", "pothole-field", "animal-village",
    "yarna-desert", "ancient-ruins", "rapids-ride", "taltal-mountains-east", "taltal-mountains-west", "color-dungeon",
    "tail-cave", "bottle-grotto", "key-cavern", "angler-tunnel", "catfish-maw", "face-shrine", "eagle-tower",
    "turtle-rock"};
static const char *const kSetKeys[] = {"excluded-locations", "absent-locations", "junk-locations"};  // (Python sets)

static std::string setting_str(const std::string &key, const Value &v) {
    for (const char *k : kSetKeys)
        if (key == k && v.is_list()) {
            std::vector<std::string> items;
            for (const Value &x : v.list()) items.push_back(x.s());
            return set_repr(items);
        }
    return py_str(v);
}

static std::string las_spoiler(const Value &placements, const Value &logic_defs) {
    std::map<std::string, std::vector<std::string>> regions;
    for (const char *r : kSpoilerRegions) regions[r];
    for (const auto &e : logic_defs.dict()) {
        if (starts(e.key, "starting-item")) continue;
        std::string t = e.value.get("type").is_str() ? e.value.get("type").s() : "";
        if (t != "item" && t != "follower") continue;
        std::string r = py_str(e.value.at("spoiler-region"));
        auto it = regions.find(r);
        if (it == regions.end()) throw PyError("KeyError", py_repr(r));
        it->second.push_back(e.key);
    }
    const Value &settings = placements.at("settings");
    std::string o = "settings:\n";
    for (const std::string &k : sorted_keys(settings))
        if (k != "excluded-locations" && k != "starting-items" && k != "logic")
            o += "    " + k + ":  " + setting_str(k, settings.at(k)) + "\n";
    o += "\nstarting-items:\n";
    std::vector<std::string> items;
    for (const Value &x : placements.at("starting-items").list()) items.push_back(py_str(x));
    std::sort(items.begin(), items.end());
    for (const std::string &i : items) o += "    " + i + "\n";
    o += "\nexcluded-locations:\n";
    std::vector<std::string> junk;
    for (const Value &x : placements.at("force-junk").list()) junk.push_back(py_str(x));
    std::sort(junk.begin(), junk.end());
    for (const std::string &j : junk) o += "    " + j + "\n";
    o += "\ndungeon-entrances:\n";
    for (const auto &e : placements.at("dungeon-entrances").dict()) o += "    " + e.key + " -> " + py_str(e.value) + "\n";
    o += "\n";
    const Value &indexes = placements.at("indexes");
    for (const char *r : kSpoilerRegions) {
        o += std::string(r) + ":\n";
        for (const std::string &loc : regions[r]) {
            std::string item = py_str(placements.at(loc));
            int64_t index = indexes.has(loc) ? indexes.at(loc).i() : -1;
            std::string idx = index > -1 ? "[" + std::to_string(index) + "]" : "";
            if (starts(loc, "starting-dungeon-item")) continue;
            if (ends(item, "trap")) item = "trap";
            o += "    " + loc + ":  " + item + idx + "\n";
        }
    }
    return o;
}

// ---------------------------------------------------------------- settings.json
static Value final_settings(const OutputsInput &in) {
    // the fill's settings, as LAS's ModsProcess leaves them (ASM/assemble.py: stealing always / never become True /
    // False), then the final ones: the fill's keys keep its order (and its value where the final one is the same:
    // dicts keep the fill's order), keys added later follow
    Value s = in.settings.copy();
    if (s.get("stealing") == Value("always")) s.set("stealing", true);
    else if (s.get("stealing") == Value("never")) s.set("stealing", false);
    if (!in.final_settings.truthy()) return s;
    const Value &fin = in.final_settings;
    auto same = [](const std::string &k, const Value &a, const Value &b) {
        for (const char *sk : kSetKeys)
            if (k == sk) return str_set(a) == str_set(b);
        return a == b;
    };
    Value out = Value::new_dict();
    for (const auto &e : s.dict()) {
        if (!fin.has(e.key)) continue;
        out.set(e.key, same(e.key, e.value, fin.at(e.key)) ? e.value : fin.at(e.key));
    }
    for (const auto &e : fin.dict())
        if (!out.has(e.key)) out.set(e.key, e.value);
    return out;
}

static Value sorted_list(const Value &v) {
    std::vector<std::string> l;
    if (v.is_list())
        for (const Value &x : v.list()) l.push_back(x.s());
    std::sort(l.begin(), l.end());
    Value o = Value::new_list();
    for (const std::string &x : l) o.list().push_back(x);
    return o;
}

static Value settings_json(const OutputsInput &in) {
    Value s = final_settings(in);
    s.set("excluded-locations", sorted_list(s.get("excluded-locations")));
    s.set("absent-locations", sorted_list(s.get("absent-locations")));
    s.set("junk-locations", sorted_list(s.get("junk-locations")));
    s.set("escape-bottle", true);
    Value de = in.placements.get("dungeon-entrances");
    s.set("dungeon-entrances", de.truthy() ? de.copy() : Value::new_dict());
    s.dict().erase("mixed-dungeon-ways");
    s.dict().erase("dungeon-exits");
    s.set("hint-sheet", true);
    // (pets.enabled: the Ghost and Marin as items)
    s.set("ghost-item", s.get("pet-ghost").truthy());
    s.set("marin-stays", !s.get("open-desert", Value(true)).truthy());
    s.set("underwater-items", true);
    s.set("silent-pickups", true);
    long shells = 0;
    for (const auto &e : in.placements.dict()) shells += e.value == Value("seashell");
    s.set("seashell-total", (long long)(shells > 0 && shells <= 50 ? shells : 50));
    return s;
}

}  // namespace out

// ---------------------------------------------------------------- the whole run
struct OutputsBuilder::Impl {
    OutputsInput in;
    Outputs res;
    out::Ctx c;
    std::string spoiler;
    bool has_spoiler = false, first_done = false;
    explicit Impl(OutputsInput i) : in(std::move(i)), c(in, res) {}
};

OutputsBuilder::OutputsBuilder(OutputsInput in) : d_(new Impl(std::move(in))) {}
OutputsBuilder::~OutputsBuilder() {}
Outputs &OutputsBuilder::result() { return d_->res; }

Outputs &OutputsBuilder::before_build() {
    using namespace out;
    const OutputsInput &in = d_->in;
    Outputs &res = d_->res;
    Ctx &c = d_->c;
    d_->first_done = true;
    c.seed = py_str(in.settings.at("seed"));
    // LAS's spoiler: written by the fill (shuffler.run) when create-spoiler is on
    std::string &spoiler = d_->spoiler;
    bool &has_spoiler = d_->has_spoiler;
    has_spoiler = in.placements.at("settings").get("create-spoiler").truthy();
    if (has_spoiler) spoiler = las_spoiler(in.placements, in.logic_defs);
    c.write("logic.json", logic_json(in.logic_defs));
    // the playthrough check: LAS's logic never defines D6-4D / D6-2A (check-only aliases from the next room)
    Value defs = in.logic_defs.copy();
    for (const auto &a : std::vector<std::pair<const char *, const char *>>{{"D6-4D", "D6-5D"}, {"D6-2A", "D6-4D"}})
        if (!defs.has(a.first)) {
            Value n = Value::new_dict();
            n.set("type", "room"), n.set("condition-basic", a.second), n.set("_check_only", true);
            defs.set(a.first, n);
        }
    c.w.reset(new World(defs, in.settings));
    World &w = *c.w;
    Access access = w.start;
    std::vector<char> seen;
    c.spheres = spheres(c, access, seen);
    c.write("spheres.json", py_json(c.spheres));
    c.pl = placements_of(c);
    hint_steps(c);
    try {
        route(c);
    } catch (const PyError &e) {
        res.log.push_back(std::string("route: skipped: ") + e.what());
    }
    if (in.settings.get("hints").truthy()) {
        // (layouts: the room plan's; a room-shuffle seed's come from its dungeon_rooms.tsv)
        Value layouts = in.room_layouts;
        std::string tsv;
        if (!layouts.truthy() && in.settings.get("dungeon-room-shuffle", Value("off")) != Value("off") &&
            c.seed_file("dungeon_rooms.tsv", tsv)) {
            layouts = Value::new_dict();
            for (const std::string &ln : split(tsv, '\n')) {
                std::vector<std::string> f = split(ln, '\t');
                if (f.size() < 3) continue;
                if (!layouts.has(f[0])) layouts.set(f[0], Value::new_dict());
                layouts.at(f[0]).set(f[0] + "_" + f[1], f[2]);
            }
        }
        seed_hints_pick(c, layouts);
    }
    // the verdict
    std::vector<std::string> missing;
    for (const char *i : odata::INSTRUMENTS)
        if (!access.count(w.id(i))) missing.push_back(i);
    if (!access.count(w.id("kill-nightmare"))) missing.push_back("kill-nightmare");
    Value th = in.settings.get("triforce-hunt", Value(0));
    int tri_needed = th.truthy() ? (int)th.to_int() : 0;
    if (tri_needed) {
        int got = access.count(w.id(odata::TRIFORCE));
        missing.clear();
        if (got < tri_needed) missing.push_back(fmt("triforce pieces: %d of %d reachable", got, tri_needed));
    }
    std::set<std::string> absent = str_set(in.settings.get("absent-locations"));
    size_t locs = 0, reached = 0;
    for (size_t k = 0; k < w.keys.size(); k++) {
        if (w.defs.at(w.keys[k]).get("_check_only").truthy() || w.type[k] != "item" || absent.count(w.keys[k])) continue;
        locs++, reached += seen[k] != 0;
    }
    res.log.push_back(fmt("playthrough: %s - %zu/%zu item locations reachable",
                          missing.empty() ? (tri_needed ? "enough Triforce pieces reachable"
                                                        : "all instruments reachable, the Nightmare beatable")
                                          : ("MISSING " + missing[0]).c_str(),
                          reached, locs));
    if (!missing.empty()) {
        if (has_spoiler) c.write("spoiler_" + c.seed + ".txt", spoiler);
        res.exit_code = 3;
    }
    return res;
}

void OutputsBuilder::after_build(const Value &final_settings,
                                 std::function<bool(const std::string &rel, std::string &data)> read_seed) {
    using namespace out;
    if (!d_->first_done) before_build();
    if (d_->res.exit_code) return;
    d_->in.final_settings = final_settings;
    d_->in.read_seed = std::move(read_seed);
    Ctx &c = d_->c;
    Value sj = settings_json(d_->in);
    c.write("settings.json", py_json(sj, 1));
    build_tracker(c, sj);
    if (d_->has_spoiler) c.write("spoiler.txt", d_->spoiler);
    build_spoiler_view(c, sj);
}

Outputs build_outputs(const OutputsInput &in) {
    OutputsBuilder b(in);
    b.before_build();
    b.after_build(in.final_settings, in.read_seed);
    return std::move(b.result());
}

}  // namespace solve
}  // namespace rando
