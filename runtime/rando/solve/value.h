// Python-shaped data for the native solver (docs/randomizer.md, stage 5): None, bool, int, float, str, list and dict,
// with Python's semantics where the solver depends on them.
//
//   - Reference semantics, as Python names: copying a Value shares its list / dict (placements['settings'] is the
//     settings object itself, so a change through one is seen through the other). copy() is list(x) / dict(x),
//     deepcopy() is copy.deepcopy(x).
//   - Dicts keep insertion order (Python 3.7+): setting an existing key keeps its place, erasing removes it.
//   - == is Python's: 1 == 1.0 == True, lists in order, dicts by their items whatever the order.
//   - Errors are PyError, named as the Python's exception ("KeyError: 'x'"), so callers can match the same texts.
//
// Sets only appear as lists in Python's iteration order (tools/rando/record.py checkpoints); a set the solver builds
// itself keeps CPython's order with PyStrSet (pyset.h).
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace rando {
namespace solve {

// a Python exception: what() is "<Type>: <message>", as the last line of the Python's traceback
struct PyError : std::runtime_error {
    std::string type;
    PyError(const std::string &type_, const std::string &msg) : std::runtime_error(type_ + ": " + msg), type(type_) {}
};

class Value;
class Dict;
using List = std::vector<Value>;

class Value {
public:
    enum Type : uint8_t { NONE, BOOL, INT, FLOAT, STR, LIST, DICT };

    Value() {}
    Value(std::nullptr_t) {}
    Value(bool b) : v_(b) {}
    Value(int i) : v_((int64_t)i) {}
    Value(long i) : v_((int64_t)i) {}
    Value(long long i) : v_((int64_t)i) {}
    Value(unsigned i) : v_((int64_t)i) {}
    Value(double d) : v_(d) {}
    Value(const char *s) : v_(std::string(s)) {}
    Value(std::string s) : v_(std::move(s)) {}
    Value(List l);
    Value(Dict d);
    static Value new_list() { return Value(List{}); }
    static Value new_dict();

    Type type() const { return (Type)v_.index(); }
    bool is_none() const { return type() == NONE; }
    bool is_bool() const { return type() == BOOL; }
    bool is_int() const { return type() == INT; }      // (not bool: isinstance(True, int) callers ask is_number)
    bool is_float() const { return type() == FLOAT; }
    bool is_number() const { return type() == BOOL || type() == INT || type() == FLOAT; }
    bool is_str() const { return type() == STR; }
    bool is_list() const { return type() == LIST; }
    bool is_dict() const { return type() == DICT; }

    // the value as that type (PyError TypeError otherwise); i() takes a bool as 0 / 1, f() any number
    bool b() const;
    int64_t i() const;
    double f() const;
    const std::string &s() const;
    List &list() const;   // (shared: a change is seen by every Value naming this list)
    Dict &dict() const;

    bool truthy() const;           // bool(x)
    int64_t to_int() const;        // int(x): numbers (floats truncated) and decimal strings
    Value copy() const;            // list(x) / dict(x): a new container holding the same values
    Value deepcopy() const;        // copy.deepcopy(x)
    size_t len() const;            // len(x): str, list, dict

    // dict helpers (PyError TypeError if not a dict)
    bool has(const std::string &key) const;                              // key in d
    const Value &at(const std::string &key) const;                       // d[key] (KeyError)
    Value &at(const std::string &key);
    Value get(const std::string &key, const Value &dflt = Value()) const;  // d.get(key, dflt)
    void set(const std::string &key, Value v);                           // d[key] = v
    // `x in self`: a dict's keys, a list's items (==), a substring of a str
    bool contains(const Value &x) const;

    std::string repr() const;  // repr(x), for messages
    std::string json() const;  // compact JSON (dict order kept)

    // JSON in: objects become dicts in file order (a repeated key keeps its first place and its last value, as
    // json.loads), integers ints, other numbers floats. PyError ValueError on bad JSON.
    static Value parse_json(const char *data, size_t len);
    static Value load_json(const std::string &path);

    friend bool operator==(const Value &a, const Value &b);  // Python ==
    friend bool operator!=(const Value &a, const Value &b) { return !(a == b); }

private:
    std::variant<std::monostate, bool, int64_t, double, std::string, std::shared_ptr<List>, std::shared_ptr<Dict>> v_;
};

// an insertion-ordered str -> Value map (Python's dict)
class Dict {
public:
    struct Entry {
        std::string key;
        Value value;
        bool live = true;
    };

    size_t size() const { return live_; }
    bool empty() const { return live_ == 0; }
    bool contains(const std::string &key) const { return index_.count(key) != 0; }
    Value *find(const std::string &key);
    const Value *find(const std::string &key) const;
    Value &at(const std::string &key);  // KeyError
    const Value &at(const std::string &key) const;
    Value &operator[](const std::string &key);  // d[key], inserted at the end (None) when missing
    void set(const std::string &key, Value v) { (*this)[key] = std::move(v); }
    bool erase(const std::string &key);  // del d[key] (false when missing)
    void clear();

    // iteration in insertion order: for (auto &e : d) e.key, e.value. Like Python, don't add or remove keys while
    // iterating.
    template <class E, class V> class Iter {
    public:
        Iter(V *v, size_t i) : v_(v), i_(i) { skip(); }
        E &operator*() const { return (*v_)[i_]; }
        E *operator->() const { return &(*v_)[i_]; }
        Iter &operator++() { i_++, skip(); return *this; }
        bool operator!=(const Iter &o) const { return i_ != o.i_; }
        bool operator==(const Iter &o) const { return i_ == o.i_; }

    private:
        void skip() {
            while (i_ < v_->size() && !(*v_)[i_].live) i_++;
        }
        V *v_;
        size_t i_;
    };
    using iterator = Iter<Entry, std::vector<Entry>>;
    using const_iterator = Iter<const Entry, const std::vector<Entry>>;
    iterator begin() { return iterator(&entries_, 0); }
    iterator end() { return iterator(&entries_, entries_.size()); }
    const_iterator begin() const { return const_iterator(&entries_, 0); }
    const_iterator end() const { return const_iterator(&entries_, entries_.size()); }
    std::vector<std::string> keys() const;

private:
    void compact();
    std::vector<Entry> entries_;  // (erased entries stay as holes until compact())
    std::unordered_map<std::string, size_t> index_;
    size_t live_ = 0;
};

// Python's repr() of a str
std::string py_repr(const std::string &s);

}  // namespace solve
}  // namespace rando
