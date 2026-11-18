#!/usr/bin/env python3
"""Texture banks (runtime/rando/fmt/bntx.h) against tools/rando/bntx_merge.py: every result must be the same bytes.

    <LAS venv>/bin/python tools/test_bntx_formats.py <romfs> [<seed romfs> ...]

On the game's romfs: open + save of every .bntx and of timg/__Combined.bntx in ui/StartUp.arc and ui/DgnTex.arc,
subset / merge as dungeon_gen builds a level's map bank (room models from other levels), and patch_in_place /
copy_renamed on DgnTex's bank as the dungeon map images use them. On a seed's romfs: open + save of each bank there.
Each Python result is also checked for soundness (read back: same textures, every name found through the
dictionary, every pointer relocated and inside the file); a problem there is reported, not counted as a failure,
since the native code matches the Python's bytes either way.
Builds build/bntx-check (tools/bntx_check.cpp). Banks are handled one at a time (some are 270 MB).
"""
import gc
import glob
import os
import random
import re
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(HERE, 'rando'))
import bntx_merge  # noqa: E402
import resdic  # noqa: E402
import oead  # noqa: E402

EXE = os.path.join(ROOT, 'build', 'bntx-check')
BANK = 'timg/__Combined.bntx'
fails = 0
cases = 0
problems = []
null_relocs = []  # (banks whose relocation table lists null pointers)


def build():
    os.makedirs(os.path.dirname(EXE), exist_ok=True)
    subprocess.check_call(['c++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Iruntime', 'tools/bntx_check.cpp',
                           'runtime/rando/fmt/bntx.cpp', 'runtime/rando/fmt/sarc.cpp', '-o', EXE], cwd=ROOT)


def bank(spec):
    if '::' not in spec:
        return open(spec, 'rb').read()
    arc, inner = spec.split('::')
    return bytes(oead.Sarc(open(arc, 'rb').read()).get_file(inner).data)


def native(args, tmp):
    out = os.path.join(tmp, 'native.bin')
    if os.path.exists(out):
        os.remove(out)
    r = subprocess.run([EXE] + args(out), capture_output=True, text=True)
    if r.returncode:
        return None, r.stderr.strip()
    return out, None


def compare(label, py, args, tmp):
    """py: the Python's bytes (or an exception it raised); args(out) -> bntx-check arguments."""
    global fails, cases
    cases += 1
    out, err = native(args, tmp)
    if isinstance(py, Exception):
        if out is None:
            print(f'  ok   {label}: both refuse ({type(py).__name__} / {err})')
        else:
            fails += 1
            print(f'  FAIL {label}: Python raised {type(py).__name__}: {py}, native wrote a file')
        return
    if out is None:
        fails += 1
        print(f'  FAIL {label}: native error: {err}')
        return
    nat = open(out, 'rb').read()
    if nat == py:
        print(f'  ok   {label} ({len(py)} bytes)')
    else:
        fails += 1
        at = next((i for i in range(min(len(py), len(nat))) if py[i] != nat[i]), min(len(py), len(nat)))
        print(f'  FAIL {label}: python {len(py)} bytes, native {len(nat)}, first difference at {at:#x}')
    del nat


def lookup(names_at, bits, lefts, rights, key):
    """The ResDic walk (tools/rando/resdic.py): from the root's left child while the bit index grows."""
    k = key.encode()
    bits = [b - (1 << 32) if b >= 1 << 31 else b for b in bits]
    prev, cur = 0, lefts[0]
    while bits[prev] < bits[cur]:
        prev, cur = cur, (rights[cur] if resdic._bit(k, bits[cur]) else lefts[cur])
    return names_at[cur]


