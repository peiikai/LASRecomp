#!/usr/bin/env python3
"""Writes runtime/rando/solve/solve_data.cpp: the data the native solver's preparation (runtime/rando/solve/prepare.h,
docs/randomizer.md stage 5) starts from - LAS-Randomizer's logic.yml and items.yml (MIT, Owen-Splat and contributors)
as yaml.safe_load gives them, its location groups, las_headless.py's default settings, and the tables of our
tools/rando modules that las_headless.main() runs before the item fill (pots, bushes, rocks, digs, apple trees, souls,
shop / Trendy checks, pets, Triforce hunt, the Field regions). One JSON document, dicts in the Python's order, parsed
by the C++ (solve::Value) when first used.

With --romfs <dir> it also writes runtime/rando/solve/solve_gate_data.cpp: what souls.vanilla_gate works out for the
vanilla dungeon layouts (room_logic's room graph from the game's rooms, PYTHONHASHSEED=0 as the generator runs):
per dungeon, the soul atoms each cell needs (none of the game's data: cell names and logic atoms) and the boss arena
cell. That one needs the game's files, so --check only checks it when --romfs is given.

    <LAS venv>/bin/python tools/rando/export_solve_data.py [--romfs <dir>]          # rewrite the file(s)
    <LAS venv>/bin/python tools/rando/export_solve_data.py [--romfs <dir>] --check  # exit 1 when out of date

Needs PyYAML (the LAS venv: third_party/LAS-Randomizer/venv).
"""
import ast
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
LAS = os.path.join(ROOT, 'third_party', 'LAS-Randomizer')
DATA = os.path.join(LAS, 'RandomizerCore', 'Data')
OUT = os.path.join(ROOT, 'runtime', 'rando', 'solve', 'solve_data.cpp')
OUT_GATE = os.path.join(ROOT, 'runtime', 'rando', 'solve', 'solve_gate_data.cpp')
sys.path.insert(0, HERE)


def las_data():
    """As RandomizerCore/randomizer_data.py loads them."""
    import yaml
    with open(os.path.join(DATA, 'items.yml')) as f:
        items = yaml.safe_load(f)['Item_Pool']
    with open(os.path.join(DATA, 'logic.yml')) as f:
        f.readline()  # (LOGIC_VERSION)
        logic = yaml.safe_load(f.read())
    with open(os.path.join(DATA, 'locations.yml')) as f:
        locs = yaml.safe_load(f)
    groups = {'FISHING_REWARDS': locs['Fishing_Rewards'], 'RAPIDS_REWARDS': locs['Rapids_Rewards'],
              'DAMPE_REWARDS': locs['Dampe_Rewards'], 'TRADE_GIFT_LOCATIONS': locs['Trade_Gifts'],
              'BLUE_RUPEES': locs['Blue_Rupees']}
    for k, v in groups.items():
        assert isinstance(v, list), k  # (set.update / difference_update take them in list order)
    return items, logic, groups


def headless_tables():
    """default_settings()'s dict (its 'excluded-locations' is built in C++) and ROOM_MODES, read from the source."""
    src = open(os.path.join(HERE, 'las_headless.py')).read()
    tree = ast.parse(src)
    out = {}
    for node in tree.body:
        if isinstance(node, ast.FunctionDef) and node.name == 'default_settings':
            d = node.body[-1].value
            assert isinstance(d, ast.Dict)
            defaults = {}
            for k, v in zip(d.keys, d.values):
                key = ast.literal_eval(k)
                if key in ('seed', 'excluded-locations'):
                    defaults[key] = None  # (filled in by the C++: str(seed), the set)
                else:
                    defaults[key] = ast.literal_eval(v)
            out['defaults'] = defaults
        if isinstance(node, ast.Assign) and getattr(node.targets[0], 'id', None) == 'ROOM_MODES':
            out['room_modes'] = ast.literal_eval(node.value)
    # the excluded set's sources, in default_settings' order
    out['default_excluded'] = ['FISHING_REWARDS', 'RAPIDS_REWARDS', 'DAMPE_REWARDS', 'TRADE_GIFT_LOCATIONS',
                               'BLUE_RUPEES', ['30-seashell-reward', '40-seashell-reward', '50-seashell-reward']]
    assert "excluded.update(['30-seashell-reward', '40-seashell-reward', '50-seashell-reward'])" in src
    return out


def hexkey(k):
    return '%016X' % k


