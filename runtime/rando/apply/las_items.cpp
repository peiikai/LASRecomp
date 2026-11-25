// LAS-Randomizer's item edits: data sheets, general events, chest contents, NPC / boss / shop event flows, the trade
// quest: the generator's steps ported (seed_build.h); each makes exactly the writes its Python does
// (lasrando check-steps, tools/rando/test_apply.py). Ported from LAS-Randomizer's RandomizerCore/mod_generator.py and
// Randomizers/{npcs, item_drops, conditions, crane_prizes, flags, player_start, chests, seashell_mansion, tarin,
// miscellaneous, marin, rapids, fishing, mad_batter, dampe, trade_quest}.py (MIT, Owen-Splat and contributors).
//
// Python quirks kept for the same files (each marked "as the Python"):
//   - npcs.makeNewNpcs: oead_tools.listToSheetArray keeps only dict items, so the new rows' behavior.parameters and
//     collision.parameters come out empty;
//   - makeChestContentFixes, panel rooms: each ObjTreasureBox actor's content is written into the room's first
//     chest (setChestContent's chest_index stays 0), so the last one wins;
//   - makeGeneralDatasheetChanges, Items.gsheet: ShopShovel / ShopBow / ShopHeart keep the npcKey of Dampe's last
//     reward, and the trap rows the gettingFlag ''.
#include <string.h>

#include <functional>
#include <stdexcept>

#include "../../../third_party/yyjson/yyjson.h"
#include "item_get.h"
#include "las_data.h"
#include "steps.h"

