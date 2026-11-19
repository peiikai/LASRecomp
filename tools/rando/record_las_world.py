"""Decisions recorded for the las world steps (tools/rando/record.py; docs/randomizer.md stage 3).

install(): hooks into the generator (once, before it runs); add(d, mods): adds this group's fields to decisions.json.

The steps (runtime/rando/apply/las_world.cpp) read, besides the placements, settings, trap models and songs:
  enemies: [[room, [actor, ...]], ...]  randomizeEnemies: each room it wrote, in its order, with what its draws did
                                        to the room's actors (enemies.shuffleEnemyActors), one entry per actor changed:
      {"e": index, "sw": 1                 switch 0 cleared (a stair guard: enemies.STAIR_GUARDS)
       "t": type, "p": [8 params], "re": n  the new enemy (params: string / int / float; re: links' enemy byte); it
                                            faces the screen (rotation y 0) at size 1
       "m": 1                               a mini-boss with its kit / rail added (enemies.add_miniboss_kit)
       "s": scale}                          its size (randomize-enemy-sizes), when not what "t" leaves (1.0) or the
                                            actor had
  chest-enemy: id                       makeExefsPatches' Chest_Enemies pick (assemble.preSetup; randomize-enemies)
"""

ENEMIES = []
CHEST_ENEMY = []


def _param(p):
    if isinstance(p, bytes):
        return p.decode('utf-8')
    if isinstance(p, bool) or not isinstance(p, (int, float)):
        raise TypeError(f'enemy parameter {p!r}: not bytes / int / float')
    return p


def install():
    from RandomizerCore.Randomizers import enemies
    from RandomizerCore.ASM import assemble

    real_shuffle, real_kit = enemies.shuffleEnemyActors, enemies.add_miniboss_kit
    kits = []

    def add_miniboss_kit(room_data, mi, arena, room_name=''):
        kits.append(mi)
        return real_kit(room_data, mi, arena, room_name)

    def shuffle(room_data, folder, file, enemy_ids, enemy_settings, rand_state):
        before = [(a.type, list(a.parameters), a.switches[0], (a.scaleX, a.scaleY, a.scaleZ), a.relationships.e, a.rotY)
                  for a in room_data.actors]
        kits.clear()
        state, edited = real_shuffle(room_data, folder, file, enemy_ids, enemy_settings, rand_state)
        if edited:
            acts = []
            for e, (typ, params, sw, scale, rel_e, rot) in enumerate(before):
                a = room_data.actors[e]
                rec = {}
                if a.switches[0] != sw:
                    assert a.switches[0] == (4, 0), (file, e, a.switches[0])
                    rec['sw'] = 1
                if (a.type != typ or list(a.parameters) != params or a.relationships.e != rel_e or a.rotY != rot
                        or e in kits):
                    rec.update(t=a.type, p=[_param(p) for p in a.parameters], re=a.relationships.e)
                    scale = (1.0, 1.0, 1.0)
                if e in kits:
                    rec['m'] = 1
                now = (a.scaleX, a.scaleY, a.scaleZ)
                if now != scale:
                    assert now[0] == now[1] == now[2], (file, e, now)
                    rec['s'] = now[0]
                if rec:
                    acts.append({'e': e, **rec})
            ENEMIES.append([file[:-4], acts])
        return state, edited

    enemies.add_miniboss_kit = add_miniboss_kit
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
