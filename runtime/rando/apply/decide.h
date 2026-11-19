// Decide mode (docs/randomizer.md, stage 5): the seed build makes the generator's choices itself, where apply mode
// reads them from the decisions.
//
// The Python generator draws while it writes the seed's files; tools/rando/record*.py records what each draw gave, so
// the runtime applies a seed without drawing (apply mode, seed_build.h). A native generator has to make those choices:
// with a Decider set on the build (SeedBuild::set_decider), each step that reads a recorded choice computes it as
// the Python does instead, stores it here (Decisions::raw then reads it as if it were recorded) and goes on as in
// apply mode. What comes out is the seed's files, its tables and a decisions document equal to the Python's
// decisions.json.
//
// The random streams are the Python's:
//   - shared(): the `random` module's. ModsProcess does random.seed(seed); random.setstate(randstate) - the state
//     the item fill left (solve/shuffler.h random_state(), postfill.json "randstate") - and draws from it in call
//     order (getItemInfo's trap looks, the music, the enemies, the chest enemy); randomizeMusic and randomizeEnemies
//     re-seed it (reseed_shared).
//   - each module's own random.Random(f"{seed}-<tag>") (stream()).
//
// Inputs the steps need that aren't the seed's decisions yet (made by other parts of the native pipeline: the
// post-fill outputs, the entrance shuffle) come as files by name (input()): the check feeds them from a recorded
// run.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include <map>
#include <string>
#include <vector>

// (random.seed(str) hashes with SHA-512: decide.cpp's, for every file that draws in decide mode)
#ifndef PYRANDOM_SHA512
extern "C" void rando_decide_sha512(uint8_t hash[64], const uint8_t *message, size_t message_size);
#define PYRANDOM_SHA512 rando_decide_sha512
#endif
#include "../pyrandom.h"
#include "seed_build.h"

namespace rando {

class Decider {
public:
    // d: the seed's decisions without the decided fields (placements, indexes, settings, item-defs, ...);
    // randstate: random.getstate()'s 624 words + index (625 numbers)
    Decider(Decisions &d, const std::vector<uint32_t> &randstate);

    PyRandom &shared() { return shared_; }
    void reseed_shared();  // random.seed(seed) (the settings' "seed")
    // random.Random(f"{seed}-{tag}")
    PyRandom stream(const std::string &tag) const;
    std::string seed() const;

    // a decided top-level field (its JSON text); Decisions::raw reads it from now on
    void set(const std::string &field, const std::string &json);
    // a decided key of "ours-world" (record_ours_world.py's fields)
    void set_ours_world(const std::string &key, const std::string &json);
    // getItemInfo's trap look at a check: random.choice(list(trap_models)) on the shared stream, recorded
    // (ModsProcess.trap_models / dungeon_trap_models: LAS's ITEM_MODELS less the starting items, and the instruments
    // when they aren't shuffled; the dungeon pool adds the dungeon items)
    void draw_trap(const std::string &check, bool dungeon, std::string &path, std::string &name);

    // a file the decisions need from elsewhere (hint_steps.json, seed_hints.json, post_entrances.json): false if none
    void set_input(const std::string &name, std::string bytes) { inputs_[name] = std::move(bytes); }
    const std::string *input(const std::string &name) const;

    // the decisions document as tools/rando/record.py writes it (json.dump(indent=1, sort_keys=True))
    std::string document() const;

    Decisions &decisions() { return d_; }

private:
    Decisions &d_;
    PyRandom shared_;
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> traps_;
    std::vector<std::pair<std::string, std::string>> pool_[2];  // (name, model file): field / dungeon
    bool pools_ = false;
    std::map<std::string, std::string> ours_world_;  // key -> JSON
    std::map<std::string, std::string> inputs_;
};

// JSON as Python's json.dumps writes it (ensure_ascii; floats as repr(); with indent: one item per line, the given
// indent, ", " / ": " separators as json.dump with indent; sort_keys sorts object keys)
std::string py_json(const char *json, size_t len, int indent, bool sort_keys);
std::string py_float_repr(double v);  // repr(float)
std::string py_str_repr(const std::string &s);  // repr(str) (single quotes unless the string has one and no ")

}  // namespace rando
