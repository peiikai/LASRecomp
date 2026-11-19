// The text tables of tools/rando/hint_sheet.py and seed_hints.py, for the ported Hint Sheet and hint steps
// (ours_items_hints.cpp). ours_items_hints_data.cpp was generated from the Python; it is edited directly now.
#pragma once
#include <string>
#include <utility>
#include <vector>

#include "ours_items_text.h"

namespace rando {
namespace ours {
namespace hints {

using StrTable = text::StrTable;
using ListTable = std::vector<std::pair<const char *, std::vector<const char *>>>;

// hint_sheet.py
extern const char *const ENTRY, *const DONE, *const MODEL_NPC;
extern const int LINE;
extern const StrTable DUNGEON, DUNGEON_KEY, LEVEL_DUNGEON, AREA, ITEM, DUNGEON_ITEM, PLACE;
extern const ListTable AREA_WORDS;
extern const std::vector<const char *> PLURAL, BOSSES;
extern const std::vector<std::pair<const char *, bool>> GET_TEXT, DONE_TEXT;
extern const StrTable ACTOR_NAME;  // (the landmarks' Field actor name prefixes)
// seed_hints.py
extern const StrTable FIELD_OWLS, DUNGEON_OWLS, SPLIT, QUEST_EN, QUEST_LABEL;
extern const text::TextTable T;

}  // namespace hints
}  // namespace ours
}  // namespace rando
