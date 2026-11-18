# The randomizer: how seeds are made and applied

This page is for contributors. Players: see the README.

## Where it is going

The randomizer is moving to one design:

1. **A native solver** (C++, inside the app on every platform) turns a seed ID and its settings into placements and
   every other decision a seed needs.
2. **One seed record** (`rando/<name>/seed.json`) holds all of it.
3. **The runtime applies the seed in memory**: when the game reads a file a seed changes, the runtime builds the
   changed version from the game's own file and the record. No game files are written to the seed folder.

Today the seeds are still made by the vendored LAS-Randomizer driven by `tools/rando/las_headless.py` (Python), which
writes changed game files into the seed folder. The move happens in stages. Each stage keeps every seed, old and new,
playing exactly as before:

| stage | what | state |
|---|---|---|
| 1 | seed record + one runtime API; Python's `random` in C++ | in progress |
| 2 | file-format library (rooms, event flows, data sheets, messages, archives, textures) | |
| 3 | runtime applies seeds in memory; `lasrando dump` writes what it changed | |
| 4 | asset builds (dungeon room shuffle, textures) in the runtime | |
| 5 | native solver | |
| 6 | generation in the app everywhere; Python leaves the builds | |

The rules for every stage:

- **Same seeds.** A seed ID gives the same seed as before (placements, hints, spoiler, game changes). The parity
  harness compares the new pipeline with the Python generator over many seeds; any difference is a bug until proven
  otherwise. Bugs found along the way are proven in game, then fixed in both.
- **Older seeds keep working.** Seed folders made by older versions load as they always did.
- **One random-number stream per purpose**, never a shared global one, and no clock or platform state in a seed.

## Seed record

`runtime/rando/seed.h`. A seed folder holds `seed.json`:

```json
{
  "format": "lasrecomp-seed",
  "version": 1,
  "seed_id": "MBHMM-13FR6-2000W-40GAZ-X700Z",
  "settings": { "escape-bottle": true, "keysanity": "all", "seashell-total": 50, ... }
}
```

- `settings`: the seed's options and values the generator worked out for the runtime. Readers use
  `rando_setting("escape-bottle")`, `rando_setting_int(...)`, `rando_setting_str(...)` on the active seed, or
  `rando_seed_load(dir)` for another folder. A missing key reads as off.
- Seeds made before `seed.json` have `settings.json` + `seed_id.txt`: `rando_seed_load` reads them into the same
  record, so readers have one path.
- `build/lasrando convert <seed folder>` prints a folder's record as `seed.json` (`make lasrando`).
- Later versions add the seed's tables (pots, digs, door jumps, tracker, ...) and its placements to the record. A
  newer version number is read as far as this build understands it.

## Random numbers

`runtime/rando/pyrandom.h` is Python's `random.Random`, bit for bit: today's seed IDs were made by Python, and the
native solver must give the same seeds. `tools/test_pyrandom.cpp` checks it against CPython 3.12
(`tools/rando/pyrandom_golden.txt`). Float sums that Python rounds twice must not be fused into one multiply-add by the
compiler (see `uniform()`).

## Tests

- `tools/test_pyrandom.cpp`: the random numbers against CPython.
- `tools/test_seed_record.c`: the seed record answers every setting the runtime reads exactly as the older
  `settings.json` readers did, for older folders and for their `seed.json`. With no arguments it uses generated
  folders; give it seed folders to check those.
- Randomizer changes still need several generated seeds checked in game (CONTRIBUTING "Testing your change").
