#!/usr/bin/env python3
"""A seed built natively (`lasrando dump`: what the runtime serves) against the same seed made by the Python generator
(docs/randomizer.md, stage 3). Every game file the generator wrote must be there with the same bytes, and nothing
else; files only steps not yet ported write are listed apart. The seed's tables (seed.json "tables": pots.tsv, flags_ext,
...) too, each the same as the folder's.

usage: python3 tools/rando/test_apply.py <romfs> <seed folder made by las_headless.py> [...]
(the seed folder holds decisions.json, romfs/ and exefs/). Exit status 1 on any difference. Needs `make lasrando`.
"""
import os
import subprocess
import sys
import tempfile

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
TOOL = os.path.join(ROOT, 'build', 'lasrando.exe' if os.name == 'nt' else 'lasrando')
# written by steps that are not ported yet (stage 4: textures, the title logo, dungeon room shuffle)
NOT_PORTED = ('romfs/region_common/ui/StartUp.arc', 'romfs/region_common/actor/ObjTreasureBoxKey.bfres',
              'romfs/region_common/actor/ObjTreasureBoxPlain.bfres')
# the tables the steps write (runtime/rando/apply/decide_check.cpp SEED_TABLES); hints.tsv needs hint_steps.json's step
# numbers, which the decisions don't hold: made in decide mode only (lasrando decide-check)
TABLES = ('pots.tsv', 'bushes.tsv', 'rocks.tsv', 'digs.tsv', 'shell_spots.tsv', 'apple_trees.tsv', 'shopsanity.tsv',
          'souls.tsv', 'triforce.tsv', 'quick_pickups.tsv', 'flags.tsv', 'flags_ext', 'roam.tsv',
          'enemies_random.tsv', 'check_flags.tsv')


def files(top):
    out = {}
    for sub in ('romfs', 'exefs'):
        base = os.path.join(top, sub)
        for d, _, names in os.walk(base):
            for n in names:
                p = os.path.join(d, n)
                out[os.path.relpath(p, top).replace(os.sep, '/')] = p
    return out


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    romfs, bad = sys.argv[1], 0
    for seed in sys.argv[2:]:
        if os.path.exists(os.path.join(seed, 'door_jumps.tsv')):
            print(f'{seed}: room shuffle (stage 4), skipped')
            continue
        with tempfile.TemporaryDirectory() as out:
            r = subprocess.run([TOOL, 'dump', romfs, os.path.join(seed, 'decisions.json'), out],
                               capture_output=True, text=True)
            if r.returncode:
                print(f'{seed}: build failed: {r.stderr.strip()}')
                bad += 1
                continue
            want, got = files(seed), files(out)
            same = differ = missing = extra = later = 0
            notes = []
            for rel, p in sorted(want.items()):
                if rel in NOT_PORTED:
                    later += 1
                    continue
                if rel not in got:
                    missing += 1
                    notes.append(f'missing {rel}')
                elif open(p, 'rb').read() == open(got[rel], 'rb').read():
                    same += 1
                else:
                    differ += 1
                    notes.append(f'differs {rel}')
            for rel in sorted(set(got) - set(want)):
                extra += 1
                notes.append(f'extra {rel}')
            tsame = 0
            for t in TABLES:
                w, g = os.path.join(seed, t), os.path.join(out, t)
                if not os.path.exists(w) and not os.path.exists(g):
                    continue
                if os.path.exists(w) and os.path.exists(g) and open(w, 'rb').read() == open(g, 'rb').read():
                    tsame += 1
                    continue
                differ += 1
                notes.append(f'table {t}: ' + ('missing' if not os.path.exists(g) else 'extra' if not os.path.exists(w)
                                                else 'differs'))
            print(f'{os.path.basename(seed.rstrip("/"))}: {same} files the same, {differ} different, {missing} missing,'
                  f' {extra} extra ({later} from steps not ported yet); {tsame} tables the same')
            for n in notes[:8]:
                print('   ', n)
            bad += differ + missing + extra
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
