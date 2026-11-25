// Python-shaped data for the native solver (value.h).
#include "value.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fstream>
#include <iterator>

#include "../../../third_party/yyjson/yyjson.h"

namespace rando {
namespace solve {

static PyError type_error(const char *what, const Value &v) {
    static const char *const names[] = {"NoneType", "bool", "int", "float", "str", "list", "dict"};
    return PyError("TypeError", std::string(what) + ", not " + names[v.type()]);
}

Value::Value(List l) : v_(std::make_shared<List>(std::move(l))) {}
Value::Value(Dict d) : v_(std::make_shared<Dict>(std::move(d))) {}
Value Value::new_dict() { return Value(Dict{}); }

bool Value::b() const {
    if (type() != BOOL) throw type_error("expected bool", *this);
    return std::get<bool>(v_);
}
int64_t Value::i() const {
    if (type() == BOOL) return std::get<bool>(v_);
    if (type() != INT) throw type_error("expected int", *this);
    return std::get<int64_t>(v_);
}
double Value::f() const {
    switch (type()) {
    case BOOL: return std::get<bool>(v_);
    case INT: return (double)std::get<int64_t>(v_);
    case FLOAT: return std::get<double>(v_);
    default: throw type_error("must be real number", *this);
    }
}
const std::string &Value::s() const {
    if (type() != STR) throw type_error("expected str", *this);
    return std::get<std::string>(v_);
}
List &Value::list() const {
    if (type() != LIST) throw type_error("expected list", *this);
    return *std::get<std::shared_ptr<List>>(v_);
}
Dict &Value::dict() const {
    if (type() != DICT) throw type_error("expected dict", *this);
    return *std::get<std::shared_ptr<Dict>>(v_);
}

bool Value::truthy() const {
    switch (type()) {
    case NONE: return false;
    case BOOL: return std::get<bool>(v_);
    case INT: return std::get<int64_t>(v_) != 0;
    case FLOAT: return std::get<double>(v_) != 0.0;
    case STR: return !s().empty();
    case LIST: return !list().empty();
    case DICT: return !dict().empty();
    }
    return false;
}

int64_t Value::to_int() const {
    switch (type()) {
    case BOOL:
    case INT: return i();
    case FLOAT: {
        double d = f();
        if (!isfinite(d)) throw PyError(isnan(d) ? "ValueError" : "OverflowError", "cannot convert float to integer");
        return (int64_t)trunc(d);
    }
    case STR: {  // int('12'): optional sign, digits, '_' between digits, surrounding whitespace
        const std::string &t = s();
        size_t a = 0, b = t.size();
        while (a < b && isspace((unsigned char)t[a])) a++;
        while (b > a && isspace((unsigned char)t[b - 1])) b--;
        bool neg = false;
        if (a < b && (t[a] == '+' || t[a] == '-')) neg = t[a++] == '-';
        int64_t v = 0;
        bool digit = false, ok = a < b;
        for (size_t k = a; k < b && ok; k++) {
            if (t[k] >= '0' && t[k] <= '9') v = v * 10 + (t[k] - '0'), digit = true;
            else ok = t[k] == '_' && digit && k + 1 < b && t[k + 1] != '_';
        }
        if (!ok || !digit) throw PyError("ValueError", "invalid literal for int() with base 10: " + py_repr(t));
        return neg ? -v : v;
    }
    default: throw type_error("int() argument must be a string or a real number", *this);
    }
}

Value Value::copy() const {
    if (type() == LIST) return Value(List(list()));
    if (type() == DICT) return Value(Dict(dict()));
    return *this;
}

Value Value::deepcopy() const {
    if (type() == LIST) {
        List l;
        l.reserve(list().size());
        for (const Value &v : list()) l.push_back(v.deepcopy());
        return Value(std::move(l));
    }
    if (type() == DICT) {
        Dict d;
        for (const auto &e : dict()) d.set(e.key, e.value.deepcopy());
        return Value(std::move(d));
    }
    return *this;
}

size_t Value::len() const {
    switch (type()) {
    case STR: return s().size();  // (bytes: callers only measure ASCII names)
    case LIST: return list().size();
    case DICT: return dict().size();
    default: throw type_error("object has no len()", *this);
    }
}

bool Value::has(const std::string &key) const { return dict().contains(key); }
const Value &Value::at(const std::string &key) const { return dict().at(key); }
Value &Value::at(const std::string &key) { return dict().at(key); }
Value Value::get(const std::string &key, const Value &dflt) const {
    const Value *v = dict().find(key);
    return v ? *v : dflt;
}
void Value::set(const std::string &key, Value v) { dict().set(key, std::move(v)); }

bool Value::contains(const Value &x) const {
    switch (type()) {
    case DICT:
        if (!x.is_str()) return false;  // (the solver's dicts have str keys only)
        return dict().contains(x.s());
    case LIST:
        for (const Value &v : list())
            if (v == x) return true;
        return false;
    case STR:
        if (!x.is_str()) throw PyError("TypeError", "'in <string>' requires string as left operand");
        return s().find(x.s()) != std::string::npos;
    default: throw type_error("argument is not iterable", *this);
    }
}

// Python ==: numbers by value across bool / int / float, containers by their contents
bool operator==(const Value &a, const Value &b) {
    if (a.is_number() && b.is_number()) {
        if (a.is_float() || b.is_float()) {
            const Value &fl = a.is_float() ? a : b, &o = a.is_float() ? b : a;
            double d = fl.f();
            if (o.is_float()) return d == o.f();
            // (an int against a float: exact, as CPython compares them)
            if (!isfinite(d) || d != trunc(d) || fabs(d) >= 9223372036854775808.0) return false;
            return (int64_t)d == o.i();
        }
        return a.i() == b.i();
    }
    if (a.type() != b.type()) return false;
    switch (a.type()) {
    case Value::NONE: return true;
    case Value::STR: return a.s() == b.s();
    case Value::LIST: {
        const List &x = a.list(), &y = b.list();
        if (&x == &y) return true;
        if (x.size() != y.size()) return false;
        for (size_t k = 0; k < x.size(); k++)
            if (!(x[k] == y[k])) return false;
        return true;
    }
    case Value::DICT: {
        const Dict &x = a.dict(), &y = b.dict();
        if (&x == &y) return true;
        if (x.size() != y.size()) return false;
        for (const auto &e : x) {
            const Value *o = y.find(e.key);
            if (!o || !(e.value == *o)) return false;
        }
        return true;
    }
    default: return false;
    }
}

// ---- Dict

Value *Dict::find(const std::string &key) {
    auto it = index_.find(key);
    return it == index_.end() ? nullptr : &entries_[it->second].value;
}
const Value *Dict::find(const std::string &key) const {
    auto it = index_.find(key);
    return it == index_.end() ? nullptr : &entries_[it->second].value;
}
Value &Dict::at(const std::string &key) {
    Value *v = find(key);
    if (!v) throw PyError("KeyError", py_repr(key));
    return *v;
}
const Value &Dict::at(const std::string &key) const {
    const Value *v = find(key);
    if (!v) throw PyError("KeyError", py_repr(key));
    return *v;
}
Value &Dict::operator[](const std::string &key) {
    auto it = index_.find(key);
    if (it != index_.end()) return entries_[it->second].value;
    index_.emplace(key, entries_.size());
    entries_.push_back(Entry{key, Value(), true});
    live_++;
    return entries_.back().value;
}
bool Dict::erase(const std::string &key) {
    auto it = index_.find(key);
    if (it == index_.end()) return false;
    Entry &e = entries_[it->second];
    e.live = false, e.value = Value(), e.key.clear();
    index_.erase(it);
    live_--;
    if (entries_.size() > 16 && live_ * 2 < entries_.size()) compact();
    return true;
}
void Dict::clear() {
    entries_.clear(), index_.clear();
    live_ = 0;
}
void Dict::compact() {
    std::vector<Entry> kept;
    kept.reserve(live_);
    for (Entry &e : entries_)
        if (e.live) kept.push_back(std::move(e));
    entries_.swap(kept);
    index_.clear();
    for (size_t k = 0; k < entries_.size(); k++) index_.emplace(entries_[k].key, k);
}
std::vector<std::string> Dict::keys() const {
    std::vector<std::string> k;
    k.reserve(live_);
    for (const auto &e : *this) k.push_back(e.key);
    return k;
}

// ---- repr / JSON

std::string py_repr(const std::string &s) {
    // (single quotes unless the text has one and no double quote, as CPython's unicode_repr)
    char q = s.find('\'') != std::string::npos && s.find('"') == std::string::npos ? '"' : '\'';
    std::string o(1, q);
    for (unsigned char c : s) {
        if (c == q || c == '\\') o += '\\', o += (char)c;
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else if (c == '\t') o += "\\t";
        else if (c < 0x20 || c == 0x7f) {
            char b[8];
            snprintf(b, sizeof b, "\\x%02x", c);
            o += b;
        } else o += (char)c;
    }
    return o + q;
}

// float.__repr__: the shortest digits that read back, laid out as CPython's 'r' format
static std::string float_repr(double d) {
    if (isnan(d)) return "nan";
    if (isinf(d)) return d < 0 ? "-inf" : "inf";
    if (d == 0) return signbit(d) ? "-0.0" : "0.0";
    char buf[40];
    int p = 1;
    for (; p < 17; p++) {
        snprintf(buf, sizeof buf, "%.*e", p - 1, d);
        if (strtod(buf, nullptr) == d) break;
    }
    snprintf(buf, sizeof buf, "%.*e", p - 1, d);
    std::string m(buf), digits;
    size_t e = m.find('e');
    int exp10 = atoi(m.c_str() + e + 1);
    bool neg = m[0] == '-';
    for (size_t k = neg; k < e; k++)
        if (m[k] != '.') digits += m[k];
    std::string out = neg ? "-" : "";
    int nd = (int)digits.size();
    if (exp10 >= -4 && exp10 < 16) {  // (CPython: fixed notation for 1e-4 <= |d| < 1e16)
        if (exp10 < 0) out += "0." + std::string(-exp10 - 1, '0') + digits;
        else if (nd <= exp10 + 1) out += digits + std::string(exp10 + 1 - nd, '0') + ".0";
        else out += digits.substr(0, exp10 + 1) + "." + digits.substr(exp10 + 1);
    } else {
        out += digits.substr(0, 1);
        if (nd > 1) out += "." + digits.substr(1);
        char x[8];
        snprintf(x, sizeof x, "e%c%02d", exp10 < 0 ? '-' : '+', abs(exp10));
        out += x;
    }
    return out;
}

static void json_str(std::string &o, const std::string &s) {
    o += '"';
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') o += '\\', o += (char)c;
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else if (c == '\t') o += "\\t";
        else if (c < 0x20) {
            char b[8];
            snprintf(b, sizeof b, "\\u%04x", c);
            o += b;
        } else o += (char)c;
    }
    o += '"';
}

static void write_value(std::string &o, const Value &v, bool as_repr) {
    switch (v.type()) {
    case Value::NONE: o += as_repr ? "None" : "null"; break;
    case Value::BOOL: o += v.b() ? (as_repr ? "True" : "true") : (as_repr ? "False" : "false"); break;
    case Value::INT: o += std::to_string(v.i()); break;
    case Value::FLOAT: {
        std::string f = float_repr(v.f());
        if (!as_repr && (f == "nan" || f == "inf" || f == "-inf")) f = f == "nan" ? "NaN" : f == "inf" ? "Infinity" : "-Infinity";
        o += f;
        break;
    }
    case Value::STR:
        if (as_repr) o += py_repr(v.s());
        else json_str(o, v.s());
        break;
    case Value::LIST: {
        o += '[';
        bool first = true;
        for (const Value &x : v.list()) {
            if (!first) o += as_repr ? ", " : ",";
            first = false;
            write_value(o, x, as_repr);
        }
        o += ']';
        break;
    }
    case Value::DICT: {
        o += '{';
        bool first = true;
        for (const auto &e : v.dict()) {
            if (!first) o += as_repr ? ", " : ",";
            first = false;
            if (as_repr) o += py_repr(e.key);
            else json_str(o, e.key);
            o += as_repr ? ": " : ":";
            write_value(o, e.value, as_repr);
        }
        o += '}';
        break;
    }
    }
}

std::string Value::repr() const {
    std::string o;
    write_value(o, *this, true);
    return o;
}
std::string Value::json() const {
    std::string o;
    write_value(o, *this, false);
    return o;
}

static Value from_yy(yyjson_val *v) {
    switch (yyjson_get_type(v)) {
    case YYJSON_TYPE_NULL: return Value();
    case YYJSON_TYPE_BOOL: return Value((bool)yyjson_get_bool(v));
    case YYJSON_TYPE_NUM:
        switch (yyjson_get_subtype(v)) {
        case YYJSON_SUBTYPE_SINT: return Value((long long)yyjson_get_sint(v));
        case YYJSON_SUBTYPE_UINT: {
            uint64_t u = yyjson_get_uint(v);
            if (u > (uint64_t)INT64_MAX) throw PyError("ValueError", "integer out of range in JSON");
            return Value((long long)u);
        }
        default: return Value(yyjson_get_real(v));
        }
    case YYJSON_TYPE_STR: return Value(std::string(yyjson_get_str(v), yyjson_get_len(v)));
    case YYJSON_TYPE_ARR: {
        List l;
        l.reserve(yyjson_arr_size(v));
        yyjson_arr_iter it;
        yyjson_arr_iter_init(v, &it);
        while (yyjson_val *x = yyjson_arr_iter_next(&it)) l.push_back(from_yy(x));
        return Value(std::move(l));
    }
    case YYJSON_TYPE_OBJ: {
        Dict d;
        yyjson_obj_iter it;
        yyjson_obj_iter_init(v, &it);
        while (yyjson_val *k = yyjson_obj_iter_next(&it))
            d.set(std::string(yyjson_get_str(k), yyjson_get_len(k)), from_yy(yyjson_obj_iter_get_val(k)));
        return Value(std::move(d));
    }
    default: throw PyError("ValueError", "unsupported JSON value");
    }
}

Value Value::parse_json(const char *data, size_t len) {
    yyjson_read_err err;
    yyjson_doc *doc = yyjson_read_opts((char *)data, len, YYJSON_READ_ALLOW_INF_AND_NAN, nullptr, &err);
    if (!doc) throw PyError("ValueError", std::string("bad JSON: ") + err.msg + " at " + std::to_string(err.pos));
    try {
        Value v = from_yy(yyjson_doc_get_root(doc));
        yyjson_doc_free(doc);
        return v;
    } catch (...) {
        yyjson_doc_free(doc);
        throw;
    }
}

Value Value::load_json(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw PyError("FileNotFoundError", "No such file: " + py_repr(path));
    std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return parse_json(data.data(), data.size());
}

}  // namespace solve
}  // namespace rando
