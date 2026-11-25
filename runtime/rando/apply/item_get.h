// Item-get events: ported from LAS-Randomizer's RandomizerCore/Randomizers/item_get.py (MIT, Owen-Splat and
// contributors), with its LAS Recomp additions (boss souls, Triforce pieces, the Ghost as a pet).
//
// The module globals LAS Recomp sets before the generator runs (tools/rando/las_headless.py: item_get.GHOST_PET,
// item_get.TRIFORCE_NEEDED, chests.SOUL_KEYS, chests.TRIFORCE) come from the seed's decisions ("las-globals",
// recorded by tools/rando/record_las_items.py), so a step needs nothing else.
#pragma once
#include "las_lib.h"

namespace rando {
namespace las {

struct ItemGetGlobals {
    bool ghost_pet = false;        // item_get.GHOST_PET
    int triforce_needed = 0;       // item_get.TRIFORCE_NEEDED
    bool triforce = false;         // chests.TRIFORCE
    std::vector<std::string> soul_keys;  // chests.SOUL_KEYS
};
ItemGetGlobals item_get_globals(const Decisions &d);

// insertItemGetAnimation: the item's get events after `before`, going on to `after`; the first event's name.
// index: the item's index (an int, or an argument such as arg("itemIndex"))
std::string insert_item_get(Flow &f, const std::string &item, const V &index, const std::string &before = "",
                            const std::string &after = "", bool play_extra_anim = true, bool can_hurt_player = true);
// insertTriforceGet (LAS Recomp Triforce hunt): give the piece, then show the message for the count left
std::string insert_triforce_get(Flow &f, const V &index, const std::string &before = "", const std::string &after = "");
// insertItemWithoutAnimation: the action defs that give the item without holding it up
ActionDefs item_without_animation(const Decisions &d, const std::string &item, const V &index);
// insertDampeItemGet: Dampe's reward chain (no get animation), going on to `after`; the first event's name
std::string insert_dampe_item_get(Flow &f, const std::string &item, const V &index, const std::string &after = "");

}  // namespace las
}  // namespace rando
