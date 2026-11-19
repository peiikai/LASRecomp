// CPython's set of str with set_merge (prepare_pyset.h).
#include "prepare_pyset.h"

#include "pyset.h"

namespace rando {
namespace solve {

static const size_t LINEAR_PROBES = 9, PERTURB_SHIFT = 5;

long CpStrSet::find(const std::string &k, int64_t h) const {
    size_t mask = table_.size() - 1, perturb = (size_t)h, i = (size_t)h & mask;
    for (;;) {
        size_t probes = i + LINEAR_PROBES <= mask ? LINEAR_PROBES : 0;
        for (size_t j = 0; j <= probes; j++) {
            const E &e = table_[i + j];
            if (e.state == 0) return -1;
            if (e.state == 1 && e.hash == h && e.key == k) return (long)(i + j);
        }
        perturb >>= PERTURB_SHIFT;
        i = (i * 5 + 1 + perturb) & mask;
    }
}

bool CpStrSet::contains(const std::string &k) const { return find(k, py_str_hash(k)) >= 0; }

void CpStrSet::add(const std::string &k) { add_entry(k, py_str_hash(k)); }

void CpStrSet::add_entry(const std::string &k, int64_t h) {
    size_t mask = table_.size() - 1, perturb = (size_t)h, i = (size_t)h & mask;
    E *freeslot = nullptr;
    for (;;) {
        size_t probes = i + LINEAR_PROBES <= mask ? LINEAR_PROBES : 0;
        for (size_t j = 0; j <= probes; j++) {
            E &e = table_[i + j];
            if (e.state == 0) {
                if (freeslot) {  // (a dummy passed on the way is reused: fill stays, no resize - as pyset.cpp)
                    freeslot->state = 1, freeslot->hash = h, freeslot->key = k;
                    used_++;
                    return;
                }
                e.state = 1, e.hash = h, e.key = k;
                fill_++, used_++;
                if (fill_ * 5 >= mask * 3) resize(used_ > 50000 ? used_ * 2 : used_ * 4);
                return;
            }
            if (e.state == 1 && e.hash == h && e.key == k) return;
            if (e.state == 2) freeslot = &e;
        }
        perturb >>= PERTURB_SHIFT;
        i = (i * 5 + 1 + perturb) & mask;
    }
}

bool CpStrSet::discard(const std::string &k) {
    long at = find(k, py_str_hash(k));
    if (at < 0) return false;
    E &e = table_[(size_t)at];
    e.state = 2, e.hash = -1, e.key.clear();
    used_--;
    return true;
}

void CpStrSet::update(const std::vector<std::string> &items) {
    for (const std::string &k : items) add(k);
}

void CpStrSet::insert_clean(std::vector<E> &t, const std::string &k, int64_t h) {
    size_t mask = t.size() - 1, perturb = (size_t)h, i = (size_t)h & mask;
    for (;;) {
        size_t probes = i + LINEAR_PROBES <= mask ? LINEAR_PROBES : 0;
        for (size_t j = 0; j <= probes; j++) {
            if (t[i + j].state == 0) {
                E &e = t[i + j];
                e.state = 1, e.hash = h, e.key = k;
                return;
            }
        }
        perturb >>= PERTURB_SHIFT;
        i = (i * 5 + 1 + perturb) & mask;
    }
}

void CpStrSet::update(const CpStrSet &o) {
    if (&o == this || o.used_ == 0) return;
    size_t mask = table_.size() - 1;
    if ((fill_ + o.used_) * 5 >= mask * 3) resize((used_ + o.used_) * 2);
    if (fill_ == 0 && table_.size() == o.table_.size() && o.fill_ == o.used_) {  // (the same table: a straight copy)
        table_ = o.table_;
        fill_ = o.fill_, used_ = o.used_;
        return;
    }
    if (fill_ == 0) {  // (an empty table: set_insert_clean in the other's table order)
        for (const E &e : o.table_)
            if (e.state == 1) insert_clean(table_, e.key, e.hash);
        fill_ = used_ = o.used_;
        return;
    }
    for (const E &e : o.table_)
        if (e.state == 1) add_entry(e.key, e.hash);
}

CpStrSet CpStrSet::copy() const {
    CpStrSet s;
    s.update(*this);
    return s;
}

void CpStrSet::dummies_check() {
    if (fill_ - used_ <= (table_.size() - 1) / 4) return;
    resize(used_ > 50000 ? used_ * 2 : used_ * 4);
}

void CpStrSet::difference_update(const std::vector<std::string> &items) {
    for (const std::string &k : items) discard(k);
    dummies_check();
}

void CpStrSet::resize(size_t minused) {
    size_t newsize = 8;
    while (newsize <= minused) newsize <<= 1;
    std::vector<E> old;
    old.swap(table_);
    table_.assign(newsize, E());
    for (E &e : old)
        if (e.state == 1) insert_clean(table_, e.key, e.hash);
    fill_ = used_;
}

std::vector<std::string> CpStrSet::items() const {
    std::vector<std::string> out;
    out.reserve(used_);
    for (const E &e : table_)
        if (e.state == 1) out.push_back(e.key);
    return out;
}

}  // namespace solve
}  // namespace rando
