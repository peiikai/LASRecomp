# The randomizer: how seeds are made and applied

This page is for contributors. Players: see the README.

## Overview

Everything happens in the app, on every platform, in C++ (`runtime/rando/`):

1. **The generator** (`solve/`) turns a seed ID and its settings into the item placement and every other decision a
   seed needs.
2. **One seed record** (`rando/<name>/seed.json`) holds all of it.
3. **The runtime applies the seed in memory** (`apply/`): when the game reads a file the seed changes, the runtime
   builds the changed version from the player's own game file and the record. No game files are written to the seed
   folder.

The rules:

- **Same seeds.** A seed ID gives the same seed in every version (placements, hints, spoiler, game changes). A change
  that would move a seed is a new option or a new seed ID format, never a silent change to old IDs.
- **Older seeds keep working.** Seed folders made by older versions load as they always did.
- **One random-number stream per purpose**, never a shared global one, and no clock or platform state in a seed.

## Making a seed

The Randomizer tab (`runtime/ui/rando_tab.mm`) holds the options, and the seed ID that encodes them:

- Seed ID: 30 characters (format 5), the options' bits and a 36-bit random part, with a checksum. Older formats (20
  and 25 characters) still decode, with the options they don't hold off. The layout and its history are at
  `seed_id_encode` in `rando_tab.mm`.
- Generate writes the options as `rando/<name>/request.json` (the generator's settings) and runs
  `rando::solve::generate` on a worker thread. Progress goes to `generate.log` as `stage <fraction> <label>` lines, and
  the menu's bar shows them.
- The result is the seed folder below. When the settings can't work (too few places for the items, a Triforce hunt
  with too few pieces) the player gets a message they can act on (`GenerateResult::player_error`); anything else says
  where the details are (`generate.log`).

The generator (`solve/generate.h`), in order:

- settings and the item / logic pool (`prepare`), including the checks the options add (pots, bushes, rocks, digs,
  apple trees, the shop, the Trendy Game, souls, the Triforce hunt);
- dungeon room shuffle's layout planning (`rooms*.cpp`), with its own retries: what it needs from the game's rooms
  (tile grids, actors, stairs, textures, map pages) is read from the player's own files when planning starts
  (`rooms_game.cpp`; `lasrando room-facts <romfs>` prints it);
- the entrance randomizer (`entrances`);
- the item fill and its logic (`shuffler`, `logic`: LAS-Randomizer's, ported), which gives up with the player message
  above after a fixed number of retries;
- the playthrough check (every instrument reachable, the Nightmare beatable) and the outputs (`outputs`: hints, route,
  spoiler, tracker);
- the seed build in decide mode (`apply/decide.h`): the file-writing steps make their own random draws (enemies,
  trap looks, prices, ...) and record them as the seed's decisions.

## Seed folder

`rando/<name>/`:

| file | what |
|---|---|
| `seed.json` | the seed record (below) |
| `exefs/1.0.0.pchtxt` | the code and data patches (every supported game version reads it: `runtime/rando.c`) |
| `spoiler.txt`, `spoiler_view.txt`, `spoiler.html` | the spoiler, for people to read |
| `hints.txt`, `seed_hints.txt` | the Hint Sheet and the owl / phone hints |
| `pots_<ID>.txt`, `bushes_<ID>.txt`, `rocks_<ID>.txt`, `digs_<ID>.txt`, `entrances_<ID>.txt` | readable lists of the seed's extra checks and entrances |
| `request.json`, `generate.log`, `seed_id.txt` | what the menu asked for, the generator's log, the seed ID |
| `save/` | the seed's own save slots |

Seeds made before `seed.json` hold `romfs/` (the changed game files), `settings.json`, `seed_id.txt` and `*.tsv`
tables instead; they load as they always did (`rando_seed_load` reads them into the same record, and their files come
from `romfs/`).

### Seed record

`runtime/rando/seed.h`:

```json
{
  "format": "lasrecomp-seed",
  "version": 1,
  "seed_id": "MBHMM-13FR6-2000W-40GAZ-X700Z",
  "settings": { "escape-bottle": true, "keysanity": "all", "seashell-total": 50, ... },
  "tables": { "pots": ["ArmosShrine\t5.25\t1.5\t30.75\t1291", ...], "door_jumps": [...], ... },
  "decisions": { "placements": {...}, "room-plan": {...}, "shop-prices": {...}, ... }
}
```

- `settings`: the seed's options and the values the generator worked out for the runtime. Readers use
  `rando_setting("escape-bottle")`, `rando_setting_int(...)`, `rando_setting_str(...)` on the active seed, or
  `rando_seed_load(dir)` for another folder. A missing key reads as off.
- `tables`: the data the runtime's hooks read, each the lines of one tab-separated table (`apple_trees`, `bushes`,
  `digs`, `door_jumps`, `dungeon_map`, `enemies_random`, `flags`, `flags_ext`, `hints`, `pots`, `quick_pickups`,
  `roam`, `rocks`, `shell_spots`, `shopsanity`, `souls`, `tracker`, `triforce`). Each row format is documented at its
  reader. Readers use `rando_text_open` / `rando_text_gets`, which read a table the way `fgets` reads a file.
- `decisions`: everything the seed build needs (placements, the room plan, entrances, prices, enemy rolls, hint
  picks, ...), so the game files can be built again on any machine from the player's own files.
- A newer `version` is read as far as this build understands it.

## Applying a seed