namespace rando {
namespace las {
namespace {

using namespace data;

bool S(SeedBuild &b, const char *key) { return setting_or(b.decisions(), key, false); }
std::string seashell_param(SeedBuild &b, const std::string &check, const char *prefix) {
    return prefix + std::string("Seashell:") + std::to_string(index_or(b.decisions(), check));
}
bool is_seashell(SeedBuild &b, const std::string &check) { return placed(b.decisions(), check) == "seashell"; }
GStruct layout(int32_t category, const std::string &parameter, int32_t id) {
    return {{"category", gi(category)}, {"parameter", gs(parameter)}, {"layoutID", gi(id)}};
}
void erase_at(GValue &arr, size_t i) {
    if (arr.kind != GValue::StructArr || i >= arr.structs.size()) throw std::runtime_error("las: no such sheet array item");
    arr.structs.erase(arr.structs.begin() + (long)i);
}
void erase_fork(EvflEvent &e, size_t i) {
    if (i >= e.forks.size()) throw std::runtime_error("las: no fork " + std::to_string(i) + " in " + e.name);
    e.forks.erase(e.forks.begin() + (long)i);
}
// switch `to` asks what `from` asks (actor and query), with these parameters (trade_quest.py)
void copy_query(Flow &f, const std::string &from, const std::string &to, const Params &p) {
    EvflEvent &src = f.event(from), &dst = f.event(to);
    dst.actor = src.actor, dst.action = src.action;
    dst.params = container(p);
}

// ======== makeGeneralDatasheetChanges

// npcs.makeNpcChanges
void npc_changes(SeedBuild &b, GStruct &npc) {
    const std::string sym = at(npc, "symbol").s;
    auto F = [&](const char *name) -> GValue & { return at(npc, name); };
    auto graphics = [&](const char *path, const char *model) {
        at(at(npc, "graphics").structs.at(0), "path") = gs(path);
        at(at(npc, "graphics").structs.at(0), "model") = gs(model);
    };
    auto trigger = [&](size_t i) -> GStruct & { return item(F("eventTriggers"), i); };
    auto shell = [&](int32_t c, const std::string &p) { F("shellSensor").structs.push_back(cond(c, p)); };
    auto shell_if = [&](const char *check) {
        if (is_seashell(b, check)) shell(2, seashell_param(b, check, "!"));
    };
    auto sub = [&](const char *field) -> GStruct & { return F(field).structs.at(0); };

    if (sym == "NpcMadBatter") {
        at(trigger(0), "entryPoint") = gs("$2");
        erase_at(F("layoutConditions"), 1);
        return;
    }
    if (sym == "ItemGoldenLeaf") {  // the leaves' actors never appear
        F("eventTriggers").structs.clear();
        for (int i = 0; i < 3; i++) erase_at(F("layoutConditions"), 0);
        return;
    }
    if (sym == "ItemSmallKey") {
        graphics("$1", "$2");
        at(trigger(2), "entryPoint") = gs("$3");
        shell(9, "$4");
        return;
    }
    if (sym == "ItemYoshiDoll") {  // the ocarina and the instruments: still taken with A
        graphics("$0", "$1");
        F("eventInfo") = gst({{"eventAsset", gs("SinkingSword.bfevfl")}, {"actorName", gs("SinkingSword")}});
        at(trigger(0), "entryPoint") = gs("$2");
        F("doAction") = gst({{"type", gi(7)}, {"yOffset", gf(0.0f)}, {"xzDistance", gf(1.2999999523162842f)},
                             {"yDistance", gf(1.7999999523162842f)}, {"playerAngleRange", gf(45.0f)},
                             {"reactionAngleRange", gf(180.0f)}});
        F("layoutConditions").structs.push_back(layout(1, "$3", -1));
        GStruct &col = sub("collision");
        at(col, "traits") = gs(""), at(col, "isStatic") = gb(true), at(col, "filter") = gi(5);
        shell(9, "$4");
        return;
    }
    if (sym == "ItemHoneycomb") {
        graphics("$0", "$1");
        shell_if("tarin-ukuku");
        return;
    }
    if (sym == "ItemStick") graphics("$1", "$2");  // (no return: as the Python)
    if (sym == "ObjClothBag") {
        item(F("layoutConditions"), 1) = layout(1, "TradePineappleGet", 0);
        return;
    }
    if (sym == "NpcGrandmaUlrira") {
        item(F("layoutConditions"), 1) = layout(9, "true", 4);
        return;
    }
    if (sym == "ObjSinkingSword") {
        graphics("$0", "$1");
        at(at(sub("graphics"), "waterChannel").structs.at(0), "limitDepth") = gf(0.5f);
        at(trigger(0), "condition") = gi(0);
        at(trigger(0), "entryPoint") = gs("$2");
        F("doAction") = gst({{"type", gi(0)}, {"yOffset", gf(0.0f)}, {"xzDistance", gf(0.0f)}, {"yDistance", gf(0.0f)},
                             {"playerAngleRange", gf(0.0f)}, {"reactionAngleRange", gf(0.0f)}});
        at(item(F("layoutConditions"), 0), "parameter") = gs("$3");
        GStruct &col = sub("collision");
        at(col, "traits") = gs("HeartPiece"), at(col, "isStatic") = gb(false), at(col, "filter") = gi(7);
        at(at(col, "offset").structs.at(0), "y") = gf(0.25f);
        shell(9, "$4");
        return;
    }
    if (sym == "ObjRoosterBones") {
        erase_at(F("layoutConditions"), 0);
        return;
    }
    // (LAS Recomp: not with Treat Tunics as Upgrades, where the phones keep Ulrira's call)
    if (sym == "ObjTelephone" && !setting_or(b.decisions(), "tunic-upgrades", false)) {
        F("talk") = gst({{"personalSpace", gf(1.5f)}, {"talkerLabel", gs("NpcFairyQueen")}});
        return;
    }
    if ((sym == "ItemBomb" || sym == "ItemFeatherBomb") && S(b, "shuffle-bombs")) {
        F("layoutConditions").structs.push_back(layout(1, std::string("!") + BOMBS_FOUND_FLAG, -1));
        return;
    }
    if ((sym == "ItemMagicPowder" || sym == "ItemFeatherMagicPowder") && S(b, "shuffle-powder")) {
        F("layoutConditions").structs.push_back(layout(1, "!GetMagicPowder", -1));
        return;
    }
    if (sym == "NpcBowWow") {
        item(F("layoutConditions"), 2) = layout(3, "BowWow", -1);
        return;
    }
    if (sym == "NpcMadamMeowMeow") {
        item(F("layoutConditions"), 2) = layout(1, "BowWowJoin", 3);
        erase_at(F("layoutConditions"), 1);
        return;
    }
    if (sym == "NpcChorusFrog") {
        erase_at(F("layoutConditions"), 0);
        return;
    }
    if (sym == "NpcKiki" && S(b, "open-bridge")) {
        item(F("layoutConditions"), 0) = layout(1, "KikiGone", -1);
        return;
    }
    if (sym == "NpcChristine") {
        erase_at(F("shellSensor"), 0);
        shell_if("christine-trade");
        shell_if("christine-grateful");
        return;
    }
    if (sym == "NpcTarin") {
        item(at(trigger(5), "additionalConditions"), 0) = cond(1, "!ShieldGet");
        item(at(trigger(1), "additionalConditions"), 0) = cond(4, "3");
        item(F("layoutConditions"), 2) = layout(1, "HoneycombDrop", -1);
        item(F("layoutConditions"), 4) = layout(1, "TradeStickGet", 3);
        return;
    }
    if (sym == "NpcPapahl") {
        item(F("layoutConditions"), 1) = layout(1, "TradePineappleGet", 2);
        shell_if("papahl");
        return;
    }
    if (sym == "NpcMarin") {
        if (is_seashell(b, "marin")) {
            shell(4, "2");  // (only Mabe's Marin rings the sensor)
            shell(2, seashell_param(b, "marin", "!"));
        }
        return;
    }
    if (sym == "NpcSecretZora") {
        erase_at(F("shellSensor"), 0);
        shell_if("invisible-zora");
        return;
    }
    if (sym == "NpcGoriya") { shell_if("goriya-trader"); return; }
    if (sym == "ObjGhostsGrave") { shell_if("ghost-reward"); return; }
    if (sym == "NpcWalrus") {
        erase_at(F("shellSensor"), 0);
        shell_if("walrus");
        return;
    }
    if (sym == "NpcFairyQueen") {
        shell_if("D0-fairy-1");
        shell_if("D0-fairy-2");
        return;
    }
    if (sym == "NpcManboTamegoro") { shell_if("manbo"); return; }
    if (sym == "NpcMamu") {
        erase_at(F("layoutConditions"), 0);
        shell_if("mamu");
        return;
    }
    if (sym == "NpcGameShopOwner") { shell_if("trendy-prize-final"); return; }
    if (sym == "NpcDanpei") { shell(9, "!DampeShellsComplete"); return; }
    if (sym == "NpcRaftShopMan") { shell(9, "!RapidsShellsComplete"); return; }
    if (sym == "NpcFisherman") { shell(9, "!FishingShellsComplete"); return; }
    if (sym == "NpcShellMansionMaster") { shell(9, "!MansionShellsComplete"); return; }
    if (sym == "ObjTreasureBox" &&
        (b.decisions().setting_str("chest-aspect") == "camc" || setting_or(b.decisions(), "chest-looks", false)))
        graphics("$6", "$7");
}

// npcs.DUMMY_NPC, as oead_tools.dictToStruct makes it (lists of plain values come out empty: as the Python)
GStruct dummy_npc() {
    auto anim = [](const char *name, int lipsync) {
        return GStruct{{"name", gs(name)}, {"blendTime", gi(4)}, {"lipsync", gi(lipsync)}, {"cull", gi(2)}};
    };
    auto xyz = [](float x, float y, float z) { return gst({{"x", gf(x)}, {"y", gf(y)}, {"z", gf(z)}}); };
    return {
        {"symbol", gs("ItemOcarina")},
        {"graphics", gst({{"path", gs("ItemOcarina.bfres")}, {"model", gs("Ocarina")},
                          {"animations", gst({{"idle", gs("")}, {"talk", gs("")}, {"walk", gs("")}, {"run", gs("")}})},
                          {"rootAnimEnabled", gb(false)},
                          {"animSettings", gsa({anim("wait", 0), anim("talk", 1), anim("walk", 0), anim("run", 0)})},
                          {"drsb", gs("ItemOcarina.drsb")},
                          {"waterChannel", gst({{"enable", gb(true)}, {"offsetY", gf(0.0f)}, {"limitDepth", gf(0.5f)}})},
                          {"facial", gs("")}, {"interestIK", gs("")},
                          {"turn", gst({{"enable", gb(false)}, {"autoTurn", gb(false)}, {"threshold", gf(30.0f)},
                                        {"reactionTime", gf(1.0f)}, {"duration", gf(0.25f)}})},
                          {"cull", gi(1)}})},
        {"eventInfo", gst({{"eventAsset", gs("ItemCommon.bfevfl")}, {"actorName", gs("Item")}})},
        {"eventTriggers", gsa({create_event_trigger(2, {}, "Ocarina")})},
        {"doAction", gst({{"type", gi(7)}, {"yOffset", gf(0.0f)}, {"xzDistance", gf(1.5f)},
                          {"yDistance", gf(1.7999999523162842f)}, {"playerAngleRange", gf(45.0f)},
                          {"reactionAngleRange", gf(180.0f)}})},
        {"talk", gst({{"personalSpace", gf(1.5f)}, {"talkerLabel", gs("")}})},
        {"actorArgs", gst({{"layoutID", gi(0)}, {"identifier", gi(0)}})},
        {"popConditions", gst({{"conditions", gsa({})}, {"depopEnable", gb(false)}})},
        {"layoutConditions", gsa({})},
        {"lookAtOffsetY", gf(0.0f)},
        {"behavior", gst({{"type", gi(2)}, {"parameters", gstrs({})}})},  // (['0'] * 5 in the Python: dropped)
        {"movement", gst({{"walk", gst({{"moveSpeed", gf(1.5f)}, {"turnSpeed", gf(45.0f)}})},
                          {"run", gst({{"moveSpeed", gf(2.5f)}, {"turnSpeed", gf(120.0f)}})}})},
        {"collision", gst({{"shape", gi(3)}, {"traits", gs("Ocarina")}, {"component", gi(1)}, {"isStatic", gb(false)},
                           {"filter", gi(5)}, {"material", gi(0)}, {"groundCheck", gb(true)},
                           {"offset", xyz(0.0f, 0.25f, 0.0f)}, {"rotation", xyz(0.0f, 0.0f, 0.0f)},
                           {"parameters", gfloats({})}})},  // ([1.8, 0.5, 1.8] in the Python: dropped)
        {"damageCollision", gst({{"enable", gb(false)}, {"sizeDiff", gf(0.0f)}})},
        {"ocarina", gst({{"enable", gb(false)}, {"distance", gf(0.0f)}})},
        {"shellSensor", gsa({})},
        {"attribute", gi(0)},
    };
}

// npcs.makeNewNpcs: rows showing the right model when Link holds these items up
void new_npcs(SeedBuild &b, GSheet &sheet) {
    const Decisions &d = b.decisions();
    GStruct dummy = dummy_npc();
    auto row = [&](const std::string &symbol, const std::string &path, const std::string &model) {
        at(dummy, "symbol") = gs(symbol);
        GStruct &g = at(dummy, "graphics").structs.at(0);
        at(g, "path") = gs(path), at(g, "model") = gs(model);
        sheet.values.push_back(dummy);
    };
    row("PatchSmallKey", "ItemSmallKey.bfres", "SmallKey");
    row("PatchYoshiDoll", "ItemYoshiDoll.bfres", "YoshiDoll");
    row("PatchHoneycomb", "ItemHoneycomb.bfres", "Honeycomb");
    row("PatchStick", "ItemStick.bfres", "Stick");
    for (const auto &s : {std::make_pair("SyrupPowder", "syrup"), std::make_pair("WalrusShell", "walrus"),
                          std::make_pair("FishNecklace", "bay-fisherman")}) {
        std::string item = placed(d, s.second);
        row(s.first, item_field(d, item, "model-path"), item_field(d, item, "model-name"));
    }
}

// item_drops.makeDatasheetChanges
void item_drop_changes(SeedBuild &b, GSheet &sheet) {
    long first_heart = -1;
    bool extended = S(b, "extended-consumable-drop");
    for (size_t i = 0; i < sheet.values.size(); i++) {
        GStruct &row = sheet.values[i];
        const std::string key = at(row, "mKey").s;
        GValue &lots = at(row, "mLotTable");
        if (key == "HeartContainer0") first_heart = (long)i;
        if (key == "AnglerKey" || key == "FaceKey" || key == "HookShot") at(item(lots, 0), "mType") = gs("");
        // (LAS Recomp no-buff-drops: the kill-streak buffs drop nothing)
        if (S(b, "no-buff-drops") && (key == "GuardianAcorn" || key == "PowerPiece")) at(item(lots, 0), "mType") = gs("");
        if (key == "Grass") {
            at(item(lots, 1), "mWeight") = gi(18);
            at(item(lots, 2), "mWeight") = gi(3);
            at(item(lots, 3), "mWeight") = gi(extended ? 56 : 71);
            if (extended) {
                GStruct dummy = item(lots, 0);
                for (const char *t : {"Bomb", "Arrow", "MagicPowder"}) {
                    at(dummy, "mType") = gs(t), at(dummy, "mWeight") = gi(5);
                    lots.structs.push_back(dummy);
                }
            }
        }
    }
    if (first_heart < 0) throw std::runtime_error("las: no HeartContainer0 in ItemDrop.gsheet");
    for (long i = 0; i < 8; i++) at(item(at(sheet.values.at(first_heart + i), "mLotTable"), 0), "mType") = gs("");
}

// mod_generator.makeGeneralDatasheetChanges: Items.gsheet
void items_changes(SeedBuild &b, GSheet &sheet) {
    const Decisions &d = b.decisions();
    GStruct dummy;
    bool have_dummy = false;
    for (GStruct &it : sheet.values) {
        const std::string sym = at(it, "symbol").s;
        if (sym == "Flippers") at(it, "gettingFlag") = gs("FlippersFound");  // (for the water loading zones)
        // (LAS Recomp: a heart held up with the item-get sequence brings the music back with this jingle)
        if (sym == "Heart") {
            GValue &j = at(it, "jingle");
            if (j.kind == GValue::Null || j.s.empty()) j = gs("BGM_FANFARE_ITEM_GET");
        }
        if (sym == "SmallKey") at(it, "npcKey") = gs("PatchSmallKey");
        if (sym == "Honeycomb") at(it, "npcKey") = gs("PatchHoneycomb");
        if (sym == "Stick") at(it, "npcKey") = gs("PatchStick");
        if (sym == "YoshiDoll") {
            at(it, "npcKey") = gs("PatchYoshiDoll");
            dummy = it, have_dummy = true;
        }
        if (sym == "Song_WindFish") at(it, "npcKey") = gs("NpcMarin");
        if (sym == "Song_Mambo") at(it, "npcKey") = gs("NpcManboTamegoro");
        if (sym == "Song_Soul") at(it, "npcKey") = gs("NpcMamu");
        if (sym == "ClothesGreen" || sym == "ClothesRed" || sym == "ClothesBlue") at(it, "npcKey") = gs("");
    }
    if (!have_dummy) throw std::runtime_error("las: ItemYoshiDoll was not found in Items.gsheet");
    auto add = [&](const char *symbol, int id) {
        at(dummy, "symbol") = gs(symbol), at(dummy, "itemID") = gi(id);
        sheet.values.push_back(dummy);
    };
    const std::pair<const char *, const char *> dampe[] = {{"Dampe1", "dampe-page-1"}, {"DampeHeart", "dampe-heart-challenge"},
        {"Dampe2", "dampe-page-2"}, {"DampeBottle", "dampe-bottle-challenge"}, {"DampeFinal", "dampe-final"}};
    int id = 63;
    for (const auto &dm : dampe) {  // (their gettingFlag: Dampe's rewards' real work is done by these flags)
        at(dummy, "gettingFlag") = gs(dm.first);
        at(dummy, "npcKey") = gs(item_field(d, placed(d, dm.second), "npc-key"));
        add(dm.first, id++);
    }
    at(dummy, "gettingFlag") = gs("");
    add("ShopShovel", 68);
    add("ShopBow", 69);
    add("ShopHeart", 70);
    // seashell mansion presents give the first item with the trap's ID: each trap gets its own
    if (d.setting_str("traps") != "none") {
        at(dummy, "npcKey") = gs("NpcToolShopkeeper");
        add("ZapTrap", 127);
        add("DrownTrap", 128);
        add("SquishTrap", 129);
        add("DeathballTrap", 130);
        add("QuakeTrap", 131);
    }
    for (const auto &s : {std::make_pair("FishNecklace", 200), std::make_pair("SyrupPowder", 201),
                          std::make_pair("WalrusShell", 202)}) {
        at(dummy, "npcKey") = gs(s.first);
        add(s.first, s.second);
    }
}

// conditions.editConditions
void edit_condition(SeedBuild &b, GStruct &c) {
    const Decisions &d = b.decisions();
    const std::string sym = at(c, "symbol").s;
    GValue &cs = at(c, "conditions");
    if (sym == "MarinVillageStay") { erase_at(cs, 1); return; }  // (Marin stays in Mabe after the pineapple)
    if (sym == "AnimalPop") { item(cs, 0) = cond(9, "false"); return; }
    if (sym == "BroomInvisible") { erase_at(cs, 0); return; }
    // (LAS Recomp: the Ghost as a pet doesn't join by himself; skip-story-events: no kids after the Cello)
    if (sym == "GhostJoinCheck" && setting_or(d, "pet-ghost", false)) { cs = gsa({cond(9, "false")}); return; }
    if (sym == "RescueBowWow" && setting_or(d, "skip-story-events", true)) { cs = gsa({cond(9, "false")}); return; }
    if (sym == "BowWowKidnap" && setting_or(d, "skip-story-events", true)) { item(cs, 0) = cond(2, "FullMoonCello"); return; }
    if (sym == "BowWowMissionStart" || sym == "BowWowMissionEnd") { erase_at(cs, 0); return; }
    if (sym == "ShopShieldCondition") { item(cs, 0) = cond(1, SHIELD_FOUND_FLAG); return; }
    if (sym == "ShopBombCondition" && (S(b, "unlocked-bombs") || S(b, "shuffle-bombs"))) item(cs, 0) = cond(1, BOMBS_FOUND_FLAG);
}

// conditions.makeConditions: the shell sensor's conditions for the NPCs with several rewards
void make_conditions(SeedBuild &b, GSheet &sheet) {
    const std::pair<const char *, std::vector<const char *>> sets[] = {
        {"DampeShellsComplete", {"dampe-page-1", "dampe-heart-challenge", "dampe-page-2", "dampe-bottle-challenge", "dampe-final"}},
        {"RapidsShellsComplete", {"rapids-race-30", "rapids-race-35", "rapids-race-45"}},
        {"FishingShellsComplete", {"fishing-orange", "fishing-cheep-cheep", "fishing-ol-baron", "fishing-loose",
                                   "fishing-50", "fishing-100", "fishing-150"}},
        {"MansionShellsComplete", {"5-seashell-reward", "15-seashell-reward", "30-seashell-reward",
                                   "40-seashell-reward", "50-seashell-reward"}}};
    for (const auto &s : sets) {
        GStruct c = create_condition(s.first, {{9, "true"}});
        for (const char *loc : s.second)
            if (is_seashell(b, loc)) at(c, "conditions").structs.push_back(cond(2, seashell_param(b, loc, "")));
        sheet.values.push_back(c);
    }
}

// crane_prizes.makeDatasheetChanges
void crane_prize_changes(SeedBuild &b, GSheet &sheet) {
    for (GStruct &prize : sheet.values) {
        const std::string sym = at(prize, "symbol").s;
        GValue &layouts = at(prize, "layouts");
        auto conds = [&](size_t i) -> GValue & { return at(item(layouts, i), "conditions"); };
        if (sym == "Bomb" && S(b, "shuffle-bombs")) {
            item(conds(0), 0) = cond(1, BOMBS_FOUND_FLAG);
            continue;
        }
        if (sym == "Shield") {
            conds(0).structs.push_back(cond(1, SHIELD_FOUND_FLAG));
            continue;
        }
        if (sym == "RupeeRed") {  // (fills the slot the two conditioned prizes leave empty)
            layouts.structs.push_back({{"itemIndex", gi(-1)}, {"conditions", gsa({})},
                                       {"place", gst({{"type", gi(2)}, {"index", gi(1)}})}});
            if (S(b, "shuffle-bombs")) conds(2).structs.push_back(cond(1, std::string("!") + BOMBS_FOUND_FLAG));
            else conds(2).structs.push_back(cond(2, "!SurfHarp"));
            conds(2).structs.push_back(cond(1, std::string("!") + SHIELD_FOUND_FLAG));
            conds(0).structs.push_back(cond(1, "TradeYoshiDollGet"));
            continue;
        }
        if (sym == "YoshiDoll") {
            conds(0).structs.push_back(cond(1, "!TradeYoshiDollGet"));
            at(at(prize, "options").structs.at(0), "isFeatured") = gb(false);
            continue;
        }
        if (sym == "SmallBowWow" || sym == "BowWow") {
            erase_at(conds(0), 0);
            continue;
        }
    }
}

void general_datasheets(SeedBuild &b) {
    {
        GSheet s = b.read_sheet("Npc.gsheet");
        for (GStruct &npc : s.values) npc_changes(b, npc);
        new_npcs(b, s);
        b.write_sheet("Npc.gsheet", s);
    }
    {
        GSheet s = b.read_sheet("ItemDrop.gsheet");
        item_drop_changes(b, s);
        b.write_sheet("ItemDrop.gsheet", s);
    }
    {
        GSheet s = b.read_sheet("Items.gsheet");
        items_changes(b, s);
        b.write_sheet("Items.gsheet", s);
    }
    {
        GSheet s = b.read_sheet("Conditions.gsheet");
        for (GStruct &c : s.values) edit_condition(b, c);
        make_conditions(b, s);
        b.write_sheet("Conditions.gsheet", s);
    }
    {
        GSheet s = b.read_sheet("CranePrize.gsheet");
        crane_prize_changes(b, s);
        b.write_sheet("CranePrize.gsheet", s);
    }
    {
        GSheet s = b.read_sheet("CranePrizeFeaturedPrizeGroup1.gsheet");
        if (s.values.empty()) throw std::runtime_error("las: CranePrizeFeaturedPrizeGroup1 is empty");
        s.values.erase(s.values.begin());  // (no Yoshi Doll as a featured prize)
        b.write_sheet("CranePrizeFeaturedPrizeGroup1.gsheet", s);
    }
    {  // flags.makeFlags: the generator's own flags (las_data.h GLOBAL_FLAGS: the steps read their indexes there)
        GSheet s = b.read_sheet("GlobalFlags.gsheet");
        for (const auto &f : GLOBAL_FLAGS) s.values.push_back({{"symbol", gs(f.first)}, {"index", gi(f.second)}});
        b.write_sheet("GlobalFlags.gsheet", s);
    }
    if (S(b, "fast-fishing")) {
        GSheet s = b.read_sheet("FishingFish.gsheet");
        for (GStruct &fish : s.values) {
            GValue &v = at(fish, "mOpenItem");
            if (!v.s.empty()) v = gs("ClothesGreen");
        }
        b.write_sheet("FishingFish.gsheet", s);
    }
}

// ======== makeGeneralEventChanges

// data.start_dungeon_items
std::vector<std::string> start_dungeon_items(const Decisions &d) {
    std::vector<std::string> out;
    if (yyjson_val *v = setting_val(d, "dungeon-items-start")) {
        yyjson_val *x;
        yyjson_arr_iter it = yyjson_arr_iter_with(v);
        while ((x = yyjson_arr_iter_next(&it)))
            if (const char *s = yyjson_get_str(x)) out.push_back(s);
        return out;
    }
    std::string di = d.setting_str("dungeon-items");
    if (di == "mc") return {"map", "compass"};
    if (di == "mcb") return {"map", "compass", "stone-beak"};
    if (di == "stone-beak") return {"stone-beak"};
    return out;
}

bool contains(const std::vector<std::string> &v, const char *s) {
    for (const std::string &x : v)
        if (x == s) return true;
    return false;
}

// player_start.makeStartChanges
void start_changes(Flow &f) {
    const Decisions &d = f.b.decisions();
    std::string first = f.action("EventFlags", "SetFlag", {{"symbol", "FirstClear"}, {"value", true}});
    std::string check = f.sw("EventFlags", "CheckFlag", {{"symbol", "FirstClear"}}, {{0, first}, {1, ""}});
    std::vector<std::string> flags(START_FLAGS.begin(), START_FLAGS.end());
    if (!setting_or(d, "open-desert", true))  // (LAS Recomp pets: the Walrus sleeps until Marin sings)
        for (auto it = flags.begin(); it != flags.end(); ++it)
            if (*it == "WalrusAwaked") { flags.erase(it); break; }
    if (setting_or(d, "open-kanalet", false)) flags.push_back("GateOpen_Switch_KanaletCastle_01B");
    if (setting_or(d, "open-bridge", false)) flags.push_back("StickDrop");
    if (setting_or(d, "open-mamu", false)) flags.push_back("MamuMazeClear");
    if (!setting_or(d, "shuffle-bombs", false) && setting_or(d, "unlocked-bombs", false)) flags.push_back(BOMBS_FOUND_FLAG);
    if (setting_or(d, "randomize-enemies", false)) flags.push_back("AppearStairsFld10N"), flags.push_back("AppearStairsFld11O");
    if (setting_or(d, "fast-stalfos", false))
        for (const char *s : {"DoorOpen_Btl1_L05_05F", "DoorOpen_Btl2_L05_04H", "DoorOpen_Btl3_L05_01F"}) flags.push_back(s);
    if (setting_or(d, "boss-cutscenes", false)) flags.insert(flags.end(), BOSS_FLAGS.begin(), BOSS_FLAGS.end());
    // (LAS Recomp dungeon room shuffle: Eagle's Tower starts collapsed) settings.get(...) not in ('off', None, False)
    yyjson_val *rs = setting_val(d, "dungeon-room-shuffle");
    bool room_shuffle = rs && !yyjson_is_null(rs) && !yyjson_is_false(rs) && !(yyjson_is_num(rs) && yyjson_get_num(rs) == 0) &&
                        !(yyjson_is_str(rs) && !strcmp(yyjson_get_str(rs), "off"));
    if (room_shuffle)
        for (const char *s : {"TowerPillarBroken_L07_02B", "TowerPillarBroken_L07_02C", "TowerPillarBroken_L07_03B",
                              "TowerPillarBroken_L07_03C", "TowerPillarBrokenAll", "Lv7BrokenPillar"})
            flags.push_back(s);
    ActionDefs defs;
    for (const std::string &fl : flags) defs.push_back({"EventFlags", "SetFlag", {{"symbol", fl}, {"value", true}}});
    f.insert_after("Event558", check);
    f.action_chain(first, defs);
    f.insert_after("Level7DungeonIn_FlyingCucco", "Event476");  // (the rooster isn't killed after D7)
    if (setting_or(d, "fast-stealing", false))
        f.action_chain("Event774", {{"EventFlags", "SetFlag", {{"symbol", "StealSuccess"}, {"value", false}}}});
    std::vector<std::string> start = start_dungeon_items(d);
    if (!start.empty()) {  // (given on entering a dungeon: the level is told apart that way)
        ActionDefs give;
        auto more = [&](const ActionDefs &x) { give.insert(give.end(), x.begin(), x.end()); };
        if (contains(start, "map")) more(item_without_animation(d, "DungeonMap", -1));
        if (contains(start, "compass")) more(item_without_animation(d, "Compass", -1));
        if (contains(start, "stone-beak")) more(item_without_animation(d, "StoneBeak", -1));
        f.action_chain("Event539", give);
        f.action_chain("Event550", give);
    }
    for (const char *e : {"Event637", "Event660", "Event693", "Event696", "Event371", "Event407", "Event478"})
        f.event(e).params.set("timeOut", EvflValue::of_float(0.0f));
}

// chests.writeChestEvent / seashell_mansion.changeRewards: compare the item key down a chain of switches, each item
// with its own get events (going on to `after`); the head of the chain
std::string item_compare_chain(Flow &f, const std::string &after, const std::string &otherwise) {
    static const char *const kItems[] = {"SwordLv1", "Shield", "PowerBraceletLv1", "MagicPowder_MaxUp", "Bomb_MaxUp",
        "Arrow_MaxUp", "ClothesRed", "ClothesBlue", "FullMoonCello", "SurfHarp", "YoshiDoll", "Ribbon", "DogFood",
        "Bananas", "Stick", "Honeycomb", "Pineapple", "Hibiscus", "Letter", "Broom", "FishingHook", "PinkBra",
        "MermaidsScale", "MagnifyingLens", "ZapTrap", "DrownTrap", "SquishTrap", "DeathballTrap", "QuakeTrap", "Bomb",
        "MagicPowder"};
    std::string head = otherwise;
    for (const char *it : kItems) {
        std::string get = insert_item_get(f, it, -1, "", after);
        head = f.sw("FlowControl", "CompareString", {{"value1", "itemKey"}, {"value2", it}}, {{0, get}, {1, head}});
    }
    // (CompareInt is patched to compare for equality)
    std::string bottle = insert_item_get(f, "Bottle", 1, "", after);
    std::string index_check = f.sw("FlowControl", "CompareInt", {{"value1", "itemIndex"}, {"value2", 1}}, {{0, bottle}, {1, head}});
    return f.sw("FlowControl", "CompareString", {{"value1", "itemKey"}, {"value2", "Bottle"}}, {{0, index_check}, {1, head}});
}

void write_chest_event(Flow &f) {
    std::string save = f.action("GameControl", "RequestAutoSave", {});
    std::string head = item_compare_chain(f, save, "Event33");
    ItemGetGlobals g = item_get_globals(f.b.decisions());
    for (const std::string &soul : g.soul_keys) {  // (LAS Recomp boss souls)
        std::string get = insert_item_get(f, soul, -1, "", save);
        head = f.sw("FlowControl", "CompareString", {{"value1", "itemKey"}, {"value2", soul}}, {{0, get}, {1, head}});
    }
    if (g.triforce) {  // (LAS Recomp Triforce hunt: each piece has its own index, the chest's)
        std::string get = insert_triforce_get(f, arg("itemIndex"), "", save);
        head = f.sw("FlowControl", "CompareString", {{"value1", "itemKey"}, {"value2", "TriforcePiece"}}, {{0, get}, {1, head}});
    }
    f.insert_after("Event32", head);  // TreasureBox_Open, TreasureBox_ShockOpen
    f.insert_after("Event28", head);
    f.insert_after("Event40", save);  // (the other items save too)
    f.insert_after("Event5", save);
    f.insert_after("TreasureBox_ShockOpen", "Event27");  // (the D6 pot chest: does it hold an enemy)
    f.insert_after("Event15", "Event28");
    std::string enemy = f.sw("TreasureBox", "ContainsEnemy", {}, {{0, "Event15"}, {1, "Event42"}});
    f.insert_after("Event27", enemy);
}

void general_events(SeedBuild &b) {
    const Decisions &d = b.decisions();
    {
        EventFlow fl = b.read_flow("PlayerStart.bfevfl");
        Flow f(b, fl.flowchart);
        start_changes(f);
        if (S(b, "randomize-music")) f.insert_after("Event150", "Event151");  // (no BGM_HOUSE_FIRST over the zone's)
        b.write_flow("PlayerStart.bfevfl", fl);
    }
    {
        EventFlow fl = b.read_flow("TreasureBox.bfevfl");
        Flow f(b, fl.flowchart);
        write_chest_event(f);
        if (S(b, "fast-chests")) {  // chests.makeChestsFaster
            EvflEvent &e = f.event("Event44");
            erase_fork(e, 0);
            f.insert_after("Event52", "");
        }
        b.write_flow("TreasureBox.bfevfl", fl);
    }
    {
        EventFlow fl = b.read_flow("ShellMansionPresent.bfevfl");
        Flow f(b, fl.flowchart);
        f.insert_after("Event25", item_compare_chain(f, "Event0", "Event4"));
        b.write_flow("ShellMansionPresent.bfevfl", fl);
    }
    {
        EventFlow fl = b.read_flow("Item.bfevfl");
        Flow f(b, fl.flowchart);
        for (const char *ep : {"MagicPowder_MaxUp", "Bomb_MaxUp", "Arrow_MaxUp"}) {
            f.add_entry_point(ep);
            f.action_chain(ep, {{"Dialog", "Show", {{"message", "SubEvent:ByebyeMadBatter"}}}});
        }
        for (const auto &r : {std::make_pair("GreenClothes", "ClothesGreen"), std::make_pair("RedClothes", "ClothesRed"),
                              std::make_pair("BlueClothes", "ClothesBlue"), std::make_pair("Necklace", "PinkBra")}) {
            EvflEntryPoint *ep = f.find_entry(r.first);
            if (!ep) throw std::runtime_error(std::string("las: no entry point ") + r.first);
            ep->name = r.second;
        }
        for (const auto &dm : {std::make_pair("Dampe1", "dampe-page-1"), std::make_pair("DampeHeart", "dampe-heart-challenge"),
                               std::make_pair("Dampe2", "dampe-page-2"), std::make_pair("DampeBottle", "dampe-bottle-challenge"),
                               std::make_pair("DampeFinal", "dampe-final")}) {
            f.add_entry_point(dm.first);
            std::string key = item_key_at(d, dm.second);
            bool trap = key.size() >= 4 && !key.compare(key.size() - 4, 4, "Trap");
            if (!trap && key.compare(0, 7, "Clothes")) f.insert_after(dm.first, f.subflow("", key, {}));
        }
        b.write_flow("Item.bfevfl", fl);
    }
    {  // Madam MeowMeow always takes BowWow back, whatever the horn
        EventFlow fl = b.read_flow("MadamMeowMeow.bfevfl");
        Flow f(b, fl.flowchart);
        f.insert_after("Event69", "Event18");
        f.insert_after("Event22", "Event5");
        f.set_case("Event5", 0, "Event0");
        f.set_case("Event5", 1, "Event52");
        f.set_case("Event0", 0, "Event40");
        f.set_case("Event0", 1, "Event21");
        f.set_case("Event21", 0, "Event80");
        f.event("Event21").params.set("symbol", EvflValue::of_str("BowWowJoin"));
        b.write_flow("MadamMeowMeow.bfevfl", fl);
    }
    {
        EventFlow fl = b.read_flow("WindFishsEgg.bfevfl");
        Flow f(b, fl.flowchart);
        f.insert_after("Event142", "");  // (no Owl after the egg opens)
        b.write_flow("WindFishsEgg.bfevfl", fl);
    }
    {  // the powder guard: 40 powder, and 60 bombs
        EventFlow fl = b.read_flow("SkeletalGuardBlue.bfevfl");
        Flow f(b, fl.flowchart);
        f.event("Event19").params.set("count", EvflValue::of_int(40));
        std::string bombs = f.action("Inventory", "AddItem", {{"itemType", 4}, {"count", 60}, {"autoEquip", false}});
        if (S(b, "shuffle-powder")) {
            std::string check = f.sw("EventFlags", "CheckFlag", {{"symbol", "GetMagicPowder"}}, {{0, "Event54"}, {1, "Event46"}});
            f.set_case("Event7", 1, check);
        }
        if (S(b, "shuffle-bombs")) {
            std::string check = f.sw("EventFlags", "CheckFlag", {{"symbol", BOMBS_FOUND_FLAG}}, {{0, ""}, {1, bombs}});
            f.insert_after("Event19", check);
        } else {
            f.insert_after("Event19", bombs);
        }
        b.write_flow("SkeletalGuardBlue.bfevfl", fl);
    }
    {  // Save & Quit after a game over: back to Marin's house
        EventFlow fl = b.read_flow("Common.bfevfl");
        Flow f(b, fl.flowchart);
        std::string jump = f.action("GameControl", "RequestLevelJump",
                                    {{"level", "Field"}, {"locator", "Field_11C"}, {"offsetX", 0.0}, {"offsetZ", 0.0}}, "Event67");
        f.set_case("Event64", 1, jump);
        if (S(b, "randomize-music")) f.insert_after("Event167", "");
        b.write_flow("Common.bfevfl", fl);
    }
    {  // crane_prizes.makeEventChanges: Yoshi doesn't replace the lens
        EventFlow fl = b.read_flow("PrizeCommon.bfevfl");
        Flow f(b, fl.flowchart);
        std::string yoshi_lens = f.action_chain("", {{"EventFlags", "SetFlag", {{"symbol", "TradeYoshiDollGet"}, {"value", true}}},
                                                     {"EventFlags", "SetFlag", {{"symbol", LENS_FOUND_FLAG}, {"value", true}}},
                                                     {"Inventory", "SetWarashibeItem", {{"itemType", 15}}}});
        std::string yoshi = f.action_chain("", {{"Inventory", "SetWarashibeItem", {{"itemType", 0}}},
                                                {"EventFlags", "SetFlag", {{"symbol", "TradeYoshiDollGet"}, {"value", true}}}});
        std::string lens = f.sw("EventFlags", "CheckFlag", {{"symbol", LENS_FOUND_FLAG}}, {{0, yoshi}, {1, yoshi_lens}});
        std::string check = f.sw("Inventory", "HasItem", {{"itemType", 30}, {"count", 1}}, {{0, ""}, {1, lens}});
        for (const char *e : {"Event3", "Event7", "Event9"}) f.insert_after(e, check);
        b.write_flow("PrizeCommon.bfevfl", fl);
    }
}

// ======== makeChestContentFixes
void chest_contents(SeedBuild &b) {
    const Decisions &d = b.decisions();
    std::string aspect = d.setting_str("chest-aspect");
    if (aspect == "camc")  // (its texture models are copied from LAS's resources: not ported)
        throw std::runtime_error("las: chest-aspect camc isn't ported");
    decide_chest_looks(b);
    yyjson_val *looks = d.raw("chest-looks");  // LAS Recomp chest looks (mod_generator.CHEST_LOOK)
    auto look = [&](const std::string &room) {
        yyjson_val *v = room.empty() ? yyjson_obj_get(looks, "panel") : yyjson_obj_get(yyjson_obj_get(looks, "rooms"), room.c_str());
        if (!yyjson_is_str(v)) throw std::runtime_error("las: no chest look recorded for " + room);
        return std::string(yyjson_get_str(v));
    };
    StrTable rooms = CHEST_ROOMS;
    if (looks) rooms.insert(rooms.end(), PANEL_CHEST_ROOMS.begin(), PANEL_CHEST_ROOMS.end());
    NumTable sizes = CHEST_SIZES;
    for (auto &s : sizes) {
        if (aspect == "default") s.second = 1.0;
        else if (!strcmp(s.first, "seashell") && !S(b, "seashells-important")) s.second = 0.8;
        else if (!strcmp(s.first, "trade") && !S(b, "trade-important")) s.second = 0.8;
    }
    auto size_of = [&](const std::string &type) {
        double v;
        if (!lookup(sizes, type, v)) throw std::runtime_error("las: no chest size for " + type);
        return v;
    };
    for (const auto &r : rooms) {
        const std::string room = r.first, file = std::string(r.second) + ".leb";
        LebRoom rd = b.read_room(file);
        if (!room.compare(0, 6, "panel-")) {  // (Dampe's panels: their own content, a plain look)
            std::string model = looks ? look("") : lookup(CHEST_TEXTURES, "default");
            for (size_t i = 0; i < rd.actors.size(); i++) {
                const LebActor &a = rd.actors[i];
                if (a.name.compare(0, 14, "ObjTreasureBox")) continue;
                if (a.params[1].kind != LebParam::Str) throw std::runtime_error("las: panel chest without content in " + file);
                std::string content = a.params[1].s;
                LebParam index = a.params[2];
                set_chest_content(rd, content, index, 0, 1.0, &model);  // (the first chest: as the Python)
            }
            b.write_room(file, rd);
            continue;
        }
        SeedBuild::ItemInfo info = b.item_info(room, false);
        std::string item = placed(d, room), type = item_field(d, item, "type"), chest_type;
        bool has_chest_type = item_field(d, item, "chest-type", chest_type);
        double size;
        if (aspect == "csmc") {
            const std::string &k = info.key;
            if (k == "HeartContainer" || k == "ClothesRed" || k == "ClothesBlue") size = size_of("junk");
            else if (k == "SmallKey" || k == "Bomb_MaxUp" || k == "Arrow_MaxUp" || k == "MagicPowder_MaxUp") size = size_of("important");
            else size = size_of(type);
        } else {
            size = size_of(type);
        }
        if (info.key == "StoneBeak" && S(b, "owl-dungeon-gifts")) has_chest_type = true, chest_type = "default", size = size_of("important");
        (void)has_chest_type;  // (camc's texture choice: not ported)
        std::string model;
        bool with_model = false;
        if (looks) size = 1.0, model = look(room), with_model = true;
        if (room == "taltal-5-chest-puzzle") {
            for (size_t i = 0; i < 5; i++) set_chest_content(rd, info.key, info.index, i, size, with_model ? &model : nullptr);
        } else {
            set_chest_content(rd, info.key, info.index, 0, size, with_model ? &model : nullptr);
        }
        b.write_room(file, rd);
        // D7's rooms before and after the collapse: the same chest twice
        const char *twin = room == "D7-grim-creeper" ? "Lv07EagleTower_06H.leb" : room == "D7-3f-horseheads" ? "Lv07EagleTower_05G.leb" : nullptr;
        if (twin) {
            LebRoom t = b.read_room(twin);
            set_chest_content(t, info.key, info.index, 0, size, with_model ? &model : nullptr);
            b.write_room(twin, t);
        }
    }
}

// ======== makeEventContentChanges: one step per NPC / boss

// a flow whose only change is the item at `check` given between two events
void simple_get(SeedBuild &b, const char *file, const char *check, const char *before, const char *after,
                const std::vector<std::pair<const char *, const char *>> &songs = {}) {
    EventFlow fl = b.read_flow(file);
    Flow f(b, fl.flowchart);
    SeedBuild::ItemInfo it = b.item_info(check, false);
    insert_item_get(f, it.key, it.index, before ? before : "", after ? after : "");
    if (S(b, "randomize-music"))
        for (const auto &s : songs) f.set_song(s.first, song(b.decisions(), s.second));
    b.write_flow(file, fl);
}

void tarin(SeedBuild &b) {
    const Decisions &d = b.decisions();
    EventFlow fl = b.read_flow("Tarin.bfevfl");
    Flow f(b, fl.flowchart);
    insert_item_get(f, item_key_at(d, "tarin"), index_or(d, "tarin"), "Event52", "Event31");
    {  // Tarin detains Link by whether they talked, not by the shield
        EvflEvent &e0 = f.event("Event0"), &e78 = f.event("Event78");
        e0.actor = e78.actor, e0.action = e78.action, e0.params = e78.params;
    }
    ActionDefs defs;
    int swords = 0, shields = 0, bracelets = 0;
    for (const std::string &i : starting_items(d)) {
        std::string key = item_field(d, i, "item-key");
        if (key == "SwordLv1") { if (++swords == 2) key = "SwordLv2"; }
        else if (key == "Shield") { if (++shields == 2) key = "MirrorShield"; }
        else if (key == "PowerBraceletLv1") { if (++bracelets == 2) key = "PowerBraceletLv2"; }
        ActionDefs x = item_without_animation(d, key, -1);
        defs.insert(defs.end(), x.begin(), x.end());
    }
    std::string after = "Event52";
    int64_t rupees = setting_int(d, "starting-rupees");
    if (rupees > 0) {
        fl.flowchart.actors[f.actor("Link")].add_action("AddRupee");
        after = f.action("Link", "AddRupee", {{"amount", (int)rupees}}, "Event52");
    }
    if (!defs.empty()) f.action_chain("Event36", defs, after);
    else f.insert_after("Event36", after);
    b.write_flow("Tarin.bfevfl", fl);
}

// miscellaneous.change*: a loose item turned into a sinking-sword style actor with its own entry point
void loose_item(Flow &f, const SeedBuild::ItemInfo &it, LebActor &act, const char *entry, const char *flag, int type,
                bool face_screen) {
    std::string get;
    if (!it.key.compare(0, 3, "Rup"))  // (rupees: given without the fanfare)
        get = f.action("Inventory", "AddItemByKey", {{"itemKey", it.key}, {"count", 1}, {"index", it.index}, {"autoEquip", false}});
    else
        get = insert_item_get(f, it.key, it.index);
    f.add_entry_point(entry);
    f.action_chain(entry, {{"SinkingSword", "Destroy", {}}, {"EventFlags", "SetFlag", {{"symbol", flag}, {"value", true}}}}, get);
    act.type = (uint16_t)type;
    if (face_screen) act.rot[1] = 0;
    act.params[0] = str_param(it.model_path);
    act.params[1] = str_param(it.model_name);
    act.params[2] = str_param(entry);
    act.params[3] = str_param(flag);
    act.params[4] = str_param(it.key == "Seashell" ? "true" : "false");
    apply_model_look(act, it.model_name);
}

void sinking_sword(SeedBuild &b) {
    EventFlow fl = b.read_flow("SinkingSword.bfevfl");
    Flow f(b, fl.flowchart);
    {  // the beach: miscellaneous.changeSunkenSword
        LebRoom r = b.read_room("Field_16C.leb");
        bool music = S(b, "randomize-music");  // (its music would be cut off)
        SeedBuild::ItemInfo it = b.item_info("washed-up", true);
        std::string end;
        if (music) erase_fork(f.event("Event0"), 0);
        else end = "Event8";
        if (!it.key.compare(0, 3, "Rup")) {
            std::string rup = f.action("Inventory", "AddItemByKey", {{"itemKey", it.key}, {"count", 1}, {"index", it.index}, {"autoEquip", false}}, end);
            f.insert_after("Event5", rup);
        } else {
            insert_item_get(f, it.key, it.index, "Event5", end);
        }
        erase_fork(f.event("Event0"), 0);  // (the sword's own get animation)
        f.remove_after("Event10");
        erase_fork(f.event("Event8"), 1);  // (the spin attack)
        f.event("Event2").params.set("symbol", EvflValue::of_str(BEACH_LOOSE_FLAG));
        LebActor &act = r.actors.at(4);
        act.rot[1] = 0.0;
        act.params[0] = str_param(it.model_path);
        act.params[1] = str_param(it.model_name);
        act.params[2] = str_param("examine");
        act.params[3] = str_param(BEACH_LOOSE_FLAG);
        act.params[4] = str_param(it.key == "Seashell" ? "true" : "false");
        apply_model_look(act, it.model_name);
        b.write_room("Field_16C.leb", r);
    }
    struct Loose { const char *room, *check; size_t actor; const char *entry, *flag; int type; bool face; };
    const Loose loose[] = {
        {"EagleKeyCave_01A.leb", "taltal-rooster-cave", 0, "TalTal", ROOSTER_CAVE_FLAG, 0x194, false},
        {"DreamShrine_01A.leb", "dream-shrine-left", 5, "DreamShrine", DREAM_SHRINE_FLAG, 0x8E, false},  // (a Yoshi doll)
        {"Field_06A.leb", "woods-loose", 3, "Woods", WOODS_LOOSE_FLAG, 0x194, false},
        {"MermaidStatue_01A.leb", "mermaid-cave", 7, "MermaidCave", MERMAID_CAVE_FLAG, 0x194, true}};
    for (const Loose &l : loose) {
        LebRoom r = b.read_room(l.room);
        SeedBuild::ItemInfo it = b.item_info(l.check, true);
        loose_item(f, it, r.actors.at(l.actor), l.entry, l.flag, l.type, l.face);
        b.write_room(l.room, r);
    }
    b.write_flow("SinkingSword.bfevfl", fl);
}

void walrus(SeedBuild &b) { simple_get(b, "Walrus.bfevfl", "walrus", "Event53", "Event110"); }
void christine(SeedBuild &b) { simple_get(b, "Christine.bfevfl", "christine-grateful", "Event44", "Event36"); }

void invisible_zora(SeedBuild &b) {
    EventFlow fl = b.read_flow("SecretZora.bfevfl");
    Flow f(b, fl.flowchart);
    SeedBuild::ItemInfo it = b.item_info("invisible-zora", false);
    insert_item_get(f, it.key, it.index, "Event23", "Event27");
    f.insert_after("Event32", "Event23");
    b.write_flow("SecretZora.bfevfl", fl);
}

void marin(SeedBuild &b) {
    EventFlow fl = b.read_flow("Marin.bfevfl");
    Flow f(b, fl.flowchart);
    SeedBuild::ItemInfo it = b.item_info("marin", false);
    if (S(b, "fast-songs")) {  // (no song cutscene: Link is sad about it)
        std::string sad = f.action("Link", "SetFacialExpression", {{"expression", "sad"}});
        std::string flag = f.action("EventFlags", "SetFlag", {{"symbol", "MarinsongGet"}, {"value", true}}, sad);
        f.insert_after("Event92", flag);
        insert_item_get(f, it.key, it.index, sad, "Event666");
    } else {
        insert_item_get(f, it.key, it.index, "Event246", "Event666");
    }
    // marin.makeEventChanges: no Ballad from her; her checks ask for the song flag
    erase_fork(f.event("Event249"), 0);
    f.insert_after("Event27", "Event249");
    {
        EvflEvent &e20 = f.event("Event20"), &e160 = f.event("Event160"), &e676 = f.event("Event676");
        e160.actor = e676.actor = e20.actor;
        e160.action = e676.action = e20.action;
        e160.params.set("symbol", EvflValue::of_str("MarinsongGet"));
        e676.params.set("symbol", EvflValue::of_str("MarinsongGet"));
    }
    f.set_case("Event21", 0, "Event674");  // (no beach talk)
    f.set_case("Event2", 1, "Event21");
    b.write_flow("Marin.bfevfl", fl);
}

void ghost_reward(SeedBuild &b) {
    EventFlow fl = b.read_flow("Owl.bfevfl");
    Flow f(b, fl.flowchart);
    std::string gone = f.action("Owl", "Destroy", {});
    SeedBuild::ItemInfo it = b.item_info("ghost-reward", false);
    insert_item_get(f, it.key, it.index, "Event34", gone);
    b.write_flow("Owl.bfevfl", fl);
}

void clothes_fairy(SeedBuild &b) {
    const Decisions &d = b.decisions();
    EventFlow fl = b.read_flow("FairyQueen.bfevfl");
    Flow f(b, fl.flowchart);
    SeedBuild::ItemInfo two = b.item_info("D0-fairy-2", false);
    std::string item2 = insert_item_get(f, two.key, two.index, "Event0", "Event180");
    SeedBuild::ItemInfo one = b.item_info("D0-fairy-1", false);
    insert_item_get(f, one.key, one.index, "Event0", item2);
    f.insert_after("Event128", "Event58");
    // she sends Link out of the color dungeon's door, wherever it is
    std::string destin;
    yyjson_val *exits = setting_val(d, "dungeon-exits");
    if (py_truthy(exits)) {  // (LAS Recomp: dungeon doors mixed with houses)
        const char *s = yyjson_get_str(yyjson_obj_get(exits, "color-dungeon"));
        if (!s) throw std::runtime_error("las: no dungeon exit for color-dungeon");
        destin = s;
    } else if (S(b, "shuffle-dungeons")) {
        yyjson_val *ents = d.raw("dungeon-entrances"), *k;
        yyjson_obj_iter it = yyjson_obj_iter_with(ents);
        std::string from;
        while ((k = yyjson_obj_iter_next(&it))) {
            const char *v = yyjson_get_str(yyjson_obj_iter_get_val(k));
            if (v && !strcmp(v, "color-dungeon")) { from = yyjson_get_str(k); break; }
        }
        for (const DungeonEntrance &e : DUNGEON_ENTRANCES)
            if (from == e.name) destin = std::string(e.field_room) + e.field_suffix;
        if (destin.empty()) throw std::runtime_error("las: no dungeon entrance leads to color-dungeon");
    }
    if (!destin.empty()) {
        EvflEvent &warp = f.event("Event37");
        warp.params.set("level", EvflValue::of_str(room_level(destin)));
        warp.params.set("locator", EvflValue::of_str(destin));
    }
    b.write_flow("FairyQueen.bfevfl", fl);
}

// Goriya / Manbo / Mamu: a flag marks the gift given, checked before the trade
void flag_gift(SeedBuild &b, const char *file, const char *check, const char *flag, const char *flag_next,
               const char *before, const char *switch_after, const char *off, const char *on) {
    EventFlow fl = b.read_flow(file);
    Flow f(b, fl.flowchart);
    std::string set = f.action("EventFlags", "SetFlag", {{"symbol", flag}, {"value", true}}, flag_next);
    SeedBuild::ItemInfo it = b.item_info(check, false);
    insert_item_get(f, it.key, it.index, before, set);
    std::string sw = f.sw("EventFlags", "CheckFlag", {{"symbol", flag}}, {{0, off}, {1, on}});
    f.insert_after(switch_after, sw);
    b.write_flow(file, fl);
}

void goriya(SeedBuild &b) { flag_gift(b, "Goriya.bfevfl", "goriya-trader", GORIYA_FLAG, "Event4", "Event87", "Event24", "Event7", "Event15"); }
void manbo(SeedBuild &b) {
    flag_gift(b, "ManboTamegoro.bfevfl", "manbo", MANBO_FLAG, "Event13", S(b, "fast-songs") ? "Event44" : "Event31",
              "Event9", "Event37", "Event35");
}
void mamu(SeedBuild &b) {
    flag_gift(b, "Mamu.bfevfl", "mamu", MAMU_FLAG, "Event40", S(b, "fast-songs") ? "Event55" : "Event85", "Event10",
              "Event14", "Event98");
}

void rapids(SeedBuild &b) {
    const Decisions &d = b.decisions();
    EventFlow fl = b.read_flow("RaftShopMan.bfevfl");
    Flow f(b, fl.flowchart);
    // rapids.makePrizesStack: a faster time gives the slower prizes not yet won too
    insert_item_get(f, item_key_at(d, "rapids-race-45"), index_or(d, "rapids-race-45"), "Event42", "Event88");
    std::string get35 = insert_item_get(f, item_key_at(d, "rapids-race-35"), index_or(d, "rapids-race-35"), "", "Event86");
    std::string sub45 = f.subflow("", "5minfirst", {}, get35);
    std::string check45 = f.sw("EventFlags", "CheckFlag", {{"symbol", "5minGaul"}}, {{0, sub45}, {1, get35}});
    f.insert_after("Event40", check45);
    std::string get30 = insert_item_get(f, item_key_at(d, "rapids-race-30"), index_or(d, "rapids-race-30"), "", "Event85");
    std::string sub35 = f.subflow("", "3minfirst", {}, get30);
    std::string check35 = f.sw("EventFlags", "CheckFlag", {{"symbol", "3minGaul"}}, {{0, sub35}, {1, get30}});
    f.insert_after("Event38", check35);
    if (S(b, "randomize-music")) {  // (the race music is gone in music rando: so are its StopBGMs)
        f.insert_after("timeAttackGoal", "Event27");
        f.insert_after("normalGoal", "Event20");
    }
    b.write_flow("RaftShopMan.bfevfl", fl);
}

void fishing(SeedBuild &b) {
    const Decisions &d = b.decisions();
    EventFlow fl = b.read_flow("Fisherman.bfevfl");
    Flow f(b, fl.flowchart);
    const char *const defs[][3] = {{"fishing-orange", "Event113", "Event212"}, {"fishing-cheep-cheep", "Event3", "Event10"},
        {"fishing-ol-baron", "Event133", "Event140"}, {"fishing-50", "Event182", "Event240"},
        {"fishing-100", "Event191", "Event247"}, {"fishing-150", "Event193", "Event255"},
        {"fishing-loose", "Event264", "Event265"}};
    for (const auto &x : defs) insert_item_get(f, item_key_at(d, x[0]), index_or(d, x[0]), x[1], x[2], false, false);
    std::string bottle = f.action("EventFlags", "SetFlag", {{"symbol", "FishingBottleGet"}, {"value", true}}, "Event264");
    f.insert_after("Event20", "Event3");
    f.insert_after("Event18", "Event133");
    f.insert_after("Event24", "Event191");
    f.insert_after("FishingGetBottle", bottle);
    // fishing.fixFishingBottle (RemoveItem itemType 0 is patched to take bottle 1)
    std::string take = f.action("Inventory", "RemoveItem", {{"itemType", 0}}, "Event74");
    std::string check = f.sw("EventFlags", "CheckFlag", {{"symbol", "FishingBottleGet"}}, {{0, take}, {1, "Event83"}});
    f.insert_after("Event83", "Event74");
    for (const char *e : {"Event315", "Event316", "Event317"}) f.insert_after(e, check);
    std::string give = f.action("Inventory", "AddBottle", {{"index", 1}}, "Event45");
    std::string take2 = f.action("Inventory", "RemoveItem", {{"itemType", 0}}, "Event45");
    std::string check2 = f.sw("EventFlags", "CheckFlag", {{"symbol", "Bottle2Get"}}, {{0, take2}, {1, give}});
    f.insert_after("Event189", check2);
    b.write_flow("Fisherman.bfevfl", fl);
}

void trendy(SeedBuild &b) { simple_get(b, "GameShopOwner.bfevfl", "trendy-prize-final", "Event112", "Event239"); }

void seashell_mansion(SeedBuild &b) {
    const Decisions &d = b.decisions();
    EventFlow fl = b.read_flow("ShellMansionMaster.bfevfl");
    Flow f(b, fl.flowchart);
    const char *const presents[][3] = {{"5-seashell-reward", "Event36", "GetSeashell10"}, {"15-seashell-reward", "Event10", "GetSeashell20"},
        {"30-seashell-reward", "Event11", "GetSeashell30"}, {"50-seashell-reward", "Event13", "GetSeashell50"}};
    for (const auto &p : presents) {
        SeedBuild::ItemInfo it = b.item_info(p[0], false);
        f.event(p[1]).params = container({{"pointIndex", 0}, {"itemKey", it.key}, {"itemIndex", it.index}, {"flag", p[2]}});
    }
    SeedBuild::ItemInfo it = b.item_info("40-seashell-reward", false);
    insert_item_get(f, it.key, it.index, "Event91", "Event79");
    // seashell_mansion.makeEventChanges: the 40 shells' reward isn't a present
    erase_fork(f.event("Event65"), 0);
    f.insert_after("Event64", "Event65");
    std::string sc1 = f.sw("EventFlags", "CheckFlag", {{"symbol", SWORD_FOUND_FLAG}}, {{0, "Event65"}, {1, "Event64"}});
    f.insert_after("Event80", sc1);  // (no sword shown when Link has none)
    bool sword = placed(d, "40-seashell-reward") == "sword";
    if (!sword) {
        std::string sc2 = f.sw("EventFlags", "CheckFlag", {{"symbol", SWORD_FOUND_FLAG}}, {{0, "Event48"}, {1, "Event47"}});
        f.insert_after("Event54", sc2);
        f.insert_after("Event74", "Event19");
    } else {  // (a sword here: given before the cutscene ends, as in the game)
        std::string g1 = f.action("Inventory", "AddItemByKey", {{"itemKey", "SwordLv1"}, {"count", 1}, {"index", -1}, {"autoEquip", false}}, "Event19");
        std::string g2 = f.action("Inventory", "AddItemByKey", {{"itemKey", "SwordLv2"}, {"count", 1}, {"index", -1}, {"autoEquip", false}}, "Event19");
        std::string sc3 = f.sw("EventFlags", "CheckFlag", {{"symbol", SWORD_FOUND_FLAG}}, {{0, g1}, {1, g2}});
        f.insert_after("Event74", sc3);
    }
    b.write_flow("ShellMansionMaster.bfevfl", fl);
}

void mad_batter(SeedBuild &b) {
    EventFlow fl = b.read_flow("MadBatter.bfevfl");
    Flow f(b, fl.flowchart);
    std::string items[3];
    const char *const checks[3] = {"mad-batter-bay", "mad-batter-woods", "mad-batter-taltal"};
    for (int i = 0; i < 3; i++) {
        SeedBuild::ItemInfo it = b.item_info(checks[i], false);
        items[i] = insert_item_get(f, it.key, it.index, "", "Event23");
    }
    // mad_batter.writeEvents: one entry point per batter (the rooms' third parameter), without the upgrade choice
    f.insert_after("Event19", "Event13");
    const char *const entries[3] = {"BatterA", "BatterB", "BatterC"};
    for (int i = 0; i < 3; i++) {
        f.add_entry_point(entries[i]);
        std::string sub = f.subflow("", "talk2", {});
        f.insert_after(entries[i], sub);
        f.insert_after(sub, items[i]);
    }
    if (S(b, "randomize-music")) {
        f.set_song("Event18", song(b.decisions(), "BGM_MADBATTER"));
        f.set_song("Event150", song(b.decisions(), "BGM_MADBATTER"));
    }
    b.write_flow("MadBatter.bfevfl", fl);
}

void dampe(SeedBuild &b) {
    const Decisions &d = b.decisions();
    // dampe.makeDatasheetChanges: his rewards are stand-in items whose flags do the real work after
    auto reward = [](GSheet &s, size_t row, const char *key) {
        GStruct &r = s.values.at(row);
        at(r, "mRewardItem") = gs(key), at(r, "mRewardItemIndex") = gi(0), at(r, "mRewardItemEventEntry") = gs(key);
    };
    {
        GSheet s = b.read_sheet("MapPieceClearReward.gsheet");
        reward(s, 3, "Dampe1"), reward(s, 7, "Dampe2"), reward(s, 12, "DampeFinal");
        b.write_sheet("MapPieceClearReward.gsheet", s);
    }
    {
        GSheet s = b.read_sheet("MapPieceTheme.gsheet");
        reward(s, 3, "DampeHeart"), reward(s, 9, "DampeBottle");
        b.write_sheet("MapPieceTheme.gsheet", s);
    }
    // dampe.makeEventChanges: after the reward, each flag set gives its item and is cleared
    EventFlow fl = b.read_flow("Danpei.bfevfl");
    Flow f(b, fl.flowchart);
    const char *const rewards[][2] = {{"DampeFinal", "dampe-final"}, {"DampeBottle", "dampe-bottle-challenge"},
                                      {"Dampe2", "dampe-page-2"}, {"DampeHeart", "dampe-heart-challenge"},
                                      {"Dampe1", "dampe-page-1"}};
    std::string next = "Event42";
    for (const auto &r : rewards) {
        std::string remove = f.action("EventFlags", "SetFlag", {{"symbol", r[0]}, {"value", false}}, next);
        std::string give = insert_dampe_item_get(f, item_key_at(d, r[1]), index_or(d, r[1]), remove);
        next = f.sw("EventFlags", "CheckFlag", {{"symbol", r[0]}}, {{0, next}, {1, give}});
    }
    f.insert_after("Event39", next);
    b.write_flow("Danpei.bfevfl", fl);
}

// the bosses: their item instead of the heart container (with music rando, their songs too)
void moldorm(SeedBuild &b) {
    simple_get(b, "DeguTail.bfevfl", "D1-moldorm", "Event8", "Event45",
               {{"Event16", "BGM_DUNGEON_BOSS"}, {"Event19", "BGM_PANEL_RESULT"}, {"Event65", "BGM_DUNGEON_BOSS"}, {"Event30", "BGM_DUNGEON_BOSS"}});
}
void genie(SeedBuild &b) {
    simple_get(b, "PotDemonKing.bfevfl", "D2-genie", "Event29", "Event56",
               {{"Event5", "BGM_DUNGEON_BOSS"}, {"Event6", "BGM_DUNGEON_BOSS"}, {"Event53", "BGM_PANEL_RESULT"}, {"Event50", "BGM_DUNGEON_BOSS"}});
}
void slime_eye(SeedBuild &b) {
    simple_get(b, "DeguZol.bfevfl", "D3-slime-eye", "Event29", "Event43",
               {{"Event17", "BGM_DUNGEON_BOSS"}, {"Event36", "BGM_PANEL_RESULT"}, {"Event32", "BGM_DUNGEON_BOSS"}});
}
void angler(SeedBuild &b) {
    simple_get(b, "Angler.bfevfl", "D4-angler", "Event25", "Event50",
               {{"Event5", "BGM_DUNGEON_BOSS"}, {"Event28", "BGM_DUNGEON_BOSS"}, {"Event29", "BGM_DUNGEON_BOSS"}, {"Event51", "BGM_PANEL_RESULT"}});
}
void slime_eel(SeedBuild &b) {
    simple_get(b, "Hooker.bfevfl", "D5-slime-eel", "Event28", "Event13",
               {{"Event26", "BGM_DUNGEON_BOSS"}, {"Event33", "BGM_PANEL_RESULT"}, {"Event49", "BGM_DUNGEON_BOSS"}, {"Event20", "BGM_DUNGEON_BOSS"}});
}
void facade(SeedBuild &b) {
    simple_get(b, "MatFace.bfevfl", "D6-facade", "Event8", "Event35",
               {{"Event22", "BGM_DUNGEON_BOSS"}, {"Event29", "BGM_PANEL_RESULT"}, {"Event78", "BGM_DUNGEON_BOSS"}, {"Event19", "BGM_DUNGEON_BOSS"}});
}
void eagle(SeedBuild &b) {
    simple_get(b, "Albatoss.bfevfl", "D7-eagle", "Event40", "Event51",
               {{"Event15", "BGM_DUNGEON_LV7_BOSS"}, {"Event20", "BGM_DUNGEON_BOSS"}, {"Event66", "BGM_PANEL_RESULT"}});
}
void hothead(SeedBuild &b) {
    simple_get(b, "DeguFlame.bfevfl", "D8-hothead", "Event13", "Event15",
               {{"Event28", "BGM_DUNGEON_BOSS"}, {"Event40", "BGM_DUNGEON_BOSS"}, {"Event63", "BGM_PANEL_RESULT"},
                {"Event17", "BGM_DUNGEON_BOSS"}, {"Event70", "BGM_DUNGEON_BOSS"}});
}
void lanmola(SeedBuild &b) {
    simple_get(b, "Lanmola.bfevfl", "lanmola", "Event34", "Event9",
               {{"Event2", "BGM_DUNGEON_BOSS_MIDDLE"}, {"Event18", "BGM_DUNGEON_BOSS_MIDDLE"}, {"Event22", "BGM_DUNGEON_BOSS_MIDDLE"}});
}

void armos_knight(SeedBuild &b) {  // (the doors open before the item)
    EventFlow fl = b.read_flow("DeguArmos.bfevfl");
    Flow f(b, fl.flowchart);
    f.remove_after("Event2");
    f.insert_after("Event2", "Event8");
    SeedBuild::ItemInfo it = b.item_info("armos-knight", false);
    insert_item_get(f, it.key, it.index, "Event47", "");
    if (S(b, "randomize-music"))
        for (const char *e : {"Event4", "Event23"}) f.set_song(e, song(b.decisions(), "BGM_DUNGEON_BOSS_MIDDLE"));
    b.write_flow("DeguArmos.bfevfl", fl);
}

void master_stalfos(SeedBuild &b) {
    std::vector<std::pair<const char *, const char *>> songs;
    for (const char *e : {"Event0", "Event1", "Event3", "Event132", "Event157", "Event2", "Event4", "Event10", "Event23"})
        songs.emplace_back(e, "BGM_DUNGEON_BOSS_MIDDLE");
    simple_get(b, "MasterStalfon.bfevfl", "D5-master-stalfos", "Event37", "Event194", songs);
}

void syrup(SeedBuild &b) { simple_get(b, "Syrup.bfevfl", "syrup", "Event93", nullptr); }

// ======== makeTradeQuestChanges
void trade_quest(SeedBuild &b) {
    // (file, check, before, after, switch to copy from, switch to change, its flag)
    struct Trade { const char *file, *check, *before, *after, *from, *to, *flag; };
    auto give = [&](Flow &f, const char *check, const char *before, const char *after) {
        SeedBuild::ItemInfo it = b.item_info(check, false);
        return insert_item_get(f, it.key, it.index, before ? before : "", after ? after : "");
    };
    auto trade = [&](const Trade &t, const std::function<void(Flow &)> &more = nullptr) {
        EventFlow fl = b.read_flow(t.file);
        Flow f(b, fl.flowchart);
        give(f, t.check, t.before, t.after);
        if (t.from) copy_query(f, t.from, t.to, {{"symbol", t.flag}});
        if (more) more(f);
        b.write_flow(t.file, fl);
    };
    trade({"QuadrupletsMother.bfevfl", "mamasha", "Event15", nullptr, "Event1", "Event3", "TradeYoshiDollGet"});
    trade({"RibbonBowWow.bfevfl", "ciao-ciao", "Event21", nullptr, "Event1", "Event3", "TradeRibbonGet"});
    trade({"Sale.bfevfl", "sale", "Event31", nullptr, "Event0", "Event2", "TradeDogFoodGet"});
    {  // Kiki: trade_quest.kikiChanges, then the stick he drops shows the item
        EventFlow fl = b.read_flow("Kiki.bfevfl");
        Flow f(b, fl.flowchart);
        SeedBuild::ItemInfo it = b.item_info("kiki", true);
        std::string get = insert_item_get(f, it.key, it.index, "", "Event102");
        std::string bananas = f.sw("EventFlags", "CheckFlag", {{"symbol", "TradeBananasGet"}}, {{0, "Event118"}, {1, "Event32"}});
        f.insert_after("Event91", bananas);
        f.insert_after("Event84", "Event15");  // (the trade slot isn't emptied)
        f.insert_after("Event29", "Event88");
        erase_fork(f.event("Event88"), 0);
        bool open = S(b, "open-bridge");
        if (open) {
            f.insert_after("Event9", "Event31");
            f.insert_after("Event10", "Event31");
            EvflEvent &e28 = f.event("Event28");
            for (size_t i : {1, 1, 1, 1, 2}) erase_fork(e28, i);
            erase_fork(f.event("Event31"), 0);
            std::string gone = f.action("EventFlags", "SetFlag", {{"symbol", "KikiGone"}, {"value", true}}, get);
            f.insert_after("Event89", gone);
        } else {
            f.insert_after("Event89", get);
        }
        b.write_flow("Kiki.bfevfl", fl);
        LebRoom r = b.read_room("Field_08L.leb");
        LebActor &kiki = r.actors.at(0), &stick = r.actors.at(7);
        if (open) kiki.pos[0] += 1.5, stick.pos[0] += 1.5, stick.pos[2] -= 1.5;
        stick.params[1] = str_param(it.model_path);
        stick.params[2] = str_param(it.model_name);
        b.write_room("Field_08L.leb", r);
    }
    trade({"Tarin.bfevfl", "tarin-ukuku", "Event130", "Event29", nullptr, nullptr, nullptr});
    trade({"ChefBear.bfevfl", "chef-bear", "Event16", nullptr, "Event1", "Event11", "TradeHoneycombGet"});
    trade({"Papahl.bfevfl", "papahl", "Event32", "Event62", "Event81", "Event2", "TradePineappleGet"});
    trade({"Christine.bfevfl", "christine-trade", "Event15", "Event22", "Event0", "Event10", "TradeHibiscusGet"},
          [](Flow &f) { f.insert_after("Event28", "Event15"); });
    trade({"DrWrite.bfevfl", "mr-write", "Event48", "Event46", "Event0", "Event2", "TradeLetterGet"}, [](Flow &f) {
        f.insert_after("Event7", "Event47");
        erase_fork(f.event("Event47"), 1);
    });
    trade({"GrandmaUlrira.bfevfl", "grandma-yahoo", "Event54", "Event33", nullptr, nullptr, nullptr}, [](Flow &f) {
        std::string broom = f.sw("EventFlags", "CheckFlag", {{"symbol", "TradeBroomGet"}}, {{0, "Event69"}, {1, "Event79"}});
        f.insert_after("Event11", "Event0");
        f.set_case("Event0", 0, broom);
        f.insert_after("Event81", "Event53");
        erase_fork(f.event("Event53"), 1);
    });
    trade({"MarthasBayFisherman.bfevfl", "bay-fisherman", "Event28", "Event42", "Event0", "Event2", "TradeFishingHookGet"},
          [](Flow &f) {
              f.insert_after("Event32", "Event33");
              erase_fork(f.event("Event27"), 1);
          });
    trade({"MermaidMartha.bfevfl", "mermaid-martha", "Event73", "Event55", "Event0", "Event2", "TradeNecklaceGet"},
          [](Flow &f) { erase_fork(f.event("Event71"), 1); });
    {
        EventFlow fl = b.read_flow("MarthaStatue.bfevfl");
        Flow f(b, fl.flowchart);
        std::string scale = f.sw("EventFlags", "CheckFlag", {{"symbol", "TradeMermaidsScaleGet"}}, {{0, "Event28"}, {1, "Event32"}});
        f.set_case("Event3", 0, scale);
        b.write_flow("MarthaStatue.bfevfl", fl);
    }
}

}  // namespace
}  // namespace las

std::vector<BuildStep> steps_las_items() {
    using namespace las;
    return {
        {"mod_generator.makeGeneralDatasheetChanges", general_datasheets},
        {"mod_generator.makeGeneralEventChanges", general_events},
        {"mod_generator.makeChestContentFixes", chest_contents},
        // makeEventContentChanges: each of its methods writes under its own name
        {"mod_generator.tarinChanges", tarin},
        {"mod_generator.sinkingSwordChanges", sinking_sword},
        {"mod_generator.walrusChanges", walrus},
        {"mod_generator.christineChanges", christine},
        {"mod_generator.invisibleZoraChanges", invisible_zora},
        {"mod_generator.marinChanges", marin},
        {"mod_generator.ghostRewardChanges", ghost_reward},
        {"mod_generator.clothesFairyChanges", clothes_fairy},
        {"mod_generator.goriyaChanges", goriya},
        {"mod_generator.manboChanges", manbo},
        {"mod_generator.mamuChanges", mamu},
        {"mod_generator.rapidsChanges", rapids},
        {"mod_generator.fishingChanges", fishing},
        {"mod_generator.trendyChanges", trendy},
        {"mod_generator.seashellMansionChanges", seashell_mansion},
        {"mod_generator.madBatterChanges", mad_batter},
        {"mod_generator.dampeChanges", dampe},
        {"mod_generator.moldormChanges", moldorm},
        {"mod_generator.genieChanges", genie},
        {"mod_generator.slimeEyeChanges", slime_eye},
        {"mod_generator.anglerChanges", angler},
        {"mod_generator.slimeEelChanges", slime_eel},
        {"mod_generator.facadeChanges", facade},
        {"mod_generator.eagleChanges", eagle},
        {"mod_generator.hotheadChanges", hothead},
        {"mod_generator.lanmolaChanges", lanmola},
        {"mod_generator.armosKnightChanges", armos_knight},
        {"mod_generator.masterStalfosChanges", master_stalfos},
        {"mod_generator.syrupChanges", syrup},
        {"mod_generator.makeTradeQuestChanges", trade_quest},
    };
}

}  // namespace rando
