// The lossless fixed hash reader / writer LAS Recomp's level builder uses (tools/rando/fixedhash.py, ported): every
// stored field kept (bucket words, hashes, chains, the names section), only sizes and offsets recomputed, so the
// generated dungeon levels (.lvb) come out as the Python writes them. LAS's own reader (fixed_hash.h) recomputes names
// and hashes and is what rooms and level configs are written with; this one is for whole levels.
#pragma once
#include <stdint.h>

#include <memory>
#include <string>
#include <vector>

namespace rando {

struct FHash;

struct FHEntry {
    uint16_t node = 0;  // <= 0xFFED: a nested container (sub); else raw bytes (data)
    std::string name;
    uint32_t hash = 0, next = 0;
    std::shared_ptr<FHash> sub;
    std::string data;
    long name_off = -1;  // the name's offset in the names section (-1: None - found or appended when written)
    bool is_hash() const { return node <= 0xFFED; }
};

struct FHash {
    uint8_t magic = 0, version = 0;
    uint16_t nodes = 0, x6 = 0;
    std::vector<uint32_t> buckets;  // (bucket count + 1 words, as stored)
    std::vector<FHEntry> entries;
    std::string names;

    static FHash parse(const std::string &d, size_t off = 0);  // throws on bad data
    std::string write(size_t off = 0);  // (assigns names' offsets, as the Python does)
    FHEntry *get(const std::string &name);
    void rebuild_index();  // hashes, buckets and chains after adding / renaming entries
    FHash deep_copy() const;  // (nested containers copied too)
};

uint32_t fhash_name_hash(const std::string &name);

}  // namespace rando
