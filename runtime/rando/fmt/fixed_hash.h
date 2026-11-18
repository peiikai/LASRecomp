// Fixed hash: the container of the game's room (.leb) and level (.lvb) files: named entries, each either a nested
// fixed hash or a block of bytes, with a names section.
//
// Ported from LAS-Randomizer's RandomizerCore/Tools/fixed_hash.py (MIT, Owen-Splat and contributors). The writer
// produces exactly the bytes that one does: seeds made by the Python generator and by this code must be the same
// files (docs/randomizer.md), so its layout choices are kept as they are, quirks included (noted below).
#pragma once
#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <string>
#include <vector>

namespace rando {

using Bytes = std::string;  // (raw bytes; std::string for its value semantics)

uint32_t fixed_hash_string(const Bytes &s);  // the entries' name hash

struct FixedHash;

struct FhEntry {
    uint16_t node = 0;   // <= 0xFFED: a nested fixed hash; >= 0xFFF0: bytes
    Bytes name;
    uint32_t next = 0;
    std::shared_ptr<FixedHash> sub;  // (node <= 0xFFED)
    Bytes data;                      // (node >= 0xFFF0)
};

struct FixedHash {
    uint8_t magic = 0, version = 0;
    uint16_t num_buckets = 0, num_nodes = 0, x6 = 0;
    std::vector<uint32_t> buckets;  // (num_buckets of them; the file has one more, which the writer drops)
    std::vector<FhEntry> entries;
    Bytes names;  // the names section

    // parse the fixed hash at `off` in `data` (offsets in it are relative to the whole buffer); throws on bad data
    static FixedHash parse(const Bytes &data, size_t off = 0);
    // the bytes, laid out as if at `offset` in the file (alignment)
    Bytes to_binary(size_t offset = 0) const;

    const FhEntry *find(const Bytes &name) const;
    FhEntry *find(const Bytes &name);
};

// little-endian helpers shared by the format code
uint64_t read_le(const Bytes &d, size_t at, int n);  // (bytes past the end read as missing, as Python's slice)
float read_f32(const Bytes &d, size_t at);
Bytes read_cstr(const Bytes &d, size_t at);
void put_le(Bytes &out, uint64_t v, int n);
void put_f32(Bytes &out, double v);  // (rounded to 32 bits: struct.pack('<f'))

}  // namespace rando
