// The generator's steps, ported (seed_build.h): shared helpers.
#pragma once
#include <vector>

#include "seed_build.h"

namespace rando {

// LAS Recomp's enemies.remove_actors: drop room actors (with their mini-boss kits) and renumber the links
void remove_actors(LebRoom &room, std::vector<size_t> indices);

}  // namespace rando
