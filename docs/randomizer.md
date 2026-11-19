# The randomizer: how seeds are made and applied

This page is for contributors. Players: see the README.

## Design

The randomizer is built on one design:

1. **A native solver** (C++, inside the app on every platform) turns a seed ID and its settings into placements and
   every other decision a seed needs.
2. **One seed record** (`rando/<name>/seed.json`) holds all of it.
3. **The runtime applies the seed in memory**: when the game reads a file a seed changes, the runtime builds the
   changed version from the game's own file and the record. No game files are written to the seed folder.

Seeds used to be made by the vendored LAS-Randomizer (Python) driven by LAS Recomp's own Python modules, which wrote
changed game files into the seed folder. The move to this design happened in six stages, each proven against the
Python generator file for file, so every seed ID gives the same seed as before. The Python is gone from the repository
since (the last commit with it: 989114e1); the C++ is the only randomizer now, and its data tables (the files the
exporters once generated: `*_data.cpp`) are edited directly.

| stage | what | state |
|---|---|---|
| 1 | seed record + one runtime API; Python's `random` in C++ | done |
| 2 | file-format library (rooms, event flows, data sheets, messages, archives, textures) | done: rooms, level configs, data sheets, messages, archives, event flows, texture banks |
| 3 | runtime applies seeds in memory; `lasrando dump` writes what it changed | done: every step built natively, file for file as the generator |
| 4 | asset builds (dungeon room shuffle, textures) in the runtime | done: textures (chest looks, title logo), dungeon rooms; room texture banks are built when the game reads them |
| 5 | native solver | done: the whole generator, dungeon room shuffle's planning and room logic included, file for file as today |
| 6 | generation in the app everywhere; Python leaves the builds | done: no seed generator helper, no embedded Python on iOS, no randomizer modules on Android |

The rules:

- **Same seeds.** A seed ID gives the same seed in every version (placements, hints, spoiler, game changes). A change
  that would move a seed is a new option or a new seed ID format, never a silent change to old IDs.
- **Older seeds keep working.** Seed folders made by older versions load as they always did.
- **One random-number stream per purpose**, never a shared global one, and no clock or platform state in a seed.

## Seed record

`runtime/rando/seed.h`. A seed folder holds `seed.json`:

```json
{
  "format": "lasrecomp-seed",
  "version": 1,
  "seed_id": "MBHMM-13FR6-2000W-40GAZ-X700Z",
  "settings": { "escape-bottle": true, "keysanity": "all", "seashell-total": 50, ... },
  "tables": { "pots": ["ArmosShrine\t5.25\t1.5\t30.75\t1291", ...], "door_jumps": [...], ... }
}
```

- `settings`: the seed's options and values the generator worked out for the runtime. Readers use
  `rando_setting("escape-bottle")`, `rando_setting_int(...)`, `rando_setting_str(...)` on the active seed, or
  `rando_seed_load(dir)` for another folder. A missing key reads as off.
- Seeds made before `seed.json` have `settings.json` + `seed_id.txt`: `rando_seed_load` reads them into the same
  record, so readers have one path.