def module_tables():
    import apple_trees
    import bushes
    import digs
    import eagle_tower
    import field_regions
    import hookshot_logic
    import pets
    import pots
    import rocks
    import shopsanity
    import souls
    import triforce
    import trendsanity
    import yaml
    with open(os.path.join(HERE, 'entrance_data.yml')) as f:
        entrance_data = yaml.safe_load(f)
    return {
        'hookshot_routes': hookshot_logic.ROUTES,
        'eagle': {'collapsed_2f': eagle_tower.COLLAPSED_2F, 'missable_collapsed': list(eagle_tower.MISSABLE_ROOMS_COLLAPSED)},
        'souls': {'bosses': [list(b) for b in souls.BOSSES], 'minibosses': [[m, t, k, list(l)] for m, t, k, l in souls.MINIBOSSES],
                  'junk': list(souls.JUNK), 'model': souls.MODEL},
        'pots': {'types': sorted(pots.POT_TYPES), 'skip_level_prefix': pots.SKIP_LEVEL_PREFIX,
                 'skip_levels': list(pots.SKIP_LEVELS), 'dungeons': pots.DUNGEONS, 'vanilla_rooms': pots.VANILLA_ROOMS,
                 'special': pots.SPECIAL, 'junk_mix': pots.JUNK_MIX, 'filler_defs': pots.FILLER_DEFS,
                 'traps': pots.POT_TRAPS, 'missable': list(pots.MISSABLE_ROOMS),
                 # (pots._door_logic: dest level -> [outer region, condition])
                 'doors': pots._door_logic(entrance_data)},
        'bushes': {'type': bushes.BUSH_TYPE, 'junk_mix': bushes.JUNK_MIX,
                   'open_mabe_removed': sorted(hexkey(k) for k in bushes.OPEN_MABE_REMOVED)},
        'rocks': {'type': rocks.ROCK_TYPE, 'golden_leaf': hexkey(rocks.GOLDEN_LEAF_ROCK),
                  'classic_d2': hexkey(rocks.CLASSIC_D2_ROCK), 'screens': rocks.SCREENS, 'border': rocks.BORDER,
                  'junk_mix': rocks.JUNK_MIX},
        'digs': {'junk_mix': digs.JUNK_MIX, 'filler_defs': digs.FILLER_DEFS, 'open_actors': list(digs.OPEN_ACTORS),
                 'lawn': digs.LAWN_TYPE, 'cut': digs.CUT, 'rank': digs.RANK, 'shell_type': 0x87,
                 'verified': sorted(digs.verified_tiles() or [])},
        'apples': {'tree': apple_trees.T_TREE, 'slots': apple_trees.SLOTS, 'stand': apple_trees.STAND,
                   'filler': apple_trees.FILLER, 'filler_defs': apple_trees.FILLER_DEFS},
        'field': {'screens': field_regions.SCREENS, 'zone_area': {str(k): v for k, v in field_regions.ZONE_AREA.items()},
                  'region_area': field_regions.REGION_AREA},
        'shop': {'checks': shopsanity.CHECKS, 'dropped': shopsanity.DROPPED, 'door': shopsanity.SHOP_DOOR},
        'trendy': {'checks': trendsanity.CHECKS, 'dropped': trendsanity.DROPPED, 'final': trendsanity.FINAL,
                   'door': trendsanity.DOOR},
        'pets': {'pets': pets.PETS, 'order': pets.ORDER},
        'triforce': {'las_name': triforce.LAS_NAME, 'item_key': triforce.ITEM_KEY, 'model': triforce.MODEL},
    }


def generate_json():
    items, logic, groups = las_data()
    data = {'logic': logic, 'items': items, 'groups': groups,
            'stealing': ['always', 'never', 'normal']}
    sys.path.insert(0, os.path.join(LAS))
    data.update(headless_tables())
    data.update(module_tables())
    # (randomizer_data.STEALING_REQUIREMENTS, read from the source: importing it needs the LAS paths)
    src = open(os.path.join(LAS, 'RandomizerCore', 'randomizer_data.py')).read()
    for node in ast.parse(src).body:
        if isinstance(node, ast.Assign) and getattr(node.targets[0], 'id', None) == 'STEALING_REQUIREMENTS':
            data['stealing'] = list(ast.literal_eval(node.value))
    return json.dumps(data, ensure_ascii=True, separators=(',', ':'))


