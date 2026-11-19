// Dungeon room shuffle (Coupled / Decoupled), natively: tools/rando/dungeon_gen.py's planning (plan_all_chaos: every
// dungeon's new layout of real rooms and the door jumps that join them) and tools/rando/room_logic.py (the item logic
// that follows the new layouts), as tools/rando/las_headless.py runs them before the item fill.
//
// Same layouts, jump rows and logic as the Python, draw for draw (lasrando rooms-check compares them with a recorded
// run). What the Python reads from the game's rooms (each room's areas, doors, stairs, landing spots, textures, map
// pages) comes from tools/rando/export_room_data.py (room_game_data.cpp), worked out by the Python itself.
//
//   RoomPlan p = plan_all_chaos(sub_seed, madness, settings, ctx);  // PyError RuntimeError 'no Chaos layout'
//   room_logic_apply(p, logic_defs, item_defs, settings, extra, ctx); // the logic rewritten in place
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "prepare.h"
#include "rooms_lib.h"
#include "value.h"

namespace rando {
namespace solve {

struct RoomPlan {
    Value layouts;  // {level: {'Lv01TailCave_08D': room}} in the Python's order
    Value jumps;    // door_jumps rows: [level, cell, side, own, partner, cell, side, depth, height(, lateral)]
};

// dungeon_gen.plan_all_chaos(cat, seed, romfs, madness=madness, settings=settings); `said`: its printed lines
RoomPlan plan_all_chaos(const std::string &seed, bool madness, const Value &settings, const rooms::Ctx &ctx,
                        std::vector<std::string> &said);

// room_logic.apply(cat, romfs, layouts, jumps, logic_defs, item_defs, settings, extra): the summary it returns.
// settings' 'excluded-locations' / 'absent-locations' are PyStrSets here (rooms_apply's callers turn them into lists)
std::string room_logic_apply(const RoomPlan &plan, Value &logic_defs, Value &item_defs, Value &settings,
                             PyStrSet &excluded, PyStrSet &absent, const Value &extra, const rooms::Ctx &ctx);

// las_headless.py main()'s room section, from the 'post_entrances' state: plan_all_chaos for sub-seeds '<seed>' then
// '<seed>~<attempt>' from `first` on (LARECOMP_ROOM_ATTEMPT), room_logic.apply on fresh deep copies of st's settings,
// items and logic, until a layout's dungeons can be filled. st's settings / items / logic become that layout's; returns
// its attempt. PyError RuntimeError 'no dungeon room layout for this seed' after the 40th.
int plan_rooms(Prepared &st, int first, RoomPlan &plan, const std::function<void(const std::string &)> &say,
               const std::function<bool()> &cancelled = nullptr);
// copy.deepcopy(settings): the Python set (excluded-locations) made again from its items, in its order
Value deepcopy_settings(const Value &settings);

namespace rooms {
// the generator's checks on a dungeon's jump rows (dungeon_gen.plan_all_chaos): room_logic.boss_door_ok / fill_ok.
// layout: {'Lv01TailCave_08D': room}
bool boss_door_ok(const std::string &level, const Value &layout, const Value &rows, const Ctx &ctx);
bool fill_ok(const std::string &level, const Value &layout, const Value &rows, const Value &settings, const Ctx &ctx);
}  // namespace rooms

}  // namespace solve
}  // namespace rando
