#!/usr/bin/env python3
"""Readable spoiler for a seed: rando/<seed>/spoiler_view.txt (shown in game, runtime/ui/spoiler_view.mm) and
rando/<seed>/spoiler.html (the same, for a browser).

  spoiler_view.py <seed dir> [...]

Sections: seed and settings, the Route (only what's needed to win, in an order that can be followed: route.json from
tools/rando/route.py), everything useful by playthrough step, every item by area (the tracker's check names and the
game's place names, areas in map order, small items folded away), dungeon entrances, shuffled dungeon rooms,
shuffled entrances, hints (Hints on). Seeds made before spheres.json existed are regenerated into build/ (same seed =
same placements) to compute the step list. Presentation only: nothing here changes a seed.

Markup read by the game: '# ' section, '## ' sub-heading, '### ' folded group inside a sub-heading (closed at first),
'- ' bullet, '  ' continuation, plain text otherwise. A line may end in TAB <location> TAB <flags> (not shown): the game
greys the line once the tracker has that check collected; flag 'u' = an item that opens things up.
"""
import html
import json
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))

DUNGEONS = {'tail-cave': 'Tail Cave (D1)', 'bottle-grotto': 'Bottle Grotto (D2)', 'key-cavern': 'Key Cavern (D3)',
            'angler-tunnel': "Angler's Tunnel (D4)", 'catfish-maw': "Catfish's Maw (D5)", 'face-shrine': 'Face Shrine (D6)',
            'eagle-tower': "Eagle's Tower (D7)", 'turtle-rock': 'Turtle Rock (D8)', 'color-dungeon': 'Color Dungeon'}
DPREFIX = {'D0': 'Color Dungeon', 'D1': 'Tail Cave', 'D2': 'Bottle Grotto', 'D3': 'Key Cavern', 'D4': "Angler's Tunnel",
           'D5': "Catfish's Maw", 'D6': 'Face Shrine', 'D7': "Eagle's Tower", 'D8': 'Turtle Rock'}
DKEY = {'D1': 'tail-cave', 'D2': 'bottle-grotto', 'D3': 'key-cavern', 'D4': 'angler-tunnel', 'D5': 'catfish-maw',
        'D6': 'face-shrine', 'D7': 'eagle-tower', 'D8': 'turtle-rock', 'D0': 'color-dungeon'}
LEVEL_D = {'Lv10ClothesDungeon': 'D0', 'Lv01TailCave': 'D1', 'Lv02BottleGrotto': 'D2', 'Lv03KeyCavern': 'D3',
           'Lv04AnglersTunnel': 'D4', 'Lv05CatfishsMaw': 'D5', 'Lv06FaceShrine': 'D6', 'Lv07EagleTower': 'D7',
           'Lv08TurtleRock': 'D8'}
AREAS = {'mabe-village': 'Mabe Village', 'toronbo-shores': 'Toronbo Shores', 'mysterious-woods': 'Mysterious Woods',
         'koholint-prairie': 'Koholint Prairie', 'tabahl-wasteland': 'Tabahl Wasteland', 'ukuku-prairie': 'Ukuku Prairie',
         'sign-maze': 'Signpost Maze', 'goponga-swamp': 'Goponga Swamp', 'taltal-heights': 'Tal Tal Heights',
         'marthas-bay': "Martha's Bay", 'kanalet-castle': 'Kanalet Castle', 'pothole-field': 'Pothole Field',
         'animal-village': 'Animal Village', 'yarna-desert': 'Yarna Desert', 'ancient-ruins': 'Ancient Ruins',
         'rapids-ride': 'Rapids Ride', 'taltal-mountains-east': 'Tal Tal Mountains (east)',
         'taltal-mountains-west': 'Tal Tal Mountains (west)', **DUNGEONS}
