// A randomizer seed's record: everything the runtime knows about a seed, in one place (notes on the design:
// docs/randomizer.md "Seed record").
//
// A seed folder rando/<name>/ holds seed.json (format "lasrecomp-seed", version 1). Seeds made before it have
// settings.json and seed_id.txt instead: they are read into the same record, so every reader has one path and older
// seeds keep working as they did.
//
// The active seed's records are never freed while the app runs: a reader may keep a pointer into one across a seed
// switch (seed switches are rare and a record is a few KB).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RandoSeed RandoSeed;

// The active seed's record (NULL: vanilla, or the folder isn't a seed). Loaded with the seed (runtime/rando.c).
const RandoSeed *rando_seed(void);

// A seed folder's record (NULL: no seed there), freed with rando_seed_free. The active seed's: rando_seed().
const RandoSeed *rando_seed_load(const char *dir);
void rando_seed_free(const RandoSeed *s);
bool rando_seed_dir_bool(const char *dir, const char *key);  // load, read one setting, free (the start screen)

// settings: the seed's options and the values the generator worked out for the runtime ("escape-bottle", "keysanity",
// "seashell-total", ...). A missing key reads as false / dflt / NULL.
bool rando_seed_bool(const RandoSeed *s, const char *key);                 // the value is true
int64_t rando_seed_int(const RandoSeed *s, const char *key, int64_t dflt); // a whole number, else dflt
const char *rando_seed_str(const RandoSeed *s, const char *key);           // a string, else NULL
bool rando_seed_has(const RandoSeed *s, const char *key);

const char *rando_seed_id(const RandoSeed *s);  // the seed ID that regenerates it ("" if unknown)
int rando_seed_format(const RandoSeed *s);      // 1 = seed.json; 0 = an older folder read into the record

// The record as seed.json (pretty-printed, malloc'd, NUL-terminated): `lasrando convert` writes it for older folders.
char *rando_seed_json(const RandoSeed *s, size_t *len);

// The active seed's settings (false / dflt / NULL without a seed).
bool rando_setting(const char *key);
int64_t rando_setting_int(const char *key, int64_t dflt);
const char *rando_setting_str(const char *key);

// runtime/rando.c: load the active seed's record (called with the seed folder, or NULL for vanilla).
void rando_seed_activate(const char *dir);

#ifdef __cplusplus
}
#endif
