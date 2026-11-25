// Level config files (region_common/level/<Level>/<Level>.lvb): a fixed hash (fixed_hash.h) with the level's zones
// (one per room: music, ambience, room type) and its config (companions allowed, ...).
//
// Ported from LAS-Randomizer's RandomizerCore/Tools/lvb.py (MIT, Owen-Splat and contributors); LvbLevel::repack
// writes what its Level.repack() writes: each zone's four names written back padded to 32 bytes, the config as 7
// bytes (companions as 0 / 1, then 0xFF).
#pragma once
#include "fixed_hash.h"

namespace rando {

struct LvbZone {
    uint32_t room_id = 0;
    Bytes unknown1;  // (bytes 0x04-0x3B)
    Bytes bgm, se_amb, group_amb;
    Bytes unknown2;  // (bytes 0x9C-0xAF)
    Bytes room_type;
};

struct LvbConfig {
    uint8_t attr1 = 0, attr3 = 0, attr4 = 0, attr5 = 0, attr6 = 0;
    bool allow_companions = false;
};

struct LvbLevel {
    FixedHash fh;
    std::vector<LvbZone> zones;
    LvbConfig config;

    static LvbLevel parse(const Bytes &data);
    Bytes repack();
};

}  // namespace rando
