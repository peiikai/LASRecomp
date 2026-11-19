// LAS-Randomizer's constant tables (RandomizerCore/Randomizers/data.py, MIT, Owen-Splat and contributors), for the
// ported generator steps (seed_build.h). las_data.cpp is generated from the Python by tools/rando/export_las_data.py
// (--check tells whether it is current), so the two can't drift.
//
// Each Python dict is a table of (key, value) in the dict's order: the steps walk them in that order, as the Python.
#pragma once
#include <string>
#include <utility>
#include <vector>

namespace rando {
namespace las {
namespace data {

using StrTable = std::vector<std::pair<const char *, const char *>>;
using NumTable = std::vector<std::pair<const char *, double>>;

// the value at a key (nullptr / false: no such key)
const char *lookup(const StrTable &t, const std::string &key);
bool lookup(const NumTable &t, const std::string &key, double &out);

// flags (data.py names)
extern const char *const SWORD_FOUND_FLAG, *const SHIELD_FOUND_FLAG, *const BRACELET_FOUND_FLAG, *const LENS_FOUND_FLAG;
extern const char *const RED_TUNIC_FOUND_FLAG, *const BLUE_TUNIC_FOUND_FLAG;
extern const char *const GORIYA_FLAG, *const MAMU_FLAG, *const MANBO_FLAG;
extern const char *const BEACH_LOOSE_FLAG, *const WOODS_LOOSE_FLAG, *const POTHOLE_FLAG, *const DREAM_SHRINE_FLAG;
extern const char *const ROOSTER_CAVE_FLAG, *const MERMAID_CAVE_FLAG;
extern const char *const BOMBS_FOUND_FLAG, *const ROOSTER_FOUND_FLAG, *const BOWWOW_FOUND_FLAG;
extern const char *const INFINITE_BOMBS, *const INFINITE_ARROWS, *const INFINITE_POWDER, *const HIGH_JUMP;

// check -> flag; check -> room (file name without .leb); item key -> model file; look -> chest model file
extern const StrTable INSTRUMENT_FLAGS, HEART_FLAGS;
extern const StrTable CHEST_ROOMS, PANEL_CHEST_ROOMS, SMALL_KEY_ROOMS, GOLDEN_LEAF_ROOMS, INSTRUMENT_ROOMS, HEART_ROOMS;
extern const StrTable ITEM_MODELS, CHEST_TEXTURES;
// item type -> chest size; model name -> actor scale; model name -> y rotation
extern const NumTable CHEST_SIZES, MODEL_SIZES, MODEL_ROTATIONS;

struct DungeonEntrance { const char *name, *room, *suffix, *field_room, *field_suffix; int x4; };  // (the tuple's fields)
extern const std::vector<DungeonEntrance> DUNGEON_ENTRANCES;
struct DungeonMapIcon { const char *name, *map, *icon; };
extern const std::vector<DungeonMapIcon> DUNGEON_MAP_ICONS;
extern const std::vector<std::pair<const char *, std::vector<int>>> WATER_LOADING_ZONES;
extern const std::vector<const char *> BGM_TRACKS;

// player_start.py
extern const std::vector<const char *> START_FLAGS, BOSS_FLAGS, MESSAGE_FLAGS;

// the flags flags.makeFlags appends to GlobalFlags.gsheet (mod_generator's global_flags: name -> index), in order
extern const std::vector<std::pair<const char *, int>> GLOBAL_FLAGS;
int global_flag(const std::string &name);  // its index (throws when there is none)

}  // namespace data
}  // namespace las
}  // namespace rando
