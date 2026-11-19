"""Decisions recorded for the ours items steps (tools/rando/record.py; docs/randomizer.md stage 3).

install(): hooks into the generator (once, before it runs); add(d, mods): adds this group's fields to decisions.json.

The steps (runtime/rando/apply/ours_items*.cpp) read, besides the placements and settings, what these record (each
part below says which fields it adds).
"""


# ---- shopsanity, trendsanity, pets, escape bottle, keysanity / bag texts, refills, silent pickups
def _install_shop():
    pass


def _add_shop(d, mods):
    """shop-prices: [six prices] - shopsanity 'random': the goods' prices, drawn as shopsanity.apply draws them (its own
    stream, f'{seed}-shop', one draw per good in CHECKS order)"""
    s = d.get('settings') or {}
    if s.get('shopsanity') == 'random':
        import random
        import shopsanity
        rng = random.Random(f"{s['seed']}-shop")
        d['shop-prices'] = [rng.choices(shopsanity.RANDOM_PRICES, shopsanity.RANDOM_WEIGHTS)[0] for _ in shopsanity.CHECKS]


# ---- souls, Triforce
def _install_souls():
    pass


def _add_souls(d, mods):
    pass


# ---- Hint Sheet, seed hints
#   hint-sheet: {steps: [{loc, item, region, landmark?, door-area?, door-name?}], entered-via: {dungeon: way in}}
#       hint_sheet.apply's steps as its texts see them (landmarks and shuffled doors filled in; hint_steps.json + the
#       entrance shuffle), with the ways into the dungeons (via_doors: a dungeon key or {name, area} of a door)
#   seed-hints: {hints: [seed_hints.json's, items behind a shuffled door with its door-name], via: {...}}
#       seed_hints.apply's input (seed_hints.json, the door names entrance_data.yml gives, via_doors)
_HINT_SHEET = None
_SEED_HINTS = None


def _install_hints():
    import json
    import os
    import hint_sheet
    import seed_hints
    real_apply, real_text, real_hints = hint_sheet.apply, hint_sheet.hint_text, seed_hints.apply

    def apply(out_dir, romfs, settings, ent=None, logic_defs=None):
        global _HINT_SHEET
        if os.path.exists(os.path.join(out_dir, 'hint_steps.json')):
            _HINT_SHEET = {'steps': [], 'entered-via': {}}
        return real_apply(out_dir, romfs, settings, ent, logic_defs)

    def hint_text(step, dungeon_of, entered_via):
        if _HINT_SHEET is not None:
            _HINT_SHEET['steps'].append({k: step[k] for k in ('loc', 'item', 'region', 'landmark', 'door-area',
                                                               'door-name') if k in step})
            _HINT_SHEET['entered-via'] = entered_via
        return real_text(step, dungeon_of, entered_via)

    def hints_apply(out_dir, romfs, settings):
        global _SEED_HINTS
        p = os.path.join(out_dir, 'seed_hints.json')
        if os.path.exists(p):
            import entrance_logic as el
            with open(p) as f:
                data = json.load(f)
            doors = {d['id']: d for d in el.load()['doors']}
            hints = []
            for h in data['hints']:
                h = dict(h)
                if doors.get(h.get('door')):
                    h['door-name'] = hint_sheet.door_name(doors[h['door']])
                hints.append(h)
            _SEED_HINTS = {'hints': hints, 'via': hint_sheet.via_doors(data.get('entered-via') or {}, romfs)}
        return real_hints(out_dir, romfs, settings)
    hint_sheet.apply, hint_sheet.hint_text, seed_hints.apply = apply, hint_text, hints_apply


def _add_hints(d, mods):
    if _HINT_SHEET is not None:
        d['hint-sheet'] = _HINT_SHEET
    if _SEED_HINTS is not None:
        d['seed-hints'] = _SEED_HINTS


def install():
    _install_shop()
    _install_souls()
    _install_hints()


def add(d, mods):
    _add_shop(d, mods)
    _add_souls(d, mods)
    _add_hints(d, mods)
