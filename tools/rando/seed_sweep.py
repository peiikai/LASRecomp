#!/usr/bin/env python3
"""Seed sweep (LAS Recomp test tool, issue #144): generate many seeds and re-check each one's beatability against the
Field's own walkability (field_reach.py) - not only against the logic the seed was made with.

Each seed is made by las_headless.py with LARECOMP_CHECK_ONLY (item placement + its playthrough check, no game files
written). The second check repeats the playthrough with every bush / dig location also needing a region its tile can
actually be walked to from (field_reach.tile_regions): a seed the logic finishes but this doesn't is a bush / dig tile
the logic thinks is open too early - the circular dig-spot gates of #81 / #144. Seed IDs are encoded like
runtime/ui/rando_tab.mm (seed_id_encode, request.json in start_generation): keep the two in step.

usage: seed_sweep.py <romfs> [-n seeds] [-j jobs] [--id SEED-ID ...] [--keep dir] [--stealing M] [--shopsanity N]
                     [--souls] [--rooms N ...] [--triforce N] [--miniboss-freq F] [--entrances T] [--full] [--labyrinth]
       (default: the fixed seed IDs below + 40 random option mixes with digsanity on, every logic level)
--miniboss-freq F: every random mix randomizes enemies with mini-boss frequency F (0 - 3: Normal, 2x, 3x, All), enemy
mode Seeded or Random; --full: each seed is also generated in full (game files, enemies and all) and must finish -
including its room-link check (room_links.py: every actor link within each room sound, else the game crashes there).
--labyrinth: every random mix is a Labyrinth (tools/rando/labyrinth.py): room shuffle cycled Coupled / Decoupled
(unless --rooms), keysanity off / on alternating; its statistics (cross-wing links, wings only another wing leads to,
planning time) and a key-count sanity check are printed per seed.
Exit status 1 if any seed fails either check. Run it with the LAS venv's Python (third_party/LAS-Randomizer/venv).
"""
import argparse
import concurrent.futures
import json
import os
import random
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
LAS = os.path.join(ROOT, 'third_party', 'LAS-Randomizer')
# seeds reported unbeatable (made by older builds; with the fix they come out different, and must be beatable)
FIXED = ['MBHMM-13FR6-2000W-40GAZ-X700Z',  # #144: shovel in the Pothole Field, all 5 golden leaves buried
         'MBME8-68NR6-2000W-48GTZ-X700Y',  # #81: shovel next to the Tail Key chest, feather / rooster / powder buried
         '5F5HF-AB703-5RKZW-E4VTZ-9D100-0035Z']  # entrance shuffle: the Flippers in a toadstool clearing bush (bush-05A-1)

# ---- seed IDs (runtime/ui/rando_tab.mm)
B32 = '0123456789ABCDEFGHJKMNPQRSTVWXYZ'
FIELDS = [('logic', 3), ('dungeon_items', 2), ('keysanity', 2), ('traps', 3), ('seashells', 3), ('tunic', 4),
          ('entrances', 2), ('start_rupees', 10), ('room_shuffle', 2), ('shopsanity', 2)]
FLAGS = ['bomb_bag', 'powder_bag', 'environments', 'pets_in_dungeons', 'skip_story', 'enemy_minibosses', 'inc_fishing',
         'inc_rapids', 'inc_dampe', 'inc_trade', 'inc_blue', 'shuffle_instruments', 'shuffle_dungeons',
         'randomize_enemies', 'randomize_music', 'open_mabe', 'fast_songs', 'boss_cutscenes', 'spoiler', 'decoupled',
         'pots', 'no_buffs', 'bushes', 'pet_rooster', 'pet_ghost', 'pet_marin', 'trendsanity', 'boss_souls',
         'hookshot_any', 'digs', 'miniboss_souls', 'no_start_sword', 'no_start_shield', 'no_start_ocarina',
         'tunic_upgrades', 'shellsanity', 'race', 'random_cosmetics', 'labyrinth']
ROOM_IDS = {'off': 0, 'coupled': 2, 'decoupled': 3}  # room_shuffle in the ID (1: the retired Coherent)
BITS = 150  # (v5: 30 characters; v4 IDs had 125 - decode reads both)
TIMEOUT = 900  # (seconds per seed)