def soundness(label, data, expect=None):
    """Reads a bank the Python wrote back and checks it can be used; expect: {name: (format, w, h, mips, data)}."""
    if isinstance(data, Exception):
        return
    issues = []
    try:
        b = bntx_merge._open(data).bntx
    except Exception as e:  # noqa: BLE001
        problems.append(f'{label}: does not read back ({type(e).__name__}: {e})')
        return
    size = len(data)
    if struct.unpack_from('<I', data, 0x1C)[0] != size:
        issues.append('file size field')
    names = [t.name for t in b.textures]
    if expect is not None:
        if sorted(names) != sorted(expect):
            issues.append('texture names differ')
        for t in b.textures:
            e = expect.get(t.name)
            if e and (t.format_, t.width, t.height, t.numMips, bytes(t.data)) != e:
                issues.append(f'texture {t.name} differs')
                break
    # dictionary: every name found
    d = b.texNameDict
    strs = [None] + [b.strTbl[x.strIdx] for x in d.entries[1:]]
    bits = [x.referenceBit for x in d.entries]
    lefts = [x.leftIdx for x in d.entries]
    rights = [x.rightIdx for x in d.entries]
    missing = [n for n in names if lookup(strs, bits, lefts, rights, n) != n]
    if missing:
        issues.append(f'{len(missing)} names not found through the dictionary (e.g. {missing[0]})')
    if d.count != len(names):
        issues.append(f'dictionary count {d.count} != {len(names)} textures')
    # relocation table: every pointer field listed, each inside the file
    rlt = struct.unpack_from('<I', data, 0x18)[0]
    nsec = struct.unpack_from('<I', data, rlt + 8)[0]
    secs = [struct.unpack_from('<Q2I2i', data, rlt + 16 + 24 * i) for i in range(nsec)]
    ent0 = rlt + 16 + 24 * nsec
    listed = {}
    for s, (_, spos, ssize, idx, cnt) in enumerate(secs):
        for j in range(idx, idx + cnt):
            pos, nstruct, noff, npad = struct.unpack_from('<IH2B', data, ent0 + 8 * j)
            p = pos
            for _ in range(nstruct):
                for _ in range(noff):
                    listed[p] = s
                    p += 8
                p += 8 * npad
    want = {0x28: 0, 0x30: 1, 0x38: 0}
    for i in range(len(b.textures)):
        want[0x198 + 8 * i] = 0
    for i in range(len(d.entries)):
        want[d.pos + 16 + 16 * i] = 0
    for t in b.textures:
        for off in (0x50, 0x58, 0x60, 0x70, 0x78):
            want[t.pos + off] = 0
        for m in range(t.numMips):
            want[t.pos + 0x290 + 8 * m] = 1
    unlisted = [p for p in want if p not in listed]
    if unlisted:
        issues.append(f'{len(unlisted)} pointers not relocated (first at {min(unlisted):#x})')
    nulls = [p for p in listed if struct.unpack_from('<Q', data, p)[0] == 0]
    if nulls:
        null_relocs.append(label)
    for p, s in listed.items():
        v = struct.unpack_from('<Q', data, p)[0]
        base, ssize = secs[s][1], secs[s][2]
        if v and not (base <= v < base + ssize):
            issues.append(f'pointer at {p:#x} = {v:#x} outside its section {s} ({base:#x}+{ssize:#x})')
            break
        if p + 8 > size:
            issues.append(f'relocated pointer at {p:#x} past the end')
            break
    if issues:
        problems.append(f'{label}: ' + '; '.join(issues))


def shapes(data):
    b = bntx_merge._open(data).bntx
    return {t.name: (t.format_, t.width, t.height, t.numMips, bytes(t.data)) for t in b.textures}


def save_cases(romfs, tmp, tag):
    specs = sorted(glob.glob(os.path.join(romfs, '**', '*.bntx'), recursive=True))
    for a in ('StartUp', 'DgnTex'):
        p = os.path.join(romfs, 'region_common', 'ui', a + '.arc')
        if os.path.exists(p):
            specs.append(p + '::' + BANK)
    for spec in specs:
        label = f'{tag} save {os.path.relpath(spec, romfs)}'
        data = bank(spec)
        try:
            py = bntx_merge._open(data).save()
        except Exception as e:  # noqa: BLE001
            py = e
        compare(label, py, lambda out: ['save', spec, out], tmp)
        if not isinstance(py, Exception):
            if py == data:
                print('       (same bytes as the source)')
            soundness(label, py, shapes(data))
        del data, py
        gc.collect()