def gate_json(romfs):
    """souls.vanilla_gate's room-graph part for every vanilla dungeon, with and without hookshot-anything."""
    assert os.environ.get('PYTHONHASHSEED') == '0', 'run with PYTHONHASHSEED=0 (as the generator)'
    import types
    sys.path.insert(0, LAS)
    sys.modules.setdefault('PySide6', types.ModuleType('PySide6'))
    sys.modules.setdefault('PySide6.QtCore', types.ModuleType('PySide6.QtCore'))
    import dungeon_gen
    import room_logic
    import souls
    cat = json.load(open(os.path.join(HERE, 'room_catalog.json')))
    room_logic.EXIT_REQ.clear()
    room_logic.EXIT_REQ.update(souls.exit_req(cat))
    gates = set().union(*room_logic.EXIT_REQ.values())
    out = {'levels': [lv for lv in dungeon_gen.ENTRANCES if lv != 'Lv07EagleTower'], 'need': {}, 'arena': {}}
    for hook in (False, True):
        room_logic.HOOK_ANYTHING = hook
        need_all = {}
        for lv in out['levels']:
            layout = {k.rsplit('_', 1)[1]: k for k in cat if k.startswith(lv + '_')}
            edges, _ = room_logic.level_graph(cat, romfs, lv, layout, [], dungeon_gen.openings)
            start = room_logic.entrance_node(cat, romfs, lv, layout)
            terms = room_logic.cell_terms(room_logic.reach_terms(edges, start, room_logic.LEVELS[lv][1]))
            need = {}
            for cell, ts in terms.items():
                souls_in = [{a for a in t if a in gates} for t in ts]
                if souls_in:
                    common = set.intersection(*souls_in)
                    if common:
                        need[cell] = sorted(common)
            need_all[lv] = dict(sorted(need.items()))
            if not hook:  # (the arena: room_logic.py's boss_cell, the first boss door's other side)
                for cell, room in layout.items():
                    for d in cat.get(room, {}).get('doors', []):
                        if len(d) > 3 and d[2] == 'bossdoor' and lv not in out['arena']:
                            c, r = room_logic.cell_pos(cell)
                            dc, dr = room_logic.STEP[d[0]]
                            out['arena'][lv] = [room_logic.LEVELS[lv][0], f'{lv}_{room_logic.cell_name(c + dc, r + dr)}']
        out['need']['hook' if hook else 'plain'] = need_all
    room_logic.HOOK_ANYTHING = False
    lr = json.load(open(os.path.join(HERE, 'location_rooms.json')))
    out['targets'] = {k: v.get('room') for k, v in lr.items()}
    return json.dumps(out, ensure_ascii=True, separators=(',', ':'))


def c_chunks(text, n=8000):
    out = []
    for i in range(0, len(text), n):
        s = text[i:i + n].replace('\\', '\\\\').replace('"', '\\"')
        out.append(f'    "{s}",')
    return out


def cpp(text, func, what):
    L = [f'// Generated by tools/rando/export_solve_data.py: {what}. Don\'t edit: rerun the script.',
         '#include <string>', '', 'namespace rando {', 'namespace solve {', '',
         'static const char *const kChunks[] = {']
    L += c_chunks(text)
    L += ['};', '',
          f'std::string {func}() {{',
          '    std::string s;',
          f'    s.reserve({len(text)});',
          '    for (const char *c : kChunks) s += c;',
          '    return s;',
          '}', '', '}  // namespace solve', '}  // namespace rando', '']
    return '\n'.join(L)


def main():
    args = sys.argv[1:]
    romfs = args[args.index('--romfs') + 1] if '--romfs' in args else None
    outs = [(OUT, cpp(generate_json(), 'solve_data_json',
                      "LAS-Randomizer's logic.yml / items.yml / location groups (MIT, Owen-Splat and contributors), "
                      'las_headless.py\'s defaults and the tables of tools/rando\'s pre-fill modules, as JSON'))]
    if romfs:
        outs.append((OUT_GATE, cpp(gate_json(os.path.abspath(romfs)), 'solve_gate_json',
                                   "souls.vanilla_gate's soul atoms per vanilla dungeon cell (room_logic's room graph) "
                                   'and location_rooms.json, as JSON')))
    bad = 0
    for path, text in outs:
        if '--check' in args:
            old = open(path).read() if os.path.exists(path) else ''
            if old != text:
                print(f'{os.path.relpath(path, ROOT)} is out of date: run tools/rando/export_solve_data.py', file=sys.stderr)
                bad = 1
        else:
            with open(path, 'w') as f:
                f.write(text)
    return bad


if __name__ == '__main__':
    sys.exit(main())
