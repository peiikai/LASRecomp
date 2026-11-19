// The randomizer's logic, evaluated natively (docs/randomizer.md, stage 5).
//
// LAS-Randomizer (MIT, Owen-Splat and contributors) writes a check's requirements as condition strings,
// "hookshot & (feather | rooster) & key-D6:5 & !classic-d2", and its shuffler turns each one into a Python expression
// over the access dict (parseCondition) and eval()s it. Here each string is parsed once into a tree with the same
// meaning:
//   - a name (letters, digits, '-', '[', ']') is hasAccess(access, name, 1): the name is in the access dict and its
//     count is at least 1; "name:N" asks for N (N = 0 still needs the name present);
//   - '!' is Python's not, '&' and, '|' or, with Python's precedence (not > and > or) and parentheses;
//   - text the Python can't compile (two names in a row, "a|b" with no spaces: "...)orself..." to Python's tokenizer,
//     a number with a leading zero) is a SyntaxError when the condition is evaluated, as for the Python; "a (b)" is a
//     call on a bool (a TypeError if evaluated: 'or' / 'and' may never get to it) and "()" an empty tuple (false).
//     Checked against Python's eval on 120,000 random conditions, errors included (none of these is in LAS's logic).
// Logic::check is tools/rando/las_headless.py's check_access (the semantics seeds are made with): an item or follower
// node needs its region first; a node passes on its basic condition, or with no basic condition at all; an enemy
// tried as "name[pit]" / "name[heavy]" passes on that condition first; then, every node, enemies included, passes on
// its advanced / glitched / hell conditions when the logic level allows them; logic 'none' passes everything.
//
// Search runs the shuffler's reachability loops (canReachLocation and its LAS Recomp variants): passes over the
// nodes in logic_defs order until a pass adds nothing. The order matters ('!' makes the logic non-monotonic), so it is
// the Python's order; a node is only evaluated again once a name it reads has changed (the same answer, sooner).
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <unordered_map>
#include <vector>

#include "value.h"

