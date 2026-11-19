// LAS-Randomizer's world edits: small keys, golden leaves, heart pieces, instruments, owls, the telephone, Color
// Dungeon's blue rupees, dungeon entrances and their map icons, level configs, music, enemies, open Mabe, water zones,
// the rapids, the code patches: the generator's steps ported (seed_build.h); each makes exactly the writes its Python
// does (lasrando check-steps, tools/rando/test_apply.py). Ported from LAS-Randomizer's RandomizerCore/mod_generator.py
// and Randomizers/{small_keys, golden_leaves, heart_pieces, instruments, owls, tunic_swap, rupees}.py, with LAS
// Recomp's additions there (dungeon exits) (MIT, Owen-Splat and contributors). The enemies are las_enemies.cpp's.
//
// The generator's random draws come from the decisions (tools/rando/record_las_world.py): the trap models, the songs,
// what randomizeEnemies did to each room's actors ("enemies") and makeExefsPatches' chest enemy ("chest-enemy"); in
// decide mode (decide.h) the chest enemy is drawn here.
// The code patches come assembled from las_world_data.cpp (tools/rando/export_las_world_tables.py).
//
// Python quirks kept for the same files:
//   - makeSmallKeyChanges writes the key rooms but leaves SmallKey.bfevfl to makeGoldenLeafChanges, which writes it
//     with the key events in: the golden leaf step makes the key events again (from the AutoEvent number they started
//     at), so either step can run on its own;
//   - makeOwlStatueChanges, owls.makeDungeonChanges: the dungeon flow's fork Event50 loses its sixth branch, whatever
//     it is;
//   - makeExefsPatches: a setting turns a patch on only when it equals True / False (an int 1 / 0 does too, a string
//     never), Stealing's "always" / "never" counting as True / False (assemble.preSetup).
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <map>
#include <stdexcept>

#include "../../../third_party/yyjson/yyjson.h"
#include "decide.h"
#include "item_get.h"
#include "las_data.h"
#include "las_enemies.h"
#include "las_world_data.h"
#include "steps.h"

