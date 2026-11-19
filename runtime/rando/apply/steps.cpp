// The ported steps, in the generator's order (a full build runs them so; lasrando check-steps runs each on its own).
#include <string.h>

#include <algorithm>
#include <string>

#include "steps.h"
#include "textures.h"
#include "../../../third_party/yyjson/yyjson.h"

namespace rando {

// The generator's order: mod_generator.run(), then tools/rando/las_headless.py main() after it. A step registered
// as "a|b" or "p>a|b" sorts by its first name after any ">". Some steps write only the seed's tables
// (SeedBuild::table_write: souls.write_spots, roam.write, quick_pickups.apply, ...) and some only decide
// (randomizeMusic): lasrando check-steps finds no writes of theirs in the log.
static const char *const kOrder[] = {
    "mod_generator.randomizeMusic",  // (decide mode: the songs; ModsProcess.run draws them first)
    "mod_generator.makeGeneralLEBChanges", "mod_generator.makeGeneralDatasheetChanges",
    "mod_generator.makeGeneralEventChanges", "mod_generator.makeChestContentFixes", "mod_generator.tarinChanges",
    "mod_generator.sinkingSwordChanges", "mod_generator.walrusChanges", "mod_generator.christineChanges",
    "mod_generator.invisibleZoraChanges", "mod_generator.marinChanges", "mod_generator.ghostRewardChanges",
    "mod_generator.clothesFairyChanges", "mod_generator.goriyaChanges", "mod_generator.manboChanges",
    "mod_generator.mamuChanges", "mod_generator.rapidsChanges", "mod_generator.fishingChanges",
    "mod_generator.trendyChanges", "mod_generator.seashellMansionChanges", "mod_generator.madBatterChanges",
    "mod_generator.dampeChanges", "mod_generator.moldormChanges", "mod_generator.genieChanges",
    "mod_generator.slimeEyeChanges", "mod_generator.anglerChanges", "mod_generator.slimeEelChanges",
    "mod_generator.facadeChanges", "mod_generator.eagleChanges", "mod_generator.hotheadChanges",
    "mod_generator.lanmolaChanges", "mod_generator.armosKnightChanges", "mod_generator.masterStalfosChanges",
    "mod_generator.syrupChanges", "mod_generator.makeTradeQuestChanges", "mod_generator.makeSmallKeyChanges",
    "mod_generator.makeGoldenLeafChanges", "mod_generator.makeHeartPieceChanges",
    "mod_generator.makeInstrumentChanges", "mod_generator.makeOwlStatueChanges",
    "mod_generator.makeTelephoneChanges", "mod_generator.makeGeneralARCChanges",
    "mod_generator.makeLv10RupeeChanges", "mod_generator.shuffleDungeons", "mod_generator.shuffleDungeonIcons",
    "mod_generator.changeLevelConfigs", "mod_generator.makeMusicChanges", "mod_generator.makeEventMusicChanges",
    "mod_generator.randomizeEnemies", "mod_generator.openMabe", "mod_generator.fixWaterLoadingZones",
    "mod_generator.fixRapidsRespawn", "mod_generator.makeExefsPatches",
    // las_headless.py main(), after mod_generator
    "shopsanity.apply", "trendsanity.apply", "pets.apply", "souls.apply", "triforce.apply", "pots.apply",
    "las_headless.write_room", "entrance_logic.raft_shop_to_dock", "dungeon_gen.write_all", "escape_bottle.apply",
    "hint_sheet.apply", "seed_hints._owl_flows", "souls.write_spots", "triforce.write_spots", "digs.apply",
    "underwater.apply", "seashell_spots.apply", "apple_trees.apply", "keysanity_text.apply", "roam.write",
    "miniboss_guard.apply", "enemy_random.write", "refill_gets.apply", "bomb_bag_text.apply",
    "powder_bag_text.apply", "silent_pickups._flow", "chest_looks.write_models", "chest_looks.sweep",
    "fast_egg.apply", "quick_pickups.apply", "seed_tables.write_flags", "environments.apply",
};

static size_t order_of(const char *name) {
    std::string n = name;
    size_t gt = n.find('>');
    if (gt != std::string::npos) n = n.substr(gt + 1);
    n = n.substr(0, n.find('|'));
    for (size_t i = 0; i < sizeof kOrder / sizeof *kOrder; i++)
        if (n == kOrder[i]) return i;
    return (size_t)-1;  // (unknown: last; lasrando dump reports it)
}

const std::vector<BuildStep> &build_steps() {
    static const std::vector<BuildStep> all = [] {
        std::vector<BuildStep> v;
        for (auto group : {las::steps_las_decide, steps_las_general, steps_las_items, steps_las_world, steps_ours_items, steps_ours_world, steps_textures})
            for (const BuildStep &s : group()) v.push_back(s);
        std::stable_sort(v.begin(), v.end(), [](const BuildStep &a, const BuildStep &b) { return order_of(a.name) < order_of(b.name); });
        return v;
    }();
    return all;
}

bool build_step_known(const char *name) { return order_of(name) != (size_t)-1; }

// Steps las_headless.py runs only under a setting (its `if settings.get(...)` lines); the others decide themselves
// (or always run). A full build asks this before each step; lasrando check-steps doesn't (it runs a step where the
// Python's write log shows it).
static bool truthy(const Decisions &d, const char *key) {
    yyjson_val *v = yyjson_obj_get(d.raw("settings"), key);
    if (!v || yyjson_is_null(v)) return false;
    if (yyjson_is_bool(v)) return yyjson_get_bool(v);
    if (yyjson_is_num(v)) return yyjson_get_num(v) != 0;
    if (yyjson_is_str(v)) return yyjson_get_len(v) != 0;
    if (yyjson_is_arr(v)) return yyjson_arr_size(v) != 0;
    if (yyjson_is_obj(v)) return yyjson_obj_size(v) != 0;
    return true;
}

bool build_step_runs(const char *name, const Decisions &d) {
    std::string n = name;
    size_t gt = n.find('>');
    if (gt != std::string::npos) n = n.substr(gt + 1);
    n = n.substr(0, n.find('|'));
    if (n == "shopsanity.apply") {  // (shopsanity.enabled)
        yyjson_val *v = yyjson_obj_get(d.raw("settings"), "shopsanity");
        const char *s = yyjson_get_str(v);
        return s && (!strcmp(s, "vanilla") || !strcmp(s, "random"));
    }
    if (n == "trendsanity.apply") return truthy(d, "trendsanity");
    if (n == "seed_hints._owl_flows") return truthy(d, "hints");
    if (n == "digs.apply") return truthy(d, "digs");
    if (n == "apple_trees.apply") return truthy(d, "apples");
    if (n == "miniboss_guard.apply") return truthy(d, "enemy-minibosses");
    if (n == "refill_gets.apply")  // (pot / bush / dig / rock checks in the seed)
        return truthy(d, "pots") || truthy(d, "bushes") || truthy(d, "digs") || truthy(d, "rocks");
    if (n == "chest_looks.write_models" || n == "chest_looks.sweep") return truthy(d, "chest-looks");
    if (n == "fast_egg.apply") return truthy(d, "fast-egg");
    if (n == "environments.apply") return truthy(d, "randomize-environments");
    return true;
}

}  // namespace rando
