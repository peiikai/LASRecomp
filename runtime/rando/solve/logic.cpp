// The randomizer's logic, evaluated natively (logic.h).
#include "logic.h"

#include <limits.h>

namespace rando {
namespace solve {

int Names::id(const std::string &s) {
    auto it = ids_.find(s);
    if (it != ids_.end()) return it->second;
    int id = (int)names_.size();
    ids_.emplace(s, id);
    names_.push_back(s);
    return id;
}

int Names::find(const std::string &s) const {
    auto it = ids_.find(s);
    return it == ids_.end() ? -1 : it->second;
}

bool Access::operator==(const Access &o) const {
    size_t m = n.size() > o.n.size() ? n.size() : o.n.size();
    for (size_t i = 0; i < m; i++)
        if (count((int)i) != o.count((int)i)) return false;
    return true;
}

// ---- conditions

enum { OP_LEAF, OP_NOT, OP_AND, OP_OR, OP_EMPTY, OP_CALL };

static bool name_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '[' || c == ']';
}

// recursive descent over the condition text, with the grammar Python sees after parseCondition's rewrite:
// or_expr := and_expr ('|' and_expr)*, and_expr := not_expr ('&' not_expr)*, not_expr := '!' not_expr | call,
// call := atom ('(' [or_expr] ')')*, atom := name[:N] | '(' or_expr ')' | '()'
// A call is Python calling a bool ("a (b)" is hasAccess(...)(hasAccess(...))): a TypeError once evaluated, which
// 'or' / 'and' may never get to; '()' is an empty tuple, false.
struct CondParser {
    Conditions &C;
    Names &names;
    const std::string &s;
    size_t p = 0;
    std::vector<int> &used;

    [[noreturn]] void fail(const std::string &why) { throw PyError("SyntaxError", why + " in condition " + py_repr(s)); }
    void ws() {
        while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) p++;
    }
    int node(uint8_t kind, int32_t a, int32_t b) {
        C.ops_.push_back(Conditions::Op{kind, a, b});
        return (int)C.ops_.size() - 1;
    }
    int list(uint8_t kind, const std::vector<int> &xs) {
        if (xs.size() == 1) return xs[0];
        int first = (int)C.kids_.size();
        for (int x : xs) C.kids_.push_back(x);
        return node(kind, first, (int32_t)xs.size());
    }
    // '|' becomes "or" and '&' "and" with no spaces around them: a name or '!' right after one glues into one
    // identifier for Python ("orself", "andnot")
    void op_follow() {
        if (p < s.size() && (name_char(s[p]) || s[p] == '!')) fail("invalid syntax (operator joined to a name)");
    }
    int or_expr() {
        std::vector<int> xs{and_expr()};
        for (ws(); p < s.size() && s[p] == '|'; ws()) {
            p++, op_follow();
            xs.push_back(and_expr());
        }
        return list(OP_OR, xs);
    }
    int and_expr() {
        std::vector<int> xs{not_expr()};
        for (ws(); p < s.size() && s[p] == '&'; ws()) {
            p++, op_follow();
            xs.push_back(not_expr());
        }
        return list(OP_AND, xs);
    }
    int not_expr() {
        ws();
        if (p < s.size() && s[p] == '!') {
            p++;
            return node(OP_NOT, not_expr(), 0);
        }
        int x = atom();
        for (ws(); p < s.size() && s[p] == '('; ws()) {
            p++;
            ws();
            if (p < s.size() && s[p] != ')') or_expr();  // (the argument: never evaluated, the call fails first)
            ws();
            if (p >= s.size() || s[p] != ')') fail("'(' was never closed");
            p++;
            x = node(OP_CALL, x, 0);
        }
        return x;
    }
    int atom() {
        ws();
        if (p >= s.size()) fail("unexpected end");
        if (s[p] == '(') {
            p++;
            ws();
            if (p < s.size() && s[p] == ')') return p++, node(OP_EMPTY, 0, 0);
            int x = or_expr();
            ws();
            if (p >= s.size() || s[p] != ')') fail("'(' was never closed");
            p++;
            return x;
        }
        size_t a = p;
        while (p < s.size() && name_char(s[p])) p++;
        if (p == a) fail(std::string("invalid character ") + py_repr(std::string(1, s[p])));
        int id = names.id(s.substr(a, p - a));
        int64_t amount = 1;
        if (p + 1 < s.size() && s[p] == ':' && s[p + 1] >= '0' && s[p + 1] <= '9') {
            size_t d = ++p;
            while (p < s.size() && s[p] >= '0' && s[p] <= '9') p++;
            std::string num = s.substr(d, p - d);
            if (num.size() > 1 && num[0] == '0' && num.find_first_not_of('0') != std::string::npos)
                fail("leading zeros in decimal integer literals are not permitted");
            amount = 0;
            for (char c : num) amount = amount < INT32_MAX ? amount * 10 + (c - '0') : amount;
            if (amount > INT32_MAX) amount = INT32_MAX;  // (more than any count: false, as for Python's big int)
        }
        used.push_back(id);
        return node(OP_LEAF, id, (int32_t)amount);
    }
};

