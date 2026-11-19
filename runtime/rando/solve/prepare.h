// The native solver's preparation (docs/randomizer.md, stage 5): tools/rando/las_headless.py main() from the request
// to the item fill, apart from the entrance shuffle and dungeon room shuffle (their own ports).
//
//   prepare()        the request merged into the defaults, LAS's logic and item pool (logic.yml, items.yml) and every
//                    change made to them before the entrance section: the Escape Bottle / Hint Sheet bottles,
//                    hookshot-anything, the collapsed Eagle's Tower, keysanity, souls, pots, bushes, rocks, digs,
//                    apple trees, shopsanity, trendsanity, pets, the Triforce hunt. = the 'pre_entrances' checkpoint.
//   finish_prefill() what runs after the entrance section and before the fill, room shuffle off: the mini-boss souls'
//                    gates on the vanilla dungeons. = the 'prefill' checkpoint.
//
// Same results as the Python, values and order (dicts in insertion order, settings['excluded-locations'] a list in
// CPython's set order): lasrando prepare-check compares them with a recorded run. Random draws (the trap filler of
// pots, bushes, rocks, apple trees) come from their own PyRandom streams, seeded as the Python seeds them.
//
// Game files are read through GameFiles (the room files the -sanity scans walk, Field.lvb's place names), as the
// seed build does (apply/seed_build.h): the same code runs on files on disk (lasrando) or in the app's romfs.
// Ported from tools/rando (ours) and LAS-Randomizer's data (MIT, Owen-Splat and contributors).
#pragma once

#include <stdint.h>

#include <functional>
#include <string>
#include <vector>

#include "value.h"

namespace rando {
namespace solve {

struct GameFiles {
    // a game file by its romfs path ("region_common/level/Field/Field_16C.leb"); false: no such file
    std::function<bool(const std::string &path, std::string &out)> read;
    // the names in a romfs folder (files and folders, any order); false: no such folder
    std::function<bool(const std::string &dir, std::vector<std::string> &names)> list;
};

struct Prepared {
    Value settings, item_defs, logic_defs;  // (dicts; settings['excluded-locations'] a list in set order)
    // what the later steps read from the -sanity modules (las_headless.py's locals of the same names), location ->
    // dict: pots {level, room, key (16 hex digits), n, shell}, bushes / rocks {level, room, key, n, tile, zone, shell},
    // digs {room, tile, y, blocker, shell, zone}, apple trees {room, key, tile, zone, slot}
    Value pot_locs, bush_locs, rock_locs, dig_locs, apple_locs;
    Value sanity_inside;  // shop / Trendy checks inside interiors: {door: [[location, condition], ...]}
    std::vector<std::string> soul_kinds;  // 'boss', 'mini'
    std::vector<std::string> pets;
    int64_t tri_needed = 0, tri_placed = 0;
    std::vector<std::string> said;  // the lines las_headless prints along the way
};

// las_headless.main() up to the 'pre_entrances' checkpoint. Throws PyError as the Python raises (SystemExit for a bad
// setting, field_regions.NoRegion as LookupError...).
Prepared prepare(const Value &request, const GameFiles &game);

// from the 'post_entrances' checkpoint (st.settings / item_defs / logic_defs as the entrance section left them) to
// the 'prefill' one, for a seed without dungeon room shuffle (PyError otherwise: that part isn't here)
// shuffled: the dungeons the room shuffle laid out (rooms.h: the souls' gates are only for vanilla layouts)
void finish_prefill(Prepared &st, const std::vector<std::string> &shuffled = {});

// GameFiles reading a romfs folder on disk
GameFiles game_files_dir(const std::string &romfs);

// lasrando prepare-check <romfs> <request.json> <record folder>: prepare() against pre_entrances.json and, for a seed
// without room shuffle, finish_prefill() from post_entrances.json against prefill.json. 0 when both are exact.
// room_plan_file: a room-shuffle run's placements.json (LARECOMP_CHECK_ONLY) or decisions.json, for its room plan
int prepare_check(const std::string &romfs, const std::string &request, const std::string &record,
                  const std::string &room_plan_file = "");

}  // namespace solve
}  // namespace rando
