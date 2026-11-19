// A randomizer seed's record: everything the runtime knows about a seed, in one place (notes on the design:
// docs/randomizer.md "Seed record").
//
// A seed folder rando/<name>/ holds seed.json (format "lasrecomp-seed", version 1). Seeds made before it have
// settings.json, seed_id.txt and the tables as *.tsv files instead: they are read into the same record, so every reader has one path and older
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

// tables: the per-seed data the runtime hooks read (pots, digs, door jumps, tracker, ...), each the lines of a
// tab-separated table whose row format is documented at its reader. Names (seed.c kTables): "pots", "door_jumps", ...
// Read like a file, so each reader parses exactly as before:
//     RandoText t;
//     if (rando_text_open(&t, "pots")) while (rando_text_gets(line, sizeof line, &t)) ...
// rando_text_gets is fgets over the table: each line ends in '\n' (a file's last line without one, too; a CRLF reads
// as '\n', as a text-mode read gives it), a line longer than the buffer comes in pieces.
typedef struct { const void *arr; size_t row, n, col; } RandoText;
bool rando_seed_text(const RandoSeed *s, RandoText *t, const char *table);  // false: the seed has no such table
bool rando_text_open(RandoText *t, const char *table);                     // the active seed's
char *rando_text_gets(char *buf, int cap, RandoText *t);
bool rando_seed_has_table(const RandoSeed *s, const char *table);

const char *rando_seed_id(const RandoSeed *s);

// The seed's decisions as JSON (malloc'd; NULL: the seed has none - its files come from its folder). The runtime
// builds the seed's game files from them (runtime/rando/apply/, docs/randomizer.md).
char *rando_seed_decisions_json(const RandoSeed *s, size_t *len);  // the seed ID that regenerates it ("" if unknown)
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
