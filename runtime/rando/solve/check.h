// lasrando solve-check <prefill.json> <postfill.json>: the native fill (shuffler.h) run from a recorded Python run's
// state before the fill (tools/rando/record.py checkpoint 'prefill': settings, item_defs, logic_defs as LAS's
// ItemShuffler gets them) and compared with what the Python made ('postfill': placements with every key and the key
// order, random.getstate(), the settings as the fill left them). Prints each comparison and the time; 0 when exact.
#pragma once

#include <string>

namespace rando {
namespace solve {

int solve_check(const std::string &prefill_path, const std::string &postfill_path);

}  // namespace solve
}  // namespace rando