namespace rando {
namespace solve {

// interned names (items, nodes, settings, names in conditions): one id each
class Names {
public:
    int id(const std::string &s);         // interns
    int find(const std::string &s) const;  // -1 if never interned
    const std::string &name(int id) const { return names_[(size_t)id]; }
    size_t size() const { return names_.size(); }

private:
    std::unordered_map<std::string, int> ids_;
    std::vector<std::string> names_;
};

// the shuffler's access dict {name: count}: a name is in it while its count is above 0 (addAccess / removeAccess
// keep it so)
class Access {
public:
    std::vector<int32_t> n;
    int32_t count(int id) const { return (size_t)id < n.size() ? n[(size_t)id] : 0; }
    bool in(int id) const { return count(id) > 0; }                              // name in access
    bool has(int id, int64_t amount = 1) const {                                 // hasAccess(access, name, amount)
        int32_t c = count(id);
        return c > 0 && c >= amount;
    }
    void add(int id) { grow(id), n[(size_t)id]++; }                              // addAccess
    void remove(int id) { if (in(id)) n[(size_t)id]--; }                        // removeAccess
    void set(int id, int32_t v) { grow(id), n[(size_t)id] = v; }                 // access[name] = v
    bool operator==(const Access &o) const;                                      // dict ==
    bool operator!=(const Access &o) const { return !(*this == o); }

private:
    void grow(int id) {
        if ((size_t)id >= n.size()) n.resize((size_t)id + 1, 0);
    }
};

// condition strings, each compiled once
class Conditions {
public:
    int compile(const std::string &text, Names &names);  // -> condition index (the same text: the same index)
    bool eval(int c, const Access &a) const;              // PyError SyntaxError for text Python can't compile
    const std::vector<int> &names_in(int c) const { return conds_[(size_t)c].names; }
    const std::string &text(int c) const { return conds_[(size_t)c].text; }

private:
    struct Op {
        uint8_t kind;  // LEAF, NOT, AND, OR
        int32_t a, b;  // LEAF: name id, amount; NOT: child; AND / OR: first child in kids_, count
    };
    struct Cond {
        std::string text, error;
        int root = -1;
        std::vector<int> names;
    };
    bool eval_op(int op, const Access &a) const;
    std::unordered_map<std::string, int> index_;
    std::vector<Cond> conds_;
    std::vector<Op> ops_;
    std::vector<int32_t> kids_;
    friend struct CondParser;
};

enum NodeKind : uint8_t { NODE_ITEM, NODE_FOLLOWER, NODE_ENEMY, NODE_OTHER };
enum CondSlot { C_BASIC, C_ADVANCED, C_GLITCHED, C_HELL, C_PIT, C_HEAVY, C_SLOTS };

struct LogicNode {
    std::string key, type, subtype;
    int id = -1;          // the key's name id
    NodeKind kind = NODE_OTHER;
    int type_node = -1;   // check_access looks the type up under the key cut at its first character outside
                          // [a-zA-Z0-9-] (the node itself for every key in LAS's logic); -1: none (KeyError)
    int region = -1;      // name id of 'region' (-1: no region key)
    int cond[C_SLOTS] = {-1, -1, -1, -1, -1, -1};  // condition index per 'condition-*' key, -1 when absent
    int pit = -1, heavy = -1;  // name ids of key[pit] / key[heavy]
    std::vector<int> deps;     // every name check() may read for this node
};

class Logic {
public:
    // logic_defs as the shuffler holds it when it fills (after its own changes), and settings['logic']
    Logic(const Value &logic_defs, const std::string &level, Names &names);
    const std::string &level() const { return level_; }
    size_t size() const { return nodes_.size(); }
    const LogicNode &node(size_t i) const { return nodes_[i]; }
    int index_of(int name_id) const;  // the node of that key, -1 if none
    const Conditions &conditions() const { return conds_; }
    // las_headless check_access(newCheck, access, logic): node i as "key" (param 0), "key[pit]" (1), "key[heavy]" (2)
    bool check(size_t i, int param, const Access &a) const;
    // the same from a check name ("moblin", "moblin[pit]"); PyError KeyError for unknown nodes
    bool check_name(const std::string &name, const Access &a) const;
    const std::vector<int> &dependents(int name_id) const;  // nodes whose check reads that name

private:
    std::string level_;
    bool none_, adv_, glitched_, hell_;
    std::vector<LogicNode> nodes_;
    std::vector<int> node_of_;  // name id -> node index
    std::vector<std::vector<int>> rdeps_;
    Conditions conds_;
    Names &names_;
};

// one reachability search: the Python's
//     while added: added = False; for key in logic_defs: if key not in access: ...
// loop. pass(on_reach) runs one pass and returns whether it added anything (or STOP when on_reach asked to stop).
class Search {
public:
    Search(const Logic &L, Access start);
    Access access;
    void add(int id);  // addAccess, and the nodes reading that name are evaluated again

    enum { NOTHING = 0, ADDED = 1, STOP = 2 };
    // for each node not in access, in order: if check(key): add(key), then on_reach(node index) (return true to stop
    // the search at once), then for an enemy add key[pit] and key[heavy]; else for an enemy, each of its
    // 'condition-pit' / 'condition-heavy' not yet in access: if check(key[v]): add(key[v])
    template <class F> int pass(F &&on_reach) {
        int r = NOTHING;
        const size_t n = L_.size();
        for (size_t k = 0; k < n; k++) {
            const LogicNode &nd = L_.node(k);
            if (access.in(nd.id) || !dirty_[k]) continue;
            dirty_[k] = 0;
            if (L_.check(k, 0, access)) {
                add(nd.id);
                r = ADDED;
                if (on_reach(k)) return STOP;
                if (nd.kind == NODE_ENEMY) add(nd.pit), add(nd.heavy);
            } else if (nd.kind == NODE_ENEMY) {
                if (nd.cond[C_PIT] >= 0 && !access.has(nd.pit) && L_.check(k, 1, access)) add(nd.pit), r = ADDED;
                if (nd.cond[C_HEAVY] >= 0 && !access.has(nd.heavy) && L_.check(k, 2, access)) add(nd.heavy), r = ADDED;
            }
        }
        return r;
    }
    // passes until one adds nothing; false if on_reach stopped it
    template <class F> bool run(F &&on_reach) {
        for (;;) {
            int r = pass(on_reach);
            if (r == STOP) return false;
            if (r == NOTHING) return true;
        }
    }

private:
    const Logic &L_;
    std::vector<uint8_t> dirty_;
};

}  // namespace solve
}  // namespace rando
