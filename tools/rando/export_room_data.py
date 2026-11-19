#!/usr/bin/env python3
"""Writes the data the native dungeon room shuffle (runtime/rando/solve/rooms.cpp: dungeon_gen.plan_all_chaos and
room_logic.apply, docs/randomizer.md stage 5) starts from, as JSON documents the C++ parses (solve::Value):

  runtime/rando/solve/room_data.cpp       tools/rando/room_catalog.json and location_rooms.json (in their file order),
                                          room_overrides.py's door requirements, souls.exit_req (the mini-boss rooms)
  runtime/rando/solve/room_game_data.cpp  (--romfs) what dungeon_gen / room_logic read from the game's rooms, per room
                                          and per level, as they work it out (PYTHONHASHSEED=0, as the generator runs):
                                          each room's walkable areas and the moves between them (room_areas, with and
                                          without hookshot-anything), its locks, keyhole blocks, shutters, what a pot door
                                          takes, where a door jump lands (landing_depth), drop-down-only doorways
                                          (inward_only), the stairs and holes between rooms, the map textures each room
                                          needs and where each dungeon floor sits on the pause-menu map. Numbers and names
                                          only: no game data (no tiles, models or text)

    <LAS venv>/bin/python tools/rando/export_room_data.py [--romfs <dir>]          # rewrite the file(s)
    <LAS venv>/bin/python tools/rando/export_room_data.py [--romfs <dir>] --check  # exit 1 when out of date

The game-derived file needs the game's files, so --check only checks it when --romfs is given.
"""
import json
import os
import sys
import types

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
LAS = os.path.join(ROOT, 'third_party', 'LAS-Randomizer')
OUT = os.path.join(ROOT, 'runtime', 'rando', 'solve', 'room_data.cpp')
OUT_GAME = os.path.join(ROOT, 'runtime', 'rando', 'solve', 'room_game_data.cpp')
sys.path.insert(0, HERE)
sys.path.insert(0, LAS)
sys.modules.setdefault('PySide6', types.ModuleType('PySide6'))
sys.modules.setdefault('PySide6.QtCore', types.ModuleType('PySide6.QtCore'))


def load(name):
    with open(os.path.join(HERE, name)) as f:
        return json.load(f)


def static_json():
    import room_overrides
    import souls
    cat = load('room_catalog.json')
    doors = {}
    for room, fix in room_overrides.ROOMS.items():
        for (side, slot), _ in fix.get('door', {}).items():
            row = [side, slot]
            for way in ('in', 'out'):
                v = room_overrides.door(room, side, slot, way)
                row.append(None if v is None else sorted(v))
            doors.setdefault(room, []).append(row)
    out = {'catalog': cat, 'location_rooms': load('location_rooms.json'), 'door_overrides': doors,
           'exit_req': {k: sorted(v) for k, v in souls.exit_req(cat).items()}}
    return json.dumps(out, ensure_ascii=True, separators=(',', ':'))


