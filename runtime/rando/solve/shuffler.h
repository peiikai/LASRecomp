// The item fill, natively: LAS-Randomizer's ItemShuffler (third_party/LAS-Randomizer/RandomizerCore/shuffler.py)
// with every LAS Recomp change in it, and the condition evaluation tools/rando/las_headless.py installs
// (check_access). Ported from LAS-Randomizer's shuffler.py (MIT, Owen-Splat and contributors).
//
// Given the same settings, item_defs and logic_defs it makes the same placements and leaves Python's random module
// in the same state, draw for draw: one PyRandom seeded with random.seed(settings seed), the same calls in the same
// order (lasrando solve-check compares both with a recorded run).
//
//   ItemShuffler sh(settings['seed'], settings, item_defs, logic_defs.deepcopy());
//   Value placements = sh.run();          // PyError on failure, with the Python's message ("fill stuck: ...")
//   sh.random_state();                    // random.getstate() after the fill
//
// As in the Python, settings and item_defs are the caller's objects and are changed in place (starting items,
// trap counts, 'excluded-locations' becomes a set...), and logic_defs is the shuffler's own (las_headless passes a
// deep copy and reads sh.logic_defs afterwards). The placements dict holds, in the Python's order: 'settings',
// 'force-junk', 'force-vanilla', 'starting-items', 'dungeon-entrances', 'indexes', then each item location (item name
// or None), then 'moblin-cave' / 'rooster-statue' when they aren't item locations. Python sets (force-junk =
// settings['excluded-locations'], force-vanilla) are lists in CPython's iteration order (PyStrSet).
#pragma once

#include <stdint.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "value.h"

namespace rando {
namespace solve {

class ItemShuffler {
public:
    ItemShuffler(const std::string &seed, Value settings, Value item_defs, Value logic_defs);
    ~ItemShuffler();

    Value run();  // ItemShuffler.run: the placements
    // ItemShuffler.stop (the GUI's cancel; thread_active = False): run() gives up soon after, with PyError
    // "RuntimeError: stopped". Safe from another thread. (Like the Python, a fill that can't finish undoes and retries
    // for ever unless it is a dungeon room shuffle layout: FILL_STUCK.)
    void stop();

    Value settings, item_defs, logic_defs;
    Value vanilla_locations;   // (after run: the set, as a list in CPython's order)
    Value dungeon_entrances;
    std::function<void(const std::string &)> log;  // the Python's print() lines (default: none)

    // random.getstate(): (3, (624 words + the index), None) as [3, [625 ints]]
    std::vector<int64_t> random_state() const;
    int random_version() const { return 3; }

    // also sort each paced order with this platform's pow() and count where it would differ (Stats; checks only)
    void compare_libm_pow(bool on);

    // what the fill did, for the check and for tuning
    struct Stats {
        long searches = 0;           // reachability searches
        long pow_calls = 0;          // pacing keys computed with pow (exponent != 1)
        long pow_libm_differs = 0;   // ... where this platform's pow() gives another double
        long pow_order_changes = 0;  // paced orders that this platform's pow() would have sorted differently
        long undo_dungeon = 0, undo_fill = 0, undo_shells = 0, undo_triforce = 0;  // undone placements, by cause
        long tarin_tries = 0;        // items tried at Tarin before one opened a check
    } stats;

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

}  // namespace solve
}  // namespace rando