int Conditions::compile(const std::string &text, Names &names) {
    auto it = index_.find(text);
    if (it != index_.end()) return it->second;
    Cond c;
    c.text = text;
    try {
        CondParser P{*this, names, text, 0, c.names};
        int root = P.or_expr();
        P.ws();
        if (P.p != text.size()) P.fail("invalid syntax");
        c.root = root;
    } catch (const PyError &e) {
        c.error = e.what();
        c.root = -1;
    }
    int idx = (int)conds_.size();
    conds_.push_back(std::move(c));
    index_.emplace(text, idx);
    return idx;
}

bool Conditions::eval_op(int op, const Access &a) const {
    const Op &o = ops_[(size_t)op];
    switch (o.kind) {
    case OP_LEAF: return a.has(o.a, o.b);
    case OP_NOT: return !eval_op(o.a, a);
    case OP_EMPTY: return false;
    case OP_CALL: throw PyError("TypeError", "'bool' object is not callable");
    case OP_AND:
        for (int32_t k = 0; k < o.b; k++)
            if (!eval_op(kids_[(size_t)(o.a + k)], a)) return false;
        return true;
    default:
        for (int32_t k = 0; k < o.b; k++)
            if (eval_op(kids_[(size_t)(o.a + k)], a)) return true;
        return false;
    }
}

bool Conditions::eval(int c, const Access &a) const {
    const Cond &cd = conds_[(size_t)c];
    if (cd.root < 0) throw PyError("SyntaxError", cd.error);
    return eval_op(cd.root, a);
}

// ---- nodes

static const char *const kCondKeys[C_SLOTS] = {"condition-basic", "condition-advanced", "condition-glitched",
                                               "condition-hell", "condition-pit", "condition-heavy"};

Logic::Logic(const Value &logic_defs, const std::string &level, Names &names) : level_(level), names_(names) {
    none_ = level == "none";
    adv_ = level == "advanced" || level == "glitched" || level == "hell";
    glitched_ = level == "glitched" || level == "hell";
    hell_ = level == "hell";
    const Dict &defs = logic_defs.dict();
    nodes_.reserve(defs.size());
    for (const auto &e : defs) {
        LogicNode n;
        n.key = e.key;
        n.id = names.id(e.key);
        const Value &v = e.value;
        n.type = v.at("type").s();
        n.kind = n.type == "item" ? NODE_ITEM : n.type == "follower" ? NODE_FOLLOWER : n.type == "enemy" ? NODE_ENEMY : NODE_OTHER;
        if (const Value *st = v.dict().find("subtype")) n.subtype = st->is_str() ? st->s() : st->repr();
        if (const Value *r = v.dict().find("region")) n.region = names.id(r->s());
        for (int c = 0; c < C_SLOTS; c++)
            if (const Value *t = v.dict().find(kCondKeys[c])) {
                if (!t->is_str()) throw PyError("TypeError", "expected string or bytes-like object, got " + t->repr());
                n.cond[c] = conds_.compile(t->s(), names);
            }
        if (n.kind == NODE_ENEMY) n.pit = names.id(e.key + "[pit]"), n.heavy = names.id(e.key + "[heavy]");
        nodes_.push_back(std::move(n));
    }
    node_of_.assign(names.size(), -1);
    for (size_t i = 0; i < nodes_.size(); i++) node_of_[(size_t)nodes_[i].id] = (int)i;
    for (LogicNode &n : nodes_) {
        size_t cut = 0;  // (re.match('[a-zA-Z0-9-]+', key): no match is an error the check raises)
        while (cut < n.key.size() && name_char(n.key[cut]) && n.key[cut] != '[' && n.key[cut] != ']') cut++;
        int tn = cut ? names.find(n.key.substr(0, cut)) : -1;
        n.type_node = tn >= 0 && (size_t)tn < node_of_.size() ? node_of_[(size_t)tn] : -1;
        for (const LogicNode *m : {&n, n.type_node >= 0 ? &nodes_[(size_t)n.type_node] : &n})
            for (int c = 0; c < C_SLOTS; c++)
                if (m->cond[c] >= 0)
                    for (int id : conds_.names_in(m->cond[c])) n.deps.push_back(id);
        if (n.region >= 0) n.deps.push_back(n.region);
        if (n.pit >= 0) n.deps.push_back(n.pit), n.deps.push_back(n.heavy);
    }
    rdeps_.assign(names.size(), {});
    for (size_t i = 0; i < nodes_.size(); i++) {
        std::vector<int> &d = nodes_[i].deps;
        for (int id : d)
            if (rdeps_[(size_t)id].empty() || rdeps_[(size_t)id].back() != (int)i) rdeps_[(size_t)id].push_back((int)i);
    }
}

