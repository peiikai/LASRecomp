// The native seed generator (docs/randomizer.md, stage 5): tools/rando/las_headless.py main() in C++, from the
// request the Randomizer tab writes to the seed's files, in the Python's order and with its results, file for file.
//
//   prepare()            the request on the defaults, LAS's logic and pool and every -sanity module (prepare.h)
//   shuffle_entrances()  the entrance shuffle (entrances.h)
//   plan_rooms()         dungeon room shuffle: the layouts and their item logic (rooms.h); a layout the fill or the
//                        playthrough rejects: the run starts again on the next one, as the Python runs itself again
//   finish_prefill()     the mini-boss souls' gates (prepare.h)
//   ItemShuffler         LAS's item fill (shuffler.h)
//   OutputsBuilder       logic.json, the playthrough check (unbeatable: GenStatus::Unbeatable), the Hint Sheet's
//                        steps, the Route, the hint picks, LAS's spoiler (outputs.h)
//   the seed build       decide mode (apply/decide.h): every random choice of the file-writing steps, the decisions,
//                        the tables and the game files
//   OutputsBuilder       settings.json, the tracker, the readable spoiler
//
// What it writes (through GenerateOutput::write, by path in the seed folder):
//   seed.json            the record the runtime reads (runtime/rando/seed.c): format, version, seed_id, settings
//                        (settings.json's), tables (each the lines of one of the runtime's tables) and decisions (what
//                        runtime/rando/apply/build.cpp builds the game files from when the seed loads)
//   exefs/*.pchtxt       the code patches
//   spoiler.txt, spoiler_view.txt, spoiler.html (create-spoiler), entrances_<seed>.txt, hints.txt, seed_hints.txt,
//   pots_<seed>.txt, bushes_<seed>.txt, rocks_<seed>.txt, digs_<seed>.txt: the files people read
// legacy (GenerateOptions::legacy) adds what the Python generator writes, for older builds and comparisons: romfs/
// (the changed game files), each table as its file (pots.tsv, flags_ext, check_flags.tsv, ...), settings.json,
// decisions.json, logic.json, spheres.json, hint_steps.json, route.json, seed_hints.json, tracker.json, tracker.tsv.
//
// For the app:
//   - Thread: run it on a worker thread. It keeps no global state (several may run at once); the callbacks are called
//     on that thread.
//   - Memory: everything is held in memory until the end: the seed's game files (up to ~100 MB with every -sanity
//     option on) besides the logic and the fill's state; files are written after the last step.
//   - Progress: progress(fraction, label) with las_headless.py's `stage` labels and fractions: "Preparing" 0.01,
//     "Shuffling items" 0.04, "Shuffling items (<n>/<total> placed)" up to 0.66, "Checking the seed is beatable"
//     0.67, "Building game files (<n> tasks)" 0.72 - 0.98, "Building the check tracker" 0.99, "Done" 1.0.
//   - Cancel: set *cancel; it stops within a fraction of a second (GenStatus::Cancelled), nothing written.
//   - Failure: nothing is written; GenerateResult::error says why, player_error is the reason a player can act on
//     (las_headless.py's 'error <text>' lines), empty when there is none.
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "prepare.h"
#include "value.h"

namespace rando {
namespace solve {

enum class GenStatus {
    Done = 0,
    Failed = 1,       // the fill (or a setting) failed: las_headless.py's exit 1
    BuildFailed = 2,  // a file-writing step failed: its exit 2 ('MODS ERROR')
    Unbeatable = 3,   // the playthrough check found the seed unbeatable: its exit 3
    // (4: retired - dungeon room shuffle was handed to the Python generator before it was native)
    Cancelled = 5,
    WriteFailed = 6,  // GenerateOutput::write said no
};

struct GenerateOptions {
    bool legacy = false;      // also the Python generator's own files (above)
    std::string folder_name;  // the seed folder's name (the readable spoiler's title)
};

struct GenerateOutput {
    // a file of the seed, by its path in the seed folder ("seed.json", "exefs/1.0.0.pchtxt", "romfs/region_common/...");
    // false: it couldn't be written (the generation stops)
    std::function<bool(const std::string &path, const std::string &bytes)> write;
};

struct GenerateResult {
    GenStatus status = GenStatus::Failed;
    std::string error;         // what went wrong (one line; empty when done)
    std::string player_error;  // a reason the player can act on ("Triforce hunt: ..."), or empty
    std::vector<std::string> log;    // what las_headless.py prints along the way (its main lines)
    std::vector<std::string> files;  // the paths written, in order
    double seconds = 0;
};

// request: the request.json object (rando_tab.mm start_generation). progress / cancel may be empty / null.
GenerateResult generate(const Value &request, const GameFiles &game, const GenerateOutput &out,
                        const std::function<void(double fraction, const std::string &label)> &progress,
                        const std::atomic<bool> *cancel, const GenerateOptions &opt);

// GenerateOutput writing into a folder on disk (its subfolders made as needed)
GenerateOutput generate_to_dir(const std::string &dir);

// lasrando generate <romfs> <request.json> <out folder> [--legacy]: prints the stage lines as las_headless.py does
// (`stage <fraction> <label>`), then 'done' or the error ('error <text>' for a player's reason); exit status as
// las_headless.py's
int generate_main(const std::string &romfs, const std::string &request, const std::string &out, bool legacy);

}  // namespace solve
}  // namespace rando
