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
| 2 | file-format library (rooms, event flows, data sheets, messages, archives, textures) | done: rooms, level configs, data sheets, messages, archives, event flows, texture banks |
| 3 | runtime applies seeds in memory; `lasrando dump` writes what it changed | in progress: every step but room shuffle and textures built natively, file for file as the generator; the runtime's side next |
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
choices come from the seed's decisions). Each step is a port of one generator function, in group files
(`las_*.cpp`, `ours_*.cpp`), ordered and gated as the generator runs them (`steps.cpp`).

- `tools/rando/record.py`: the generator writes `decisions.json` (placements, trap looks, enemy rolls, prices, hint
  picks, ...); with `LARECOMP_RECORD=<folder>` it also logs every file it writes, in order.
- `build/lasrando check-steps <romfs> <decisions.json> <record>`: each ported step run from the generator's state just
  before it, its writes compared byte for byte.
- `build/lasrando dump <romfs> <decisions.json> <out>` and `tools/rando/test_apply.py <romfs> <seed folder>...`: a
  whole seed built natively against the folder the generator wrote, file for file.
- Generated tables (`tools/rando/export_*.py --check`) keep the C++ copies of Python data in step.

## Random numbers

`runtime/rando/pyrandom.h` is Python's `random.Random`, bit for bit: today's seed IDs were made by Python, and the
native solver must give the same seeds. `tools/test_pyrandom.cpp` checks it against CPython 3.12
(`tools/rando/pyrandom_golden.txt`). Float sums that Python rounds twice must not be fused into one multiply-add by the
compiler (see `uniform()`).

## File formats

`runtime/rando/fmt/`: the game's data files, read and written by the same code in the game and in `lasrando`.

- `fixed_hash`, `leb` (rooms), `lvb` (level configs): ported from LAS-Randomizer's Python (MIT, credited in each file).
- `msbt` (messages): ported from our `tools/rando/msbt.py`.
- `gsheet` (data sheets), `sarc` (archives), `bfevfl` (event flows), `bntx` (texture banks, with the merge, subset,
  rename and pixel-copy operations of `tools/rando/bntx_merge.py`): written for LAS Recomp from the format; they lay
  files out exactly as oead 1.3 / evfl 1.2 / LAS's bundled BNTX editor do (GPL code: read for the layout only, no code copied).

Each writer produces the same bytes as the Python code seeds are made with today, so the two pipelines can be compared
file by file. `tools/rando/test_formats.py <romfs> [kind ...]` checks that on every game file of a kind (or on a seed's
`romfs/`), with the LAS venv's Python and `make lasrando`; texture banks: `tools/test_bntx_formats.py <romfs> [<seed romfs> ...]`.

## Tests

- `tools/test_pyrandom.cpp`: the random numbers against CPython.
- `tools/test_seed_record.c`: the seed record answers every setting the runtime reads exactly as the older
  `settings.json` readers did, and every table reads line for line as `fgets` read its file (at several buffer sizes,
  CRLF seeds included), for older folders and for their `seed.json`. With no arguments it uses generated
  folders; give it seed folders to check those.
- Randomizer changes still need several generated seeds checked in game (CONTRIBUTING "Testing your change").
