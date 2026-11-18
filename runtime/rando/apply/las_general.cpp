// LAS-Randomizer's general edits (RandomizerCore/mod_generator.py, MIT, Owen-Splat and contributors), ported step by
// step; each step makes exactly the writes its Python does (tools/rando/test_apply.py).
#include <stdexcept>

#include "steps.h"

namespace rando {

// LAS Recomp's enemies.remove_actors: drop actors (and the mini-boss kit actors that came with them: their key's top
// byte is KIT_KEY_TAG and they link back to their mini-boss) and renumber every actor's links
static const uint8_t KIT_KEY_TAG = 0x6B;

void remove_actors(LebRoom &room, std::vector<size_t> indices) {
    std::vector<bool> gone(room.actors.size(), false);
    for (size_t i : indices)
        if (i < gone.size()) gone[i] = true;
    for (size_t i = 0; i < room.actors.size(); i++) {
        const LebActor &a = room.actors[i];
        if ((uint8_t)(a.key >> 56) != KIT_KEY_TAG) continue;
        for (uint32_t j : a.links.ids)
            if (j < gone.size() && gone[j]) { gone[i] = true; break; }
    }
    std::vector<long> renum(room.actors.size(), -1);
    std::vector<LebActor> keep;
    for (size_t i = 0; i < room.actors.size(); i++)
        if (!gone[i]) renum[i] = (long)keep.size(), keep.push_back(room.actors[i]);
    room.actors = std::move(keep);
    for (LebActor &a : room.actors) {
        LebLinks &l = a.links;
        std::vector<LebLinks::ActorLink> s1;
        for (auto &k : l.actors)
            if (k.actor < renum.size() && renum[k.actor] >= 0) { auto n = k; n.actor = (uint32_t)renum[k.actor]; s1.push_back(n); }
        std::vector<uint32_t> s3;
        for (uint32_t j : l.ids)
            if (j < renum.size() && renum[j] >= 0) s3.push_back((uint32_t)renum[j]);
        l.actors = std::move(s1), l.ids = std::move(s3);
        l.x = (uint8_t)l.actors.size(), l.y = (uint8_t)l.ids.size();
    }
}

// mod_generator.makeGeneralLEBChanges: always made, whatever the placements
static void general_leb(SeedBuild &b) {
    // the Mad Batters' event entry points, by well
    static const struct { const char *room; int actor; const char *entry; } kBatters[] = {
        {"MadBattersWell01_01A.leb", 2, "BatterA"}, {"MadBattersWell02_01A.leb", 6, "BatterB"},
        {"MadBattersWell03_01A.leb", 0, "BatterC"}};
    for (auto &w : kBatters) {
        LebRoom r = b.read_room(w.room);
        r.actors.at(w.actor).params[2] = LebParam::of_str(w.entry);
        b.write_room(w.room, r);
    }
    {  // Lanmola Cave: no AnglerKey (type 0x8B) actor
        LebRoom r = b.read_room("LanmolaCave_02A.leb");
        std::vector<size_t> drop;
        for (size_t i = 0; i < r.actors.size(); i++)
            if (r.actors[i].type == 0x8B) drop.push_back(i);
        remove_actors(r, drop);
        b.write_room("LanmolaCave_02A.leb", r);
    }
    if (b.decisions().setting("classic-d2")) {  // the rock in front of Dungeon 2 becomes a swamp flower
        LebRoom r = b.read_room("Field_03E.leb");
        r.actors.at(12).type = 0x0E;
        b.write_room("Field_03E.leb", r);
    }
    {  // no BoyA / BoyB cutscene after the Full Moon Cello: the boy's link to the event box goes
        LebRoom r = b.read_room("Field_12A.leb");
        LebLinks &boy = r.actors.at(1).links, &box = r.actors.at(8).links;
        boy.x--, boy.actors.erase(boy.actors.begin());
        box.y--, box.ids.erase(box.ids.begin());
        b.write_room("Field_12A.leb", r);
    }
    {  // the honeycomb in the tree shows the item placed there
        LebRoom r = b.read_room("Field_09H.leb");
        SeedBuild::ItemInfo it = b.item_info("tarin-ukuku", true);
        r.actors.at(0).params[0] = LebParam::of_str(it.model_path);
        r.actors.at(0).params[1] = LebParam::of_str(it.model_name);
        b.write_room("Field_09H.leb", r);
    }
}

const std::vector<BuildStep> &build_steps() {
    static const std::vector<BuildStep> steps = {
        {"mod_generator.makeGeneralLEBChanges", general_leb},
    };
    return steps;
}

}  // namespace rando