# the game's place names on the overworld map (tools/rando/tracker.py PLACE: tracker.json 'ar'), and their map order:
# north to south, west to east in each band
PLACE = ['Kanalet Castle', 'Rapids Ride', 'Southern Face Shrine', 'Animal Village', 'Yarna Desert', '', "Martha's Bay",
         'Pothole Field', '', '', 'Mabe Village', 'Mysterious Forest', 'Goponga Swamp', 'Koholint Prairie',
         'Tabahl Wasteland', 'Tal Tal Mountain Range', 'Mt. Tamaranch', 'Tal Tal Heights', 'Toronbo Shores',
         'Ukuku Prairie', 'Cemetery']
MAP_ORDER = [15, 16, 17, 14, 12, 0, 1, 11, 13, 20, 2, 10, 19, 7, 3, 18, 6, 4]
# LAS spoiler regions (no tracker data) in the same order
REGION_ORDER = ['taltal-mountains-west', 'taltal-mountains-east', 'taltal-heights', 'tabahl-wasteland', 'goponga-swamp',
                'kanalet-castle', 'rapids-ride', 'mysterious-woods', 'koholint-prairie', 'ancient-ruins', 'mabe-village',
                'ukuku-prairie', 'sign-maze', 'pothole-field', 'animal-village', 'toronbo-shores', 'marthas-bay',
                'yarna-desert']
DUNGEON_ORDER = ['D1', 'D2', 'D3', 'D4', 'D5', 'D6', 'D7', 'D8', 'D0']
ITEMS = {'rooster': 'Flying Rooster (pet)', 'ghost': 'Ghost (pet)', 'marin-companion': 'Marin (pet)',
         'seashell': 'Secret Seashell','heart-piece': 'Piece of Heart', 'heart-container': 'Heart Container',
         'rupee-5': '5 Rupees', 'rupee-20': '20 Rupees', 'rupee-50': '50 Rupees', 'rupee-100': '100 Rupees',
         'rupee-300': '300 Rupees', 'pot-rupee-5': '5 Rupees', 'pot-rupee-20': '20 Rupees', 'pot-rupee-50': '50 Rupees',
         'pot-rupee-100': '100 Rupees', 'dig-rupee-1': '1 Rupee', 'dig-rupee-5': '5 Rupees', 'dig-rupee-20': '20 Rupees',
         'pot-heart': 'Heart', 'pot-arrows': 'Arrows', 'pot-bombs': 'Bombs', 'pot-powder': 'Magic Powder',
         'bow-wow': 'BowWow', 'shell-sensor': 'Seashell Sensor', 'mermaid-scale': "Mermaid's Scale",
         'bracelet': 'Power Bracelet', 'feather': "Roc's Feather", 'boots': 'Pegasus Boots',
         'rod': 'Magic Rod', 'powder': 'Magic Powder', 'bomb': 'Bombs', 'bombs': 'Bombs', 'bow': 'Bow',
         'hookshot': 'Hookshot', 'boomerang': 'Boomerang', 'shovel': 'Shovel', 'flippers': 'Flippers',
         'ocarina': 'Ocarina', 'shield': 'Shield', 'sword': 'Sword', 'trap': 'Trap!', 'song-ballad': 'Ballad of the Wind Fish',
         'song-mambo': "Manbo's Mambo", 'song-soul': 'Frog Song of Soul', 'full-moon-cello': 'Full Moon Cello',
         'conch-horn': 'Conch Horn', 'sea-lilys-bell': "Sea Lily's Bell", 'surf-harp': 'Surf Harp',
         'wind-marimba': 'Wind Marimba', 'coral-triangle': 'Coral Triangle', 'evening-calm-organ': 'Evening Calm Organ',
         'thunder-drum': 'Thunder Drum', 'tail-key': 'Tail Key', 'slime-key': 'Slime Key', 'angler-key': 'Angler Key',
         'face-key': 'Face Key', 'bird-key': 'Bird Key', 'magnifying-lens': 'Magnifying Lens', 'yoshi-doll': 'Yoshi Doll',
         'ribbon': 'Ribbon', 'dog-food': 'Dog Food', 'bananas': 'Bananas', 'stick': 'Stick', 'honeycomb': 'Honeycomb',
         'pineapple': 'Pineapple', 'hibiscus': 'Hibiscus', 'letter': 'Letter', 'broom': 'Broom',
         'fishing-hook': 'Fishing Hook', 'necklace': 'Necklace', 'scale': "Mermaid's Scale", 'red-tunic': 'Red Tunic',
         'blue-tunic': 'Blue Tunic', 'golden-leaf': 'Golden Leaf', 'mushroom': 'Sleepy Toadstool',
         'medicine': 'Secret Medicine', 'bottle': 'Bottle', 'zol-trap': 'Trap!', 'bomb-capacity': 'Bomb bag upgrade',
         'arrow-capacity': 'Quiver upgrade', 'powder-capacity': 'Magic Powder upgrade', 'chamber-stone': 'Chamber Stone',
         'apple': 'Apple', 'green-apple': 'Green Apple', 'triforce-piece': 'Triforce piece'}