def subset_cases(romfs, tmp):
    mapdir = os.path.join(romfs, 'region_common', 'map')
    files = os.listdir(mapdir)
    levels = sorted(f[:-5] for f in files if re.match(r'Lv\d\d.*\.bntx$', f))
    rooms = {lv: sorted(f[:-6] for f in files if f.startswith(lv + '_') and f.endswith('.bfres')) for lv in levels}
    roofs = {f[:-6] for f in files if f.startswith('DungeonRoof') and f.endswith('.bfres')}
    rng = random.Random(7)
    for n, level in enumerate(levels):
        if not rooms[level]:
            continue
        others = rng.sample([lv for lv in levels if lv != level and rooms[lv]], 2 if n % 3 else 3)
        models = set(rng.sample(rooms[level], min(len(rooms[level]), 12))) | roofs
        for lv in others:
            models |= set(rng.sample(rooms[lv], min(len(rooms[lv]), 6)))
        only = set()
        for m in models:
            raw = open(os.path.join(mapdir, m + '.bfres'), 'rb').read()
            only |= {x.group().decode() for x in re.finditer(rb'MI_[A-Za-z0-9_.]+', raw)}
        srcs = [level] + sorted({m.rsplit('_', 1)[0] for m in models if m.startswith('Lv')} - {level})
        paths = [os.path.join(mapdir, lv + '.bntx') for lv in srcs]
        lst = os.path.join(tmp, 'only.txt')
        open(lst, 'w').write(''.join(x + '\n' for x in sorted(only)))
        try:
            py = bntx_merge.subset([open(p, 'rb').read() for p in paths], only)
        except Exception as e:  # noqa: BLE001
            py = e
        label = f'subset {level} <- {", ".join(s[:4] for s in srcs[1:])} ({len(only)} names)'
        compare(label, py, lambda out: ['subset', out, lst] + paths, tmp)
        if not isinstance(py, Exception):
            soundness(label, py)
        del py
        gc.collect()
    # plain merges: everything, and a filtered one
    small = [os.path.join(mapdir, x + '.bntx') for x in ('Lv09WindFishsEgg', 'Lv10ClothesDungeon', 'Lv06FaceShrine')]
    small = [p for p in small if os.path.exists(p)]
    if len(small) >= 2:
        datas = [open(p, 'rb').read() for p in small]
        py = bntx_merge.merge(datas[0], datas[1:])
        compare('merge all of ' + ', '.join(os.path.basename(p)[:4] for p in small), py,
                lambda out: ['merge', out, '-'] + small, tmp)
        soundness('merge all', py)
        names = sorted({t.name for d in datas[1:] for t in bntx_merge._open(d).bntx.textures})
        only = set(random.Random(3).sample(names, len(names) // 3))
        lst = os.path.join(tmp, 'only.txt')
        open(lst, 'w').write(''.join(x + '\n' for x in sorted(only)))
        py = bntx_merge.merge(datas[0], datas[1:], only)
        compare(f'merge {len(only)} named', py, lambda out: ['merge', out, lst] + small, tmp)
        del datas, py
        gc.collect()


def dgntex_cases(romfs, tmp):
    spec = os.path.join(romfs, 'region_common', 'ui', 'DgnTex.arc') + '::' + BANK
    if not os.path.exists(spec.split('::')[0]):
        return
    data = bank(spec)
    texs = {t.name: t for t in bntx_merge._open(data).bntx.textures}
    rooms = sorted(n for n in texs if n + '_Mask' in texs)
    rng = random.Random(11)

    def shape(n):
        t = texs[n]
        return (t.format_, t.width, t.height, t.numMips, t.imageSize)

    def same(a, b):
        return shape(a) == shape(b) and shape(a + '_Mask') == shape(b + '_Mask')
    set_from = []
    for cell in rng.sample(rooms, 40):
        src = rng.choice([r for r in rooms if r != cell and same(r, cell)] or [cell])
        if src != cell:
            set_from += [(cell, src), (cell + '_Mask', src + '_Mask')]
    spec_file = os.path.join(tmp, 'spec.txt')

    def run_patch(label, sf, ren):
        open(spec_file, 'w').write(''.join(f'set\t{a}\t{b}\n' for a, b in sf) + ''.join(f'ren\t{a}\t{b}\n' for a, b in ren))
        try:
            py = bntx_merge.patch_in_place(data, dict(sf), dict(ren))
        except Exception as e:  # noqa: BLE001
            py = e
        compare(label, py, lambda out: ['patch', spec, spec_file, out], tmp)
        return py
    py = run_patch(f'patch_in_place DgnTex ({len(set_from)} pixel copies)', set_from, [])
    soundness('patch_in_place DgnTex', py)
    run_patch('patch_in_place DgnTex (copy between shapes: refused)',
              [(rooms[0], next(n for n in texs if shape(n) != shape(rooms[0])))], [])
    # renames keep the length: swap a room's last letter for one no other room uses
    ren = []
    used = set(texs)
    in_set = {n for pair in set_from for n in pair}
    for r in rng.sample([r for r in rooms if r not in in_set], 12):
        for c in 'QRSTUVWXYZ':
            new = r[:-1] + c
            if new not in used and new + '_Mask' not in used:
                ren += [(r, new), (r + '_Mask', new + '_Mask')]
                used |= {new, new + '_Mask'}
                break
    py = run_patch(f'patch_in_place DgnTex ({len(set_from)} copies, {len(ren)} renames)', set_from, ren)
    soundness('patch_in_place DgnTex renamed', py)
    run_patch('patch_in_place DgnTex (copy from a renamed-away name: refused)', [(rooms[0], ren[0][0])], ren[:2])
    run_patch('patch_in_place DgnTex (rename to another length: refused)', [], [(rooms[0], rooms[0] + 'X')])

    # copy_renamed: new cells (names the dungeon never had) and one replaced cell, as the dungeon map uses it
    pairs = []
    for r in rng.sample(rooms, 10):
        lv, cell = r.rsplit('_', 1)
        for num in range(10, 99):
            new = f'{lv}_{num:02d}{cell[-1]}'
            if new not in texs and all(new != p[1] for p in pairs):
                pairs += [(r, new), (r + '_Mask', new + '_Mask')]
                break
    a, b = rooms[1], rooms[2]
    pairs += [(a, b), (a + '_Mask', b + '_Mask'), ('NoSuchTexture', 'NoSuchCopy')]
    pf = os.path.join(tmp, 'pairs.txt')
    open(pf, 'w').write(''.join(f'{x}\t{y}\n' for x, y in pairs))
    py = bntx_merge.copy_renamed(data, pairs)
    compare(f'copy_renamed DgnTex ({len(pairs)} pairs)', py, lambda out: ['copy', spec, pf, out], tmp)
    soundness('copy_renamed DgnTex', py)
    # chained, as _dungeon_map_images: patch, then copies of the patched bank
    open(spec_file, 'w').write(''.join(f'set\t{a}\t{b}\n' for a, b in set_from))
    patched = bntx_merge.patch_in_place(data, dict(set_from))
    py = bntx_merge.copy_renamed(patched, pairs[:-3])
    mid = os.path.join(tmp, 'patched.bntx')
    r = subprocess.run([EXE, 'patch', spec, spec_file, mid], capture_output=True, text=True)
    if r.returncode:
        print('  FAIL chained patch:', r.stderr.strip())
    open(pf, 'w').write(''.join(f'{x}\t{y}\n' for x, y in pairs[:-3]))
    compare('patch_in_place + copy_renamed DgnTex', py, lambda out: ['copy', mid, pf, out], tmp)
    soundness('patch + copy_renamed DgnTex', py)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    build()
    with tempfile.TemporaryDirectory() as tmp:
        romfs = sys.argv[1]
        print('open + save:', romfs)
        save_cases(romfs, tmp, 'game')
        print('subset / merge:')
        subset_cases(romfs, tmp)
        print('patch_in_place / copy_renamed:')
        dgntex_cases(romfs, tmp)
        for seed in sys.argv[2:]:
            print('open + save:', seed)
            save_cases(seed, tmp, 'seed ' + os.path.basename(os.path.dirname(seed.rstrip('/'))))
    if null_relocs:
        problems.append(f'{len(null_relocs)} of the banks checked relocate their textures\' null user data and user '
                        'dictionary pointers (the game\'s own banks don\'t list them)')
    if problems:
        print('\nin the Python\'s own output:')
        for p in problems:
            print('  ' + p)
    print(f'\n{cases - fails}/{cases} byte-identical')
    sys.exit(1 if fails else 0)


if __name__ == '__main__':
    main()
