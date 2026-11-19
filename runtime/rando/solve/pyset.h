// A CPython (3.12) set of str, laid out as CPython lays it out, so it iterates in CPython's order. The generator runs
// with PYTHONHASHSEED=0 (runtime/ui/rando_tab.mm), which makes that order fixed: the str hash is SipHash-1-3 with a zero
// key. The shuffler iterates sets it builds itself (vanilla_locations, excluded-locations), and that order reaches its
// output (the order of placements['indexes'], the lists the seed records).
//
// Written from Objects/setobject.c and Python/pyhash.c (algorithms only): linear probes then perturbation, deleted
// entries left as dummies, a resize to used*4 (used*2 past 50000) once fill*5 >= mask*3, and difference_update's
// resize when over a quarter of the table is dummies. An add that passes dummies on its way to a free slot takes the
// last dummy it passed: measured against CPython 3.12 (random adds, discards and -= on thousands of names, ASCII and
// not, every order the same), not the first one as a reading of the source suggests.
#pragma once

#include <stdint.h>

#include <string>
#include <vector>

namespace rando {
namespace solve {

int64_t py_str_hash(const std::string &utf8);  // hash(str) under PYTHONHASHSEED=0

class PyStrSet {
public:
    PyStrSet() : table_(8) {}
    bool contains(const std::string &k) const;
    void add(const std::string &k);              // set.add
    bool discard(const std::string &k);          // set.discard (true if it was there)
    void difference_update(const PyStrSet &o);   // s -= o
    size_t size() const { return used_; }
    std::vector<std::string> items() const;      // iteration order

private:
    struct E {
        int state = 0;  // 0 unused, 1 active, 2 dummy
        int64_t hash = 0;
        std::string key;
    };
    long find(const std::string &k, int64_t h) const;
    void resize(size_t minused);
    std::vector<E> table_;
    size_t fill_ = 0, used_ = 0;
};

}  // namespace solve
}  // namespace rando
