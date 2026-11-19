// The enemy randomizer: mod_generator.randomizeEnemies (LAS-Randomizer's Randomizers/enemies.py with LAS Recomp's
// mini-bosses and Random mode, MIT, Owen-Splat and contributors) and the two tables that come of it,
// tools/rando/roam.py (roam.tsv) and tools/rando/enemy_random.py (enemies_random.tsv).
//
// Apply mode reads what the draws did from the decisions' "enemies" (tools/rando/record_las_world.py); decide mode
// (decide.h) makes the draws, as shuffleEnemyActors does, and records them there. The data the draws need comes
// from las_enemies_data.cpp (tools/rando/export_las_enemies.py).
#pragma once
#include <string>
#include <utility>
#include <vector>

#include "seed_build.h"

class PyRandom;  // (../pyrandom.h)

namespace rando {
namespace las {
namespace enemy_data {

struct EnemyValue {  // a parameter value as the YAML / enemies.py give it (strings become bytes in the room)
    enum Kind { Str, Int, Float } kind;
    const char *s;
    long long i;
    double f;
};
struct EnemyParam {  // a list: random.choice of its values; else the one value
    bool pick;
    std::vector<EnemyValue> values;
};
struct EnemyActor {  // ENEMY_DATA['Actors'], in the file's order
    const char *name;
    int id;
    const char *type;  // land, air, water, water2D, water-shallow, tree, hole
    int enemy;         // the links' enemy byte (int(value['enemy']))
    bool minibosses_only;
    bool has_params;   // (none: every parameter becomes an empty string - the Python's KeyError path)
    std::vector<EnemyParam> params;
};

extern const std::vector<EnemyActor> ACTORS;
extern const std::vector<int> CHEST_ENEMIES;
extern const std::vector<std::pair<const char *, std::vector<int>>> EXCLUDED_ACTORS;
extern const std::vector<const char *> ROOMS_2D, INCLUDED_FOLDERS;

// enemies.py
extern const std::vector<std::pair<int, std::vector<EnemyValue>>> FIELD_MINIBOSSES;  // (sorted by type)
extern const std::vector<std::pair<int, std::vector<std::vector<EnemyValue>>>> MINIBOSS_VARIANTS;
extern const std::vector<int> ANY_TERRAIN_MINIBOSSES, SMALL_MINIBOSSES, SOLID_OBJECTS;  // (sorted)
extern const std::vector<std::pair<int, int>> NEEDS_ROOM;
extern const int BIG_ROOM;
extern const double MB_GAP;
extern const std::vector<const char *> SMALL_ONLY_SCREENS;
extern const bool SMALL_ON_AIR;
extern const std::vector<double> MINIBOSS_CHANCES;
extern const std::vector<int> NO_2D, FLY_BOMBS, BLOCKING, ONE_PER_ROOM, ARENA_TYPES;
extern const std::vector<std::pair<const char *, int>> STAIR_GUARDS;
extern const int OBJ_CLOSED_DOOR;
// miniboss_guard.ANYWHERE (enemies_random.tsv's mark line)
extern const std::vector<int> MARK_ANYWHERE;

}  // namespace enemy_data

// the steps (las_world.cpp registers them)
void randomize_enemies(SeedBuild &b);   // mod_generator.randomizeEnemies
void write_roam(SeedBuild &b);          // roam.write (las_headless.py: with enemy-minibosses)
void write_enemy_random(SeedBuild &b);  // enemy_random.write (las_headless.py: when mods.enemy_random is set)
// makeExefsPatches' Chest_Enemies pick (assemble.preSetup: random.choice on the shared stream; decide mode)
int draw_chest_enemy(PyRandom &shared);

}  // namespace las
}  // namespace rando