int Logic::index_of(int name_id) const {
    return name_id >= 0 && (size_t)name_id < node_of_.size() ? node_of_[(size_t)name_id] : -1;
}

const std::vector<int> &Logic::dependents(int name_id) const {
    static const std::vector<int> none;
    return name_id >= 0 && (size_t)name_id < rdeps_.size() ? rdeps_[(size_t)name_id] : none;
}

bool Logic::check(size_t i, int param, const Access &a) const {
    const LogicNode &n = nodes_[i];
    if (n.type_node < 0) throw PyError("KeyError", py_repr(n.key));
    if (none_) return true;
    if (nodes_[(size_t)n.type_node].kind == NODE_ENEMY) {
        const LogicNode &e = nodes_[(size_t)n.type_node];
        if (param) {
            int c = e.cond[param == 1 ? C_PIT : C_HEAVY];
            if (c < 0) throw PyError("KeyError", py_repr(param == 1 ? "condition-pit" : "condition-heavy"));
            if (conds_.eval(c, a)) return true;
        }
        if (e.cond[C_BASIC] < 0) throw PyError("KeyError", "'condition-basic'");
        if (conds_.eval(e.cond[C_BASIC], a)) return true;
        // (falls through: an enemy's advanced / glitched / hell ways, as for any node below)
        if (e.cond[C_ADVANCED] >= 0 && adv_ && conds_.eval(e.cond[C_ADVANCED], a)) return true;
        if (e.cond[C_GLITCHED] >= 0 && glitched_ && conds_.eval(e.cond[C_GLITCHED], a)) return true;
        if (e.cond[C_HELL] >= 0 && hell_ && conds_.eval(e.cond[C_HELL], a)) return true;
        return false;
    }
    if (param) throw PyError("KeyError", py_repr(n.key + (param == 1 ? "[pit]" : "[heavy]")));
    if (n.kind == NODE_ITEM || n.kind == NODE_FOLLOWER) {
        if (n.region < 0) throw PyError("KeyError", "'region'");
        if (!a.has(n.region)) return false;
    }
    if (n.cond[C_BASIC] < 0) return true;
    if (conds_.eval(n.cond[C_BASIC], a)) return true;
    if (n.cond[C_ADVANCED] >= 0 && adv_ && conds_.eval(n.cond[C_ADVANCED], a)) return true;
    if (n.cond[C_GLITCHED] >= 0 && glitched_ && conds_.eval(n.cond[C_GLITCHED], a)) return true;
    if (n.cond[C_HELL] >= 0 && hell_ && conds_.eval(n.cond[C_HELL], a)) return true;
    return false;
}

bool Logic::check_name(const std::string &name, const Access &a) const {
    int param = 0;
    std::string key = name;
    size_t br = name.find('[');
    if (br != std::string::npos && name.size() > br + 2 && name.back() == ']') {
        std::string p = name.substr(br + 1, name.size() - br - 2);
        if (p == "pit" || p == "heavy") param = p == "pit" ? 1 : 2, key = name.substr(0, br);
    }
    int i = index_of(names_.find(key));
    if (i < 0) throw PyError("KeyError", py_repr(key));
    return check((size_t)i, param, a);
}

// ---- searches

Search::Search(const Logic &L, Access start) : access(std::move(start)), L_(L), dirty_(L.size(), 1) {}

void Search::add(int id) {
    access.add(id);
    for (int k : L_.dependents(id)) dirty_[(size_t)k] = 1;
}

}  // namespace solve
}  // namespace rando
