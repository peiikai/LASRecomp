"""Decisions recorded for the las items steps (tools/rando/record.py; docs/randomizer.md stage 3).

install(): hooks into the generator (once, before it runs); add(d, mods): adds this group's fields to decisions.json.

The steps (runtime/rando/apply/las_items.cpp, item_get.cpp) read, besides the placements and settings:
  item-defs (each item's) type, chest-type, npc-key   chest sizes, chest models, Dampe's Items.gsheet rows
  songs: {bgm: bgm}                                  mod_generator.songs_dict (randomizeMusic's draws)
  chest-looks: {rooms: {check: model}, panel: model} LAS Recomp's chest looks (mod_generator.CHEST_LOOK)
  las-globals: {ghost-pet, triforce, triforce-needed, soul-keys}
                                                     the module globals las_headless.py sets (item_get.GHOST_PET,
                                                     TRIFORCE_NEEDED, chests.TRIFORCE, chests.SOUL_KEYS)
"""


def install():
    pass


def add(d, mods):
    if mods is None:
        return
    from RandomizerCore import mod_generator
    from RandomizerCore.Randomizers import chests, data, item_get
    defs = d.setdefault('item-defs', {})
    for k, v in mods.item_defs.items():
        extra = {f: v[f] for f in ('type', 'chest-type', 'npc-key') if f in v}
        if extra:
            defs.setdefault(k, {}).update(extra)
    if mods.songs_dict:
        d['songs'] = dict(mods.songs_dict)
    look = mod_generator.CHEST_LOOK
    if look:
        rooms = {}
        for room in data.CHEST_ROOMS:
            try:
                rooms[room] = look(room)
            except KeyError:  # (nothing placed there: the generator never asks)
                pass
        d['chest-looks'] = {'rooms': rooms, 'panel': look(None)}
    d['las-globals'] = {'ghost-pet': bool(item_get.GHOST_PET), 'triforce': bool(chests.TRIFORCE),
                        'triforce-needed': int(item_get.TRIFORCE_NEEDED), 'soul-keys': list(chests.SOUL_KEYS)}
