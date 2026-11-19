// The tables of the seed's text outputs (outputs.h): the Python's own constants, as C++. outputs_data.cpp is generated
// by tools/rando/export_outputs_data.py (--check tells whether it is current).
#pragma once
#include <stdint.h>

#include <string>
#include <utility>
#include <vector>

namespace rando {
namespace solve {
namespace odata {

using StrMap = std::vector<std::pair<const char *, const char *>>;  // (a dict, in its order)
using StrList = std::vector<const char *>;
using ListMap = std::vector<std::pair<const char *, StrList>>;

struct Door {  // entrance_data.yml
    const char *id, *field_room, *dest_level;
    StrList inside;  // (the locations inside)
};
struct LocRoom {  // location_rooms.json
    const char *name, *room, *level;  // (nullptr: None)
};

// playthrough.py
extern const StrList INSTRUMENTS;
// hint_sheet.py
extern const char *const HS_JUNK, *const TRIFORCE;
extern const StrList GATED, HS_PLACE, BOSSES;
extern const StrMap LEVEL_DUNGEON, DOOR_NAME;
// seed_hints.py (+ LAS randomizer_data TRADE_GIFT_LOCATIONS)
extern const StrList FIELD_OWLS, GOOD, SH_NOT_PROGRESS, TRADE_GIFT_LOCATIONS;
extern const StrMap DUNGEON_OWLS;
extern const ListMap QUESTS;
extern const int MIN_CHECKS, PHONES;
// field_regions.py
extern const std::vector<std::pair<int, const char *>> ZONE_AREA;
extern const StrMap REGION_AREA;
// spoiler_view.py
extern const StrMap SV_DUNGEONS, SV_DPREFIX, SV_DKEY, SV_LEVEL_D, SV_AREAS, SV_ITEMS;
extern const StrList SV_PLACE, SV_REGION_ORDER, SV_DUNGEON_ORDER, SV_PROGRESSIVE, SV_INTERNAL;
extern const std::vector<int> SV_MAP_ORDER;
extern const char *const SV_JUNK, *const SV_CSS, *const SV_JS;
// tracker.py (+ LAS data.py flags, underwater.py)
extern const StrMap TR_DUNGEONS, TR_EVENT_FLAGS, TR_HEART_FLAGS, TR_SHELL_FLAGS, TR_ENTRANCE_KEYS, TR_SEEN_AT;
extern const ListMap TR_LEVELS;
extern const StrList TR_TRICK_LEVELS, TR_PLACE, TR_NOT_PROGRESS;
// pots.py, bushes.py, rocks.py, eagle_tower.py
extern const StrMap POT_PREFIX;  // (pots.DUNGEONS: level -> its LAS location prefix)
extern const std::vector<int> POT_TYPES;
extern const char *const POT_SKIP_LEVEL_PREFIX;
extern const StrList POT_SKIP_LEVELS, POT_MISSABLE_ROOMS, POT_MISSABLE_ROOMS_COLLAPSED;
extern const int BUSH_TYPE, ROCK_TYPE;
extern const uint64_t GOLDEN_LEAF_ROCK;
// check_names.py, entrance_names.py, dungeon_gen.py
extern const StrMap CN_NAMES, CN_KIND_WORD, CN_HOUSES, EN_NAMES, DG_ENTRANCES;
// entrance_data.yml, location_rooms.json
extern const std::vector<Door> DOORS;
extern const std::vector<LocRoom> LOC_ROOMS;

// lookups: d[key] (nullptr: missing); key in list
const char *get(const StrMap &m, const std::string &key);
bool in(const StrList &l, const std::string &key);
const StrList *get(const ListMap &m, const std::string &key);

}  // namespace odata
}  // namespace solve
}  // namespace rando