JUNK = re.compile(r'^((pot-|dig-)?rupee-\d+|pot-heart|apple|green-apple|pot-arrows|pot-bombs|pot-powder|trap|.*-trap|arrows?(-\d+)?|bombs?-\d+|powder-\d+)$')
PROGRESSIVE = ('sword', 'shield', 'bracelet')  # (a second copy is the upgrade: "Shield (2nd)")
# settings.json entries the generator adds for the runtime (not player options)
INTERNAL = {'create-spoiler', 'escape-bottle', 'hint-sheet', 'ghost-item', 'marin-stays', 'underwater-items',
            'silent-pickups'}
TAG = '\t'


def pretty_item(it):
    base = re.sub(r'\[\d+\]$', '', it)
    m = re.match(r'^(D\d)-(small-key|key|nightmare-key|compass|map|stone-beak|beak)$', base)
    if m:
        kind = {'small-key': 'Small Key', 'key': 'Small Key', 'nightmare-key': 'Nightmare Key', 'compass': 'Compass',
                'map': 'Dungeon Map', 'stone-beak': 'Stone Beak', 'beak': 'Stone Beak'}[m.group(2)]
        return f'{kind} ({DPREFIX[m.group(1)]})'
    m = re.match(r'^(small-key|key|nightmare-key|compass|map|stone-beak)-?(D\d)$', base)
    if m:
        return pretty_item(f'{m.group(2)}-{m.group(1)}')
    if base.startswith('soul-'):  # boss souls (tools/rando/souls.py)
        name = base[5:].replace('-', ' ').title()
        return name + ("' Soul" if name.endswith('s') else "'s Soul")
    return ITEMS.get(base) or base.replace('-', ' ').title()


def pretty_loc(loc):
    m = re.match(r'^(D\d)-(.*)$', loc)
    if m:
        rest = m.group(2)
        where = DPREFIX[m.group(1)]
        pm = re.match(r'^pot-(\d\d[A-H])-(\d+)$', rest)
        if pm:
            return f'{where}: pot {pm.group(2)} in room {pm.group(1)}'
        return f'{where}: ' + rest.replace('-', ' ')
    pm = re.match(r'^pot-([A-Za-z0-9]+)-(\d\d[A-H])-(\d+)$', loc)
    if pm:
        name = re.sub(r'(?<!^)(?=[A-Z])', ' ', pm.group(1))
        return f'{name}: pot {pm.group(3)}'
    pm = re.match(r'^apple-tree-(\d\d[A-P])-(\d)$', loc)  # (Applesanity)
    if pm:
        return f'Apple tree ({pm.group(1)}), {pm.group(2)} of 3'
    return loc.replace('-', ' ').capitalize()


def room_word(cell):
    """'03A' -> 'A3' (column letter, row number: as the game's dungeon map and the tracker say it)"""
    return f'{cell[2]}{int(cell[:2])}' if cell and len(cell) == 3 else cell


def parse_spoiler(path):
    sections, cur = {}, None
    for line in open(path):
        if re.match(r'^[a-z-]+:\s*$', line):
            cur = line.strip()[:-1]
            sections[cur] = []
        elif cur and line.startswith('    '):
            sections[cur].append(line.strip())
    return sections


