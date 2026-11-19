"""Decisions recorded for the ours world steps (tools/rando/record.py; docs/randomizer.md stage 3).

install(): hooks into the generator (once, before it runs); add(d, mods): adds this group's fields to decisions.json.

The steps (runtime/rando/apply/ours_world.cpp) read, besides the placements, settings and trap models, the field
"ours-world" (each key only when its step ran):
  pots: [{prefix, locs: [[location, level, room, actor key (hex)]]}]
                       each pots.apply call in order (pots LARPot, bushes LARBush, rocks LARRock): its locations in the
                       order the solver made them (pots.scan / bushes.scan / rocks.scan, the logic's choice of rooms)
  digs: [[location, room, tile, ground height, blocker or null, seashell index or null]]
                       the digsanity checks (digs.add_to_logic), by tile id
  shell-spots: {vanilla, spots: [[location, seashell index, kind, level, room, host key (hex), parameter]]}
                       the Secret Seashell spots left to seashell_spots.apply (by seashell index) and where each is
                       (seashell_spots.scan)
  apple-looks: {location: [model path, model name]}
                       the looks a trap in an apple tree takes (apple_trees.apply's random.Random(f'{seed}-apple-looks'))
  entrance-rooms: [[room, [[loading zone n, [parameters]]]]]
                       entrance_logic.apply_rooms: each room it writes, in order, and the loading zones it changed
  raft-shop-moved: true when the raft shop's exit is shuffled (entrance_logic.raft_shop_to_dock edits the flow)
  environments: [room type]
                       environments.apply's draws (random.Random(f'{seed}-environments')), in order: it runs after the
                       decisions are written, so add() makes the same draws from the same files (nothing writes a level
                       config in between)
"""
import functools

OURS = {}


def _hook(mod, name, wrap):
    real = getattr(mod, name)
    if getattr(real, '_ours_world', False):
        return
    w = functools.wraps(real)(wrap(real))
    w._ours_world = True
    setattr(mod, name, w)


def _pots(real):
    def apply(locs, *a, flag_prefix='LARPot', **k):
        OURS.setdefault('pots', []).append({'prefix': flag_prefix, 'locs': [
            [name, p['level'], p['room'], f"{p['key']:x}"] for name, p in locs.items()]})
        return real(locs, *a, flag_prefix=flag_prefix, **k)
    return apply


def _digs(real):
    def apply(locs, digs, *a, **k):
        import digs as _d
        OURS['digs'] = [[name, d['room'], d['tile'], d['y'], d['blocker'], d.get('shell')]
                        for name, d in sorted(locs.items(), key=lambda kv: _d.tile_id(kv[1]))]
        return real(locs, digs, *a, **k)
    return apply


def _shell_scan(real):
    def scan(*a, **k):
        r = real(*a, **k)
        OURS['_shell-scan'] = r
        return r
    return scan


def _shells(real):
    def apply(locs, *a, vanilla=False, **k):
        r = real(locs, *a, vanilla=vanilla, **k)
        spots = OURS.pop('_shell-scan', {})
        OURS['shell-spots'] = {'vanilla': bool(vanilla), 'spots': [
            [name, index] + ([spots[index][0], spots[index][1], spots[index][2], f'{spots[index][6]:x}',
                              spots[index][7]] if index in spots else [])
            for name, index in sorted(locs.items(), key=lambda kv: kv[1])]}
        return r
    return apply


def _apples(real):
    def apply(locs, mods, *a, **k):
        # (the same draws as apply's own stream: a trap hanging in a tree looks like a real item of the seed)
        import random
        looks = sorted({(d['model-path'], d['model-name']) for it, d in mods.item_defs.items()
                        if it in set(v for v in mods.placements.values() if isinstance(v, str))
                        and d.get('model-path') and not d.get('item-key', '').endswith('Trap')})
        rng = random.Random(f'{mods.seed}-apple-looks')
        picks = {}
        for name in sorted(locs):
            if mods.item_defs[mods.placements[name]]['item-key'].endswith('Trap') and looks:
                picks[name] = list(rng.choice(looks))
        OURS['apple-looks'] = picks
        return real(locs, mods, *a, **k)
    return apply


def _zones(room):
    return [list(a.parameters) for a in room.actors if a.type == 0x190]


def _entrances(real):
    def apply_rooms(doors, entry, exit_, read_room, write_room, *a, **k):
        rooms = OURS.setdefault('entrance-rooms', [])

        def write(name, room):  # (the zones it changed: the room as read before, as apply_rooms reads it)
            before = _zones(read_room(name))
            changed = [[i, [p.decode('utf-8') if isinstance(p, bytes) else p for p in z]]
                       for i, z in enumerate(_zones(room))
                       if [(type(p), p) for p in z] != [(type(p), p) for p in before[i]]]
            rooms.append([name, changed])
            return write_room(name, room)
        return real(doors, entry, exit_, read_room, write, *a, **k)
    return apply_rooms


def _raft(real):
    def raft_shop_to_dock(exit_, *a, **k):
        OURS['raft-shop-moved'] = exit_.get('raft-shop', 'raft-shop') != 'raft-shop'
        return real(exit_, *a, **k)
    return raft_shop_to_dock


def install():
    import apple_trees
    import digs
    import entrance_logic
    import pots
    import seashell_spots
    _hook(pots, 'apply', _pots)
    _hook(digs, 'apply', _digs)
    _hook(seashell_spots, 'scan', _shell_scan)
    _hook(seashell_spots, 'apply', _shells)
    _hook(apple_trees, 'apply', _apples)
    _hook(entrance_logic, 'apply_rooms', _entrances)
    _hook(entrance_logic, 'raft_shop_to_dock', _raft)


def _environments(mods):
    """environments.apply's draws, as it makes them (it reads the same level configs: none is written after this)."""
    import os
    import random
    import environments
    from RandomizerCore.Tools import lvb
    rng = random.Random(f"{mods.settings['seed']}-environments")
    base = os.path.join(mods.rom_path, 'region_common', 'level')
    picks = []
    for lv in sorted(os.listdir(base)):
        src = os.path.join(mods.romfs_dir, 'region_common', 'level', lv, lv + '.lvb')
        if not os.path.exists(src):
            src = os.path.join(base, lv, lv + '.lvb')
        if not os.path.exists(src):
            continue
        with open(src, 'rb') as f:
            level = lvb.Level(f.read())
        for z in level.zones:
            if z.room_type in environments.TOPDOWN_ENVIRONMENTS:
                picks.append(rng.choice(environments.TOPDOWN_ENVIRONMENTS))
            elif z.room_type in environments.SIDESCROLLER_ENVIRONMENTS:
                picks.append(rng.choice(environments.SIDESCROLLER_ENVIRONMENTS))
    return picks


def add(d, mods):
    if mods is not None and mods.settings.get('randomize-environments'):
        OURS['environments'] = _environments(mods)
    out = {k: v for k, v in OURS.items() if not k.startswith('_')}
    if out:
        d['ours-world'] = out
