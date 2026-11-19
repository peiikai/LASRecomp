// What LAS Recomp's world steps (ours_world.cpp) share with their scans and tables (ours_world_scan.cpp): the game's
// rooms read as tools/rando/{seashell_spots, apple_trees, digs}.py scan them, the seed folder's tables the modules
// write (pots.write_tsv, digs.tsv, ...), and, for decide mode (decide.h), the -sanity locations the modules' scans
// and add_to_logic make before the item fill.
#pragma once
#include <stdint.h>

#include <array>
#include <map>
#include <string>
#include <vector>

#include "seed_build.h"

namespace rando {
namespace las {

// seashell_spots.scan: {seashell index: (kind, level, room, x, y, z, host key, parameter)}; x y z: where the item
// shows (a buried shell / a host: where it stands; a tree / bomb hole: the free tile it lands on)
struct ShellSpot {
    std::string kind, level, room;
    double x = 0, y = 0, z = 0;
    uint64_t key = 0;
    int param = 0;
};
std::map<int64_t, ShellSpot> shell_scan(SeedBuild &b);

// apple_trees.scan: each apple tree's room, actor key and its items' 3 landing spots (x, y, z)
struct AppleTree {
    std::string room;
    uint64_t key = 0;
    std::vector<std::array<double, 3>> landings;
};
std::vector<AppleTree> apple_scan(SeedBuild &b);

// digs.scan: every dig tile of the Field (room, tile 0-79, ground height, what stands on it: "" for nothing, else
// "bush" / "grass" / "other"; shell: the seashell index buried there, -1 for none), in the scan's order
struct DigTile {
    std::string room;
    int tile = 0;
    double y = 0;
    std::string blocker;
    int64_t shell = -1;
};
std::vector<DigTile> dig_scan(SeedBuild &b);

// pots.write_tsv over the seed's rooms as they are now (hide_scale 0: no cut flags, as for pots)
std::string pots_tsv(SeedBuild &b, const char *prefix, double hide_scale);

std::string py_g(double v);                     // f'{v:g}'
std::string pretty_item(const std::string &it);  // spoiler_view.pretty_item

// Decide mode: the pre-fill -sanity locations (las_headless.py: pots, bushes, rocks, digs, each module's scan +
// add_to_logic on LAS's logic, in that order) as JSON: {"pots" / "bushes" / "rocks": {location: {level, room, key
// (16 hex digits), ...}}, "digs": {location: {room, tile, y, blocker, shell}}, "shells": {location: seashell index}
// (seashell_spots.locations of the logic they leave)}. Worked out once per build (SeedBuild::carry).
const std::string &sanity_json(SeedBuild &b);

// tools/rando/loading_zones.json as [[level, room, index, [parameters]]] (ours_world_data.cpp)
std::string loading_zones_json();

}  // namespace las
}  // namespace rando
