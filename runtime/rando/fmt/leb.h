// Room files (region_common/level/<Level>/<Room>.leb): a fixed hash (fixed_hash.h) whose "actor" entry holds one
// record per actor, plus points, rails and the tile grid.
//
// Ported from LAS-Randomizer's RandomizerCore/Tools/leb.py (MIT, Owen-Splat and contributors). Room::repack writes
// exactly what that one's Room.repack() writes, so a room edited here is the same file the Python generator makes:
//   - the names section is rebuilt: per actor its name, its string parameters, its links' strings (no sharing), then
//     each top-level entry's name;
//   - a parameter that isn't a float or a string is written back as a whole number (type 3), whatever its type was;
//   - values are kept as the Python keeps them (floats as doubles) and rounded to 32 bits when written;
//   - with the grid read (with_grid), each tile's bytes 4-7 are written as a copy of bytes 0-3.
#pragma once
#include <array>

#include "fixed_hash.h"

namespace rando {

struct LebParam {
    enum Kind : uint8_t { Int, Float, Str } kind = Int;
    uint32_t i = 0;  // (Int; also what a Str is read from: an offset into the names)
    double f = 0;    // (Float)
    Bytes s;         // (Str)

    static LebParam of_int(uint32_t v) { LebParam p; p.kind = Int, p.i = v; return p; }
    static LebParam of_float(double v) { LebParam p; p.kind = Float, p.f = v; return p; }
    static LebParam of_str(const Bytes &v) { LebParam p; p.kind = Str, p.s = v; return p; }
    bool operator==(const LebParam &o) const;
};

struct LebLinks {  // leb.py Relationship: the actor's links to other actors and to rails
    uint8_t e = 0, k = 0, b = 0, x = 0, y = 0, z = 0;
    Bytes null6;  // (bytes 0x8A-0x8F, kept)
    struct ActorLink { LebParam p[2]; uint32_t actor = 0; };       // section 1 (x of them)
    struct RailLink { LebParam p[2]; uint32_t rail = 0, point = 0; };  // section 2 (z)
    std::vector<ActorLink> actors;
    std::vector<RailLink> rails;
    std::vector<uint32_t> ids;  // section 3 (y)
};

struct LebActor {
    uint64_t key = 0;
    Bytes name;  // (ends in the key as 16 hex digits)
    uint16_t type = 0, xE = 0;
    uint32_t room_id = 0;
    double pos[3] = {0, 0, 0}, rot[3] = {0, 0, 0}, scale[3] = {1, 1, 1};
    LebParam params[8];
    std::array<std::pair<uint8_t, uint16_t>, 4> switches{};
    LebLinks links;
};

struct LebTile {  // leb.py Grid.TileData
    uint8_t flags[4] = {0, 0, 0, 0};  // (bytes 0-3; bytes 4-7 are written as their copy)
    uint32_t chain = 0;
    double elevation = 0;
};

struct LebGrid {
    const FhEntry *data_entry = nullptr;  // (into the room's fixed hash)
    const FhEntry *chain_entry = nullptr;
    uint16_t room_height = 0, room_width = 0;
    double tile_size = 0, x = 0, z = 0;
    std::vector<LebTile> tiles;  // 80 (top-down) or 20 (side view)
};

struct LebRoom {
    FixedHash fh;
    std::vector<LebActor> actors;
    bool has_grid = false;
    LebGrid grid;

    // with_grid: read the tile grid too (leb.py Room(data, edit_grid=True)); repack then rewrites it
    static LebRoom parse(const Bytes &data, bool with_grid = false);
    Bytes repack();
};

}  // namespace rando