def _check(bits):
    h = 0x811c9dc5
    for b in bits[:len(bits) - 5]:
        h = ((h ^ b) * 0x01000193) & 0xffffffff
    return (h ^ (h >> 5) ^ (h >> 10) ^ (h >> 15)) & 31


def decode(text):
    bits = []
    for ch in text.upper().replace('-', '').replace(' ', ''):
        v = B32.index({'O': '0', 'I': '1', 'L': '1'}.get(ch, ch))
        bits += [(v >> k) & 1 for k in range(5)]
    n = len(bits)
    assert n in (BITS, 125) and sum(bits[-5 + k] << k for k in range(5)) == _check(bits), text
    pos = [0]

    def get(w):
        v = sum(bits[pos[0] + i] << i for i in range(w))
        pos[0] += w
        return v
    assert get(4) == (5 if n == BITS else 4), 'seed ID format 4 / 5 only'
    o = {'rng': get(36)}
    o.update((k, get(w)) for k, w in FIELDS)
    o.update((k, bool(get(1))) for k in FLAGS)
    pos[0] = n - 18  # (fixed places before the checksum: rando_tab.mm id_hunt .. id_mbfreq; v5 only: the hunt)
    o['triforce_needed'], o['triforce_extra'] = (get(6), get(2)) if n == BITS else (0, 0)
    pos[0] = n - 10
    o['steal_needs_sword'], o['no_stealing'] = bool(get(1)), bool(get(1))
    o['enemy_mode'], o['miniboss_freq'] = get(1), get(2)
    if o['room_shuffle'] == 1:  # (retired Coherent: Coupled, as rando_tab.mm decodes it)
        o['room_shuffle'] = 2
    return o


