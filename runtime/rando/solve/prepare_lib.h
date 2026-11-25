// Shared pieces of the preparation (prepare.h): the generated data, Python's small semantics, and the -sanity scans
// (prepare_scan.cpp).
#pragma once

#include <stdint.h>

#include <map>
#include <string>
#include <vector>

#include "../pyrandom.h"
#include "prepare.h"
#include "value.h"

namespace rando {
namespace solve {

// tools/rando/export_solve_data.py's JSON (solve_data.cpp, solve_gate_data.cpp), parsed once. Shared: deepcopy()
// anything that gets changed.
const Value &prep_data();
const Value &prep_gate_data();

// pots._and / seashell_spots._and / field_regions: the truthy parts as '(a) & (b)', None when there are none
Value py_and(const std::vector<Value> &conds);
double py_floordiv(double a, double b);  // a // b on floats
int64_t py_round(double x);              // round(x): halves to even
std::string py_str(const Value &v);      // str(x)
std::vector<std::string> str_list(const Value &list);
std::string hex16(uint64_t key);         // '%016X'

// the Field's place name ids per screen tile (field_regions.tile_zones): {room: [80 zone ids]}, empty without Field.lvb
using TileZones = std::map<std::string, std::vector<int64_t>>;
TileZones tile_zones(const GameFiles &game);

// the -sanity modules' scan + add_to_logic. Each returns {location: info} (the Python's locs) and adds the locations
// to logic_defs and their filler to item_defs, as the module does; rng: its Random(f"{seed}-<name>") stream.
Value pots_add(const GameFiles &game, const std::vector<std::string> &missable, Value &logic_defs, Value &item_defs,
               const std::string &traps, PyRandom &rng);
Value bushes_add(const GameFiles &game, const TileZones &zones, bool open_mabe, Value &logic_defs, Value &item_defs,
                 const std::string &traps, PyRandom &rng);
Value rocks_add(const GameFiles &game, const TileZones &zones, bool classic_d2, bool open_mabe, Value &logic_defs,
                Value &item_defs, const std::string &traps, PyRandom &rng);
Value digs_add(const GameFiles &game, const TileZones &zones, Value &logic_defs, Value &item_defs);
Value apples_add(const GameFiles &game, const TileZones &zones, Value &logic_defs, Value &item_defs,
                 const std::string &traps, PyRandom &rng);

}  // namespace solve
}  // namespace rando
