// Item-get events (item_get.h): ported from LAS-Randomizer's RandomizerCore/Randomizers/item_get.py (MIT, Owen-Splat
// and contributors). Every event is created in the Python's order (its AutoEvent names follow it).
#include "item_get.h"

#include <functional>
#include <map>

#include "../../../third_party/yyjson/yyjson.h"
#include "las_data.h"

namespace rando {
namespace las {

using namespace data;

ItemGetGlobals item_get_globals(const Decisions &d) {
    ItemGetGlobals g;
    yyjson_val *o = d.raw("las-globals"), *v;
    g.ghost_pet = yyjson_is_true(yyjson_obj_get(o, "ghost-pet"));
    g.triforce = yyjson_is_true(yyjson_obj_get(o, "triforce"));
    g.triforce_needed = (int)yyjson_get_sint(yyjson_obj_get(o, "triforce-needed"));
    yyjson_arr_iter it = yyjson_arr_iter_with(yyjson_obj_get(o, "soul-keys"));
    while ((v = yyjson_arr_iter_next(&it)))
        if (const char *s = yyjson_get_str(v)) g.soul_keys.push_back(s);
    return g;
}

// the action defs the Python spells out each time
static ActionDef add_item(const std::string &item, int count, const V &index) {
    return {"Inventory", "AddItemByKey", {{"itemKey", item}, {"count", count}, {"index", index}, {"autoEquip", false}}};
}
static ActionDef get_seq(const std::string &item, const std::string &message) {
    return {"Link", "GenericItemGetSequenceByKey", {{"itemKey", item}, {"keepCarry", false}, {"messageEntry", message}}};
}
static ActionDef set_flag(const std::string &symbol, bool value = true) {
    return {"EventFlags", "SetFlag", {{"symbol", symbol}, {"value", value}}};
}
static ActionDef tailor(const std::string &channel, double time) {
    return {"Link", "PlayTailorOtherChannelEx", {{"channel", channel}, {"index", 0}, {"restart", false}, {"time", time}}};
}
static ActionDef play_anim(const std::string &name) {
    return {"Link", "PlayAnimation", {{"blendTime", 0.1}, {"name", name}}};
}
static ActionDef wait(double time) { return {"Timer", "Wait", {{"time", time}}}; }
static ActionDef damage(int amount) { return {"Link", "Damage", {{"amount", amount}}}; }
static ActionDef hearts_on() { return {"Hud", "SetHeartUpdateEnable", {{"enable", true}}}; }

static std::string act(Flow &f, const ActionDef &a, const std::string &next = "") {
    return f.action(a.actor, a.action, a.params, next);
}

static bool is_one(const V &index) { return index.v.type == EvflValue::Int && index.v.i == 1; }

static const std::pair<const char *, const char *> kTradeFlags[] = {
    {"YoshiDoll", "TradeYoshiDollGet"}, {"Ribbon", "TradeRibbonGet"}, {"DogFood", "TradeDogFoodGet"},
    {"Bananas", "TradeBananasGet"}, {"Stick", "TradeStickGet"}, {"Honeycomb", "TradeHoneycombGet"},
    {"Pineapple", "TradePineappleGet"}, {"Hibiscus", "TradeHibiscusGet"}, {"Letter", "TradeLetterGet"},
    {"Broom", "TradeBroomGet"}, {"FishingHook", "TradeFishingHookGet"}, {"PinkBra", "TradeNecklaceGet"},
    {"MermaidsScale", "TradeMermaidsScaleGet"}};

static const char *trade_flag(const std::string &item) {
    for (const auto &t : kTradeFlags)
        if (item == t.first) return t.second;
    return nullptr;
}

static bool is_soul(const std::string &item) { return !item.compare(0, 8, "BossSoul") || !item.compare(0, 12, "MinibossSoul"); }

static ActionDefs harp_flags(bool ghost_pet) {  // (set before the harp: else the Ghost's quest is done mid get)
    ActionDefs d = {set_flag("GhostClear1"), set_flag("Ghost2_Clear"), set_flag("Ghost3_Clear")};
    if (!ghost_pet) d.push_back(set_flag("Ghost4_Clear"));
    return d;
}

static ActionDefs cat(ActionDefs a, const ActionDefs &b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

std::string insert_item_get(Flow &f, const std::string &item, const V &index, const std::string &before,
                            const std::string &after, bool play_extra_anim, bool can_hurt_player) {
    // progressive items
    if (item == "PowerBraceletLv1") return f.progressive_switch("PowerBraceletLv1", "PowerBraceletLv2", BRACELET_FOUND_FLAG, before, after);
    if (item == "SwordLv1") {
        if (!play_extra_anim) return f.progressive_switch("SwordLv1", "SwordLv2", SWORD_FOUND_FLAG, before, after);
        std::string chain = f.action_chain("", {{"Link", "RequestSwordRolling", {}},
                                                {"Link", "PlayAnimationEx", {{"blendTime", 0.1}, {"name", "slash_hold_lp"}, {"time", 0.8}}}});
        std::string spin = f.fork("", {chain}, after).first;
        return f.progressive_switch("SwordLv1", "SwordLv2", SWORD_FOUND_FLAG, before, spin);
    }
    if (item == "Shield") return f.progressive_switch("Shield", "MirrorShield", SHIELD_FOUND_FLAG, before, after);

    // capacity upgrades
    if (item == "MagicPowder_MaxUp") {
        std::string give = act(f, add_item("MagicPowder", 40, -1), after);
        std::string check = f.sw("EventFlags", "CheckFlag", {{"symbol", "GetMagicPowder"}}, {{0, after}, {1, give}});
        return f.action_chain(before, {add_item(item, 1, index), get_seq(item, item)}, check);
    }
    if (item == "Bomb_MaxUp") {
        std::string give = act(f, add_item("Bomb", 60, -1), after);
        std::string check = f.sw("EventFlags", "CheckFlag", {{"symbol", BOMBS_FOUND_FLAG}}, {{0, after}, {1, give}});
        return f.action_chain(before, {add_item(item, 1, index), get_seq(item, item)}, check);
    }
    if (item == "Arrow_MaxUp")
        return f.action_chain(before, {add_item(item, 1, index), add_item("Arrow", 60, -1), get_seq(item, item)}, after);

    // traps
    if (item == "ZapTrap") {
        std::string save = act(f, {"GameControl", "RequestAutoSave", {}}, after);
        std::string stop = act(f, {"Link", "StopTailorOtherChannel", {{"channel", "toolshopkeeper_dmg"}, {"index", 0}}}, save);
        std::vector<std::string> forks;
        forks.push_back(act(f, play_anim("ev_dmg_elec_lp")));
        forks.push_back(act(f, tailor("toolshopkeeper_dmg", 1.0)));
        forks.push_back(act(f, hearts_on()));
        if (can_hurt_player) forks.push_back(act(f, damage(6)));
        return f.fork(before, forks, stop).first;
    }
    if (item == "DrownTrap") {
        std::string save = act(f, {"GameControl", "RequestAutoSave", {}}, after);
        std::vector<std::string> forks;
        forks.push_back(act(f, play_anim("fall_water")));
        forks.push_back(act(f, hearts_on()));
        if (can_hurt_player) forks.push_back(f.action_chain("", {wait(1.5), damage(2)}));
        else forks.push_back(act(f, wait(1.5)));
        return f.fork(before, forks, save).first;
    }
    if (item == "SquishTrap") {
        std::string save = act(f, {"GameControl", "RequestAutoSave", {}}, after);
        std::vector<std::string> forks;
        forks.push_back(act(f, play_anim("dmg_press")));
        forks.push_back(act(f, hearts_on()));
        forks.push_back(act(f, wait(2.0)));
        if (can_hurt_player) forks.push_back(act(f, damage(4)));
        return f.fork(before, forks, save).first;
    }
    if (item == "DeathballTrap") {
        std::string save = act(f, {"GameControl", "RequestAutoSave", {}}, after);
        std::vector<std::string> forks;
        forks.push_back(act(f, tailor("GreatFairy_Heal", 0.0)));
        forks.push_back(f.action_chain("", {wait(0.1), play_anim("fall_deathball")}));
        if (can_hurt_player) forks.push_back(f.action_chain("", {hearts_on(), wait(1.5), damage(2)}));
        else forks.push_back(act(f, wait(1.5)));
        return f.fork(before, forks, save).first;
    }
    if (item == "QuakeTrap") {  // (hurts whatever can_hurt_player says, as the Python)
        std::string save = act(f, {"GameControl", "RequestAutoSave", {}}, after);
        std::vector<std::string> forks;
        forks.push_back(act(f, play_anim("dmg_quake")));
        forks.push_back(act(f, wait(1.5)));
        forks.push_back(act(f, hearts_on()));
        forks.push_back(act(f, damage(2)));
        return f.fork(before, forks, save).first;
    }

    // instruments
    if (item == "FullMoonCello")
        return f.action_chain(before, {set_flag("BowWowEvent"), set_flag("DoorOpen_Btl_MoriblinCave_2A", false),
                                       set_flag("DoorOpen_Btl_MoriblinCave_1A", false), add_item(item, 1, index),
                                       get_seq(item, "")}, after);
    if (item == "SurfHarp")
        return f.action_chain(before, cat(harp_flags(item_get_globals(f.b.decisions()).ghost_pet),
                                          {add_item(item, 1, index), get_seq(item, "")}), after);

    // tunics
    if (item == "ClothesRed")
        return f.action_chain(before, {set_flag(RED_TUNIC_FOUND_FLAG), tailor("Change_Color_Red_00", 3.58),
                                       add_item(item, 1, index), get_seq(item, "ClothesRed")}, after);
    if (item == "ClothesBlue")
        return f.action_chain(before, {set_flag(BLUE_TUNIC_FOUND_FLAG), tailor("Change_Color_Blue_00", 3.58),
                                       add_item(item, 1, index), get_seq(item, "ClothesBlue")}, after);
    if (item == "ClothesGreen")
        return f.action_chain(before, {tailor("Change_Color_Green_00", 3.58), add_item(item, 1, index),
                                       get_seq(item, "ClothesGreen")}, after);

    if (item == "SecretMedicine")
        return f.action_chain(before, {add_item(item, 1, index), get_seq(item, ""), {"Link", "Heal", {{"amount", 99}}}}, after);
    if (item == "Bomb")
        return f.action_chain(before, {set_flag(BOMBS_FOUND_FLAG), add_item(item, 60, index), get_seq(item, "")}, after);
    if (item == "MagicPowder")
        return f.action_chain(before, {set_flag("GetMagicPowder"), add_item(item, 40, index), get_seq(item, "")}, after);
    if (item == "Bottle" && is_one(index))  // (the fishing bottle: shown even with the second bottle owned)
        return f.action_chain(before, {set_flag("Bottle2Get"), add_item(item, 1, index), get_seq(item, "")}, after);
    if (const char *flag = trade_flag(item)) return f.action_chain(before, {set_flag(flag), get_seq(item, "")}, after);
    if (item == "MagnifyingLens")
        return f.action_chain(before, {set_flag(LENS_FOUND_FLAG), add_item(item, 1, index), get_seq(item, "")}, after);
    if (is_soul(item)) return f.action_chain(before, {set_flag(item), get_seq(item, item)}, after);
    if (item == "TriforcePiece") return insert_triforce_get(f, index, before, after);

    // everything else: held up first, then given (index-counting messages count it before)
    return f.action_chain(before, {get_seq(item, item), add_item(item, 1, index)}, after);
}

std::string insert_triforce_get(Flow &f, const V &index, const std::string &before, const std::string &after) {
    auto show = [&](const std::string &entry, const std::string &next) {
        return f.action("Link", "GenericItemGetSequenceByKey", {{"itemKey", "TriforcePiece"}, {"keepCarry", false}, {"messageEntry", entry}}, next);
    };
    std::string ending = f.action_chain("", {{"Fade", "StartParam", {{"mode", 1}, {"colorR", 1.0}, {"colorG", 1.0}, {"colorB", 1.0}, {"time", 3.0}}},
                                             wait(3.0), {"GameControl", "SaveForGameClear", {}}, {"GameControl", "GoToEnding", {}}});
    std::string complete = show("TriforceComplete", ending);
    std::string generic = show("TriforcePiece", after);
    std::map<int, std::string> leaves;
    int needed = item_get_globals(f.b.decisions()).triforce_needed;
    std::function<std::string(int, int)> node = [&](int bit, int val) -> std::string {
        if (val + (1 << (bit + 1)) - 1 < 1 || val > needed - 1) return generic;
        if (bit < 0) {
            if (!leaves.count(val)) leaves[val] = show("TriforcePiece" + std::to_string(val), after);
            return leaves[val];
        }
        std::string off = node(bit - 1, val);
        std::string on = node(bit - 1, val | (1 << bit));
        if (off == on) return off;
        return f.sw("EventFlags", "CheckFlag", {{"symbol", "TriforceRemain" + std::to_string(bit)}}, {{0, off}, {1, on}});
    };
    std::string tree = node(5, 0);
    std::string head = f.sw("EventFlags", "CheckFlag", {{"symbol", "TriforceDone"}}, {{0, tree}, {1, complete}});
    std::string add = f.action("Inventory", "AddItemByKey", {{"itemKey", "TriforcePiece"}, {"count", 1}, {"index", index}, {"autoEquip", false}}, head);
    f.insert_after(before, add);
    return add;
}

ActionDefs item_without_animation(const Decisions &d, const std::string &item, const V &index) {
    if (is_soul(item)) return {set_flag(item)};
    if (item == "PowerBraceletLv1") return {set_flag(BRACELET_FOUND_FLAG), add_item(item, 1, index)};
    if (item == "SwordLv1") return {set_flag(SWORD_FOUND_FLAG), add_item(item, 1, index)};
    if (item == "Shield") return {set_flag(SHIELD_FOUND_FLAG), add_item(item, 1, index)};
    if (item == "MagicPowder_MaxUp" || item == "Bomb_MaxUp" || item == "Arrow_MaxUp") return {add_item(item, 1, index)};
    if (item == "FullMoonCello")
        return {set_flag("BowWowEvent"), set_flag("DoorOpen_Btl_MoriblinCave_2A", false),
                set_flag("DoorOpen_Btl_MoriblinCave_1A", false), add_item(item, 1, index)};
    if (item == "SurfHarp") return cat(harp_flags(item_get_globals(d).ghost_pet), {add_item(item, 1, index)});
    if (item == "ClothesRed") return {set_flag(RED_TUNIC_FOUND_FLAG)};
    if (item == "ClothesBlue") return {set_flag(BLUE_TUNIC_FOUND_FLAG)};
    if (item == "SecretMedicine") return {add_item(item, 1, index), {"Link", "Heal", {{"amount", 99}}}};
    if (item == "Bomb") return {set_flag(BOMBS_FOUND_FLAG), add_item(item, 60, index)};
    if (item == "MagicPowder") return {set_flag("GetMagicPowder"), add_item(item, 40, index)};
    if (item == "Bottle" && is_one(index)) return {set_flag("Bottle2Get"), add_item(item, 1, index)};
    if (item == "Flippers") return {set_flag("FlippersFound"), add_item(item, 1, index)};
    if (const char *flag = trade_flag(item)) return {set_flag(flag)};
    if (item == "MagnifyingLens") return {set_flag(LENS_FOUND_FLAG), {"Inventory", "SetWarashibeItem", {{"itemType", 15}}}};
    return {add_item(item, 1, index)};
}

std::string insert_dampe_item_get(Flow &f, const std::string &item, const V &index, const std::string &after) {
    // progressive items
    struct Prog { const char *item, *second, *flag; };
    const Prog progs[] = {{"PowerBraceletLv1", "PowerBraceletLv2", BRACELET_FOUND_FLAG},
                          {"SwordLv1", "SwordLv2", SWORD_FOUND_FLAG}, {"Shield", "MirrorShield", SHIELD_FOUND_FLAG}};
    for (const Prog &p : progs) {
        if (item != p.item) continue;
        std::string give2 = act(f, add_item(p.second, 1, -1), after);
        std::string give1 = f.action_chain("", {set_flag(p.flag), add_item(item, 1, -1)}, after);
        return f.sw("EventFlags", "CheckFlag", {{"symbol", p.flag}}, {{0, give1}, {1, give2}});
    }

    // capacity upgrades
    if (item == "MagicPowder_MaxUp") {
        std::string give = act(f, add_item("MagicPowder", 40, -1), after);
        std::string check = f.sw("EventFlags", "CheckFlag", {{"symbol", "GetMagicPowder"}}, {{0, after}, {1, give}});
        return act(f, add_item(item, 1, index), check);
    }
    if (item == "Bomb_MaxUp") {
        std::string give = act(f, add_item("Bomb", 60, -1), after);
        std::string check = f.sw("EventFlags", "CheckFlag", {{"symbol", BOMBS_FOUND_FLAG}}, {{0, after}, {1, give}});
        return act(f, add_item(item, 1, index), check);
    }
    if (item == "Arrow_MaxUp") return f.action_chain("", {add_item(item, 1, index), add_item("Arrow", 60, -1)}, after);

    // traps (always hurt here)
    if (item == "ZapTrap") {
        std::string save = act(f, {"GameControl", "RequestAutoSave", {}}, after);
        std::string stop = act(f, {"Link", "StopTailorOtherChannel", {{"channel", "toolshopkeeper_dmg"}, {"index", 0}}}, save);
        std::vector<std::string> forks;
        forks.push_back(act(f, play_anim("ev_dmg_elec_lp")));
        forks.push_back(act(f, tailor("toolshopkeeper_dmg", 1.0)));
        forks.push_back(act(f, hearts_on()));
        forks.push_back(act(f, damage(6)));
        return f.fork("", forks, stop).first;
    }
    if (item == "DrownTrap") {
        std::string save = act(f, {"GameControl", "RequestAutoSave", {}}, after);
        std::vector<std::string> forks;
        forks.push_back(act(f, play_anim("fall_water")));
        forks.push_back(act(f, hearts_on()));
        forks.push_back(f.action_chain("", {wait(1.5), damage(2)}));
        return f.fork("", forks, save).first;
    }
    if (item == "SquishTrap") {
        std::string save = act(f, {"GameControl", "RequestAutoSave", {}}, after);
        std::vector<std::string> forks;
        forks.push_back(act(f, play_anim("dmg_press")));
        forks.push_back(act(f, hearts_on()));
        forks.push_back(act(f, wait(2.0)));
        forks.push_back(act(f, damage(4)));
        return f.fork("", forks, save).first;
    }
    if (item == "DeathballTrap") {
        std::string save = act(f, {"GameControl", "RequestAutoSave", {}}, after);
        std::vector<std::string> forks;
        forks.push_back(act(f, tailor("GreatFairy_Heal", 0.0)));
        forks.push_back(f.action_chain("", {wait(0.1), play_anim("fall_deathball")}));
        forks.push_back(f.action_chain("", {hearts_on(), wait(1.5), damage(2)}));
        return f.fork("", forks, save).first;
    }
    if (item == "QuakeTrap") {  // (6 damage here, 2 in insertItemGetAnimation: as the Python)
        std::string save = act(f, {"GameControl", "RequestAutoSave", {}}, after);
        std::vector<std::string> forks;
        forks.push_back(act(f, play_anim("dmg_quake")));
        forks.push_back(act(f, wait(1.5)));
        forks.push_back(act(f, hearts_on()));
        forks.push_back(act(f, damage(6)));
        return f.fork("", forks, save).first;
    }

    // instruments
    if (item == "FullMoonCello")
        return f.action_chain("", {set_flag("BowWowEvent"), set_flag("DoorOpen_Btl_MoriblinCave_2A", false),
                                   set_flag("DoorOpen_Btl_MoriblinCave_1A", false), add_item(item, 1, index)}, after);
    if (item == "SurfHarp")
        return f.action_chain("", cat(harp_flags(item_get_globals(f.b.decisions()).ghost_pet), {add_item(item, 1, index)}), after);

    // tunics
    if (item == "ClothesRed")
        return f.action_chain("", {set_flag(RED_TUNIC_FOUND_FLAG), tailor("Change_Color_Red_00", 3.58), add_item(item, 1, index)}, after);
    if (item == "ClothesBlue")
        return f.action_chain("", {set_flag(BLUE_TUNIC_FOUND_FLAG), tailor("Change_Color_Blue_00", 3.58), add_item(item, 1, index)}, after);
    if (item == "ClothesGreen") return f.action_chain("", {tailor("Change_Color_Green_00", 3.58), add_item(item, 1, index)}, after);

    if (item == "SecretMedicine") return f.action_chain("", {add_item(item, 1, index), {"Link", "Heal", {{"amount", 99}}}}, after);
    if (item == "Bomb") return f.action_chain("", {set_flag(BOMBS_FOUND_FLAG), add_item(item, 60, index)}, after);
    if (item == "MagicPowder") return f.action_chain("", {set_flag("GetMagicPowder"), add_item(item, 40, index)}, after);
    if (item == "Bottle" && is_one(index)) return f.action_chain("", {set_flag("Bottle2Get"), add_item(item, 1, index)}, after);
    if (item == "Flippers") return f.action_chain("", {set_flag("FlippersFound"), add_item(item, 1, index)}, after);
    if (const char *flag = trade_flag(item)) return f.action_chain("", {set_flag(flag)}, after);
    if (item == "MagnifyingLens") return f.action_chain("", {set_flag(LENS_FOUND_FLAG), add_item(item, 1, index)}, after);
    return f.action_chain("", {add_item(item, 1, index)}, after);
}

}  // namespace las
}  // namespace rando