def encode(o):
    bits = []

    def put(v, w):
        bits.extend((int(v) >> i) & 1 for i in range(w))
    put(5, 4)
    put(o['rng'], 36)
    for k, w in FIELDS:
        put(o.get(k, 0), w)
    for k in FLAGS:
        put(o.get(k, False), 1)
    bits.extend([0] * (BITS - 5 - len(bits)))
    del bits[BITS - 18:]
    put(o.get('triforce_needed', 0), 6)
    put(o.get('triforce_extra', 0), 2)
    put(o.get('steal_needs_sword', False), 1)
    put(o.get('no_stealing', False), 1)
    put(o.get('enemy_mode', 0), 1)
    put(o.get('miniboss_freq', 0), 2)
    put(_check(bits + [0] * 5), 5)
    s = ''.join(B32[sum(bits[c * 5 + k] << k for k in range(5))] for c in range(BITS // 5))
    return '-'.join(s[i:i + 5] for i in range(0, len(s), 5))


def request(sid, o, logic='basic'):
    """request.json as rando_tab.mm start_generation writes it (logic: the UI always says basic)."""
    shells = ['5', '15', '30', '40', '50']
    start = (['sword'] if not o['no_start_sword'] else []) + (['shield'] if not o['no_start_shield'] else []) + \
        (['ocarina', 'song-mambo'] if not o['no_start_ocarina'] else [])
    mini = o['randomize_enemies'] and o['enemy_minibosses']
    tier = min(o['entrances'], 2)
    return {
        'seed': sid, 'logic': logic, 'dungeon-items': ['none', 'stone-beak', 'mc', 'mcb'][o['dungeon_items']],
        'traps': ['none', 'few', 'several', 'many', 'trapsanity'][o['traps']],
        'shuffle-instruments': o['shuffle_instruments'],
        'shuffle-dungeons': o['shuffle_dungeons'] and not (o.get('labyrinth') and o['room_shuffle']),
        'randomize-enemies': o['randomize_enemies'], 'randomize-music': o['randomize_music'],
        'open-mabe': o['open_mabe'], 'fast-songs': o['fast_songs'], 'OHKO': False,
        'boss-cutscenes': o['boss_cutscenes'], 'create-spoiler': o['spoiler'], 'starting-rupees': o['start_rupees'],
        'blur-removal': False, 'starting-items': start, 'pots': o['pots'], 'no-buff-drops': o['no_buffs'],
        'bushes': o['bushes'], 'digs': o['digs'], 'tunic-upgrades': o['tunic_upgrades'],
        'shellsanity': o['shellsanity'],
        'include-fishing': o['inc_fishing'], 'include-rapids': o['inc_rapids'], 'include-dampe': o['inc_dampe'],
        'include-trade-gifts': o['inc_trade'], 'include-blue-rupees': o['inc_blue'],
        'shuffle-bombs': o['bomb_bag'], 'unlocked-bombs': not o['bomb_bag'], 'shuffle-powder': o['powder_bag'],
        'randomize-environments': o['environments'], 'bad-pets': o['pets_in_dungeons'], 'skip-story-events': True,
        'tunic-color': 'default', 'enemy-minibosses': mini, 'miniboss-frequency': o['miniboss_freq'] if mini else 0,
        'enemy-mode': 'random' if o['randomize_enemies'] and o['enemy_mode'] else 'seeded',
        'keysanity': ['off', 'keys', 'keys+nk', 'all'][o['keysanity']],
        'dungeon-room-shuffle': ['off', 'coupled', 'coupled', 'decoupled'][o['room_shuffle']],  # (1: retired Coherent)
        'dungeon-labyrinth': bool(o.get('labyrinth')) and o['room_shuffle'] != 0,
        'shopsanity': ['off', 'vanilla', 'random'][o['shopsanity']], 'trendsanity': o['trendsanity'],
        'stealing': stealing(o),
        'boss-souls': o['boss_souls'], 'miniboss-souls': o['miniboss_souls'], 'hookshot-anything': o['hookshot_any'],
        'entrance-tier': tier, 'entrance-decoupled': tier >= 2 and o['decoupled'],
        'pet-rooster': o['pet_rooster'], 'pet-ghost': o['pet_ghost'], 'open-desert': not o['pet_marin'],
        'triforce-hunt': o.get('triforce_needed', 0), 'triforce-extra': o.get('triforce_extra', 0) * 25,
        'include': [f'{s}-seashell-reward' for s in shells],
        'exclude': [f'{s}-seashell-reward' for s in shells[o['seashells']:]]}


def stealing(o):
    """LAS 'stealing' as rando_tab.mm writes it (stealing off only with shopsanity)."""
    return 'never' if o['shopsanity'] and o['no_stealing'] else 'normal' if o['steal_needs_sword'] else 'always'


def random_options(rng):
    """Digsanity on, the rest mixed - the options that change what's reachable or where items can go."""
    o = {k: False for k in FLAGS}
    o.update({k: 0 for k, _ in FIELDS}, rng=rng.getrandbits(36), enemy_mode=0, miniboss_freq=0)
    o.update(digs=True, skip_story=True, fast_songs=True, spoiler=True, seashells=rng.randrange(6),
             dungeon_items=rng.randrange(4), keysanity=rng.randrange(4), traps=rng.randrange(5),
             entrances=rng.choice([0, 0, 1, 2, 2]), room_shuffle=rng.choice([0, 0, 0, 2, 3]),
             shopsanity=rng.randrange(3), steal_needs_sword=rng.random() < 0.5, no_stealing=rng.random() < 0.3)
    for k in ('bomb_bag', 'powder_bag', 'bushes', 'pots', 'open_mabe', 'decoupled', 'pet_rooster', 'pet_ghost',
              'pet_marin', 'trendsanity', 'boss_souls', 'miniboss_souls', 'hookshot_any', 'no_start_sword',
              'no_start_shield', 'no_start_ocarina', 'shuffle_instruments', 'shuffle_dungeons', 'inc_trade',
              'inc_dampe', 'inc_rapids', 'shellsanity'):
        o[k] = rng.random() < 0.4
    return o


# ---- the independent check
INSTRUMENTS = ['full-moon-cello', 'conch-horn', 'sea-lilys-bell', 'surf-harp', 'wind-marimba', 'coral-triangle',
               'evening-calm-organ', 'thunder-drum']
LEVELS = {'basic': ('basic',), 'advanced': ('basic', 'advanced'), 'glitched': ('basic', 'advanced', 'glitched'),
          'hell': ('basic', 'advanced', 'glitched', 'hell')}


def _compile(cond):
    py = re.sub(r'([a-zA-Z0-9\-\[\]]+)(:(\d+))?', lambda m: f'(a.get("{m.group(1)}", 0) >= {m.group(3) or 1})', cond)
    return compile(py.replace('|', ' or ').replace('&', ' and ').replace('!', ' not '), '<logic>', 'eval')


def playthrough(logic, placements, access, level, extra=None):
    """LAS's checkAccess + las_headless's spheres; extra: {location: condition} ANDed on. -> access after all."""
    sys.path.insert(0, HERE)
    import playthrough as pt
    code = {}

    def ev(cond, a):
        c = code.get(cond)
        if c is None:
            c = code[cond] = _compile(cond)
        return eval(c, {}, {'a': a})

    def check(key, a):
        node = logic[re.match('[a-zA-Z0-9-]+', key).group(0)]
        if node['type'] == 'enemy':
            param = re.search(r'\[([a-z]+)\]', key)
            if param and ev(node['condition-' + param.group(1)], a):
                return True
        elif node['type'] in ('item', 'follower') and not a.get(node['region']):
            return False
        if extra and key in extra and not ev(extra[key], a):
            return False
        if node['type'] != 'enemy' and 'condition-basic' not in node:
            return True
        return any(f'condition-{lv}' in node and ev(node[f'condition-{lv}'], a) for lv in LEVELS[level])
    _, a, seen = pt.spheres(logic, placements, dict(access), check)
    return a, seen


def check_seed(out_dir, level, walk, needed=0):
    logic = json.load(open(os.path.join(out_dir, 'logic.json')))
    for k, v in {'D6-4D': 'D6-5D', 'D6-2A': 'D6-4D'}.items():  # (las_headless: rooms LAS's logic never defines)
        logic.setdefault(k, {'type': 'room', 'condition-basic': v})
    p = json.load(open(os.path.join(out_dir, 'placements.json')))
    # (walk: (region, fallback) pairs - a pocket's own region with the entrance shuffle, else its door's region)
    extra = {name: ' | '.join(sorted({r if r in logic else fb for r, fb in walk[name]}))
             for name in logic if name in walk}
    a, seen = playthrough(logic, p['placements'], p['access'], level)
    def missing(acc):  # (Triforce hunt: the pieces are the goal)
        if needed:
            return [] if acc.get('triforce-piece', 0) >= needed else [f"triforce {acc.get('triforce-piece', 0)}/{needed}"]
        return [i for i in INSTRUMENTS + ['kill-nightmare'] if not acc.get(i)]
    own = missing(a)
    a2, seen2 = playthrough(logic, p['placements'], p['access'], level, extra)
    strict = missing(a2)
    lost = sorted(k for k in seen - seen2 if k in extra and p['placements'].get(k)
                  and not re.match(r'(pot|dig)-|rupee|heart$|seashell|trap', p['placements'][k]))
    return own, strict, [(k, p['placements'][k], extra[k]) for k in lost]


def labyrinth_check(out_dir):
    """A Labyrinth seed's statistics + key-count sanity: every small key is the shared kind, as many as the
    labyrinth's locks; with keysanity off all of them in its checks; each wing's map / compass / Nightmare Key / beak
    in that wing (unless shuffled anywhere). -> (stats dict, [problems])"""
    p = os.path.join(out_dir, 'labyrinth.json')
    if not os.path.exists(p):
        return None, []
    info = json.load(open(p))
    pl = json.load(open(os.path.join(out_dir, 'placements.json')))['placements']
    logic = json.load(open(os.path.join(out_dir, 'logic.json')))
    probs = []
    keys = [(k, v) for k, v in pl.items() if re.match(r'key-D\d', v)]
    kinds = {v for _, v in keys}
    if kinds - {'key-D1'}:
        probs.append(f'small keys of another kind: {sorted(kinds)}')
    total = info.get('small-keys')
    if total is not None and len(keys) != total:
        probs.append(f'{len(keys)} small keys placed, {total} locks')
    if info.get('keysanity') == 'off':
        outside = [k for k, _ in keys if not re.match(r'D\d-', k) and not str(logic.get(k, {}).get('region', '')).startswith(
            ('labyrinth-wing-', 'eagle-tower'))]
        if outside:
            probs.append(f'small keys outside the labyrinth: {outside[:4]}')
    own = {'off': ('nightmare-key-', 'map-', 'compass-', 'stone-beak-'), 'keys': ('nightmare-key-', 'map-', 'compass-',
           'stone-beak-'), 'keys+nk': ('map-', 'compass-', 'stone-beak-')}.get(info.get('keysanity'), ())
    free = {'mc': ('map-', 'compass-'), 'mcb': ('map-', 'compass-', 'stone-beak-'),
            'stone-beak': ('stone-beak-',)}.get(info.get('dungeon-items'), ())
    for k, v in pl.items():
        if v.startswith(own) and not v.startswith(free) and re.search(r'-D\d$', v) and not k.startswith('starting-'):
            w = v[-1]
            region = logic.get(k, {}).get('region', '')
            if info.get('dungeon-of', {}).get(k, k[:2]) != f'D{w}' or region not in (f'labyrinth-wing-{w}', 'labyrinth') \
                    and not (w == '7' and region == 'eagle-tower'):
                probs.append(f'{v} outside its wing: {k} ({region})')
    return info, probs


def labyrinth_census(full_dir, romfs):
    """A full Labyrinth seed, counted from its written game files: the locks (locked doors by their 'opened' flag,
    keyhole blocks) of every cell the wings have (dungeon_map.tsv) + Eagle's Tower's vanilla keys must be the small keys
    placed; every row into another level must start and end at a cell that exists. -> [problems]"""
    sys.path.insert(0, LAS)
    from RandomizerCore.Tools import leb
    info = json.load(open(os.path.join(full_dir, 'labyrinth.json')))
    cells = {}
    for ln in open(os.path.join(full_dir, 'dungeon_map.tsv')):
        lv, cell = ln.split('\t')[:2]
        cells.setdefault(lv, set()).add(cell)
    locks = set()
    for lv, cs in cells.items():
        for cell in cs:
            p = os.path.join(full_dir, 'romfs', 'region_common', 'level', lv, f'{lv}_{cell}.leb')
            for a in leb.Room(open(p, 'rb').read()).actors:
                flag = next((i for t, i in a.switches if t == 1), None)
                if a.type == 0xED:
                    locks.add((lv, 'door', flag))
                elif a.type == 0x102:
                    locks.add((lv, cell, 'block', flag))
    probs = []
    et = info.get('small-keys', 0) - info.get('spare-keys', 0) - len(locks)
    if et != 3:  # (Eagle's Tower: kept whole, its 3 vanilla keys; + the spares)
        probs.append(f'{len(locks)} locks in the wings + 3 in Eagle\'s Tower + {info.get("spare-keys", 0)} spare, '
                     f'{info.get("small-keys")} small keys')
    lp = os.path.join(full_dir, 'labyrinth_links.tsv')
    for ln in open(lp) if os.path.exists(lp) else ():
        if ln.startswith('#'):
            continue
        r = ln.rstrip('\n').split('\t')
        for lv, cell in ((r[0], r[1]), (r[9], r[5])):
            if not (lv == 'Lv07EagleTower' and cell == '08B') and cell not in cells.get(lv, ()):
                probs.append(f'labyrinth link to a missing cell {lv} {cell}')
    return probs


def run_one(job):
    sid, o, level, romfs, keep, walk, full = job
    tmp = tempfile.mkdtemp(prefix='sweep-', dir=keep)
    req = os.path.join(tmp, 'request.json')
    with open(req, 'w') as f:
        json.dump(request(sid, o, level), f)
    env = dict(os.environ, PYTHONHASHSEED='0', LARECOMP_CHECK_ONLY='1')
    import time
    t0 = time.time()
    try:
        r = subprocess.run([sys.executable, os.path.join(HERE, 'las_headless.py'), romfs, os.path.join(tmp, 'out'), req],
                           env=env, capture_output=True, text=True, timeout=TIMEOUT)
    except subprocess.TimeoutExpired:
        return sid, level, f'generator timeout ({TIMEOUT} s)', None
    if r.returncode != 0:
        tail = (r.stderr.strip().splitlines() or ['?'])[-1]
        return sid, level, f'generator exit {r.returncode}: {tail}', None
    secs = time.time() - t0
    own, strict, lost = check_seed(os.path.join(tmp, 'out'), level, walk, o.get('triforce_needed', 0))
    lab, lab_probs = labyrinth_check(os.path.join(tmp, 'out'))
    if lab is not None:
        m = re.search(r'- (\d+)/(\d+) item locations reachable', r.stdout)
        lab.update(seconds=round(secs, 1), reachable=m.group(0)[2:] if m else '?')
        if not m or m.group(1) != m.group(2):  # (every check of the seed reachable: none left out by the links)
            lab_probs = lab_probs + [f"unreachable checks: {lab['reachable']}"]
        lost = (lost or []) + [('labyrinth', lab, lab_probs)]
        if lab_probs:
            return sid, level, f'LABYRINTH KEYS/ITEMS {lab_probs[:3]}', lost
    if full and not own and not strict:  # (the whole seed: enemies, mini-bosses, every room edit)
        env = dict(os.environ, PYTHONHASHSEED='0')
        env.pop('LARECOMP_CHECK_ONLY', None)
        try:
            r = subprocess.run([sys.executable, os.path.join(HERE, 'las_headless.py'), romfs, os.path.join(tmp, 'full'),
                                req], env=env, capture_output=True, text=True, timeout=TIMEOUT)
        except subprocess.TimeoutExpired:
            return sid, level, f'full generation timeout ({TIMEOUT} s)', lost
        if r.returncode == 0:  # (room links: also checked here, should the generator's own check ever be skipped)
            sys.path.insert(0, os.path.join(HERE, '..', '..', 'third_party', 'LAS-Randomizer'))
            from RandomizerCore.Tools import leb
            import room_links
            problems = room_links.check(os.path.join(tmp, 'full'), leb, romfs)
            if problems:
                return sid, level, f'room links broken in {len(problems)} place(s), first: {problems[0]}', lost
            if os.path.exists(os.path.join(tmp, 'full', 'labyrinth.json')):
                problems = labyrinth_census(os.path.join(tmp, 'full'), romfs)
                if problems:
                    return sid, level, f'LABYRINTH CENSUS {problems[:3]}', lost
        if r.returncode != 0:
            tail = (r.stderr.strip().splitlines() or ['?'])[-1]
            return sid, level, f'full generation exit {r.returncode}: {tail}', lost
    if not keep:
        import shutil
        shutil.rmtree(tmp, ignore_errors=True)
    if own:
        return sid, level, f'UNBEATABLE (own logic) {own}', lost
    if strict:
        return sid, level, f'UNBEATABLE (walkability) {strict}', lost
    return sid, level, 'ok', lost


def walk_conditions(romfs):
    """{bush / dig location: set of (region, fallback) its tile can be walked to from} (field_reach; fallback: the
    region to use where the logic has no such region - a connector cave pocket without the entrance shuffle)."""
    sys.path.insert(0, HERE)
    sys.path.insert(0, LAS)
    from RandomizerCore.Tools import leb
    import field_reach
    tr = field_reach.tile_regions(romfs, leb)
    return {name: {(r, field_reach.OUTER.get(r, r)) for r in tr[t]}
            for name, t in field_reach.location_tiles(romfs, leb).items() if t in tr}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('romfs')
    ap.add_argument('-n', type=int, default=40)
    ap.add_argument('-j', type=int, default=max(1, (os.cpu_count() or 2) // 2))
    ap.add_argument('--id', action='append', default=[])
    ap.add_argument('--levels', default='basic,advanced,glitched,hell')
    ap.add_argument('--seed', type=int, default=144)
    ap.add_argument('--keep')
    ap.add_argument('--stealing', choices=['never', 'normal', 'always'], help='every random mix with this stealing rule')
    ap.add_argument('--shopsanity', type=int, choices=[0, 1, 2], help='every random mix with this shopsanity (off / '
                    'vanilla / random prices)')
    ap.add_argument('--miniboss-freq', type=int, choices=[0, 1, 2, 3], help='every random mix with enemies randomized, '
                    'mini-bosses at this frequency (Normal / 2x / 3x / All)')
    ap.add_argument('--entrances', type=int, choices=[0, 1, 2], help='every random mix with this entrance tier '
                    '(off / dungeons / + houses, caves & connector caves), bushsanity on')
    ap.add_argument('--full', action='store_true', help='also generate each seed in full (game files)')
    ap.add_argument('--souls', action='store_true', help='every random mix with boss and mini-boss souls on')
    ap.add_argument('--triforce', type=int, metavar='N', help='every random mix a Triforce hunt for N pieces '
                    '(extra pieces cycled: none / +25%% / +50%%)')
    ap.add_argument('--labyrinth', action='store_true', help='every random mix a Labyrinth (room shuffle cycled, '
                    'keysanity off / on alternating)')
    ap.add_argument('--rooms', choices=list(ROOM_IDS), action='append',
                    help='dungeon room shuffle of the random mixes (repeat: cycled)')
    args = ap.parse_args()
    romfs = os.path.abspath(args.romfs)
    if args.keep:
        os.makedirs(args.keep, exist_ok=True)
    walk = walk_conditions(romfs)
    levels = args.levels.split(',')
    rng = random.Random(args.seed)
    jobs = [(sid, decode(sid), levels[0]) for sid in (args.id or FIXED)]
    for i in range(args.n if not args.id else 0):
        o = random_options(rng)
        if args.shopsanity is not None:
            o['shopsanity'] = args.shopsanity
        if args.miniboss_freq is not None:
            o.update(randomize_enemies=True, enemy_minibosses=True, miniboss_freq=args.miniboss_freq,
                     enemy_mode=int(rng.random() < 0.5))
        if args.souls:
            o['boss_souls'] = o['miniboss_souls'] = True
        if args.entrances is not None:
            o['entrances'], o['bushes'] = args.entrances, True
        if args.triforce:
            o['triforce_needed'], o['triforce_extra'] = args.triforce, i % 3
        if args.rooms:
            o['room_shuffle'] = ROOM_IDS[args.rooms[(i // len(levels)) % len(args.rooms)]]  # (not in step with the levels)
        if args.labyrinth:
            o['labyrinth'] = True
            if not args.rooms:
                o['room_shuffle'] = 2 + i % 2  # (Coupled / Decoupled)
            o['keysanity'] = 0 if (i // 2) % 2 == 0 else rng.choice([1, 2, 3])
        if args.stealing:
            o['no_stealing'], o['steal_needs_sword'] = args.stealing == 'never', args.stealing == 'normal'
            if args.stealing == 'never' and not o['shopsanity']:
                sys.exit('--stealing never needs shopsanity (stealing off is a shopsanity sub-option)')
        jobs.append((encode(o), o, levels[i % len(levels)]))
    bad = 0
    stats = []
    with concurrent.futures.ProcessPoolExecutor(args.j) as ex:
        for sid, level, res, lost in ex.map(run_one, [j + (romfs, args.keep, walk, args.full) for j in jobs]):
            d = decode(sid)
            print(f"{sid} {level:9} shop {['off', 'vanilla', 'random'][d['shopsanity']]:7} steal {stealing(d):6} {res}",
                  flush=True)
            labs = [x for x in lost or [] if x[0] == 'labyrinth']
            for _, lab, _ in labs:
                stats.append(lab)
                print(f"    labyrinth {lab['mode']:8} keysanity {lab.get('keysanity'):7} cross-wing links "
                      f"{lab['cross-links']:3}, only through another wing {lab['only-through-another']}, partly "
                      f"{lab['partly-through-another']}, keys {lab.get('small-keys')}, plan {lab.get('plan-seconds')} s, "
                      f"seed {lab['seconds']} s, {lab['reachable']}", flush=True)
            for k, item, need in [x for x in lost or [] if x[0] != 'labyrinth'][:8]:
                print(f'    {k}: {item} - walkable only from {need}')
            bad += res != 'ok'
    print(f'{len(jobs) - bad}/{len(jobs)} seeds beatable')
    if stats:
        for mode in ('coupled', 'decoupled'):
            st = [x for x in stats if x['mode'] == mode]
            if st:
                avg = lambda k: sum(x[k] for x in st) / len(st)
                print(f"labyrinth {mode}: {len(st)} seeds, cross-wing links avg {avg('cross-links'):.1f} "
                      f"(min {min(x['cross-links'] for x in st)}, max {max(x['cross-links'] for x in st)}), wings only "
                      f"through another avg {sum(len(x['only-through-another']) for x in st) / len(st):.1f}, partly "
                      f"{sum(len(x['partly-through-another']) for x in st) / len(st):.1f}, plan {avg('plan-seconds'):.1f} s, "
                      f"seed {avg('seconds'):.1f} s")
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
