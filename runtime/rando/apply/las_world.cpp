// LAS-Randomizer's world edits: small keys, golden leaves, heart pieces, instruments, owls, the telephone, Color
// Dungeon's blue rupees, dungeon entrances and their map icons, level configs, music, enemies, open Mabe, water zones,
// the rapids, the code patches: the generator's steps ported (seed_build.h); each makes exactly the writes its Python
// does (lasrando check-steps, tools/rando/test_apply.py). Ported from LAS-Randomizer's RandomizerCore/mod_generator.py
// and Randomizers/{small_keys, golden_leaves, heart_pieces, instruments, owls, tunic_swap, rupees, enemies}.py, with
// LAS Recomp's additions there (mini-boss kits and rails, remove_actors, dungeon exits) (MIT, Owen-Splat and
// contributors).
//
// The generator's random draws come from the decisions (tools/rando/record_las_world.py): the trap models, the songs,
// what randomizeEnemies did to each room's actors ("enemies") and makeExefsPatches' chest enemy ("chest-enemy").
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
#include <string.h>

#include <map>
#include <stdexcept>

#include "../../../third_party/yyjson/yyjson.h"
#include "item_get.h"
#include "las_data.h"
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
        SeedBuild::ItemInfo it = b.item_info(room, rooms);
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
        SeedBuild::ItemInfo it = b.item_info(room, true);
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
        SeedBuild::ItemInfo it = b.item_info("D0-rupee-" + n, true);
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

// ======== randomizeEnemies (what its draws did: the decisions' "enemies", tools/rando/record_las_world.py)

// LAS Recomp's mini-boss kits (enemies.MINIBOSS_KITS): the arena room and the actor types copied from it
struct Kit { uint16_t type; const char *arena; std::vector<uint16_t> with; };
const std::vector<Kit> &kits() {
    static const std::vector<Kit> k = {
        {0x5C, "Lv06FaceShrine_05F", {0x1E4}},  // Smasher's iron ball
        {0x51, "Lv01TailCave_06G", {0x11A}},    // Rolling Bones' spike bar
        {0x58, "Lv04AnglersTunnel_03E", {}},    // Cue Ball (its rail)
        {0x57, "Field_13O", {}},                // Lanmola (his rail)
    };
    return k;
}
const uint8_t KIT_KEY_TAG = 0x6B;  // (copied kit actors get keys of their own)
const uint16_t RAIL_FIT = 0x58;    // (Cue Ball circles a 4-point rail fitted to the room)

