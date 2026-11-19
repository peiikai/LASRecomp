// lasrando decide-check <romfs> <record folder> <seed folder> [<out folder>]: the seed built in decide mode
// (decide.h) from the item fill's result - the seed folder's decisions.json without the fields the build decides,
// and the random state in <record>/postfill.json (tools/rando/record.py) - against what the Python generator made:
// the decisions document (every field), the tables (seed.json "tables": the folder's .tsv files) and the game files.
// <out>: also writes what it made there. 0 when all the same.
#pragma once
#include <string>

namespace rando {

extern const char *const DECIDED_FIELDS[10];
extern const char *const SEED_TABLES[16];

int decide_check(const std::string &romfs, const std::string &record, const std::string &seed, const std::string &out);

}  // namespace rando
