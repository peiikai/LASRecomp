#!/usr/bin/env python3
"""Tests for tools/rando/keysanity_text.py: which get events name a keysanity item's dungeon.

  <rando venv python> tools/test_keysanity_text.py           flows built here (no game files)
  <rando venv python> tools/test_keysanity_text.py <romfs>   + the game's own SmallKey.bfevfl, patched like a seed

A dungeon item for another dungeon picked up from a floor key spot (LAS small_keys.writeKeyEvent: a bare
Inventory.AddItemByKey, no get event) said nothing, so a key on a dungeon floor didn't say which dungeon it was for
(#686). It now gets a get event with its Ks entry; items in their own dungeon (index -1) stay as they were.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, 'rando'))
import keysanity_text as kt  # noqa: E402  (puts LAS-Randomizer on the path)
import evfl  # noqa: E402
from RandomizerCore.Tools import event_tools  # noqa: E402
from RandomizerCore.Randomizers import small_keys, rupees  # noqa: E402

FAILS = []


def check(ok, msg):
    if not ok:
        FAILS.append(msg)
        print('FAIL', msg)


def action(e):
    d = e.data
    if type(d).__name__ != 'ActionEvent':
        return None, None, {}
    return d.actor.v.identifier.name, d.actor_action.v.v, d.params.data if d.params else {}


def chain(fc, ep_name):
    """the actions from an entry point, in order"""
    ep = next(ep for ep in fc.entry_points if ep.name == ep_name)
    out, e, seen = [], ep.main_event.v, set()
    while e is not None and id(e) not in seen:
        seen.add(id(e))
        a = action(e)
        out.append(a)
        e = e.data.nxt.v if a[0] else None
    return out


def gets(c):
    return [p for a, n, p in c if (a, n) == ('Link', 'GenericItemGetSequenceByKey')]


def empty_flow():
    flow = evfl.EventFlow()
    flow.flowchart = evfl.Flowchart()
    flow.flowchart.name = 'Test'
    return flow


def test_floor_keys():
    flow = empty_flow()
    fc = flow.flowchart
    # floor key spots as LAS writes them: another dungeon's items, an own-dungeon key, a non-dungeon item
    spots = {'D1-floor': ('SmallKey', 2), 'D2-floor': ('NightmareKey', 9), 'D3-floor': ('Compass', 0),
             'D4-floor': ('DungeonMap', 7), 'D5-floor': ('StoneBeak', 3), 'D6-floor': ('SmallKey', -1)}
    for room, (key, idx) in spots.items():
        small_keys.writeKeyEvent(fc, key, idx, room)
    rupees.makeEventChanges(fc, 0, 'SmallKey', 5)  # (Color Dungeon rupee spots: the same silent add)
    combos = {(k, i) for k, i in spots.values() if i >= 0} | {('SmallKey', 5)}
    n = kt.patch_flow(flow, combos, event_tools)
    want = sum(1 for k, i in spots.values() if i >= 0) + 1
    check(n == want, f'floor keys: {n} get events changed, want {want}')
    for room, (key, idx) in spots.items():
        c = chain(fc, room)
        g = gets(c)
        adds = [(i, p) for i, (a, nm, p) in enumerate(c) if nm == 'AddItemByKey']
        check(len(adds) == 1 and adds[0][1].get('index') == idx, f'{room}: the add is still there, index {idx}')
        if idx < 0:
            check(not g, f'{room}: own-dungeon key stays silent (got {g})')
            continue
        check(len(g) == 1 and g[0].get('messageEntry') == kt.entry(key, idx) and g[0].get('itemKey') == key,
              f'{room}: one get event with {kt.entry(key, idx)} (got {g})')
        gi = next((i for i, (a, nm, p) in enumerate(c) if nm == 'GenericItemGetSequenceByKey'), None)
        check(gi == adds[0][0] - 1, f'{room}: get event right before the add ({c})')
        check([nm for a, nm, p in c[:3]] == ['Deactivate', 'SetActorSwitch', 'Destroy'],
              f'{room}: the key actor is still removed first ({c})')
    g = gets(chain(fc, 'Lv10Rupee_1'))
    check(len(g) == 1 and g[0]['messageEntry'] == 'KsSmallKey5', f'rupee spot: gets {g}')
    # a second pass adds no second get event
    check(kt.patch_flow(flow, combos, event_tools) == want, 'second pass: same count (messageEntry only)')
    for room in spots:
        check(len(gets(chain(fc, room))) <= 1, f'{room}: no second get event on a second pass')


def test_constant_give():
    """a get event already in front (NPCs, standing items): only its messageEntry changes, nothing is added"""
    flow = empty_flow()
    fc = flow.flowchart
    event_tools.addEntryPoint(fc, 'npc')
    event_tools.createActionChain(fc, 'npc', [
        ('Link', 'GenericItemGetSequenceByKey',
         {'itemKey': 'SmallKey', 'keepCarry': False, 'messageEntry': 'SmallKey'}),
        ('Inventory', 'AddItemByKey', {'itemKey': 'SmallKey', 'count': 1, 'index': 4, 'autoEquip': False})])
    n = kt.patch_flow(flow, {('SmallKey', 4)}, event_tools)
    g = gets(chain(fc, 'npc'))
    check(n == 1 and len(g) == 1 and g[0]['messageEntry'] == 'KsSmallKey4', f'npc give: {n} changed, gets {g}')


def test_romfs(romfs):
    """the game's own SmallKey.bfevfl with LAS's floor key entries for another dungeon's items"""
    import tempfile
    src = os.path.join(romfs, 'region_common', 'event', 'SmallKey.bfevfl')
    flow = event_tools.readFlow(src)
    fc = flow.flowchart
    spots = {'LAR-a': ('SmallKey', 0), 'LAR-b': ('NightmareKey', 9), 'LAR-c': ('SmallKey', -1)}
    for room, (key, idx) in spots.items():
        small_keys.writeKeyEvent(fc, key, idx, room)
    n = kt.patch_flow(flow, {('SmallKey', 0), ('NightmareKey', 9)}, event_tools)
    check(n == 2, f'romfs SmallKey.bfevfl: {n} get events changed, want 2')
    with tempfile.TemporaryDirectory() as d:  # (it still writes and reads back)
        p = os.path.join(d, 'SmallKey.bfevfl')
        event_tools.writeFlow(p, flow)
        fc = event_tools.readFlow(p).flowchart
    for room, (key, idx) in spots.items():
        g = gets(chain(fc, room))
        check(len(g) == (idx >= 0) and all(x['messageEntry'] == kt.entry(key, idx) for x in g),
              f'romfs {room}: gets {g}')


if __name__ == '__main__':
    test_floor_keys()
    test_constant_give()
    romfs = sys.argv[1] if len(sys.argv) > 1 else os.environ.get('LARECOMP_ROMFS')
    if romfs:
        test_romfs(romfs)
    print('FAIL' if FAILS else 'ok', f'({len(FAILS)} failures)' if FAILS else '')
    sys.exit(1 if FAILS else 0)