def ensure_spheres(seed_dir):
    p = os.path.join(seed_dir, 'spheres.json')
    if os.path.exists(p):
        return json.load(open(p))
    req = os.path.join(seed_dir, 'request.json')
    if not os.path.exists(req):
        return None
    tmp = os.path.join(ROOT, 'build', 'spoiler_regen', os.path.basename(seed_dir))
    shutil.rmtree(tmp, ignore_errors=True)
    os.makedirs(tmp)
    py = os.path.join(ROOT, 'third_party', 'LAS-Randomizer', 'venv', 'bin', 'python')
    subprocess.run([py, os.path.join(HERE, 'las_headless.py'), os.path.join(ROOT, 'extracted', 'romfs'), tmp, req],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    src = os.path.join(tmp, 'spheres.json')
    if not os.path.exists(src):
        return None
    shutil.copy(src, p)
    return json.load(open(p))


def load_json(seed_dir, name, default=None):
    p = os.path.join(seed_dir, name)
    try:
        return json.load(open(p)) if os.path.exists(p) else default
    except ValueError:
        return default


class Seed:
    """Everything the text needs about one seed: where each location is (area, readable name) and what it holds."""

    def __init__(self, seed_dir, S, settings, sph):
        self.S, self.settings = S, settings
        self.loc_region, self.loc_item = {}, {}
        for sec, lines in S.items():
            if sec in AREAS:
                for l in lines:
                    if ':' in l:
                        loc, it = [x.strip() for x in l.split(':', 1)]
                        self.loc_region[loc], self.loc_item[loc] = sec, it
        tr = load_json(seed_dir, 'tracker.json', {})
        self.checks = {c['n']: c for c in tr.get('checks', ())}
        self.rooms_on = settings.get('dungeon-room-shuffle', 'off') not in ('off', None, False)
        self.entered_via = {}  # dungeon key -> the dungeon whose overworld door leads into it
        for l in S.get('dungeon-entrances', []):
            a, b = [x.strip() for x in l.split('->')]
            if a != b:
                self.entered_via[b] = a
        self.shells_important = bool(settings.get('seashells-important'))
        # progressive items: which copy each location holds (starting items first, then playthrough order)
        self.copy_no = {}
        count = {}
        for it in S.get('starting-items', []):
            b = re.sub(r'\[\d+\]$', '', it)
            count[b] = count.get(b, 0) + 1
        order = [loc for s in (sph or []) for loc, _ in s]
        order += sorted(set(self.loc_item) - set(order))
        for loc in order:
            b = re.sub(r'\[\d+\]$', '', self.loc_item.get(loc, ''))
            if b in PROGRESSIVE and not loc.startswith('starting-'):
                count[b] = count.get(b, 0) + 1
                self.copy_no[loc] = count[b]

    # --- where
    def area(self, loc):
        """-> (sort key, heading) of the area a location is listed under."""
        c = self.checks.get(loc, {})
        d = c.get('d') or (re.match(r'^(D\d)-', loc) or [None, None])[1]
        reg = self.loc_region.get(loc, '')
        if not d and reg in DUNGEONS:
            d = {v: k for k, v in DKEY.items()}[reg]
        if d in DKEY:
            return (1, DUNGEON_ORDER.index(d), ''), self.dungeon_title(DKEY[d])
        ar = c.get('ar', -1)
        if isinstance(ar, int) and 0 <= ar < len(PLACE) and PLACE[ar]:
            return (0, MAP_ORDER.index(ar) if ar in MAP_ORDER else 99, ''), PLACE[ar]
        if reg in AREAS:
            return (0, 50 + (REGION_ORDER.index(reg) if reg in REGION_ORDER else 49), ''), AREAS[reg]
        return (2, 0, ''), 'Elsewhere'

    def dungeon_title(self, dk):
        n = DUNGEONS[dk]
        if dk in self.entered_via:
            n += f" - enter through {DUNGEONS[self.entered_via[dk]].split(' (')[0]}'s entrance"
        return n

    def name(self, loc):
        c = self.checks.get(loc)
        if c and c.get('dn'):
            n = c['dn']
            if self.rooms_on and c.get('d') and c.get('dc'):  # (room shuffle: which room it's in now)
                n += f" (room {room_word(c['dc'])})"
            return n
        n = pretty_loc(loc)
        m = re.match(r'^(D\d)-', loc)
        if m and n.startswith(DPREFIX[m.group(1)] + ': '):  # (the heading names the dungeon)
            n = n[len(DPREFIX[m.group(1)]) + 2:]
            n = n[:1].upper() + n[1:]
        return n

    # --- what
    def item(self, loc, it=None):
        it = it if it is not None else self.loc_item.get(loc, '')
        n = pretty_item(it)
        if self.copy_no.get(loc, 1) == 2:
            n += ' (2nd)'
        return n

    def useful(self, it):
        b = re.sub(r'\[\d+\]$', '', it or '')
        return bool(b) and not JUNK.match(b) and not re.search(r'(compass|map|beak)', b) \
            and (self.shells_important or not b.startswith('seashell')) \
            and not b.startswith(('heart-piece', 'heart-container'))

    def line(self, loc, it=None, bullet='- '):
        it = it if it is not None else self.loc_item.get(loc, '')
        return f"{bullet}{self.name(loc)}  ->  {self.item(loc, it)}{TAG}{loc}{TAG}{'u' if self.useful(it) else ''}"


def natural(s):
    return [int(t) if t.isdigit() else t.lower() for t in re.split(r'(\d+)', s)]


def finish_line(settings):
    tri = int(settings.get('triforce-hunt', 0) or 0)
    if tri:
        return f'- The last of the {tri} Triforce pieces you need ends the game.'
    return '- With all eight instruments, play the Ballad of the Wind Fish at the egg on Tal Tal Heights.'


def section_route(sd, seed_dir, out):
    r = load_json(seed_dir, 'route.json')
    if not r or not r.get('spheres'):
        return
    out.append('')
    out.append('# Route')
    out.append('Only what you need to beat the seed. Each stop can be reached with the items from the stops before it.')
    stops = []  # [(title, [(loc, item)])]: the route's order, one stop per place in a row
    for stop in r['spheres']:
        for loc, it in stop:
            title = sd.area(loc)[1]
            if stops and stops[-1][0] == title:
                stops[-1][1].append((loc, it))
            else:
                stops.append((title, [(loc, it)]))
    for n, (title, items) in enumerate(stops, 1):
        head = f'## {n}. {title}'  # (the number stays as it is: spoiler_view.mm)
        out.append(head)
        for loc, it in items:  # (in order: a later one can need an earlier one, a dungeon's keys)
            out.append(sd.line(loc, it))
    out.append('## Finish')
    out.append(finish_line(sd.settings))


def section_steps(sd, sph, out):
    out.append('')
    out.append('# Everything useful step by step')
    if not sph:
        out.append('(This seed has no step list.)')
        return
    out.append('Each step can be done with what you had after the previous steps (any order within a step).')
    out.append('Only items that open things up are listed; rupees, traps and similar are left out.')
    n = 0
    for s in sph:
        keep = [(l, i) for l, i in s if not l.startswith('starting-') and sd.useful(i)]
        if not keep:
            continue
        n += 1
        out.append(f'## Step {n}')
        by_area = {}
        for l, i in keep:
            by_area.setdefault(sd.area(l), []).append((l, i))
        for (key, title) in sorted(by_area):
            out.append(f'- {title}:')
            for l, i in sorted(by_area[(key, title)], key=lambda x: natural(sd.name(x[0]))):
                out.append(sd.line(l, i, '  '))
    out.append('## Finish')
    out.append(finish_line(sd.settings))


def section_items(sd, out):
    out.append('')
    out.append('# Items by area')
    by_area = {}
    for loc, it in sd.loc_item.items():
        if loc.startswith(('starting-item', 'starting-dungeon-item')):
            continue
        by_area.setdefault(sd.area(loc), []).append(loc)
    for (key, title) in sorted(by_area):
        locs = sorted(by_area[(key, title)], key=lambda l: natural(sd.name(l)))
        small = [l for l in locs if JUNK.match(re.sub(r'\[\d+\]$', '', sd.loc_item[l]))]
        out.append(f'## {title}')
        for l in locs:
            if l not in small:
                out.append(sd.line(l))
        if small:  # (rupees, refills, traps: folded away)
            out.append(f'### Small items ({len(small)})')
            for l in small:
                out.append(sd.line(l))


def section_dungeons(sd, out):
    if not sd.S.get('dungeon-entrances'):
        return
    out.append('')
    out.append('# Dungeon entrances')
    for l in sd.S['dungeon-entrances']:
        a, b = [x.strip() for x in l.split('->')]
        out.append(f'- {DUNGEONS.get(a, a)} entrance leads into {DUNGEONS.get(b, b)}')


def section_rooms(sd, seed_dir, out):
    p = os.path.join(seed_dir, 'dungeon_rooms.tsv')  # (room shuffle: tools/rando/dungeon_gen.py)
    if not sd.rooms_on or not os.path.exists(p):
        return
    moved = {}
    for l in open(p):
        f = l.rstrip('\n').split('\t')
        if len(f) < 3 or f[0] not in LEVEL_D:
            continue
        lv, cell, room = f
        src_lv, _, src_cell = room.rpartition('_')
        if room == f'{lv}_{cell}' or src_lv not in LEVEL_D:
            continue
        moved.setdefault(LEVEL_D[lv], []).append((cell, LEVEL_D[src_lv], src_cell))
    if not moved:
        return
    out.append('')
    out.append('# Dungeon rooms')
    out.append('Where each moved room is now and where it came from.')
    for d in DUNGEON_ORDER:
        if d not in moved:
            continue
        out.append(f'## {DUNGEONS[DKEY[d]]}')
        for cell, sd_, sc in sorted(moved[d], key=lambda x: (x[0][2], x[0][:2])):
            src = DPREFIX[sd_]
            out.append(f'- Room {room_word(cell)}  ->  {src}, room {room_word(sc)}')


def section_entrances(sd, seed_dir, out):
    import entrance_names
    seed_id = sd.settings.get('seed', '')
    cands = [os.path.join(seed_dir, f'entrances_{seed_id}.txt'),
             os.path.join(seed_dir, f'entrances_{os.path.basename(os.path.normpath(seed_dir))}.txt')]
    cands += sorted(f for f in os.listdir(seed_dir) if f.startswith('entrances_') and f.endswith('.txt'))
    ent = next((c if os.path.isabs(c) else os.path.join(seed_dir, c) for c in cands
                if os.path.exists(c if os.path.isabs(c) else os.path.join(seed_dir, c))), None)
    if not ent:
        return
    doors, exits, mode = [], [], None
    for l in open(ent):
        if l.startswith('Entrances'):
            mode = 'in'
            continue
        if l.startswith('Exits'):
            mode = 'out'
            continue
        m = re.match(r'^\s+(\S+)\s+->\s+(\S+)', l)
        if m and mode:
            (doors if mode == 'in' else exits).append(m.groups())
    if not doors:
        return
    nm = entrance_names.name
    out.append('')
    out.append('# Entrances')
    shuffled = [(a, b) for a, b in doors if a != b]
    same = len(doors) - len(shuffled)
    out.append('## Doors (each door and where it now leads)')
    for a, b in sorted(shuffled, key=lambda x: natural(nm(x[0]))):
        out.append(f'- {nm(a)}  ->  {nm(b)}')
    if same:
        out.append(f'### Unchanged ({same})')
        for a, b in sorted(((a, b) for a, b in doors if a == b), key=lambda x: natural(nm(x[0]))):
            out.append(f'- {nm(a)}')
    back = {b: a for a, b in doors}
    odd = [(a, b) for a, b in exits if back.get(a) != b]  # (decoupled: leaving doesn't take you back out the same door)
    if odd:
        out.append('## Exits (each way out and where it puts you)')
        for a, b in sorted(odd, key=lambda x: natural(nm(x[0]))):
            out.append(f'- {nm(a)}  ->  {nm(b)}')


def build(seed_dir):
    seed = os.path.basename(os.path.normpath(seed_dir))
    sp = os.path.join(seed_dir, 'spoiler.txt')
    if not os.path.exists(sp):  # (seeds made before seed IDs: named after the folder)
        sp = os.path.join(seed_dir, f'spoiler_{seed}.txt')
    if not os.path.exists(sp):
        return None
    S = parse_spoiler(sp)
    settings = load_json(seed_dir, 'settings.json', {})
    sph = ensure_spheres(seed_dir)
    sd = Seed(seed_dir, S, settings, sph)

    out = [f'# Seed {seed}']
    if settings.get('seed') and settings['seed'] != seed:
        out.append(f"Seed ID: {settings['seed']}")
    on = sorted(k for k, v in settings.items() if v is True and k not in INTERNAL)
    out.append('Settings: ' + ', '.join(k.replace('-', ' ') for k in on))
    if 'starting-items' in S:
        out.append('Starting items: ' + ', '.join(pretty_item(i) for i in S['starting-items']))
    section_route(sd, seed_dir, out)
    section_steps(sd, sph, out)
    section_items(sd, out)
    section_dungeons(sd, out)
    section_rooms(sd, seed_dir, out)
    section_entrances(sd, seed_dir, out)
    hp = os.path.join(seed_dir, 'seed_hints.txt')  # (Hints on: what each owl statue and phone call says)
    if os.path.exists(hp):
        out.append('')
        out.append('# Hints')
        for l in open(hp):
            where, _, text = l.rstrip('\n').partition(': ')
            out.append(f'- {where}: {text}')
    with open(os.path.join(seed_dir, 'spoiler_view.txt'), 'w') as f:
        f.write('\n'.join(out) + '\n')
    write_html(out, seed, os.path.join(seed_dir, 'spoiler.html'))
    return len(out)


# ------------------------------------------------------------------ spoiler.html
CSS = """
:root{--bg:#f7f3ea;--fg:#2b2620;--mut:#7a7060;--line:#e2d9c6;--acc:#2f6f4f;--card:#fffdf8}
@media (prefers-color-scheme:dark){:root{--bg:#1d1b18;--fg:#ece6da;--mut:#a39a8a;--line:#3a352d;--acc:#7cc79f;--card:#25221e}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.45 -apple-system,system-ui,sans-serif}
main{max-width:860px;margin:0 auto;padding:16px}h1{font-size:22px;margin:8px 0}
.bar{position:sticky;top:0;background:var(--bg);padding:8px 0;display:flex;gap:12px;flex-wrap:wrap;align-items:center;border-bottom:1px solid var(--line);z-index:1}
input[type=search]{flex:1;min-width:200px;padding:7px 10px;border:1px solid var(--line);border-radius:8px;background:var(--card);color:var(--fg);font:inherit}
details{background:var(--card);border:1px solid var(--line);border-radius:10px;margin:10px 0;padding:6px 12px}
details details{border:none;background:none;padding:0 0 0 6px;margin:4px 0}
summary{cursor:pointer;font-weight:600;padding:4px 0}details details summary{font-weight:500}
ul{margin:4px 0 8px;padding-left:20px}li{margin:2px 0}li.sub{list-style:none;margin-left:-4px;color:var(--mut);font-weight:600}
.it{font-weight:600}.u .it{color:var(--acc)}p{margin:6px 0;color:var(--mut)}
#gate{text-align:center;padding:60px 16px}#gate button{font:inherit;padding:10px 22px;border-radius:10px;border:1px solid var(--acc);background:var(--acc);color:var(--bg);cursor:pointer}
.hide{display:none!important}
"""
JS = """
const q=document.getElementById('q'),u=document.getElementById('u');
function apply(){const s=q.value.trim().toLowerCase(),uo=u.checked;
document.querySelectorAll('li[data-l]').forEach(li=>{const t=li.textContent.toLowerCase();
li.classList.toggle('hide',(s&&!t.includes(s)&&!(li.dataset.h||'').includes(s))||(uo&&li.dataset.h&&!li.classList.contains('u')))});
document.querySelectorAll('li.sub').forEach(h=>{let n=h.nextElementSibling,any=false;
while(n&&!n.classList.contains('sub')){if(n.dataset.l!==undefined&&!n.classList.contains('hide'))any=true;n=n.nextElementSibling}h.classList.toggle('hide',!any)});
document.querySelectorAll('details').forEach(d=>{const lis=d.querySelectorAll('li[data-l]');if(!lis.length)return;
const vis=[...lis].some(li=>!li.classList.contains('hide'));d.classList.toggle('hide',!vis);if(s&&vis)d.open=true})}
q.addEventListener('input',apply);u.addEventListener('change',apply);
document.getElementById('show').onclick=()=>{document.getElementById('gate').classList.add('hide');document.getElementById('doc').classList.remove('hide')};
"""


def write_html(lines, seed, path):
    e = html.escape
    body, depth = [], {'sec': False, 'sub': False, 'grp': False, 'ul': False}
    add = body.append  # (not .append: tools/menu_strings.py takes append() strings for menu text)

    def close(level):
        if depth['ul']:
            add('</ul>')
            depth['ul'] = False
        for k in ('grp', 'sub', 'sec')[:{'grp': 1, 'sub': 2, 'sec': 3}[level]]:
            if depth[k]:
                add('</details>')
                depth[k] = False

    def li(text, cls='', loc=''):
        if not depth['ul']:
            add('<ul>')
            depth['ul'] = True
        if '  ->  ' in text:
            a, b = text.split('  ->  ', 1)
            text = f'{e(a)} &rarr; <span class="it">{e(b)}</span>'
        else:
            text = e(text)
        add(f'<li class="{cls}" data-l="" data-h="{e(loc)}">{text}</li>' if cls != 'sub' else f'<li class="sub">{text}</li>')

    title = ''
    first_sec, in_route = True, False
    for raw in lines:
        text, _, rest = raw.partition(TAG)
        loc, _, flags = rest.partition(TAG)
        cls = 'u' if 'u' in flags else ''
        if text.startswith('# '):
            close('sec')
            if not title:
                title = text[2:]
                add(f'<h1>{e(title)}</h1>')
                continue
            add(f'<details{" open" if first_sec else ""}><summary>{e(text[2:])}</summary>')
            first_sec = False
            in_route = text[2:] == 'Route'  # (its stops start open: read top to bottom)
            depth['sec'] = True
        elif text.startswith('## '):
            close('sub')
            add(f'<details{" open" if in_route else ""}><summary>{e(text[3:])}</summary>')
            depth['sub'] = True
        elif text.startswith('### '):
            close('grp')
            add(f'<details><summary>{e(text[4:])}</summary>')
            depth['grp'] = True
        elif text.startswith('- ') and text.endswith(':') and not loc:
            li(text[2:-1], 'sub')
        elif text.startswith('- ') or text.startswith('  '):
            li(text[2:], cls, loc)
        elif text:
            if depth['ul']:
                add('</ul>')
                depth['ul'] = False
            add(f'<p>{e(text)}</p>')
    close('sec')
    page = (f'<!doctype html><html lang="en"><head><meta charset="utf-8">'
            f'<meta name="viewport" content="width=device-width,initial-scale=1"><title>Spoiler - {e(seed)}</title>'
            f'<style>{CSS}</style></head><body><main>'
            f'<div id="gate"><h1>Spoiler log - {e(seed)}</h1><p>This shows where every item is, plus a route to beat '
            f'the seed.</p><button id="show">Reveal spoilers</button></div>'
            f'<div id="doc" class="hide"><div class="bar"><input id="q" type="search" placeholder="Search items and places">'
            f'<label><input id="u" type="checkbox"> Useful items only</label></div>{"".join(body)}</div>'
            f'</main><script>{JS}</script></body></html>')
    with open(path, 'w', encoding='utf-8') as f:
        f.write(page)


if __name__ == '__main__':
    for d in sys.argv[1:]:
        print(d, build(d))