namespace rando {
namespace las {
namespace {

using namespace data;

bool S(SeedBuild &b, const char *key) { return setting_or(b.decisions(), key, false); }

const char *must(const StrTable &t, const std::string &key) {
    const char *v = lookup(t, key);
    if (!v) throw std::runtime_error("las: no table entry " + key);
    return v;
}

// item_key[:3] in ['Sma', 'Nig', 'Dun', 'Com', 'Sto', 'Rup']: added without the get animation
bool plain_add(const std::string &key) {
    static const char *const kPre[] = {"Sma", "Nig", "Dun", "Com", "Sto", "Rup"};
    for (const char *p : kPre)
        if (!key.compare(0, 3, p)) return true;
    return false;
}
Params add_params(const std::string &key, int index) {
    return {{"itemKey", key}, {"count", 1}, {"index", index}, {"autoEquip", false}};
}

const DungeonEntrance &entrance(const std::string &name) {
    for (const DungeonEntrance &e : DUNGEON_ENTRANCES)
        if (name == e.name) return e;
    throw std::runtime_error("las: no dungeon entrance " + name);
}
// placements['dungeon-entrances'][door] (the dungeon behind it)
std::string dungeon_at(SeedBuild &b, const std::string &door) {
    const char *s = yyjson_get_str(yyjson_obj_get(b.decisions().raw("dungeon-entrances"), door.c_str()));
    if (!s) throw std::runtime_error("las: no dungeon entrance placed at " + door);
    return s;
}
// ent_keys[ent_values.index(dungeon)]: the door the dungeon is behind
std::string door_of(SeedBuild &b, const std::string &dungeon) {
    yyjson_val *o = b.decisions().raw("dungeon-entrances"), *k;
    yyjson_obj_iter it = yyjson_obj_iter_with(o);
    while ((k = yyjson_obj_iter_next(&it))) {
        const char *v = yyjson_get_str(yyjson_obj_iter_get_val(k));
        if (v && dungeon == v) return yyjson_get_str(k);
    }
    throw std::runtime_error("las: dungeon " + dungeon + " behind no door");
}

// ======== makeSmallKeyChanges / makeGoldenLeafChanges

// small_keys.writeKeyEvent
void write_key_event(Flow &f, const std::string &key, int index, const std::string &room) {
    std::string item_event;
    if (plain_add(key)) item_event = f.action_chain("", {{"Inventory", "AddItemByKey", add_params(key, index)}});
    else item_event = insert_item_get(f, key, index);
    f.add_entry_point(room);
    f.action_chain(room, {{"SmallKey", "Deactivate", {}},
                          {"SmallKey", "SetActorSwitch", {{"value", true}, {"switchIndex", 1}}},
                          {"SmallKey", "Destroy", {}}},
                   item_event);
}

// small_keys.makeKeysFaster
void keys_faster(Flow &f) {
    f.insert_after("pop", "Event5");
    f.insert_after("Event3", "");
    f.event("Event3").params.set("time", EvflValue::of_float(2.0f));
    f.insert_after("Lv4_04E_pop", "Event7");
    f.insert_after("Event10", "");
}

// makeSmallKeyChanges' work on SmallKey.bfevfl, and (rooms) its room writes; without the rooms no trap model is drawn
void small_key_rooms(SeedBuild &b, Flow &f, bool rooms) {
    if (S(b, "fast-keys")) keys_faster(f);
    for (const auto &kv : SMALL_KEY_ROOMS) {
        const std::string room = kv.first, file = std::string(kv.second) + ".leb";
        LebRoom r;
        if (rooms) r = b.read_room(file);
        SeedBuild::ItemInfo it = b.item_info(room, rooms, room != "pothole-final");  // (pothole: trap_models)
        if (room == "pothole-final" && rooms) {
            LebActor &act = r.actors.at(42);
            act.type = 0xA9;  // (a small key)
            act.pos[0] += 1.5, act.pos[2] -= 1.5;
            act.switches[0] = {1, (uint16_t)global_flag("PotholeKeySpawn")};
            act.switches[1] = {1, 363};  // (the get flag, now unused0363)
        }
        write_key_event(f, it.key, it.index, room);
        if (!rooms) continue;
        set_small_key_params(r, it.model_path, it.model_name, room, it.key);
        b.write_room(file, r);
        if (room == "D4-sunken-item") {  // (the same key in 06A)
            LebRoom r2 = b.read_room("Lv04AnglersTunnel_06A.leb");
            set_small_key_params(r2, it.model_path, it.model_name, room, it.key);
            b.write_room("Lv04AnglersTunnel_06A.leb", r2);
        }
    }
}

void small_keys(SeedBuild &b) {
    EventFlow fl = b.read_flow("SmallKey.bfevfl");
    Flow f(b, fl.flowchart);
    small_key_rooms(b, f, true);  // (the flow is written by the golden leaves' step)
}

// golden_leaves.py: a small key for each golden leaf check
LebActor tag_holocaust(const LebActor &from, uint64_t key, uint16_t sw0) {
    LebActor c = from;
    char hex[17];
    snprintf(hex, sizeof hex, "%016llX", (unsigned long long)key);
    c.key = key;
    c.name = std::string("TagHolocaust-") + hex;
    c.type = 0xF0;
    c.params[0] = LebParam::of_int(1), c.params[1] = LebParam::of_int(0), c.params[2] = LebParam::of_int(30);
    c.params[3] = LebParam::of_int(0), c.params[4] = LebParam::of_int(0);
    c.params[5] = c.params[6] = c.params[7] = str_param("");
    c.switches[0] = {1, sw0};
    c.switches[1] = {2, 1};
    c.links.k = 1, c.links.x = 1;
    LebLinks::ActorLink l;
    l.p[0] = l.p[1] = str_param("");
    l.actor = 0;
    c.links.actors = {l};
    return c;
}

void create_room_key(LebRoom &r, const std::string &room) {
    if (room == "kanalet-crow") {
        LebActor &crow = r.actors.at(0);
        crow.params[1] = str_param("");
        crow.links.y = 1, crow.links.ids = {8};
        LebActor &leaf = r.actors.at(6);
        leaf.type = 0xA9;
        leaf.pos[0] = 131.25, leaf.pos[2] = 60.75;
        leaf.switches[0] = {1, 37};  // FieldKanaletCrowlyDead
        leaf.switches[1] = {1, (uint16_t)global_flag("KeyGetField06I")};
        LebActor c = tag_holocaust(r.actors.at(7), 0xA1002A105CF0F2E8ull, 37);
        r.actors.push_back(c);
    } else if (room == "kanalet-mad-bomber") {
        LebActor &bomber = r.actors.at(0);
        bomber.links.y = 1, bomber.links.ids = {3};
        LebActor &leaf = r.actors.at(2);
        leaf.type = 0xA9;
        leaf.pos[0] -= 4.5, leaf.pos[1] -= 0.5, leaf.pos[2] -= 6.0;
        leaf.switches[0] = {1, 36};  // FieldKanaletBombKnuckleDead
        leaf.switches[1] = {1, (uint16_t)global_flag("KeyGetField06K")};
        LebActor c = tag_holocaust(leaf, 0xA1002A205CF0F2E8ull, 36);
        r.actors.push_back(c);
    } else if (room == "kanalet-kill-room") {
        LebActor &leaf = r.actors.at(5);
        leaf.type = 0xA9;
        leaf.switches[1] = {1, (uint16_t)global_flag("KeyGetKanalet02A")};
    } else if (room == "kanalet-bombed-guard") {
        LebActor &enemy = r.actors.at(0);
        enemy.params[0] = str_param("");  // (drops no golden leaf at all)
        enemy.links.y = 1, enemy.links.ids = {14};
        LebActor &leaf = r.actors.at(5);
        leaf.type = 0xA9;
        leaf.pos[0] = 71.25, leaf.pos[1] = 1.5, leaf.pos[2] = 1.75;
        leaf.switches[1] = {1, (uint16_t)global_flag("KeyGetKanalet01C")};
        LebActor c = tag_holocaust(leaf, 0xA1002A305CF0F2E8ull, 649);  // GoldenLeafPop_Btl_KanaletCastle_01C
        r.actors.push_back(c);
    } else if (room == "kanalet-final-guard") {
        LebActor k = r.actors.at(0);
        k.key = 0xA1002A405CF0F2E8ull;
        k.name = "ItemSmallKey-A1002A405CF0F2E8";
        k.type = 0xA9;
        k.pos[0] -= 1.5, k.pos[2] -= 1.5;
        k.switches[0] = {1, 650};  // GoldenLeafPop_Btl_KanaletCastle_01D
        k.switches[1] = {1, (uint16_t)global_flag("KeyGetKanalet01D")};
        r.actors.push_back(k);
    } else {
        throw std::runtime_error("las: no golden leaf room " + room);
    }
}

void golden_leaves(SeedBuild &b) {
    EventFlow fl = b.read_flow("SmallKey.bfevfl");
    // the key events makeSmallKeyChanges made (in memory there): made again under the names they got - counted on a
    // copy first, as the counter is past them now
    uint32_t end = b.idgen();
    {
        EventFlow scratch = fl;
        Flow f(b, scratch.flowchart);
        small_key_rooms(b, f, false);
    }
    uint32_t made = b.idgen() - end;
    b.set_idgen(end - made);
    Flow f(b, fl.flowchart);
    small_key_rooms(b, f, false);
    for (const auto &kv : GOLDEN_LEAF_ROOMS) {
        const std::string room = kv.first, file = std::string(kv.second) + ".leb";
        LebRoom r = b.read_room(file);
        SeedBuild::ItemInfo it = b.item_info(room, true);
        create_room_key(r, room);
        write_key_event(f, it.key, it.index, room);
        set_small_key_params(r, it.model_path, it.model_name, room, it.key);
        b.write_room(file, r);
    }
    b.write_flow("SmallKey.bfevfl", fl);
}

// ======== makeHeartPieceChanges

const char *const kSunken[] = {"taltal-east-drop", "south-bay-sunken", "bay-passage-sunken", "river-crossing-cave",
                               "kanalet-moat-south"};
bool sunken(const std::string &room) {
    for (const char *s : kSunken)
        if (room == s) return true;
    return false;
}

// heart_pieces.changeHeartPiece
void change_heart_piece(Flow &f, const SeedBuild::ItemInfo &it, const std::string &room, LebRoom &r) {
    LebActor *act = nullptr;
    for (LebActor &a : r.actors)
        if (a.type == 0xB0) { act = &a; break; }
    if (!act) throw std::runtime_error("las: no heart piece in " + room);
    std::string get;
    if (!it.key.compare(0, 3, "Rup")) get = f.action("Inventory", "AddItemByKey", add_params(it.key, it.index));
    else get = insert_item_get(f, it.key, it.index);
    f.add_entry_point(room);
    f.action_chain(room, {{"SinkingSword", "Destroy", {}},
                          {"EventFlags", "SetFlag", {{"symbol", must(HEART_FLAGS, room)}, {"value", true}}}},
                   get);
    act->type = 0x194;  // (a sinking sword)
    if (sunken(room)) {
        if (room == "taltal-east-drop") act->pos[1] += 2;  // (floats on the water: unseen in it)
    } else {
        act->pos[1] += room == "mabe-well" ? 0.5 : 0.375;
    }
    act->params[0] = str_param(it.model_path);
    act->params[1] = str_param(it.model_name);
    act->params[2] = str_param(room);
    act->params[3] = str_param(must(HEART_FLAGS, room));
    act->params[4] = str_param(it.key == "Seashell" ? "true" : "false");
    apply_model_look(*act, it.model_name);
}

void heart_pieces(SeedBuild &b) {
    EventFlow fl = b.read_flow("SinkingSword.bfevfl");
    Flow f(b, fl.flowchart);
    for (const auto &kv : HEART_ROOMS) {
        if (sunken(kv.first)) continue;
        const std::string room = kv.first, file = std::string(kv.second) + ".leb";
        LebRoom r = b.read_room(file);
        SeedBuild::ItemInfo it = b.item_info(room, true);
        change_heart_piece(f, it, room, r);
        b.write_room(file, r);
    }
    b.write_flow("SinkingSword.bfevfl", fl);
}

// ======== makeInstrumentChanges

// instruments.insertInstrumentFadeEvent
std::string instrument_fade(Flow &f, const std::string &level, const std::string &location) {
    std::string shine = f.action_chain("", {{"Audio", "StopAllBGM", {{"duration", 1.0}}},
                                            {"Link", "PlayInstrumentShineEffect", {}},
                                            {"Timer", "Wait", {{"time", 2}}}});
    std::string jump = f.action_chain("", {{"Timer", "Wait", {{"time", 2}}},
                                           {"GameControl", "RequestLevelJump",
                                            {{"level", level}, {"locator", location}, {"offsetX", 0.0}, {"offsetZ", 0.0}}},
                                           {"GameControl", "RequestAutoSave", {}}});
    std::string stop = f.action("Audio", "StopOtherThanSystemSE", {{"duration", 3.0}});
    std::string se = f.action("Audio", "PlayOneshotSystemSE",
                              {{"label", "SE_ENV_GET_INST_WHITEOUT2"}, {"pitch", 1.0}, {"volume", 1.0}});
    std::string fade = f.action_chain("", {{"Fade", "StartPreset", {{"preset", 3}}},
                                           {"Fade", "StartParam",
                                            {{"colorB", 0.9}, {"colorG", 0.9}, {"colorR", 0.9}, {"mode", 2}, {"time", 0.75}}}});
    return f.fork(shine, {stop, se, fade}, jump).first;
}

// instruments.changeInstrument (destination "": the level jump the actor had)
void change_instrument(Flow &f, const SeedBuild::ItemInfo &it, const std::string &room, LebRoom &r,
                       const std::string &destination) {
    LebActor &act = r.actors.at(room == "D6-instrument" ? 1 : 0);
    std::string level, location;
    if (destination.empty()) {
        if (act.params[0].kind != LebParam::Str || act.params[1].kind != LebParam::Str)
            throw std::runtime_error("las: instrument without its level jump in " + room);
        level = act.params[0].s, location = act.params[1].s;
    } else {
        level = room_level(destination), location = destination;
    }
    act.type = 0x8E;  // (a Yoshi doll: gone once Yoshi is had, which the player never is)
    act.params[0] = str_param(it.model_path);
    act.params[1] = str_param(it.model_name);
    act.params[2] = str_param(room);
    act.params[3] = str_param(must(INSTRUMENT_FLAGS, room));
    act.params[4] = str_param(it.key == "Seashell" ? "true" : "false");
    apply_model_look(act, it.model_name);
    std::string fade = instrument_fade(f, level, location);
    std::string get = insert_item_get(f, it.key, it.index, "", fade);
    f.add_entry_point(room);
    f.action_chain(room, {{"SinkingSword", "Destroy", {}},
                          {"EventFlags", "SetFlag", {{"symbol", must(INSTRUMENT_FLAGS, room)}, {"value", true}}}},
                   get);
}

void instruments(SeedBuild &b) {
    EventFlow fl = b.read_flow("SinkingSword.bfevfl");
    Flow f(b, fl.flowchart);
    yyjson_val *exits = setting_val(b.decisions(), "dungeon-exits");
    for (const auto &kv : INSTRUMENT_ROOMS) {
        const std::string room = kv.first, file = std::string(kv.second) + ".leb";
        LebRoom r = b.read_room(file);
        SeedBuild::ItemInfo it = b.item_info(room, true, true);
        std::string destination;
        const std::string cur = room_level(kv.second);
        if (py_truthy(exits)) {  // (LAS Recomp: dungeon doors mixed with houses - outside its exit door)
            bool found = false;
            for (const DungeonEntrance &e : DUNGEON_ENTRANCES) {
                if (room_level(e.room) != cur) continue;
                const char *s = yyjson_get_str(yyjson_obj_get(exits, e.name));
                if (!s) throw std::runtime_error(std::string("las: no dungeon exit for ") + e.name);
                destination = s, found = true;
                break;
            }
            if (!found) throw std::runtime_error("las: no dungeon entrance for " + room);
        } else if (S(b, "shuffle-dungeons")) {
            bool found = false;
            for (const DungeonEntrance &e : DUNGEON_ENTRANCES) {
                if (room_level(e.room) != cur) continue;
                const DungeonEntrance &d = entrance(door_of(b, e.name));
                destination = std::string(d.field_room) + d.field_suffix, found = true;
            }
            if (!found) throw std::runtime_error("las: no dungeon entrance for " + room);
        }
        change_instrument(f, it, room, r, destination);
        b.write_room(file, r);
    }
    b.write_flow("SinkingSword.bfevfl", fl);
}

// ======== makeOwlStatueChanges

// owls.addSlimeKeyCheck: the owl before the slime key drops it (owl gifts or not)
void slime_key_check(Flow &f) {
    std::string drop = f.action("EventFlags", "SetFlag", {{"symbol", "PotholeKeySpawn"}, {"value", true}}, "Event21");
    std::string shovel = f.sw("Inventory", "HasItem", {{"itemType", 10}, {"count", 1}}, {{0, "Event21"}, {1, drop}});
    std::string chest = f.sw("EventFlags", "CheckFlag", {{"symbol", "PotholeKeySpawn"}}, {{0, shovel}, {1, "Event21"}});
    f.insert_after("examine_anaboko", chest);
}

// an owl's gift after `after` (owls.makeFieldChanges / makeDungeonChanges)
void owl_gift(SeedBuild &b, Flow &f, const std::string &check, const std::string &after) {
    std::string item = placed(b.decisions(), check);
    if (item.empty()) throw std::runtime_error("las: nothing placed at " + check);
    std::string gift = insert_item_get(f, item_field(b.decisions(), item, "item-key"), index_or(b.decisions(), check));
    std::string set = f.action("EventFlags", "SetFlag", {{"symbol", check}, {"value", true}}, gift);
    std::string test = f.sw("EventFlags", "CheckFlag", {{"symbol", check}}, {{0, set}, {1, ""}});
    f.insert_after(after, test);
}

void owl_field(SeedBuild &b, Flow &f) {
    static const char *const kOwls[][2] = {
        {"owl-statue-below-D8", "Event20"},   {"owl-statue-pothole", "Event22"},  {"owl-statue-above-cave", "Event28"},
        {"owl-statue-moblin-cave", "Event30"}, {"owl-statue-south-bay", "Event32"}, {"owl-statue-desert", "Event34"},
        {"owl-statue-maze", "Event36"},        {"owl-statue-taltal-east", "Event38"}, {"owl-statue-rapids", "Event40"}};
    for (const auto &o : kOwls) owl_gift(b, f, o[0], o[1]);
    // Link doesn't back up (it sometimes kept the get animation from playing)
    for (const char *ev : {"Event17", "Event41", "Event42", "Event43", "Event44", "Event45", "Event46", "Event47", "Event48"})
        f.event(ev).params.set("keepPersonalSpace", EvflValue::of_bool(false));
}

void owl_dungeon(SeedBuild &b, Flow &f) {
    // Tail Cave's 04B and 05F statues share one event: it gets its own entry point, which both sub flow to; the
    // Color Dungeon's three likewise
    f.add_entry_point("examine_TailShared");
    f.insert_after("examine_TailShared", "Event22");
    f.add_entry_point("examine_Tail04B");
    std::string a = f.subflow("", "examine_TailShared", {});
    f.insert_after("examine_Tail04B", a);
    std::string bb = f.subflow("", "examine_TailShared", {});
    f.insert_after("examine_Tail04B05F", bb);
    f.add_entry_point("examine_Color06C");
    std::string c = f.subflow("", "examine_Color", {});
    f.insert_after("examine_Color06C", c);
    f.add_entry_point("examine_Color07D");
    std::string d = f.subflow("", "examine_Color", {});
    f.insert_after("examine_Color07D", d);
    f.add_entry_point("examine_Color05F");
    std::string e = f.subflow("", "examine_Color", {});
    f.insert_after("examine_Color05F", e);
    const std::pair<const char *, std::string> owls[] = {
        {"D1-owl-statue-spinies", a},
        {"D1-owl-statue-3-of-a-kind", bb},
        {"D1-owl-statue-long-hallway", "Event48"},
        {"D2-owl-statue-first-switch", "Event59"},
        {"D2-owl-statue-push-puzzle", "Event62"},
        {"D2-owl-statue-past-hinox", "Event65"},
        {"D3-owl-statue-basement-north", "Event27"},
        {"D3-owl-statue-arrow", "Event73"},
        {"D3-owl-statue-northwest", "Event76"},
        {"D4-owl-statue", "Event79"},
        {"D5-owl-statue-triple-stalfos", "Event82"},
        {"D5-owl-statue-before-slime-eel", "Event85"},
        {"D6-owl-statue-ledge", "Event88"},
        {"D6-owl-statue-southeast", "Event91"},
        {"D6-owl-statue-canal", "Event94"},
        {"D7-owl-statue-ball", "Event97"},
        {"D7-owl-statue-kirbys", "Event100"},
        {"D7-owl-statue-hookshot-chest", "Event103"},
        {"D8-owl-statue-above-smasher", "Event39"},
        {"D8-owl-statue-below-gibdos", "Event109"},
        {"D8-owl-statue-eye-statue", "Event112"},
        {"D0-owl-statue-nine-switches", c},
        {"D0-owl-statue-first-switches", d},
        {"D0-owl-statue-before-mini-boss", e}};
    for (const auto &o : owls) owl_gift(b, f, o.first, o.second);
    EvflEvent &fork = f.event("Event50");
    if (fork.forks.size() <= 5) throw std::runtime_error("las: Event50 has no sixth fork");
    fork.forks.erase(fork.forks.begin() + 5);
}

void owl_statues(SeedBuild &b) {
    {
        EventFlow fl = b.read_flow("FieldOwlStatue.bfevfl");
        Flow f(b, fl.flowchart);
        slime_key_check(f);
        if (S(b, "owl-overworld-gifts")) owl_field(b, f);
        b.write_flow("FieldOwlStatue.bfevfl", fl);
    }
    if (!S(b, "owl-dungeon-gifts")) return;
    {
        EventFlow fl = b.read_flow("DungeonOwlStatue.bfevfl");
        Flow f(b, fl.flowchart);
        owl_dungeon(b, f);
        b.write_flow("DungeonOwlStatue.bfevfl", fl);
    }
    static const struct { const char *room; size_t actor; const char *entry; } kRooms[] = {
        {"Lv01TailCave_04B.leb", 0, "examine_Tail04B"},
        {"Lv10ClothesDungeon_06C.leb", 9, "examine_Color06C"},
        {"Lv10ClothesDungeon_07D.leb", 4, "examine_Color07D"},
        {"Lv10ClothesDungeon_05F.leb", 4, "examine_Color05F"}};
    for (const auto &s : kRooms) {
        LebRoom r = b.read_room(s.room);
        r.actors.at(s.actor).params[0] = str_param(s.entry);
        b.write_room(s.room, r);
    }
}

// ======== makeTelephoneChanges: tunic_swap.writeSwapEvents (the phones swap tunics, as the Fairy Queen)

std::string has_tunic_event(Flow &f, const std::string &first, const std::string &second, const char *message) {
    std::string result = f.sw("Dialog", "GetLastResult4", {}, {{0, first}, {1, second}, {2, ""}});
    return f.action("Telephone", "Examine", {{"message", message}}, result);
}

void telephone(SeedBuild &b) {
    if (S(b, "tunic-upgrades")) return;  // (LAS Recomp: both tunics stay owned; the phones keep Ulrira's call)
    EventFlow fl = b.read_flow("Telephone.bfevfl");
    Flow f(b, fl.flowchart);
    f.fc.actors[f.actor("Dialog")].add_query("GetLastResult4");
    std::string green = insert_item_get(f, "ClothesGreen", -1);
    std::string red = insert_item_get(f, "ClothesRed", -1);
    std::string check_red = f.sw("EventFlags", "CheckFlag", {{"symbol", RED_TUNIC_FOUND_FLAG}}, {{0, ""}, {1, red}});
    std::string blue = insert_item_get(f, "ClothesBlue", -1);
    std::string check_blue = f.sw("EventFlags", "CheckFlag", {{"symbol", BLUE_TUNIC_FOUND_FLAG}}, {{0, ""}, {1, blue}});
    std::string red_blue = has_tunic_event(f, check_red, check_blue, "SubEvent:QuestGrandFairy1_2");
    std::string blue_green = has_tunic_event(f, check_blue, green, "SubEvent:QuestGrandFairy1_4");
    std::string red_green = has_tunic_event(f, check_red, green, "SubEvent:QuestGrandFairy1_3");
    std::string tunic_blue = f.sw("Inventory", "HasItem", {{"count", 1}, {"itemType", 20}}, {{0, red_blue}, {1, red_green}});
    std::string tunic_red = f.sw("Inventory", "HasItem", {{"count", 1}, {"itemType", 19}}, {{0, tunic_blue}, {1, blue_green}});
    std::string greeting = f.action("Telephone", "Examine", {{"message", "SubEvent:QuestGrandFairy7"}}, tunic_red);
    f.insert_after("Telephone", greeting);
    EvflEvent &fork = f.event("Event231");
    if (fork.forks.size() <= 2) throw std::runtime_error("las: Event231 has no third fork");
    fork.forks.erase(fork.forks.begin() + 2);  // (the get-hints branch)
    std::string sub = f.subflow("", "Telephone", {}, "Event113");
    f.insert_after("Event98", sub);
    b.write_flow("Telephone.bfevfl", fl);
}

// ======== makeLv10RupeeChanges: the Color Dungeon's 28 blue rupees

void lv10_rupees(SeedBuild &b) {
    if (!S(b, "blupsanity")) return;
    EventFlow fl = b.read_flow("SinkingSword.bfevfl");
    Flow f(b, fl.flowchart);
    LebRoom r = b.read_room("Lv10ClothesDungeon_08D.leb");
    for (int i = 0; i < 28; i++) {
        const std::string n = std::to_string(i + 1), entry = "Lv10Rupee_" + n;
        SeedBuild::ItemInfo it = b.item_info("D0-rupee-" + n, true, true);
        set_rupee_params(r, it.model_path, it.model_name, entry, it.key, i);
        // rupees.makeEventChanges
        f.add_entry_point(entry);
        std::string get;
        if (plain_add(it.key)) get = f.action("Inventory", "AddItemByKey", add_params(it.key, it.index));
        else get = insert_item_get(f, it.key, it.index);
        f.action_chain(entry, {{"SinkingSword", "Destroy", {}},
                               {"EventFlags", "SetFlag", {{"symbol", i == 0 ? "Lv10RupeeGet" : "Lv10RupeeGet_" + n}, {"value", true}}}},
                       get);
    }
    b.write_room("Lv10ClothesDungeon_08D.leb", r);
    b.write_flow("SinkingSword.bfevfl", fl);
}

// ======== shuffleDungeons / shuffleDungeonIcons

void shuffle_dungeons(SeedBuild &b) {
    if (!S(b, "shuffle-dungeons")) return;
    for (const DungeonEntrance &v : DUNGEON_ENTRANCES) {
        {  // in
            const std::string file = std::string(v.field_room) + ".leb";
            LebRoom r = b.read_room(file);
            const DungeonEntrance &d = entrance(dungeon_at(b, v.name));
            set_loading_zone_target(r, std::string(d.room) + d.suffix, (size_t)v.x4);
            b.write_room(file, r);
        }
        {  // out
            const std::string file = std::string(v.room) + ".leb";
            LebRoom r = b.read_room(file);
            const DungeonEntrance &d = entrance(door_of(b, v.name));
            set_loading_zone_target(r, std::string(d.field_room) + d.field_suffix, 0);
            b.write_room(file, r);
        }
    }
}

void shuffle_dungeon_icons(SeedBuild &b) {
    if (!S(b, "shuffle-dungeons")) return;
    GSheet sheet = b.read_sheet("UiFieldMapIcons.gsheet");
    for (GStruct &icon : sheet.values) {
        const std::string label = at(icon, "mNameLabel").s;
        const DungeonMapIcon *was = nullptr;
        for (const DungeonMapIcon &m : DUNGEON_MAP_ICONS)
            if (label == m.map) { was = &m; break; }
        if (!was) continue;
        const std::string now = dungeon_at(b, was->name);
        const DungeonMapIcon *to = nullptr;
        for (const DungeonMapIcon &m : DUNGEON_MAP_ICONS)
            if (now == m.name) { to = &m; break; }
        if (!to) throw std::runtime_error("las: no map icon for " + now);
        at(icon, "mNameLabel") = gs(to->map);
        at(icon, "mFirstShowFlagName") = gs(to->icon);
    }
    b.write_sheet("UiFieldMapIcons.gsheet", sheet);
}

// ======== changeLevelConfigs: companions in every dungeon but the Egg (they can collide with the Nightmare)

void level_configs(SeedBuild &b) {
    if (!S(b, "bad-pets")) return;
    for (const char *folder : world::LEVEL_FOLDERS) {
        if (strncmp(folder, "Lv", 2) || !strncmp(folder, "Lv09", 4)) continue;
        const std::string file = std::string(folder) + ".lvb";
        LvbLevel l = b.read_level(file);
        l.config.allow_companions = true;
        b.write_level(file, l);
    }
}

// ======== makeMusicChanges / makeEventMusicChanges (the songs: randomizeMusic's draws, recorded)

void music(SeedBuild &b) {
    if (!S(b, "randomize-music")) return;
    yyjson_val *songs = b.decisions().raw("songs");
    for (const char *folder : world::LEVEL_FOLDERS) {
        const std::string file = std::string(folder) + ".lvb";
        LvbLevel l = b.read_level(file);
        for (LvbZone &z : l.zones)
            if (const char *s = yyjson_get_str(yyjson_obj_get(songs, z.bgm.c_str()))) z.bgm = s;
        b.write_level(file, l);
    }
}

void event_music(SeedBuild &b) {
    if (!S(b, "randomize-music")) return;
    struct Song { const char *event, *bgm; };
    struct File { const char *flow; std::vector<Song> songs; };
    const char *const MID = "BGM_DUNGEON_BOSS_MIDDLE", *const BOSS = "BGM_DUNGEON_BOSS";
    const File files[] = {
        {"Bossblin.bfevfl", {{"Event64", MID}, {"Event68", MID}}},
        {"BossBlob.bfevfl", {{"Event6", MID}, {"Event19", MID}, {"Event12", MID}}},
        {"Dodongo.bfevfl", {{"Event5", MID}, {"Event43", MID}, {"Event3", MID}}},
        {"DonPawn.bfevfl", {{"Event21", BOSS}, {"Event30", "BGM_PANEL_RESULT"}, {"Event38", BOSS}, {"Event6", BOSS}}},
        {"Gohma.bfevfl", {{"Event0", MID}, {"Event1", MID}}},
        {"Hinox.bfevfl", {{"Event37", MID}, {"Event55", MID}, {"Event1", MID}}},
        {"HiploopHover.bfevfl", {{"Event38", MID}, {"Event7", MID}}},
        {"Jacky.bfevfl", {{"Event37", MID}, {"Event6", MID}}},
        {"MightPunch.bfevfl", {{"Event56", MID}, {"Event6", MID}}},
        {"PiccoloMaster.bfevfl", {{"Event48", MID}, {"Event53", MID}, {"Event3", MID}}},
        {"Rola.bfevfl", {{"Event20", MID}, {"Event1", MID}}},
        {"Shadow.bfevfl", {{"Event37", "BGM_LASTBOSS_WIN"}, {"Event60", "BGM_LASTBOSS_BATTLE"}, {"Event71", "BGM_LASTBOSS_BATTLE"}}},
        {"StoneHinox.bfevfl", {{"Event4", MID}, {"Event35", MID}, {"Event29", MID}}},
        {"ToolShopkeeper.bfevfl", {{"Event87", BOSS}}},
        {"TurtleRock.bfevfl", {{"Event1", "BGM_DUNGEON_LV8_ENT_BATTLE"}, {"Event26", "BGM_DUNGEON_LV8_ENT_BATTLE"}, {"Event11", "BGM_DUNGEON_LV8_ENT_BATTLE"}}},
        {"WindFish.bfevfl", {{"Event73", "BGM_DEMO_AFTER_LASTBOSS"}, {"Event74", "BGM_DEMO_AFTER_LASTBOSS"}, {"Event93", "BGM_LASTBOSS_WIN"}}},
    };
    for (const File &file : files) {
        EventFlow fl = b.read_flow(file.flow);
        Flow f(b, fl.flowchart);
        for (const Song &s : file.songs) f.set_song(s.event, song(b.decisions(), s.bgm));
        b.write_flow(file.flow, fl);
    }
}

// ======== openMabe: the grass, monsters and rocks in the way out of Mabe Village go

void open_mabe(SeedBuild &b) {
    if (!S(b, "open-mabe")) return;
    static const struct { const char *room; std::vector<uint64_t> keys; } kRooms[] = {
        {"Field_10A", {0x624A97005CD29205ull}},
        {"Field_10E", {0x62000A005D15AC9Eull, 0x620015005D15AC9Eull}},
        {"Field_15B", {0x7200BB005CFF3740ull, 0x7200B9005CFF3740ull}},
        {"Field_15C", {0x7200DC005CFF3741ull, 0x7200D6005CFF3741ull}}};
    for (const auto &s : kRooms) {
        const std::string file = std::string(s.room) + ".leb";
        LebRoom r = b.read_room(file);
        std::vector<size_t> drop;
        for (size_t i = 0; i < r.actors.size(); i++)
            for (uint64_t k : s.keys)
                if (r.actors[i].key == k) { drop.push_back(i); break; }
        remove_actors(r, drop);
        b.write_room(file, r);
    }
}

// ======== fixWaterLoadingZones: the water loading zones wait for the flippers (the rooster could softlock there)

void water_zones(SeedBuild &b) {
    for (const auto &z : WATER_LOADING_ZONES) {
        const std::string file = std::string(z.first) + ".leb";
        LebRoom r = b.read_room(file);
        for (int i : z.second) r.actors.at((size_t)i).switches[0] = {1, (uint16_t)global_flag("FlippersFound")};
        b.write_room(file, r);
    }
}

// ======== fixRapidsRespawn: no reload on the rapids' water tiles (after the race without flippers: an endless fall)

void rapids_respawn(SeedBuild &b) {
    for (const char *room : {"Field_09N", "Field_09O", "Field_09P", "Field_10P"}) {
        const std::string file = std::string(room) + ".leb", path = SeedBuild::path_of(file);
        std::string bytes;
        if (!b.read_bytes(path, bytes)) throw std::runtime_error("las: no file " + path);
        LebRoom r = LebRoom::parse(bytes, true);
        for (LebTile &t : r.grid.tiles)
            if (t.flags[2] & 4) t.flags[2] &= (uint8_t)~16;  // (water / lava: no respawn-load)
        b.write_room(file, r);
    }
}

// ======== makeExefsPatches: ASM/Patches as .pchtxt files, assembled (las_world_data.cpp)

// settings[name] == state (assemble.readASM), Stealing's always / never as True / False (assemble.preSetup)
bool cond_met(const Decisions &d, const char *name, bool state) {
    yyjson_val *v = setting_val(d, name);
    if (!v) return false;
    if (!strcmp(name, "stealing") && yyjson_is_str(v)) {
        if (yyjson_equals_str(v, "always")) return state;
        if (yyjson_equals_str(v, "never")) return !state;
        return false;
    }
    if (yyjson_is_bool(v)) return yyjson_get_bool(v) == state;
    if (yyjson_is_num(v)) return yyjson_get_num(v) == (state ? 1.0 : 0.0);
    return false;
}

std::string pchtxt(const Decisions &d, const char *build_id, int chest_enemy) {
    std::string out = std::string("@nsobid-") + build_id + "\n# Zelda: Link's Awakening [01006BB00C6F0000] " +
                      (build_id[0] == 'A' ? "1.0.0" : "1.0.1") + "\n\n@flag offset_shift 0x100\n";
    for (const world::AsmPatch &p : world::ASM_PATCHES) {
        if (p.setting && !cond_met(d, p.setting, p.state)) continue;
        const char *text = p.text;
        if (!text) {
            if (chest_enemy < 0) throw std::runtime_error("las: no chest enemy recorded (tools/rando/record_las_world.py)");
            for (const auto &v : *p.variants)
                if (v.first == chest_enemy) text = v.second;
            if (!text) throw std::runtime_error("las: chest enemy " + std::to_string(chest_enemy) + " isn't one of LAS's");
        }
        if (*p.comment) out += std::string("\n") + p.comment + "\n@enabled\n";
        char addr[16];
        snprintf(addr, sizeof addr, "%08X ", p.address);
        out += addr;
        out += text;
        out += "\n";
    }
    return out;
}

void exefs_patches(SeedBuild &b) {
    const Decisions &d = b.decisions();
    if (d.setting_str("platform") == "console")  // (LAS Recomp's seeds are always the emulator layout)
        throw std::runtime_error("las: console patches (.ips) aren't made here");
    if (Decider *dc = b.decider())
        if (S(b, "randomize-enemies"))  // (assemble.preSetup draws it from the shared stream as it is now)
            dc->set("chest-enemy", std::to_string(draw_chest_enemy(dc->shared())));
    int chest = -1;
    if (yyjson_val *c = d.raw("chest-enemy")) chest = (int)yyjson_get_sint(c);
    b.write_bytes("exefs/1.0.0.pchtxt", pchtxt(d, "AE16F71E002AF8CB059A9A74C4D90F34BA984892", chest));
    b.write_bytes("exefs/1.0.1.pchtxt", pchtxt(d, "909E904AF78AC1B8DEEFE97AB2CCDB51968F0EC7", chest));
}

}  // namespace
}  // namespace las

std::vector<BuildStep> steps_las_world() {
    using namespace las;
    return {
        {"mod_generator.makeSmallKeyChanges", small_keys},
        {"mod_generator.makeGoldenLeafChanges", golden_leaves},
        {"mod_generator.makeHeartPieceChanges", heart_pieces},
        {"mod_generator.makeInstrumentChanges", instruments},
        {"mod_generator.makeOwlStatueChanges", owl_statues},
        {"mod_generator.makeTelephoneChanges", telephone},
        // (makeGeneralARCChanges runs here: stage 4)
        {"mod_generator.makeLv10RupeeChanges", lv10_rupees},
        {"mod_generator.shuffleDungeons", shuffle_dungeons},
        {"mod_generator.shuffleDungeonIcons", shuffle_dungeon_icons},
        {"mod_generator.changeLevelConfigs", level_configs},
        {"mod_generator.makeMusicChanges", music},
        {"mod_generator.makeEventMusicChanges", event_music},
        {"mod_generator.randomizeEnemies", randomize_enemies},  // (las_enemies.cpp)
        {"mod_generator.openMabe", open_mabe},
        {"mod_generator.fixWaterLoadingZones", water_zones},
        {"mod_generator.fixRapidsRespawn", rapids_respawn},
        {"mod_generator.makeExefsPatches", exefs_patches},
        {"roam.write", write_roam},  // (las_enemies.cpp: tables only)
        {"enemy_random.write", write_enemy_random},
    };
}

}  // namespace rando
