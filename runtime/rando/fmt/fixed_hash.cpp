// Fixed hash (fixed_hash.h): ported from LAS-Randomizer's RandomizerCore/Tools/fixed_hash.py (MIT, Owen-Splat and
// contributors).
#include "fixed_hash.h"

#include <string.h>

#include <stdexcept>

namespace rando {

uint64_t read_le(const Bytes &d, size_t at, int n) {
    uint64_t v = 0;
    for (int i = 0; i < n && at + i < d.size(); i++) v |= (uint64_t)(uint8_t)d[at + i] << (8 * i);
    return v;
}

float read_f32(const Bytes &d, size_t at) {
    if (at + 4 > d.size()) throw std::out_of_range("fixed hash: float past the end");
    uint32_t u = (uint32_t)read_le(d, at, 4);
    float f;
    memcpy(&f, &u, 4);
    return f;
}

Bytes read_cstr(const Bytes &d, size_t at) {
    size_t e = at;
    while (e < d.size() && d[e]) e++;
    return at < d.size() ? d.substr(at, e - at) : Bytes();
}

void put_le(Bytes &out, uint64_t v, int n) {
    for (int i = 0; i < n; i++) out.push_back((char)(uint8_t)(v >> (8 * i)));
}

void put_f32(Bytes &out, double v) {
    float f = (float)v;
    uint32_t u;
    memcpy(&u, &f, 4);
    put_le(out, u, 4);
}

uint32_t fixed_hash_string(const Bytes &s) {
    uint32_t h = 0;
    for (unsigned char c : s) {
        if (!c) break;  // (the Python hashes up to the first NUL)
        h ^= c + (h >> 2) + (h << 5);
    }
    return h;
}

static void pad_to(Bytes &b, size_t offset, size_t align) {
    while ((b.size() + offset) % align) b.push_back(0);
}

FixedHash FixedHash::parse(const Bytes &data, size_t off) {
    FixedHash h;
    h.magic = (uint8_t)read_le(data, off, 1);
    h.version = (uint8_t)read_le(data, off + 1, 1);
    h.num_buckets = (uint16_t)read_le(data, off + 2, 2);
    h.num_nodes = (uint16_t)read_le(data, off + 4, 2);
    h.x6 = (uint16_t)read_le(data, off + 6, 2);
    for (size_t i = 0; i < h.num_buckets; i++) h.buckets.push_back((uint32_t)read_le(data, off + 8 + i * 4, 4));

    size_t entries_off = ((off + 8 + 4 * ((size_t)h.num_buckets + 1) + 3) & ~(size_t)7) + 8;
    size_t n = (size_t)(read_le(data, entries_off - 8, 8) / 0x10);
    size_t offsets_off = entries_off + n * 0x10 + 8;
    size_t data_off = ((offsets_off + 4 * n + 7) & ~(size_t)7) + 8;
    size_t names_off = ((data_off + (size_t)read_le(data, data_off - 8, 8) + 3) & ~(size_t)3) + 4;
    size_t names_size = (size_t)read_le(data, names_off - 4, 4);
    if (names_off > data.size() || names_size > data.size() - names_off) throw std::out_of_range("fixed hash: names");
    h.names = data.substr(names_off, names_size);

    for (size_t i = 0; i < n; i++) {
        size_t e = entries_off + i * 0x10;
        if (e + 0x10 > data.size()) throw std::out_of_range("fixed hash: entries");
        FhEntry en;
        en.node = (uint16_t)read_le(data, e, 2);
        en.next = (uint32_t)read_le(data, e + 8, 4);
        if (names_size) en.name = read_cstr(data, names_off + (size_t)read_le(data, e + 2, 2));
        size_t at = data_off + (size_t)read_le(data, e + 0xC, 4);
        if (en.node <= 0xFFED) {
            en.sub = std::make_shared<FixedHash>(parse(data, at));
        } else if (en.node >= 0xFFF0) {
            size_t sz = (size_t)read_le(data, at, 8);
            if (at + 8 > data.size() || sz > data.size() - at - 8) throw std::out_of_range("fixed hash: entry data");
            en.data = data.substr(at + 8, sz);
        } else {
            throw std::runtime_error("fixed hash: invalid node index");
        }
        h.entries.push_back(std::move(en));
    }
    return h;
}

Bytes FixedHash::to_binary(size_t offset) const {
    Bytes intro;
    put_le(intro, magic, 1);
    put_le(intro, version, 1);
    put_le(intro, num_buckets, 2);
    put_le(intro, num_nodes, 2);
    put_le(intro, x6, 2);
    for (uint32_t b : buckets) put_le(intro, b, 4);

    Bytes ents, offs, dat;
    put_le(ents, entries.size() * 0x10, 8);
    put_le(offs, entries.size() * 4, 8);
    for (size_t i = 0; i < entries.size(); i++) {
        const FhEntry &e = entries[i];
        put_le(ents, e.node, 2);
        // (as the Python: the name's first occurrence followed by a NUL, when the names section holds the name at
        // all - an empty name always counts - else 0)
        uint16_t name_at = 0;
        if (!names.empty() && (e.name.empty() || names.find(e.name) != Bytes::npos)) {
            size_t k = names.find(e.name + Bytes(1, '\0'));
            if (k == Bytes::npos) throw std::runtime_error("fixed hash: entry name not NUL-terminated in names");
            name_at = (uint16_t)k;
        }
        put_le(ents, name_at, 2);
        put_le(ents, fixed_hash_string(e.name), 4);
        put_le(ents, e.next, 4);
        put_le(ents, dat.size(), 4);
        put_le(offs, i * 0x10, 4);
        if (e.node <= 0xFFED) {
            dat += e.sub->to_binary(dat.size());
        } else if (e.node >= 0xFFF0) {
            put_le(dat, e.data.size(), 8);
            dat += e.data;
            dat.resize(dat.size() + 7, '\0');
            dat.resize(dat.size() & ~(size_t)7);
        } else {
            throw std::runtime_error("fixed hash: invalid node index");
        }
    }
    Bytes datsect;
    put_le(datsect, dat.size(), 8);
    datsect += dat;

    Bytes r = intro;
    pad_to(r, offset, 8);
    r += ents;
    pad_to(r, offset, 8);
    r += offs;
    pad_to(r, offset, 8);
    r += datsect;
    pad_to(r, offset, 4);
    put_le(r, names.size(), 4);
    r += names;
    return r;
}

const FhEntry *FixedHash::find(const Bytes &name) const {
    for (const FhEntry &e : entries)
        if (e.name == name) return &e;
    return nullptr;
}
FhEntry *FixedHash::find(const Bytes &name) {
    for (FhEntry &e : entries)
        if (e.name == name) return &e;
    return nullptr;
}

}  // namespace rando
