// A CPython (3.12) set of str with the operations las_headless.main() uses on settings['excluded-locations'], in
// CPython's iteration order: set(), update() from a list or from another set (set_merge: one resize up front, then a
// straight copy of the table, set_insert_clean into an empty one, or adds), set(other) as a copy, difference_update()
// from a list (the discards, then the resize when over a quarter of the table is dummies), add, discard.
//
// pyset.h's PyStrSet is the same table (same hash, probes and dummy reuse) without set_merge; this one is written from
// the same Objects/setobject.c algorithms (no code copied) so the preparation's set comes out in the same order as
// the Python's, which the recorded checkpoints keep (tools/rando/record.py).
#pragma once

#include <stdint.h>

#include <string>
#include <vector>

namespace rando {
namespace solve {

class CpStrSet {
public:
    CpStrSet() : table_(8) {}
    bool contains(const std::string &k) const;
    void add(const std::string &k);                                // set.add
    bool discard(const std::string &k);                            // set.discard
    void update(const std::vector<std::string> &items);            // s.update(list)
    void update(const CpStrSet &o);                                // s.update(other_set) (set_merge)
    CpStrSet copy() const;                                         // set(s)
    void difference_update(const std::vector<std::string> &items);  // s.difference_update(list)
    size_t size() const { return used_; }
    std::vector<std::string> items() const;                        // iteration order

private:
    struct E {
        int state = 0;  // 0 unused, 1 active, 2 dummy
        int64_t hash = 0;
        std::string key;
    };
    long find(const std::string &k, int64_t h) const;
    void add_entry(const std::string &k, int64_t h);
    void resize(size_t minused);
    static void insert_clean(std::vector<E> &t, const std::string &k, int64_t h);
    void dummies_check();
    std::vector<E> table_;
    size_t fill_ = 0, used_ = 0;
};

}  // namespace solve
}  // namespace rando
