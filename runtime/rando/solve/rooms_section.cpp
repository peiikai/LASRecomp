// las_headless.py main()'s dungeon room shuffle section (rooms.h: plan_rooms).
#include <algorithm>

#include "rooms.h"

namespace rando {
namespace solve {

Value deepcopy_settings(const Value &settings) {
    Value s = settings.deepcopy();
    if (s.has("excluded-locations") && s.at("excluded-locations").is_list()) {
        PyStrSet set;
        for (const Value &x : s.at("excluded-locations").list()) set.add(x.s());
        List l;
        for (const std::string &x : set.items()) l.push_back(x);
        s.set("excluded-locations", Value(std::move(l)));
    }
    return s;
}

int plan_rooms(Prepared &st, const GameFiles &game, int first, RoomPlan &plan, const std::function<void(const std::string &)> &say,
               const std::function<bool()> &cancelled) {
    Value room_mode = st.settings.get("dungeon-room-shuffle", "off");
    rooms::Ctx ctx;
    ctx.hook = st.settings.get("hookshot-anything").truthy();
    ctx.exit_req = std::find(st.soul_kinds.begin(), st.soul_kinds.end(), "mini") != st.soul_kinds.end();
    rooms::GameScope scope(game, ctx.hook);  // (what the game's rooms say: read once, for this generation)
    Value extra = Value::new_dict();
    for (const auto &e : st.pot_locs.dict())
        if (e.value.get("room").truthy()) extra.set(e.key, e.value.at("room"));
    Value base_logic = st.logic_defs.deepcopy(), base_items = st.item_defs.deepcopy(),
          base_settings = deepcopy_settings(st.settings);
    Value plan_settings = st.settings;
    for (int attempt = first; attempt < 40; attempt++) {
        if (cancelled && cancelled()) throw PyError("RuntimeError", "stopped");
        Value sv = plan_settings.at("seed");
        std::string seed = sv.is_str() ? sv.s() : sv.repr();
        std::string sub = attempt == 0 ? seed : seed + "~" + std::to_string(attempt);
        RoomPlan p;
        std::vector<std::string> said;
        try {
            p = plan_all_chaos(sub, room_mode == Value("decoupled"), plan_settings, ctx, said);
        } catch (const PyError &e) {
            if (e.type != "RuntimeError") throw;
            say(std::string("room layout: ") + (e.what() + e.type.size() + 2) + " - planning another");
            continue;
        }
        for (const std::string &l : said) say(l);
        // (the copy's set: made from base's items in base's order - once; the list below is its order)
        Value logic = base_logic.deepcopy(), items = base_items.deepcopy(), sets = base_settings.deepcopy();
        PyStrSet excluded, absent;
        for (const Value &x : base_settings.at("excluded-locations").list()) excluded.add(x.s());
        std::string summary = room_logic_apply(p, logic, items, sets, excluded, absent, extra, ctx);
        List ex, ab;
        for (const std::string &x : excluded.items()) ex.push_back(x);
        for (const std::string &x : absent.items()) ab.push_back(x);
        sets.set("excluded-locations", Value(std::move(ex)));
        sets.set("absent-locations", Value(std::move(ab)));
        say("room logic: " + summary);
        Value why = sets.get("room-logic-unfillable");
        sets.dict().erase("room-logic-unfillable");
        st.logic_defs = logic, st.item_defs = items, st.settings = sets, plan_settings = sets;
        if (!why.truthy()) {
            plan = p;
            return attempt;
        }
        say("room layout can't be filled - " + why.s() + " - planning another");
    }
    throw PyError("RuntimeError", "no dungeon room layout for this seed");
}

}  // namespace solve
}  // namespace rando
