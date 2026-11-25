// Dungeon room shuffle in the seed build: tools/rando/dungeon_gen.py write_all (and rooms.py transplant,
// level_builder.py, bntx_merge.py's uses) ported. The layouts and door jumps come from the seed's decisions
// ("room-plan", tools/rando/record.py); this makes the files and tables the Python writes from them.
#pragma once
#include <stddef.h>

#include <string>
#include <utility>
#include <vector>

#include "seed_build.h"

namespace rando {

// tools/rando/room_catalog.json (rooms_data.cpp, tools/rando/export_rooms_data.py)
struct RoomEntry {
    const char *name, *unit;  // (unit: its cells joined by spaces)
    bool crystal;              // (holds an ObjCrystalSw)
};
extern const RoomEntry ROOM_CATALOG[];
extern const size_t ROOM_CATALOG_SIZE;
extern const char *const DEAD_ROOMS[];
extern const size_t DEAD_ROOMS_SIZE;

namespace rooms {
// rooms.py: a cell's place on its level's grid (column from 0, row from 1) and its corner in the world
std::pair<int, int> cell_pos(const std::string &room);
std::pair<double, double> cell_origin(const std::string &room);
std::string cell_name(const std::string &level, int col, int row);
std::string level_of(const std::string &room);
}  // namespace rooms

std::vector<BuildStep> steps_rooms();  // dungeon_gen.write_all (+ the pots / bushes tables after it)

}  // namespace rando