When a seed becomes active (at boot, or on an in-place switch: `runtime/main.c`), `runtime/rando.c` loads the record
and the code patches, and `runtime/services/fs.c` builds the seed's game files from its decisions
(`apply/build.h`) and serves them in place of the game's.

- `apply/`: the build steps, in group files (`las_*.cpp`, `ours_*.cpp`), ordered and gated as the generator runs them
  (`steps.cpp`). `seed_build.h`: reads see the last write or the game's file, one event-name counter, no random draws
  (the choices come from the decisions).
- Dungeon room shuffle's texture banks are large (up to about 75 MB each), so they are built when the game first
  reads them (a recipe is kept; one bank is cached), not at load.
- Textures (`textures.cpp`: chest looks' recoloured chests, the title logo): colour conversion, blur, paste and
  bilinear resize in `fmt/texture.cpp`, compressed with rgbcx (`third_party/rgbcx`, MIT).
- Code patches: the generator (`runtime/gen/`) compiles every variant of every patched instruction into the game code
  (`tools/rando/patch_variants.json`); the seed's `.pchtxt` picks the variant (`rando_word`).
- State that belongs to a seed resets on an in-place switch through `RT_ON_RESET` (`runtime/hle.h`).

## Data

The generator's and the build's tables are C++ data files (`*_data.cpp`, `solve_data.cpp`, `room_data.cpp`): edit
them directly. Some were generated once from LAS-Randomizer's YAML and Python (its logic, items, locations, enemies,
assembled code patches), the rest are ReAwakening's own (room catalog, entrances, pots, digs, hints, text).

## File formats

`runtime/rando/fmt/`: the game's data files, read and written by the same code in the game and in `lasrando`.

- `fixed_hash`, `leb` (rooms), `lvb` (level configs): ported from LAS-Randomizer (MIT, credited in each file).
- `msbt` (messages): ReAwakening's own.
- `gsheet` (data sheets), `sarc` (archives), `bfevfl` (event flows), `bntx` (texture banks, with merge, subset, rename
  and pixel-copy operations): written for ReAwakening from the format; they lay files out exactly as oead 1.3 / evfl 1.2 /
  LAS's bundled BNTX editor do (GPL code: read for the layout only, no code copied).

## Random numbers

`runtime/rando/pyrandom.h` is Python's `random.Random`, bit for bit (the first seed IDs were made by a Python
generator, and they must keep giving the same seeds), with `runtime/rando/sha512.c` behind `random.seed(str)`. Float
sums that Python rounds twice must not be fused into one multiply-add by the compiler (see `uniform()`).

## lasrando

`make lasrando` builds `build/lasrando`, the randomizer's command-line tool, from the same code the game runs:

- `lasrando generate <romfs> <request.json> <out folder>`: a seed made as the menu makes it.
- `lasrando dump <romfs> <seed folder> <out folder>`: a seed's game files built as the runtime builds them, written
  out (`<out>/romfs`, `<out>/exefs`, the tables).
- `lasrando serve-check <romfs> <seed folder> <dump folder>`: the runtime's build (files built when read) read back in
  pieces and compared with `dump`'s, with load time and memory.
- `lasrando convert <seed folder>`, `lasrando get <seed folder> <key>...`: a folder's record as `seed.json`, settings
  as the runtime reads them.
- `lasrando room-facts <romfs>`: what dungeon room shuffle reads from the game's rooms.
- `lasrando room-cases doors|stairs`: the cases the in-game room shuffle tests walk (below).
- `lasrando strings`: every text the spoiler view and the check tracker can show (`solve/outputs_strings.cpp`), which
  `tools/menu_strings.py` reads for the translation tables. A new spoiler or tracker line goes there too.

## Adding an option

- `runtime/ui/rando_tab.mm`: the row, its place in the seed ID (append bits; older IDs must decode with the option
  off), and its field in `request.json`.
- `runtime/rando/solve/`: what it does to the pool, the logic or the fill; `apply/` for the game files it changes; a
  setting or a table for the runtime hooks that need it (`runtime/hle/`).
- Its spoiler / tracker text in `outputs_strings.cpp`, its menu text through `L()` (the translation tables are
  filled before a release: AGENTS.md).
- Check that existing seed IDs still give the same seeds (generate a few with `lasrando generate` before and after
  and compare the folders).

## Tests

- `tools/test_pyrandom.cpp`: the random numbers against values recorded from CPython 3.12
  (`tools/rando/pyrandom_golden.txt`).
- `tools/test_seed_record.c`: the seed record answers every setting the runtime reads exactly as the older
  `settings.json` readers did, and every table reads line for line as `fgets` read its file (at several buffer sizes,
  CRLF seeds included), for older folders and for their `seed.json`. With no arguments it uses generated folders; give
  it seed folders to check those.
- `tools/test_game_addresses.py`: every randomizer patch site holds the same original bytes in every game version.
- In game: `tools/doorjump_test.sh <seed>` walks every mismatched doorway of a dungeon room shuffle seed, and
  `tools/stairs_test.sh <seed>` every stairs and ladder.
- Randomizer changes still need several generated seeds checked in game (CONTRIBUTING "Testing your change").

## History

Seeds used to be made by the vendored LAS-Randomizer (Python) and ReAwakening's own Python modules, which wrote changed
game files into the seed folder. In November 2026 that moved to this design in six stages (seed record, file
formats, applying seeds in memory, asset builds, the generator, generation in the app everywhere), each proven against
the Python generator file for file, so every seed ID kept giving the same seed. The Python was then removed (the last
commit with it: 989114e1).
