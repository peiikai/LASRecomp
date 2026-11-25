// CPython's set of str, in CPython's order (pyset.h).
#include "pyset.h"

namespace rando {
namespace solve {

static uint64_t rotl(uint64_t x, int b) { return (x << b) | (x >> (64 - b)); }

// SipHash-1-3, key 0, over CPython's own storage of the str: one byte per character when every code point is below
// 256, else two (below 65536) or four, little-endian (Python/pyhash.c on the str's data)
int64_t py_str_hash(const std::string &utf8) {
    std::vector<uint32_t> cps;
    cps.reserve(utf8.size());
    uint32_t top = 0;
    for (size_t i = 0; i < utf8.size();) {
        unsigned char c = (unsigned char)utf8[i];
        uint32_t cp;
        int n;
        if (c < 0x80) cp = c, n = 1;
        else if (c >> 5 == 6) cp = c & 0x1f, n = 2;
        else if (c >> 4 == 14) cp = c & 0x0f, n = 3;
        else cp = c & 0x07, n = 4;
        for (int k = 1; k < n && i + k < utf8.size(); k++) cp = cp << 6 | ((unsigned char)utf8[i + k] & 0x3f);
        i += n;
        cps.push_back(cp);
        if (cp > top) top = cp;
    }
    int width = top < 256 ? 1 : top < 65536 ? 2 : 4;
    std::vector<uint8_t> s;
    s.reserve(cps.size() * width);
    for (uint32_t cp : cps)
        for (int k = 0; k < width; k++) s.push_back((uint8_t)(cp >> (8 * k)));
    if (s.empty()) return 0;  // (_Py_HashBytes: the empty str hashes to 0)

    uint64_t v0 = 0x736f6d6570736575ULL, v1 = 0x646f72616e646f6dULL;
    uint64_t v2 = 0x6c7967656e657261ULL, v3 = 0x7465646279746573ULL;
    auto round = [&]() {
        v0 += v1, v2 += v3;
        v1 = rotl(v1, 13) ^ v0, v3 = rotl(v3, 16) ^ v2;
        v0 = rotl(v0, 32);
        v2 += v1, v0 += v3;
        v1 = rotl(v1, 17) ^ v2, v3 = rotl(v3, 21) ^ v0;
        v2 = rotl(v2, 32);
    };
    size_t n = s.size(), i = 0;
    for (; n - i >= 8; i += 8) {
        uint64_t m = 0;
        for (int j = 7; j >= 0; j--) m = m << 8 | s[i + j];
        v3 ^= m, round(), v0 ^= m;
    }
    uint64_t b = (uint64_t)n << 56;
    for (size_t j = n - i; j-- > 0;) b |= (uint64_t)s[i + j] << (8 * j);
    v3 ^= b, round(), v0 ^= b;
    v2 ^= 0xff;
    round(), round(), round();
    int64_t h = (int64_t)((v0 ^ v1) ^ (v2 ^ v3));
    return h == -1 ? -2 : h;
}

static const size_t LINEAR_PROBES = 9, PERTURB_SHIFT = 5;

long PyStrSet::find(const std::string &k, int64_t h) const {
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

bool PyStrSet::contains(const std::string &k) const { return find(k, py_str_hash(k)) >= 0; }

void PyStrSet::add(const std::string &k) {
    int64_t h = py_str_hash(k);
    size_t mask = table_.size() - 1, perturb = (size_t)h, i = (size_t)h & mask;
    E *freeslot = nullptr;
    for (;;) {
        size_t probes = i + LINEAR_PROBES <= mask ? LINEAR_PROBES : 0;
        for (size_t j = 0; j <= probes; j++) {
            E &e = table_[i + j];
            if (e.state == 0) {
                if (freeslot) {  // (a dummy passed on the way is reused: fill stays, no resize)
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
            if (e.state == 2) freeslot = &e;  // (the last dummy passed, not the first: measured on CPython 3.12)
        }
        perturb >>= PERTURB_SHIFT;
        i = (i * 5 + 1 + perturb) & mask;
    }
}

bool PyStrSet::discard(const std::string &k) {
    long at = find(k, py_str_hash(k));
    if (at < 0) return false;
    E &e = table_[(size_t)at];
    e.state = 2, e.hash = -1, e.key.clear();
    used_--;
    return true;
}

void PyStrSet::difference_update(const PyStrSet &o) {
    // (CPython walks the smaller of the two when the other is 8x larger; which entries become dummies doesn't depend
    // on the order they are discarded in)
    for (const E &e : o.table_)
        if (e.state == 1) discard(e.key);
    if (fill_ - used_ <= (table_.size() - 1) / 4) return;
    resize(used_ > 50000 ? used_ * 2 : used_ * 4);
}

void PyStrSet::resize(size_t minused) {
    size_t newsize = 8;
    while (newsize <= minused) newsize <<= 1;
    std::vector<E> old;
    old.swap(table_);
    table_.assign(newsize, E());
    size_t mask = newsize - 1;
    for (E &e : old) {
        if (e.state != 1) continue;
        size_t perturb = (size_t)e.hash, i = (size_t)e.hash & mask;
        for (;;) {  // (set_insert_clean)
            size_t probes = i + LINEAR_PROBES <= mask ? LINEAR_PROBES : 0;
            size_t j = 0;
            for (; j <= probes && table_[i + j].state != 0; j++) {}
            if (j <= probes) {
                table_[i + j] = std::move(e);
                break;
            }
            perturb >>= PERTURB_SHIFT;
            i = (i * 5 + 1 + perturb) & mask;
        }
    }
    fill_ = used_;
}

std::vector<std::string> PyStrSet::items() const {
    std::vector<std::string> out;
    out.reserve(used_);
    for (const E &e : table_)
        if (e.state == 1) out.push_back(e.key);
    return out;
}

}  // namespace solve
}  // namespace rando
