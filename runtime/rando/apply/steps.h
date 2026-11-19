// The generator's steps, ported (seed_build.h): shared helpers.
#pragma once
#include <vector>

#include "seed_build.h"

namespace rando {

// each group's ported steps (las_general.cpp, las_items.cpp, las_world.cpp, ours_items.cpp, ours_world.cpp)
std::vector<BuildStep> steps_las_general();
std::vector<BuildStep> steps_las_items();
std::vector<BuildStep> steps_las_world();
std::vector<BuildStep> steps_ours_items();
std::vector<BuildStep> steps_ours_world();

// LAS Recomp's enemies.remove_actors: drop room actors (with their mini-boss kits) and renumber the links
void remove_actors(LebRoom &room, std::vector<size_t> indices);

}  // namespace rando
