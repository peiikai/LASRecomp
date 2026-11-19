"""Decisions recorded for the las world steps (tools/rando/record.py; docs/randomizer.md stage 3).

install(): hooks into the generator (once, before it runs); add(d, mods): adds this group's fields to decisions.json.

The steps (runtime/rando/apply/las_world.cpp) read, besides the placements, settings, trap models and songs:
  enemies: [[room, [actor, ...]], ...]  randomizeEnemies: each room it wrote, in its order, with what its draws did
                                        to the room's actors (enemies.shuffleEnemyActors), one entry per actor changed:
      {"e": index, "sw": 1                 switch 0 cleared (a stair guard: enemies.STAIR_GUARDS)
       "t": type, "p": [8 params], "re": n  the new enemy (params: string / int / float; re: links' enemy byte); it
                                            faces the screen (rotation y 0) at size 1
       "m": 1                               a mini-boss (the roll made it one: no Random-mode slot; with its kit /
                                            rail added when it has one: enemies.add_miniboss_kit)
       "s": scale}                          its size (randomize-enemy-sizes), when not what "t" leaves (1.0) or the
                                            actor had
  chest-enemy: id                       makeExefsPatches' Chest_Enemies pick (assemble.preSetup; randomize-enemies)
"""

import sys

ENEMIES = []
CHEST_ENEMY = []
_MINIS = []  # (the actors the room being rolled made mini-bosses)


class _MinibossVariants(dict):
    """enemies.MINIBOSS_VARIANTS: shuffleEnemyActors asks `mini in MINIBOSS_VARIANTS` once for each actor it makes a
    mini-boss, and nowhere else - the actor's index is its loop variable there. A mini-boss and an enemy of the same
    type rolled the ordinary way look the same afterwards (Shadow Link, Blaino), but only the second gets a Random-mode
    slot: apply mode needs to know which is which without rolling again."""
    def __contains__(self, k):
        f = sys._getframe(1)
        if f.f_code.co_name == 'shuffleEnemyActors':
            _MINIS.append(f.f_locals['e'])
        return dict.__contains__(self, k)


def _param(p):
    if isinstance(p, bytes):
        return p.decode('utf-8')
    if isinstance(p, bool) or not isinstance(p, (int, float)):
        raise TypeError(f'enemy parameter {p!r}: not bytes / int / float')
    return p


def install():
    from RandomizerCore.Randomizers import enemies
    from RandomizerCore.ASM import assemble

    real_shuffle = enemies.shuffleEnemyActors
    if not isinstance(enemies.MINIBOSS_VARIANTS, _MinibossVariants):
        enemies.MINIBOSS_VARIANTS = _MinibossVariants(enemies.MINIBOSS_VARIANTS)

    def shuffle(room_data, folder, file, enemy_ids, enemy_settings, rand_state):
        before = [(a.type, list(a.parameters), a.switches[0], (a.scaleX, a.scaleY, a.scaleZ), a.relationships.e, a.rotY)
                  for a in room_data.actors]
        _MINIS.clear()
        state, edited = real_shuffle(room_data, folder, file, enemy_ids, enemy_settings, rand_state)
        minis = set(_MINIS)
        if edited:
            acts = []
            for e, (typ, params, sw, scale, rel_e, rot) in enumerate(before):
                a = room_data.actors[e]
                rec = {}
                if a.switches[0] != sw:
                    assert a.switches[0] == (4, 0), (file, e, a.switches[0])
                    rec['sw'] = 1
                if (a.type != typ or list(a.parameters) != params or a.relationships.e != rel_e or a.rotY != rot
                        or e in minis):
                    rec.update(t=a.type, p=[_param(p) for p in a.parameters], re=a.relationships.e)
                    scale = (1.0, 1.0, 1.0)
                if e in minis:
                    rec['m'] = 1
                now = (a.scaleX, a.scaleY, a.scaleZ)
                if now != scale:
                    assert now[0] == now[1] == now[2], (file, e, now)
                    rec['s'] = now[0]
                if rec:
                    acts.append({'e': e, **rec})
            ENEMIES.append([file[:-4], acts])
        return state, edited

    enemies.shuffleEnemyActors = shuffle

    real_pre = assemble.preSetup

    def preSetup(rand_state, settings):
        data = real_pre(rand_state, settings)
        if 'CHEST_ENEMY' in data:
            CHEST_ENEMY.append(int(data['CHEST_ENEMY'].lstrip('#')))
        return data
    assemble.preSetup = preSetup


def add(d, mods):
    if ENEMIES:
        d['enemies'] = ENEMIES
    if CHEST_ENEMY:
        d['chest-enemy'] = CHEST_ENEMY[-1]
