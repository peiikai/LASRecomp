// Message files (regionXX/<lang>/LogicMsg/*.msbt etc.): MsgStdBn, UTF-8 text, version 3. Labels (LBL1), attribute
// records (ATR1, kept as they are) and strings (TXT2: raw bytes, the game's control tags included).
//
// Ported from LAS Recomp's tools/rando/msbt.py; Msbt::pack writes what its pack() writes: labels re-bucketed in
// index order, the header rebuilt (bytes 0xA-0xB and 0x16-0x1F zero), sections padded to 16 with 0xAB.
#pragma once
#include <stdint.h>

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace rando {

struct Msbt {
    std::string bom;
    uint8_t enc = 0, version = 0;
    std::vector<std::pair<std::string, std::string>> sections;  // (magic, payload) in file order
    uint32_t slots = 0;
    std::map<std::string, uint32_t> labels;  // label -> string index
    std::vector<std::string> strings;        // raw, each ending in its NUL
    bool has_atr = false;
    std::string atr;

    static Msbt parse(const std::string &data);  // throws on bad data
    std::string pack() const;

    const std::string *text(const std::string &label) const;
    void set(const std::string &label, std::string raw);           // (a new label gets the next index and a zero
                                                                   // attribute record)
    void copy_attr(const std::string &label, const std::string &src);  // label speaks like src (who, which box)
};

}  // namespace rando
