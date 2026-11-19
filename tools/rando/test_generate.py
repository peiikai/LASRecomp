#!/usr/bin/env python3
"""The native seed generator (`lasrando generate`, runtime/rando/solve/generate.h) against the Python one
(tools/rando/las_headless.py) on the same requests: every file of the two seed folders byte for byte, and the native
seed.json against the Python folder's record (`lasrando convert`: settings, tables, decisions).

usage: python3 tools/rando/test_generate.py [-j N] [--keep DIR] [--python PY] <romfs> <request.json>...
  The Python generator runs with the LAS venv's Python (--python, else $LAS_PYTHON, else
  third_party/LAS-Randomizer/venv) and PYTHONHASHSEED=0; the native one with --legacy, so it writes the Python's files
  too. Needs `make lasrando`. Exit status 1 on any difference.
  --rooms-unbuilt: a build without the dungeon room shuffle's file step (runtime/rando/apply/ room writes): on room
  requests the files that step writes (ROOM_FILES) are counted apart and don't fail the comparison.
"""
import argparse
import concurrent.futures
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
TOOL = os.path.join(ROOT, 'build', 'lasrando.exe' if os.name == 'nt' else 'lasrando')
HEADLESS = os.path.join(ROOT, 'tools', 'rando', 'las_headless.py')

# Files left out of the comparison, each with why:
EXCLUDED = {}
# Files whose textures the native build compresses itself (rgbcx; the Python used quicktex, LGPL): byte-identical
# apart from the textures' pixel data, the pixels compared by `lasrando texture-check` (tools/rando/test_apply.py)
PIXEL_CHECKED = ('romfs/region_common/ui/StartUp.arc', 'romfs/region_common/actor/ObjTreasureBoxKey.bfres',
                 'romfs/region_common/actor/ObjTreasureBoxPlain.bfres')
NATIVE_ONLY = {'seed.json'}
# what the dungeon room shuffle's file step writes (dungeon_gen.write_all, pots / bushes tables it moves): --rooms-unbuilt
ROOM_FILES = ('romfs/region_common/level/', 'romfs/region_common/map/', 'romfs/region_common/ui/', 'door_jumps.tsv',
              'dungeon_map.tsv', 'dungeon_rooms.tsv', 'pots.tsv', 'bushes.tsv', 'flags_ext', 'decisions.json',
              'tracker.tsv', 'tracker.json', 'spoiler_view.txt', 'spoiler.html')  # (checked on its own against the Python folder's record)


def venv_python(arg):
    if arg:
        return arg
    if os.environ.get('LAS_PYTHON'):
        return os.environ['LAS_PYTHON']
    v = os.path.join(ROOT, 'third_party', 'LAS-Randomizer', 'venv')
    return os.path.join(v, 'Scripts', 'python.exe') if os.name == 'nt' else os.path.join(v, 'bin', 'python')


def files(top):
    out = {}
    for d, _, names in os.walk(top):
        for n in names:
            p = os.path.join(d, n)
            out[os.path.relpath(p, top).replace(os.sep, '/')] = p
    return out


def read(p):
    with open(p, 'rb') as f:
        return f.read()


def first_diff(a, b):
    i = next((k for k in range(min(len(a), len(b))) if a[k] != b[k]), min(len(a), len(b)))
    return f'at byte {i} of {len(a)} / {len(b)}: native {a[max(0, i - 30):i + 50]!r} / python {b[max(0, i - 30):i + 50]!r}'


def check_record(native_dir, py_dir, seed_id, notes):
    """The native seed.json against the Python folder read as the runtime reads an older seed (lasrando convert)."""
    with open(os.path.join(native_dir, 'seed.json')) as f:
        mine = json.load(f)
    with tempfile.TemporaryDirectory() as tmp:
        for n in os.listdir(py_dir):
            if os.path.isfile(os.path.join(py_dir, n)):
                shutil.copy(os.path.join(py_dir, n), tmp)
        with open(os.path.join(tmp, 'seed_id.txt'), 'w') as f:
            f.write(seed_id + '\n')
        r = subprocess.run([TOOL, 'convert', tmp], capture_output=True, text=True)
    if r.returncode:
        notes.append(f'seed.json: lasrando convert failed: {r.stderr.strip()}')
        return False
    theirs = json.loads(r.stdout)
    ok = True
    for k in ('format', 'version', 'seed_id', 'settings', 'tables', 'decisions'):
        if mine.get(k) != theirs.get(k):
            ok = False
            sub = ''
            if isinstance(mine.get(k), dict) and isinstance(theirs.get(k), dict):
                ks = sorted(set(mine[k]) | set(theirs[k]))
                sub = ' (' + ', '.join(x for x in ks if mine[k].get(x) != theirs[k].get(x))[:200] + ')'
            notes.append(f'seed.json: {k} differs from the Python folder\'s record{sub}')
    return ok