def game_json(romfs):
    assert os.environ.get('PYTHONHASHSEED') == '0', 'run with PYTHONHASHSEED=0 (as the generator)'
    import dungeon_gen
    import room_logic
    import rooms
    cat = load('room_catalog.json')
    levels = list(dungeon_gen.ENTRANCES)
    out = {'rooms': {}, 'levels': {}}

    def areas_plain(room, hook):
        area, n, moves = room_logic.room_areas(romfs, room)
        tiles = [area.get((r, c), -1) for r in range(8) for c in range(10)]
        return {'tiles': tiles, 'n': n, 'moves': [[a, b, [sorted(x) for x in alts]] for a, b, alts in moves]}

    def spots(room, pts):
        return sorted(set().union(*(room_logic.spot_areas(romfs, room, *p) for p in pts))) if pts else None

    per_hook = {}
    for hook in (False, True):
        room_logic.HOOK_ANYTHING = hook
        room_logic._areas_cache.clear()
        room_logic._tiles_cache.clear()
        room_logic._hook_tiles.clear()
        got = {'areas': {room: areas_plain(room, hook) for room in cat}, 'ports': {}, 'holes': {}}
        for lv in levels:
            got['ports'][lv] = [[a, sorted(room_logic.spot_areas(romfs, a, *p)), b,
                                 sorted(room_logic.spot_areas(romfs, b, *q)), [sorted(x) for x in alts]]
                                for a, p, b, q, alts in room_logic.stairs_ports(romfs, lv, cat)]
            got['holes'][lv] = [[a, sorted(room_logic.spot_areas(romfs, a, *p)), b,
                                 spots(b, starts) if starts else list(range(room_logic.room_areas(romfs, b)[1]))]
                                for a, p, b, starts in room_logic.hole_links(romfs, lv, cat, spots=True)]
        per_hook['hook' if hook else 'plain'] = got
    room_logic.HOOK_ANYTHING = False
    room_logic._areas_cache.clear()
    room_logic._tiles_cache.clear()
    room_logic._hook_tiles.clear()
    out['areas'] = {k: v['areas'] for k, v in per_hook.items()}
    out['ports'] = {k: v['ports'] for k, v in per_hook.items()}
    out['holes'] = {k: v['holes'] for k, v in per_hook.items()}

    # textures: one name table, each room a list of indexes
    names = {}

    def tex_ids(s):
        return sorted(names.setdefault(t, len(names)) for t in sorted(s))
    land = {}
    for room, v in cat.items():
        room_logic.lock_flag(romfs, room, 'N')
        r = {'locks': [[sd, f] for sd, f in room_logic._lock_cache[room]],
             'blocks': room_logic.lock_blocks(romfs, room),
             'throw': None if room_logic.throw_req(romfs, room) is None else sorted(room_logic.throw_req(romfs, room)),
             'shutter': {s: sorted(room_logic.shutter_req(romfs, room, s)) for s in 'NSWE'},
             'tex': tex_ids(dungeon_gen.room_textures(romfs, room))}
        lands = []
        for s, xs in dungeon_gen.openings(cat, room).items():
            for x in xs:
                if isinstance(x[0], str):
                    continue
                for m in (0, 1):
                    lands.append([s, x[0], m, list(dungeon_gen.landing_depth(romfs, room, s, x[0], m))])
        r['land'] = lands
        out['rooms'][room] = r
    for lv in levels:
        off = dungeon_gen.offgrid_rooms(romfs, cat, lv)
        out['levels'][lv] = {
            'offgrid': off,
            'offgrid_tex': {k: tex_ids(dungeon_gen.room_textures(romfs, k)) for k in off},
            'base_tex': tex_ids(dungeon_gen._base_tex(romfs, lv, dungeon_gen.ENTRANCES[lv])),
            'cells': sorted(dungeon_gen.level_cells(romfs, lv)),
            'stairs': [list(x) for x in room_logic.stairs_links(romfs, lv, cat)],
            'holes': [list(x) for x in room_logic.hole_links(romfs, lv, cat)],
        }
    out['inward'] = [[k[0], k[1], k[2], v] for k, v in sorted(dungeon_gen.inward_only(romfs).items())]
    out['map_anims'] = {k: v for k, v in dungeon_gen.map_anims(romfs).items() if k.startswith('Lv')}
    out['map_shifts'] = {str(k): list(v) for k, v in sorted(dungeon_gen.map_shifts(romfs).items())}
    out['tex_names'] = len(names)
    return json.dumps(out, ensure_ascii=True, separators=(',', ':'))


def c_chunks(text, n=8000):
    out = []
    for i in range(0, len(text), n):
        s = text[i:i + n].replace('\\', '\\\\').replace('"', '\\"')
        out.append(f'    "{s}",')
    return out


def cpp(text, func, what):
    L = [f'// Generated by tools/rando/export_room_data.py: {what}. Don\'t edit: rerun the script.',
         '#include <string>', '', 'namespace rando {', 'namespace solve {', '',
         'static const char *const kChunks[] = {']
    L += c_chunks(text)
    L += ['};', '', f'std::string {func}() {{', '    std::string s;', f'    s.reserve({len(text)});',
          '    for (const char *c : kChunks) s += c;', '    return s;', '}', '', '}  // namespace solve',
          '}  // namespace rando', '']
    return '\n'.join(L)


def main():
    args = sys.argv[1:]
    romfs = args[args.index('--romfs') + 1] if '--romfs' in args else None
    outs = [(OUT, cpp(static_json(), 'room_data_json', "tools/rando/room_catalog.json, location_rooms.json, "
                                                         "room_overrides.py's doors and souls.exit_req, as JSON"))]
    if romfs:
        outs.append((OUT_GAME, cpp(game_json(os.path.abspath(romfs)), 'room_game_json',
                                   "what dungeon_gen.py / room_logic.py work out from the game's rooms (areas, "
                                   "doors, stairs, landing spots, textures, map pages), as JSON")))
    bad = 0
    for path, text in outs:
        if '--check' in args:
            old = open(path).read() if os.path.exists(path) else ''
            if old != text:
                print(f'{os.path.relpath(path, ROOT)} is out of date: run tools/rando/export_room_data.py')
                bad = 1
            else:
                print(f'{os.path.relpath(path, ROOT)}: up to date')
            continue
        with open(path, 'w') as f:
            f.write(text)
        print('wrote', os.path.relpath(path, ROOT), len(text), 'bytes of JSON')
    return bad


if __name__ == '__main__':
    sys.exit(main())
