// Archives (region_common/ui/*.arc): SARC, little endian, version 0x0100. A header, the file table (SFAT: name hash,
// name offset, data begin / end, sorted by hash), the names (SFNT, each padded to 4), then the files' data.
//
// Written for LAS Recomp from the format (no code copied). Sarc::write lays an archive out as oead 1.3's
// SarcWriter.from_sarc(...).write() does (the Python generator rewrites DgnTex.arc that way): files sorted by name
// hash; each file aligned to the archive's minimum alignment (the source's, guessed as the greatest power-of-two
// common divisor of its data offsets, at least 4), or more when the file starts with a binary file header that asks
// for it (byte order mark at 0xC, its size at 0x1C, alignment 1 << byte 0xE); the data starting at the least common
// multiple of all files' alignments.
// Only extensions that oead aligns by that rule alone are accepted for writing: the ones in the game's archives
// (bflan, bflyt, bntx, bnsh, bushvt). oead gives other extensions table-driven alignments this code doesn't know.
#pragma once
#include <stdint.h>

#include <string>
#include <utility>
#include <vector>

namespace rando {

struct Sarc {
    uint32_t hash_multiplier = 0x65;
    uint32_t min_alignment = 4;
    std::vector<std::pair<std::string, std::string>> files;  // (name, data) in table order

    static Sarc parse(const std::string &data);  // throws on bad data (little-endian archives only)
    std::string write() const;                   // throws on an extension it can't align as oead does

    const std::string *get(const std::string &name) const;
    void put(const std::string &name, const std::string &data);  // (replace or add)
};

}  // namespace rando