uint32_t crc32(const std::string &s) {  // zlib.crc32
    uint32_t c = 0xFFFFFFFFu;
    for (unsigned char ch : s) {
        c ^= ch;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

uint16_t get_u16(const Bytes &d, size_t at) { return (uint16_t)read_le(d, at, 2); }
void set_u16(Bytes &d, size_t at, uint16_t v) { d[at] = (char)(v & 0xFF), d[at + 1] = (char)(v >> 8); }
void set_u32(Bytes &d, size_t at, uint32_t v) {
    for (int i = 0; i < 4; i++) d[at + i] = (char)((v >> (8 * i)) & 0xFF);
}
void set_f32(Bytes &d, size_t at, double v) {  // struct.pack_into('<f')
    float f = (float)v;
    uint32_t u;
    memcpy(&u, &f, 4);
    set_u32(d, at, u);
}

FixedHash &section(LebRoom &r, const char *name) {
    FhEntry *e = r.fh.find(name);
    if (!e || !e->sub) throw std::runtime_error(std::string("las: room has no ") + name + " section");
    return *e->sub;
}

// enemies.fit_rail: the 4-point rail of the actor becomes the largest rectangle of free tiles with its tile as a
// corner; the actor moves onto that corner
void fit_rail(LebRoom &room, size_t mi, FixedHash &points, FhEntry &rail) {
    const FhEntry *grid = room.fh.find("grid");
    if (!grid || !grid->sub) return;
    const FhEntry *data = nullptr, *info = nullptr;
    for (const FhEntry &e : grid->sub->entries) {
        if (e.name == "data") data = &e;
        else if (e.name == "info") info = &e;
    }
    if (!info || !data) throw std::runtime_error("las: grid without data / info");
    const int height = (int)read_le(info->data, 0, 2), width = (int)read_le(info->data, 2, 2);
    const double ts = read_f32(info->data, 4), gx = read_f32(info->data, 8), gz = read_f32(info->data, 0xC);
    if (height != 8) return;  // (a side view: 2, or not a grid the Python reads)
    LebActor &act = room.actors[mi];
    auto free = [&](int row, int col) {
        if (!(0 <= row && row < height && 0 <= col && col < width)) return false;
        size_t t = 0x10 * (size_t)(row * width + col);
        uint8_t f1 = (uint8_t)read_le(data->data, t, 1), f3 = (uint8_t)read_le(data->data, t + 2, 1);
        return !((f1 & 2) || (f1 & 1) || (f3 & 4));  // (collision, deep water / lava, water / lava)
    };
    const int c0 = (int)((act.pos[0] - gx) / ts), r0 = (int)((act.pos[2] - gz) / ts);
    int best = -1, bw = 0, bh = 0, bdx = 0, bdz = 0;
    for (int w = 7; w > 1; w--)
        for (int h = 5; h > 1; h--)
            for (const auto &dd : {std::make_pair(1, 1), std::make_pair(1, -1), std::make_pair(-1, 1), std::make_pair(-1, -1)}) {
                const int dx = dd.first, dz = dd.second;
                const int cmin = std::min(c0, c0 + dx * w), cmax = std::max(c0, c0 + dx * w);
                const int rmin = std::min(r0, r0 + dz * h), rmax = std::max(r0, r0 + dz * h);
                bool ok = true;
                for (int cc = cmin; cc <= cmax && ok; cc++)
                    ok = free(r0, cc) && free(r0 + dz * h, cc);
                for (int rr = rmin; rr <= rmax && ok; rr++)
                    ok = free(rr, c0) && free(rr, c0 + dx * w);
                if (ok && (best < 0 || w * h > best)) best = w * h, bw = w, bh = h, bdx = dx, bdz = dz;
            }
    if (best < 0) return;
    auto ctr = [&](int rr, int cc, double &x, double &z) {
        volatile double px = (cc + 0.5) * ts, pz = (rr + 0.5) * ts;  // (kept apart from the add: the Python rounds twice)
        x = gx + px, z = gz + pz;
    };
    double cx[4], cz[4];
    ctr(r0, c0, cx[0], cz[0]);
    ctr(r0 + bdz * bh, c0, cx[1], cz[1]);
    ctr(r0 + bdz * bh, c0 + bdx * bw, cx[2], cz[2]);
    ctr(r0, c0 + bdx * bw, cx[3], cz[3]);
    const size_t n = std::min<size_t>(4, get_u16(rail.data, 0x2C));
    for (size_t k = 0; k < n; k++) {
        size_t pi = get_u16(rail.data, 0x30 + 2 * k);
        Bytes &pd = points.entries.at(pi).data;
        set_f32(pd, 0, cx[k]), set_f32(pd, 4, act.pos[1] + 0.2), set_f32(pd, 8, cz[k]);
    }
    act.pos[0] = cx[0], act.pos[2] = cz[0];
}

// enemies.add_miniboss_kit: the kit actors of room.actors[mi]'s mini-boss copied from its arena (same offsets, linked
// as there), every kit actor's rails copied with their points
void add_miniboss_kit(LebRoom &room, size_t mi, const Kit &kit, LebRoom &arena, const std::string &room_name) {
    const uint16_t type = room.actors.at(mi).type;
    size_t src_i = arena.actors.size();
    for (size_t i = 0; i < arena.actors.size(); i++)
        if (arena.actors[i].type == type) { src_i = i; break; }
    if (src_i == arena.actors.size()) throw std::runtime_error("las: no mini-boss in its arena " + std::string(kit.arena));
    const LebActor src = arena.actors[src_i];
    std::vector<size_t> take = {src_i};
    for (size_t i = 0; i < arena.actors.size(); i++)
        for (uint16_t t : kit.with)
            if (arena.actors[i].type == t) { take.push_back(i); break; }
    std::map<size_t, size_t> newidx = {{src_i, mi}};
    const double ax = room.actors[mi].pos[0], ay = room.actors[mi].pos[1], az = room.actors[mi].pos[2];
    const uint32_t room_id = room.actors[mi].room_id;
    const uint64_t crc = crc32(room_name) & 0xFFFFFF;
    for (size_t t = 1; t < take.size(); t++) {
        LebActor a = arena.actors[take[t]];
        a.key = ((uint64_t)KIT_KEY_TAG << 56) | (crc << 32) | ((uint64_t)(room.actors.size() & 0xFFFF) << 16) | (a.key & 0xFFFF);
        char hex[18];
        snprintf(hex, sizeof hex, "-%016llX", (unsigned long long)a.key);
        a.name = a.name.substr(0, a.name.find('-')) + hex;
        a.pos[0] = ax + (a.pos[0] - src.pos[0]), a.pos[1] = ay + (a.pos[1] - src.pos[1]), a.pos[2] = az + (a.pos[2] - src.pos[2]);
        a.room_id = room_id;
        room.actors.push_back(a);
        newidx[take[t]] = room.actors.size() - 1;
    }
    FixedHash &sp = section(arena, "point"), &sr = section(arena, "rail");
    FixedHash &dp = section(room, "point"), &dr = section(room, "rail");
    std::map<uint32_t, uint32_t> rail_map, point_map;
    for (size_t i : take) {
        const LebLinks &srl = arena.actors[i].links;
        LebLinks &rl = room.actors[newidx[i]].links;
        std::vector<LebLinks::ActorLink> s1;
        for (const auto &s : srl.actors)
            if (newidx.count(s.actor)) { auto n = s; n.actor = (uint32_t)newidx[s.actor]; s1.push_back(n); }
        std::vector<uint32_t> s3;
        for (uint32_t j : srl.ids)
            if (newidx.count(j)) s3.push_back((uint32_t)newidx[j]);
        rl.actors = s1, rl.ids = s3;
        std::vector<LebLinks::RailLink> rails;
        for (const auto &s2 : srl.rails) {
            const uint32_t ri = s2.rail;
            if (!rail_map.count(ri)) {
                FhEntry rail = sr.entries.at(ri);
                Bytes &d = rail.data;
                for (size_t k = 0, n = get_u16(d, 0x2C); k < n; k++) {
                    const uint16_t pi = get_u16(d, 0x30 + 2 * k);
                    if (!point_map.count(pi)) {
                        FhEntry pt = sp.entries.at(pi);
                        const double x = read_f32(pt.data, 0), y = read_f32(pt.data, 4), z = read_f32(pt.data, 8);
                        set_f32(pt.data, 0, ax + (x - src.pos[0]));
                        set_f32(pt.data, 4, ay + (y - src.pos[1]));
                        set_f32(pt.data, 8, az + (z - src.pos[2]));
                        dp.entries.push_back(pt);
                        point_map[pi] = (uint32_t)dp.entries.size() - 1;
                    }
                    set_u16(d, 0x30 + 2 * k, (uint16_t)point_map[pi]);
                }
                for (size_t k = 0; k < 4; k++)  // (string params point into the names, which repack rebuilds: ints)
                    set_u32(d, 0xC + 8 * k, 0), set_u32(d, 0x10 + 8 * k, 3);
                dr.entries.push_back(rail);
                rail_map[ri] = (uint32_t)dr.entries.size() - 1;
            }
            LebLinks::RailLink n = s2;
            n.rail = rail_map[ri];
            if (point_map.count(s2.point)) n.point = point_map[s2.point];
            rails.push_back(n);
        }
        rl.rails = rails;
        rl.x = (uint8_t)rl.actors.size(), rl.y = (uint8_t)rl.ids.size(), rl.z = (uint8_t)rl.rails.size();
    }
    LebActor &act = room.actors[mi];
    if (act.type == RAIL_FIT && !act.links.rails.empty())
        fit_rail(room, mi, section(room, "point"), section(room, "rail").entries.at(act.links.rails[0].rail));
}

LebParam json_param(yyjson_val *v) {
    if (yyjson_is_str(v)) return str_param(yyjson_get_str(v));
    if (yyjson_is_real(v)) return LebParam::of_float(yyjson_get_real(v));
    if (yyjson_is_int(v)) return LebParam::of_int((uint32_t)yyjson_get_sint(v));
    throw std::runtime_error("las: bad enemy parameter in the decisions");
}

void enemies(SeedBuild &b) {
    if (!S(b, "randomize-enemies") && !S(b, "randomize-enemy-sizes")) return;
    yyjson_val *rooms = b.decisions().raw("enemies"), *entry;
    if (!yyjson_is_arr(rooms)) throw std::runtime_error("las: no enemies recorded (tools/rando/record_las_world.py)");
    yyjson_arr_iter it = yyjson_arr_iter_with(rooms);
    std::map<uint16_t, LebRoom> arenas;
    while ((entry = yyjson_arr_iter_next(&it))) {
        const char *name = yyjson_get_str(yyjson_arr_get(entry, 0));
        if (!name) throw std::runtime_error("las: bad enemies entry");
        const std::string room = name, file = room + ".leb";
        LebRoom r = b.read_room(file);
        yyjson_val *acts = yyjson_arr_get(entry, 1), *a;
        yyjson_arr_iter ai = yyjson_arr_iter_with(acts);
        while ((a = yyjson_arr_iter_next(&ai))) {
            const size_t e = (size_t)yyjson_get_uint(yyjson_obj_get(a, "e"));
            LebActor &act = r.actors.at(e);
            if (yyjson_obj_get(a, "sw")) act.switches[0] = {4, 0};  // (a stair guard: the stairs stay open)
            if (yyjson_val *t = yyjson_obj_get(a, "t")) {
                act.type = (uint16_t)yyjson_get_uint(t);
                yyjson_val *p = yyjson_obj_get(a, "p");
                if (yyjson_arr_size(p) != 8) throw std::runtime_error("las: enemy parameters aren't 8 in " + room);
                for (size_t i = 0; i < 8; i++) act.params[i] = json_param(yyjson_arr_get(p, i));
                act.links.e = (uint8_t)yyjson_get_uint(yyjson_obj_get(a, "re"));
                act.rot[1] = 0;  // (facing the screen)
                act.scale[0] = act.scale[1] = act.scale[2] = 1.0;
            }
            if (yyjson_obj_get(a, "m")) {
                const Kit *kit = nullptr;
                for (const Kit &k : kits())
                    if (k.type == act.type) kit = &k;
                if (kit) {
                    auto ar = arenas.find(kit->type);
                    if (ar == arenas.end()) {  // (the game's own room, whatever this seed made of it)
                        std::string bytes;
                        if (!b.read_game(SeedBuild::path_of(std::string(kit->arena) + ".leb"), bytes))
                            throw std::runtime_error(std::string("las: no arena room ") + kit->arena);
                        ar = arenas.emplace(kit->type, LebRoom::parse(bytes)).first;
                    }
                    add_miniboss_kit(r, e, *kit, ar->second, room);
                }
            }
            if (yyjson_val *s = yyjson_obj_get(a, "s")) {
                LebActor &ac = r.actors.at(e);  // (the kit may have moved the list)
                ac.scale[0] = ac.scale[1] = ac.scale[2] = yyjson_get_num(s);
            }
        }
        b.write_room(file, r);
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
        {"mod_generator.randomizeEnemies", enemies},
        {"mod_generator.openMabe", open_mabe},
        {"mod_generator.fixWaterLoadingZones", water_zones},
        {"mod_generator.fixRapidsRespawn", rapids_respawn},
        {"mod_generator.makeExefsPatches", exefs_patches},
    };
}

}  // namespace rando
