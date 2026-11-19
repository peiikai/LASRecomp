// tools/rando/fixedhash.py, ported (fhash.h).
#include "fhash.h"

#include <string.h>

#include <stdexcept>

namespace rando {

namespace {

uint64_t rd(const std::string &d, size_t at, int n) {
    if (at + n > d.size()) throw std::runtime_error("fixed hash: read past the end");
    uint64_t v = 0;
    for (int i = 0; i < n; i++) v |= (uint64_t)(uint8_t)d[at + i] << (8 * i);
    return v;
}

void put(std::string &o, uint64_t v, int n) {
    for (int i = 0; i < n; i++) o.push_back((char)(v >> (8 * i)));
}

void put_at(std::string &o, size_t at, uint64_t v, int n) {
    for (int i = 0; i < n; i++) o[at + i] = (char)(v >> (8 * i));
}

}  // namespace

uint32_t fhash_name_hash(const std::string &name) {
    uint64_t h = 0;
    for (unsigned char c : name) h ^= (c + (h >> 2) + (h << 5)) & 0xFFFFFFFFu;
    return (uint32_t)h;
}

FHash FHash::parse(const std::string &d, size_t off) {
    FHash f;
    f.magic = (uint8_t)rd(d, off, 1), f.version = (uint8_t)rd(d, off + 1, 1);
    uint32_t nb = (uint32_t)rd(d, off + 2, 2);
    f.nodes = (uint16_t)rd(d, off + 4, 2), f.x6 = (uint16_t)rd(d, off + 6, 2);
    for (uint32_t i = 0; i <= nb; i++) f.buckets.push_back((uint32_t)rd(d, off + 8 + 4 * i, 4));
    size_t eo = ((off + 8 + 4 * (nb + 1) + 3) & ~(size_t)7) + 8;
    size_t n = rd(d, eo - 8, 8) / 16;
    size_t oo = eo + 16 * n + 8;
    size_t ds = ((oo + 4 * n + 7) & ~(size_t)7) + 8;
    size_t dsize = rd(d, ds - 8, 8);
    size_t ns = ((ds + dsize + 3) & ~(size_t)3) + 4;
    size_t nsize = rd(d, ns - 4, 4);
    if (ns + nsize > d.size()) throw std::runtime_error("fixed hash: names past the end");
    f.names = d.substr(ns, nsize);
    for (size_t i = 0; i < n; i++) {
        FHEntry e;
        e.node = (uint16_t)rd(d, eo + 16 * i, 2);
        uint16_t noff = (uint16_t)rd(d, eo + 16 * i + 2, 2);
        e.hash = (uint32_t)rd(d, eo + 16 * i + 4, 4);
        e.next = (uint32_t)rd(d, eo + 16 * i + 8, 4);
        uint32_t doff = (uint32_t)rd(d, eo + 16 * i + 12, 4);
        if (nsize) {
            size_t z = d.find('\0', ns + noff);
            if (z == std::string::npos) throw std::runtime_error("fixed hash: unterminated name");
            e.name = d.substr(ns + noff, z - (ns + noff));
        }
        e.name_off = noff;
        if (e.is_hash()) {
            e.sub = std::make_shared<FHash>(FHash::parse(d, ds + doff));
        } else {
            size_t size = rd(d, ds + doff, 8);
            if (ds + doff + 8 + size > d.size()) throw std::runtime_error("fixed hash: data past the end");
            e.data = d.substr(ds + doff + 8, size);
        }
        f.entries.push_back(std::move(e));
    }
    return f;
}

std::string FHash::write(size_t off) {
    size_t nb = buckets.size() - 1;
    std::string out;
    put(out, magic, 1), put(out, version, 1), put(out, nb, 2), put(out, nodes, 2), put(out, x6, 2);
    for (uint32_t b : buckets) put(out, b, 4);
    auto pad_to = [&](std::string &buf, size_t align, size_t base) {
        while ((base + buf.size()) % align) buf.push_back(0);
    };
    size_t n = entries.size();
    // the u64 entry-table size sits at (x + 3) & -8 (x = end of the buckets): when x % 8 == 4 it overlaps the last
    // bucket word
    size_t x = off + out.size();
    size_t size_pos = (x + 3) & ~(size_t)7;
    out.resize(size_pos - off < out.size() ? size_pos - off : out.size());
    while (off + out.size() < size_pos) out.push_back(0);
    put(out, 16 * n, 8);
    size_t eo = out.size();
    out.append(16 * n, '\0');
    put(out, 4 * n, 8);
    for (size_t i = 0; i < n; i++) put(out, 16 * i, 4);
    pad_to(out, 8, off);
    out.append(8, '\0');  // data size, patched below
    size_t ds_rel = out.size();
    std::string data;
    std::vector<size_t> offs;
    for (FHEntry &e : entries) {
        if (e.is_hash()) {
            offs.push_back(data.size());
            data += e.sub->write(off + ds_rel + data.size());
        } else {
            while ((off + ds_rel + data.size()) % 8) data.push_back(0);
            offs.push_back(data.size());
            put(data, e.data.size(), 8);
            data += e.data;
        }
    }
    if (!entries.empty() && !entries.back().is_hash())
        while ((off + ds_rel + data.size()) % 8) data.push_back(0);
    put_at(out, ds_rel - 8, data.size(), 8);
    out += data;
    pad_to(out, 4, off);
    // names: the stored table kept (its offsets stay valid); names it lacks appended
    std::string nm = names;
    for (FHEntry &e : entries) {
        if (e.name.empty()) {
            if (e.name_off < 0) e.name_off = 0;
            continue;
        }
        std::string z = e.name + std::string(1, '\0');
        if (e.name_off < 0 || nm.compare((size_t)e.name_off < nm.size() ? (size_t)e.name_off : nm.size(), z.size(), z)) {
            size_t i = nm.find(z);
            if (i == std::string::npos) i = nm.size(), nm += z;
            e.name_off = (long)i;
        }
    }
    put(out, nm.size(), 4);
    out += nm;
    for (size_t i = 0; i < n; i++) {
        const FHEntry &e = entries[i];
        put_at(out, eo + 16 * i, e.node, 2);
        put_at(out, eo + 16 * i + 2, (uint16_t)e.name_off, 2);
        put_at(out, eo + 16 * i + 4, e.hash, 4);
        put_at(out, eo + 16 * i + 8, e.next, 4);
        put_at(out, eo + 16 * i + 12, offs[i], 4);
    }
    return out;
}

FHEntry *FHash::get(const std::string &name) {
    for (FHEntry &e : entries)
        if (e.name == name) return &e;
    return nullptr;
}

void FHash::rebuild_index() {
    size_t nb = buckets.size() - 1;
    std::vector<uint32_t> heads(nb, 0xFFFFFFFFu);
    for (FHEntry &e : entries) e.hash = fhash_name_hash(e.name);
    for (size_t i = 0; i < entries.size(); i++) {  // (as the game's files: the latest entry heads its bucket)
        size_t b = entries[i].hash % nb;
        entries[i].next = heads[b];
        heads[b] = (uint32_t)(16 * i);
    }
    uint32_t last = buckets.back();
    buckets = heads;
    buckets.push_back(last);
}

FHash FHash::deep_copy() const {
    FHash c = *this;
    for (FHEntry &e : c.entries)
        if (e.sub) e.sub = std::make_shared<FHash>(e.sub->deep_copy());
    return c;
}

}  // namespace rando