- `build/lasrando convert <seed folder>` prints a folder's record as `seed.json` (`make lasrando`).
- `tables`: the data the runtime's hooks read, each the lines of one tab-separated table (`apple_trees`, `bushes`,
  `digs`, `door_jumps`, `dungeon_map`, `enemies_random`, `flags`, `flags_ext`, `hints`, `pots`, `quick_pickups`,
  `roam`, `rocks`, `shell_spots`, `shopsanity`, `souls`, `tracker`, `triforce`). Each row format is documented at its
  reader. Readers use `rando_text_open` / `rando_text_gets`, which read a table the way `fgets` reads a file, so the
  parsing is the same as when these were files (an older folder's `*.tsv` files are read into the record).
- The spoiler (`spoiler.txt`, `spoiler_view.txt`, `spoiler.html`) stays a set of files next to the record: it is for
  people to read.
- Later versions add the seed's placements and the code patches to the record. A newer version number is read as far
  as this build understands it.

## Seed build

`runtime/rando/apply/`: a seed's game-file changes made in memory, step by step as the generator makes them
(`seed_build.h`: reads see the last write or the game's file, one event-name counter, no random draws: the generator's
choices come from the seed's decisions, `decisions.json`). Each step is one group of changes, in group files
(`las_*.cpp`, `ours_*.cpp`), ordered and gated as the generator runs them (`steps.cpp`).

- `build/lasrando dump <romfs> <decisions.json> <out>`: a whole seed built as the runtime builds it, written out.
- `build/lasrando serve-check <romfs> <decisions.json> <dump folder>`: the build the runtime does (files built when
  the game reads them) read back in pieces and compared with `dump`'s, with load time and memory.

Textures (`textures.cpp`: chest looks' recoloured chests, the title logo): the image steps (colour conversion, blur,
paste and bilinear resize in `fmt/texture.cpp`, as Pillow does them) and rgbcx compression (`third_party/rgbcx`, MIT).

## Native generator

`runtime/rando/solve/`: the seed generator (`generate.h`): settings and the item / logic pool (`prepare`), the
entrance randomizer (`entrances`), LAS's fill (`shuffler`, `logic`), the post-fill outputs (`outputs`: playthrough
check, hints, route, spoiler, tracker), then the seed build in decide mode (`runtime/rando/apply/decide.h`: the
file-writing steps make the generator's random draws themselves and record them as the seed's decisions). Dungeon room
shuffle (`rooms*.cpp`): the layout planning and room logic, with the layout retries; what it needs from the game's
rooms (tile grids, actors, stairs, textures, map pages) is read from the player's own files when the planning starts
(`rooms_game.cpp`; `lasrando room-facts <romfs>` prints it).

- `build/lasrando generate <romfs> <request.json> <out> [--legacy]`: a seed folder (seed.json, the code patches, the
  spoiler); `--legacy` also writes a seed folder's older files (romfs/, the tables, settings.json, ...).
- `build/lasrando strings`: every text the spoiler view and the check tracker can show (`outputs_strings.cpp`), which
  `tools/menu_strings.py` reads for the translation tables. A new spoiler or tracker line goes there too.
- `runtime/rando/sha512.c`: the SHA-512 behind `random.seed(str)` (written from FIPS 180-4).

## Random numbers

`runtime/rando/pyrandom.h` is Python's `random.Random`, bit for bit: the seed IDs players have were made by the Python
generator, and they must keep giving the same seeds. `tools/test_pyrandom.cpp` checks it against CPython 3.12
(`tools/rando/pyrandom_golden.txt`). Float sums that Python rounds twice must not be fused into one multiply-add by the
compiler (see `uniform()`).

## File formats

`runtime/rando/fmt/`: the game's data files, read and written by the same code in the game and in `lasrando`.

- `fixed_hash`, `leb` (rooms), `lvb` (level configs): ported from LAS-Randomizer's Python (MIT, credited in each file).
- `msbt` (messages): ported from LAS Recomp's own earlier Python.
- `gsheet` (data sheets), `sarc` (archives), `bfevfl` (event flows), `bntx` (texture banks, with merge, subset, rename
  and pixel-copy operations): written for LAS Recomp from the format; they lay files out exactly as oead 1.3 / evfl 1.2 /
  LAS's bundled BNTX editor do (GPL code: read for the layout only, no code copied).

## Tests

- `tools/test_pyrandom.cpp`: the random numbers against values recorded from CPython 3.12
  (`tools/rando/pyrandom_golden.txt`).
- `tools/test_seed_record.c`: the seed record answers every setting the runtime reads exactly as the older
  `settings.json` readers did, and every table reads line for line as `fgets` read its file (at several buffer sizes,
  CRLF seeds included), for older folders and for their `seed.json`. With no arguments it uses generated
  folders; give it seed folders to check those.
- Randomizer changes still need several generated seeds checked in game (CONTRIBUTING "Testing your change").
