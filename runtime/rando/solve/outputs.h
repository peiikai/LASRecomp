// The seed's text outputs, natively (docs/randomizer.md, stage 5): what tools/rando/las_headless.py main() and the
// modules it calls write from the item fill's result, after 'placements ready'. Each is the Python's file byte for
// byte (lasrando outputs-check compares them with a seed folder the Python wrote):
//
//   logic.json         the seed's final logic (sh.logic_defs; the tracker's input)
//   spheres.json       the independent playthrough (tools/rando/playthrough.py), with its check-only aliases for the
//                      rooms LAS's logic never defines (D6-4D, D6-2A); its verdict is beatable / exit 3
//   hint_steps.json    the Hint Sheet's steps (tools/rando/hint_sheet.py steps)
//   route.json         the spoiler's Route (tools/rando/route.py)
//   seed_hints.json    the owl statue / phone hints' picks (tools/rando/seed_hints.py pick; Hints on), drawn from
//                      random.Random(f"{seed}-hints")
//   spoiler.txt        LAS's spoiler log (RandomizerCore/spoiler.py, written by the fill as spoiler_<seed>.txt;
//                      las_headless renames it)
//   settings.json      the settings the runtime reads (written near the end of main)
//   tracker.json/.tsv  the check tracker's data (tools/rando/tracker.py)
//   spoiler_view.txt, spoiler.html   the readable spoiler (tools/rando/spoiler_view.py)
//
// Ported from those files (ours) and LAS-Randomizer's spoiler.py (MIT, Owen-Splat and contributors).
//
// What the file-writing steps make is read through Input::read_seed (a seed folder's file, by its name there):
// check_flags.tsv, shell_spots.tsv, apple_trees.tsv, pots.tsv, bushes.tsv, rocks.tsv, dungeon_rooms.tsv,
// dungeon_map.tsv, entrances_<seed>.txt, seed_hints.txt, and the seed's romfs/ copies of rooms (region_common/level)
// and GlobalFlags.gsheet (the game's own file when the seed has none). The game's files come from Input::romfs.
#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "value.h"

namespace rando {
namespace solve {

struct OutputsInput {
    Value settings;     // as the fill left them (postfill.json "settings"; sets as lists in Python's order)
    Value placements;   // the fill's placements (postfill.json "placements", every key)
    Value logic_defs;   // the shuffler's logic after the fill (ItemShuffler::logic_defs)
    Value ent;          // las_headless's entrance shuffle (doors, entry, exit) or None (post_entrances.json "ent")
    // the settings as main() leaves them when it writes settings.json (decisions.json "settings": keys added after
    // the fill, values changed since); keys the fill knew keep its order, new ones follow in this dict's order
    Value final_settings;
    std::string romfs;        // the game's files
    std::string folder_name;  // the seed folder's name (spoiler_view's title)
    // a file the file-writing steps made, by its path in the seed folder; false when the seed has none
    std::function<bool(const std::string &rel, std::string &data)> read_seed;
};

struct Outputs {
    std::vector<std::pair<std::string, std::string>> files;  // (name in the seed folder, bytes), in writing order
    std::vector<std::string> log;                            // the lines the Python prints for these steps
    int exit_code = 0;  // 3: unbeatable (las_headless exits before the game files; only the files before are made)
};

// Every output above, in las_headless's order. PyError for what makes the Python fail (a KeyError in the logic...).
Outputs build_outputs(const OutputsInput &in);

// lasrando outputs-check <romfs> <record folder> <seed folder>: the fill rerun from <record>/prefill.json (checked
// against postfill.json), every output rebuilt and compared byte for byte with the seed folder's. 0 when all match.
int outputs_check(const std::string &romfs, const std::string &record, const std::string &seed_dir);

// Python's json.dumps(v) (indent < 0) / json.dumps(v, indent=n): ensure_ascii, float repr, dict order kept
std::string py_json(const Value &v, int indent = -1);

}  // namespace solve
}  // namespace rando
