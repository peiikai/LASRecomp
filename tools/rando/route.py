#!/usr/bin/env python3
"""The spoiler's Route: only the items needed to beat a seed, in an order that can be followed (route.json).

Presentation only: reads the finished placements and logic, changes nothing in the seed.

Starts from the Hint Sheet's steps (hint_sheet.steps: items that open progression or the goal) and checks them the
way a player would follow them: a playthrough where only the route's locations give their item. Where an item can
come from two places (two Power Bracelets, Feather or Hookshot for the same gap) the Hint Sheet keeps neither, so
the route could stall; then the earliest item (playthrough order) that lets the route go further is added, until
every route location and the goal are reached. Order: followed like a player would - stay in the same place (a
dungeon, an area) while the route has something reachable there, else go to the earliest reachable stop; each stop
can be reached with the items of the stops before it.

    build(logic_defs, placements, settings, check, sph, out_dir) -> number of stops (writes route.json:
        {"spheres": [[[loc, item], ...] per stop, ...], "added": [loc, ...]})
"""
import json
import os
import re

import hint_sheet
import playthrough


def build(logic_defs, placements, settings, check, sph, out_dir):
    p = os.path.join(out_dir, 'hint_steps.json')
    if not os.path.exists(p):
        return 0
    import time
    t0 = time.time()
    steps = json.load(open(p))['steps']
    route = {st['loc'] for st in steps if placements.get(st['loc'])}
    reach = _reach_fn(logic_defs, settings, check)
    full_locs, full_goal = reach(placements)
    order = [loc for s in sph for loc, item in s if not loc.startswith('starting-')]

    def only(locs):
        return {k: (v if k in locs or k.startswith('starting-') or not isinstance(v, str) else None)
                for k, v in placements.items()}
    word = re.compile(r'[A-Za-z0-9][A-Za-z0-9\-]*')
    read = {w for node in logic_defs.values() for ck, cv in node.items()
            if ck.startswith('condition-') and isinstance(cv, str) for w in word.findall(cv)}

    def complete(locs):
        got, goal = reach(only(locs))
        return goal >= full_goal and locs <= got
    added = []
    stuck, last = False, -1
    for _ in range(300):
        got, goal = reach(only(route))
        if goal >= full_goal and route <= got:
            break
        base = len(got) + 1000 * goal
        if base > last:  # (the last pick took the route further: look for a single item that does again)
            stuck = False
        last = base
        cand = [loc for loc in order if loc not in route and loc in got
                and not hint_sheet.JUNK.match(playthrough.base_item(placements.get(loc)))]
        # the earliest item reachable now that takes the route further; else (items that only help together, like a
        # dungeon's last keys) the earliest one the logic asks for at all, without testing each again until the route
        # grows - the pruning below drops what wasn't needed
        pick = None
        if not stuck:
            for loc in cand:
                g2, goal2 = reach(only(route | {loc}))
                if len(g2) + 1000 * goal2 > base:
                    pick = loc
                    break
            stuck = pick is None
        if not pick:
            pick = next((loc for loc in cand if playthrough.base_item(placements.get(loc)) in read), None)
        if not pick:
            break
        route.add(pick)
        added.append(pick)
    if complete(route):
        for loc in reversed(list(added)):  # (only what the route can't do without)
            if complete(route - {loc}):
                route.discard(loc)
                added.remove(loc)
    # Follow it like a player: stay where you are while the route has something reachable there (a dungeon's keys
    # one after another), else go where the earliest reachable stop is. Items count as soon as they're picked up.
    sphere_of = {loc: i for i, s in enumerate(sph) for loc, _ in s}
    dungeon_of = settings.get('dungeon-of') or {}

    def area(loc):
        m = re.match(r'^(D\d)-', loc)
        return dungeon_of.get(loc) or (m.group(1) if m else logic_defs.get(loc, {}).get('spoiler-region', ''))
    got, here, out = set(), None, []
    while True:
        can = [loc for loc in reach(only(got))[0] if loc in route and loc not in got]
        if not can:
            break
        now = [loc for loc in can if area(loc) == here]
        if not now:
            first = min(can, key=lambda l: (sphere_of.get(l, 999), l))
            here = area(first)
            now = [loc for loc in can if area(loc) == here]
        now.sort(key=lambda l: (sphere_of.get(l, 999), l))
        if out and out[-1][0] == here:
            out[-1][1].extend(now)
        else:
            out.append((here, now))
        got.update(now)
    left = sorted(route - got, key=lambda l: (sphere_of.get(l, 999), l))  # (shouldn't happen: kept, at the end)
    if left:
        out.append(('', left))
        print('route: not reachable in order (listed last):', ', '.join(left[:8]), flush=True)
    out = [[[loc, placements[loc]] for loc in locs] for _, locs in out]
    with open(os.path.join(out_dir, 'route.json'), 'w') as f:
        json.dump({'spheres': out, 'added': added}, f)
    print(f'route: {len(added)} added, {time.time() - t0:.1f}s', flush=True)
    return sum(len(s) for s in out)


def _reach_fn(logic_defs, settings, check):
    """hint_sheet._reach_fn, counting followers too (the Flying Rooster at its statue is a route stop): -> reach(pl)
    = (item and follower locations reached, how much of the goal). Its own copy: the Hint Sheet's must stay as it is
    (its steps are part of the seed)."""
    junk = set(settings.get('excluded-locations', ())) | set(settings.get('absent-locations', ()))
    goal = set(playthrough.INSTRUMENTS) | {'kill-nightmare'}
    tri = int(settings.get('triforce-needed', 0) or 0)
    word = re.compile(r'[A-Za-z0-9][A-Za-z0-9\-\[\]]*')
    readers = {}
    for k, node in logic_defs.items():
        toks = set()
        for ck, cv in node.items():
            if ck.startswith('condition-') and isinstance(cv, str):
                toks.update(word.findall(cv))
        if node.get('region'):
            toks.add(node['region'])
        for t in toks:
            readers.setdefault(t, []).append(k)
    keys = list(logic_defs)

    def reach(pl):
        access = {k: 1 for k, v in settings.items() if v is True}
        seen = set()
        todo, queued = list(reversed(keys)), set(keys)

        def give(tok, n=1):
            access[tok] = access.get(tok, 0) + n
            for r in readers.get(tok, ()):
                if r not in queued and r not in seen:
                    queued.add(r)
                    todo.append(r)
        while todo:
            k = todo.pop()
            queued.discard(k)
            if k in seen:
                continue
            node = logic_defs[k]
            if node.get('type') == 'enemy':
                try:
                    if check(k, access):
                        seen.add(k)
                        for t in (k, k + '[pit]', k + '[heavy]'):
                            if not access.get(t):
                                give(t)
                        continue
                    for v in ('pit', 'heavy'):
                        t = f'{k}[{v}]'
                        if f'condition-{v}' in node and not access.get(t) and check(t, access):
                            give(t)
                except Exception:
                    pass
                continue
            try:
                ok = check(k, access)
            except Exception:
                ok = False
            if not ok:
                continue
            seen.add(k)
            give(k)
            item = pl.get(k) if node.get('type') in ('item', 'follower') else None
            if item:
                give(item)
                b = playthrough.base_item(item)
                if b != item:
                    give(b)
        return (frozenset(k for k in seen if logic_defs[k].get('type') in ('item', 'follower') and k not in junk),
                min(access.get(hint_sheet.TRIFORCE, 0), tri) if tri else sum(1 for g in goal if access.get(g)))
    return reach