def run_one(romfs, req, py, keep, rooms_unbuilt=False):
    name = os.path.splitext(os.path.basename(req))[0]
    with open(req) as f:
        request = json.load(f)
    base = keep or tempfile.mkdtemp(prefix='testgen-')
    # (the same folder name: the readable spoiler is titled with it)
    nat, pyd = os.path.join(base, 'native', name), os.path.join(base, 'python', name)
    for d in (nat, pyd):
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(os.path.dirname(d), exist_ok=True)
    res = {'name': name, 'seed': request.get('seed'), 'notes': []}
    t = time.time()
    r = subprocess.run([TOOL, 'generate', romfs, req, nat, '--legacy'], capture_output=True, text=True)
    res['native_s'] = time.time() - t
    room = request.get('dungeon-room-shuffle', 'off') not in ('off', None, False)
    apart = room and rooms_unbuilt

    def room_file(rel):
        return apart and rel.startswith(ROOM_FILES)
    if True:
        env = dict(os.environ, PYTHONHASHSEED='0')
        t = time.time()
        p = subprocess.run([py, HEADLESS, romfs, pyd, req], capture_output=True, text=True, env=env)
        res['python_s'] = time.time() - t
        if r.returncode or p.returncode:
            same_exit = r.returncode == p.returncode
            err = [l for l in p.stdout.splitlines() if l.startswith('error ')]
            nerr = [l for l in r.stdout.splitlines() if l.startswith('error ')]
            res['ok'] = same_exit and err == nerr
            res['result'] = (f'both fail (exit {p.returncode})' + (f', {err[-1]}' if err else '') if res['ok'] else
                             f'exit native {r.returncode} / python {p.returncode}: native {r.stderr.strip()[-300:]} | '
                             f'python {p.stderr.strip()[-300:]}')
        else:
            want, got = files(pyd), files(nat)
            same = differ = missing = extra = room_left = 0
            for rel, path in sorted(want.items()):
                if rel in EXCLUDED:
                    continue
                if room_file(rel) and (rel not in got or read(got[rel]) != read(path)):
                    room_left += 1  # (the room file step isn't in this build)
                    continue
                if rel not in got:
                    missing += 1
                    res['notes'].append(f'missing {rel}')
                    continue
                a, b = read(got[rel]), read(path)
                if a == b:
                    same += 1
                elif rel in PIXEL_CHECKED and not subprocess.run(
                        [TOOL, 'texture-check', path, got[rel], rel[len('romfs/'):]], capture_output=True).returncode:
                    same += 1
                else:
                    differ += 1
                    res['notes'].append(f'differs {rel} {first_diff(a, b)}')
            for rel in sorted(got):
                if rel not in want and rel not in NATIVE_ONLY and rel not in EXCLUDED and not room_file(rel):
                    extra += 1
                    res['notes'].append(f'extra {rel}')
            rec = check_record(nat, pyd, str(request.get('seed', '')), res['notes'])
            res['ok'] = not (differ or missing or extra) and rec
            res['result'] = (f'{same} files the same, {differ} different, {missing} missing, {extra} extra; '
                             f'seed.json {"same as the record" if rec else "DIFFERENT"}; '
                             f'excluded here: {sum(1 for x in want if x in EXCLUDED)}' +
                             (f'; {room_left} room-step files apart (--rooms-unbuilt)' if apart else ''))
    if not keep:
        shutil.rmtree(base, ignore_errors=True)

    return res


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('romfs')
    ap.add_argument('requests', nargs='+')
    ap.add_argument('-j', type=int, default=1, help='requests at once (times are less exact above 1)')
    ap.add_argument('--keep', help='keep both seed folders there (native/<name>, python/<name>)')
    ap.add_argument('--python', help="the LAS venv's Python")
    ap.add_argument('--rooms-unbuilt', action='store_true',
                    help="room requests: the room file step's files counted apart (a build without it)")
    a = ap.parse_args()
    if not os.path.exists(TOOL):
        sys.exit('make lasrando first')
    py = venv_python(a.python)
    print('excluded from the comparison:')
    for k, v in EXCLUDED.items():
        print(f'  {k}: {v}')
    print(f'  pixels only: {", ".join(PIXEL_CHECKED)} (`lasrando texture-check`)')
    print(f'  native only: {", ".join(sorted(NATIVE_ONLY))} (checked against `lasrando convert` of the Python folder)')
    if a.keep:
        os.makedirs(a.keep, exist_ok=True)
    bad = 0
    rows = []
    with concurrent.futures.ThreadPoolExecutor(max(1, a.j)) as ex:
        for res in ex.map(lambda r: run_one(a.romfs, r, py, a.keep, a.rooms_unbuilt), a.requests):
            rows.append(res)
            print(f"{'ok  ' if res['ok'] else 'FAIL'} {res['name']} ({res['seed']}): {res['result']}", flush=True)
            for n in res['notes'][:12]:
                print('       ' + n)
            if len(res['notes']) > 12:
                print(f"       ... {len(res['notes']) - 12} more")
            bad += not res['ok']
    print('\n| request | seed | native s | python s | result |\n|---|---|---|---|---|')
    for res in rows:
        ps = f"{res['python_s']:.1f}" if 'python_s' in res else '-'
        print(f"| {res['name']} | {res['seed']} | {res['native_s']:.2f} | {ps} | {'ok' if res['ok'] else 'FAIL'} |")
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
