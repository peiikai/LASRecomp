#!/usr/bin/env python3
"""Arrow refills held up at a check (pot filler in a chest, a seashell reward, an NPC) show Item.bfevfl entry 'Arrow'
(the get's own entry 'Arrow', or none: the game then takes the item's): the game's no-bow line (System:GetArrow),
which the game itself only shows while Link has no bow - with the bow its arrows are picked up without a word. So
every such get first asks for the bow (Inventory.HasItem itemType 5, as the tool shop's bow does): with it the arrow
is held up with no message (entry ArrowNoText), without it the game's own line stays (and runtime/hle/keysanity.c
hands out a blue rupee instead). Shared gets whose item is the flow's argument (chests, seashell rewards) ask for the
arrow first (CompareString: case 0 = equal).

    apply(out_dir, romfs) -> number of gets changed
"""
import glob
import os

ENTRY = 'ArrowNoText'


def _action(e):
    try:
        return e.data.actor_action.v.v, e.data.params.data
    except AttributeError:
        return None, None


def apply(out_dir, romfs):
    import evfl
    from RandomizerCore.Tools import event_tools
    import keysanity_text as kt  # (_retarget)
    ev_dir = os.path.join(out_dir, 'romfs', 'region_common', 'event')
    n = 0
    for p in sorted(glob.glob(os.path.join(ev_dir, '*.bfevfl'))):
        if os.path.basename(p) == 'Item.bfevfl':
            continue
        flow = event_tools.readFlow(p)
        fc = flow.flowchart
        changed = False
        for e in list(fc.events):
            act, prm = _action(e)
            if act != 'GenericItemGetSequenceByKey':
                continue
            key, msg = prm.get('itemKey'), prm.get('messageEntry')
            if msg and not (isinstance(msg, str) and not isinstance(msg, evfl.common.Argument) and msg == 'Arrow'):
                continue
            argument = isinstance(key, evfl.common.Argument)
            if not argument and key != 'Arrow':
                continue
            after = e.data.nxt.v.name if e.data.nxt.v is not None else None
            have = event_tools.createActionEvent(fc, 'Link', 'GenericItemGetSequenceByKey',
                                                 {'itemKey': 'Arrow', 'keepCarry': prm.get('keepCarry', False),
                                                  'messageEntry': ENTRY}, after)
            head = event_tools.createSwitchEvent(fc, 'Inventory', 'HasItem', {'itemType': 5, 'count': 1},
                                                 {0: e.name, 1: have})
            fresh = {event_tools.findEvent(fc, have), event_tools.findEvent(fc, head)}
            if argument:
                head = event_tools.createSwitchEvent(fc, 'FlowControl', 'CompareString',
                                                     {'value1': str(key), 'value2': 'Arrow'}, {0: head, 1: e.name})
                fresh.add(event_tools.findEvent(fc, head))
            sw = event_tools.findEvent(fc, head)
            kt._retarget(fc, e, sw, fresh)
            for ep in fc.entry_points:
                if ep.main_event.v is e:
                    ep.main_event.v = sw
            changed = True
            n += 1
        if changed:
            event_tools.writeFlow(p, flow)
    if not n:
        return 0
    ip = os.path.join(ev_dir, 'Item.bfevfl')
    if not os.path.exists(ip):
        with open(os.path.join(romfs, 'region_common', 'event', 'Item.bfevfl'), 'rb') as f, open(ip, 'wb') as g:
            g.write(f.read())
    flow = event_tools.readFlow(ip)
    if event_tools.findEntryPoint(flow.flowchart, ENTRY) is None:
        event_tools.addEntryPoint(flow.flowchart, ENTRY)
        event_tools.createActionChain(flow.flowchart, ENTRY, [('Timer', 'Wait', {'time': 0.0})])
    event_tools.writeFlow(ip, flow)
    return n
