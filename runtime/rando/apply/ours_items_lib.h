// LAS Recomp's item steps (ours_items.cpp registers them): what their files share.
#pragma once
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "las_lib.h"
#include "ours_items_text.h"

namespace rando {
namespace ours {

// shopsanity.alloc_flags: new GlobalFlags rows after the highest index in use within [base, limit); (name, index)
std::vector<std::pair<std::string, int>> alloc_flags(GSheet &sheet, const std::vector<std::string> &names,
                                                      int base = 1240, int limit = 2048);
// keysanity_text._retarget: everything leading to `from` (events but those in `skip`, entry points) leads to `to`
void retarget(EvflFlowchart &fc, uint16_t from, uint16_t to, const std::set<uint16_t> &skip);
uint16_t event_index(EvflFlowchart &fc, const std::string &name);  // (throws when there's none)

// the steps, by file
void shopsanity_apply(SeedBuild &b);  // ours_items_shop.cpp
void trendsanity_apply(SeedBuild &b);
void pets_apply(SeedBuild &b);
void escape_bottle_apply(SeedBuild &b);
void souls_apply(SeedBuild &b);  // ours_items_souls.cpp
void triforce_apply(SeedBuild &b);
void souls_write_spots(SeedBuild &b);
void triforce_write_spots(SeedBuild &b);
void hint_sheet_apply(SeedBuild &b);  // ours_items_hints.cpp
void seed_hints_apply(SeedBuild &b);
void keysanity_text_apply(SeedBuild &b);  // ours_items_texts.cpp
void refill_gets_apply(SeedBuild &b);
void bomb_bag_text_apply(SeedBuild &b);
void powder_bag_text_apply(SeedBuild &b);
void silent_pickups_apply(SeedBuild &b);
void quick_pickups_apply(SeedBuild &b);  // ours_items_tables.cpp
void seed_tables_write_flags(SeedBuild &b);

// decide mode (decide.h; ours_items_decide.cpp): the decisions hint_sheet.apply / seed_hints.apply record, made from
// hint_steps.json / seed_hints.json and the entrance shuffle as their Python makes them (JSON text; "" when the
// Python records nothing)
std::string decide_hint_sheet(SeedBuild &b);
std::string decide_seed_hints(SeedBuild &b);

}  // namespace ours
}  // namespace rando
