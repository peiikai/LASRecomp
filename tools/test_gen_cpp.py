#!/usr/bin/env python3
"""Check the C++ game code generator (runtime/gen/, the lasgen tool) against tools/gen.py: the C must be byte-identical.

Usage: test_gen_cpp.py <lasgen> <game folder> [<game folder> ...] [--modes asm,c,asm-dyn,c-dyn] [--modules main,sdk]
                       [--ref-cache DIR] [--keep DIR]

Each game folder is the player's own extracted game files (it has exefs/main and exefs/sdk). For every folder, module
and mode both generators run into temporary folders and every file is compared; the first differing line is printed.
gen.py needs a Python with unicorn, capstone and lz4 (tools/venv). --ref-cache keeps gen.py's output between runs,
keyed by the module file, the mode and the Python sources, since gen.py takes minutes and lasgen seconds.
Exit status 0 only when everything matched.
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
import time

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
BASES = {'main': '0x7000000000', 'sdk': '0x7004000000'}
MODES = {'asm': ['--simd', 'asm'], 'c': ['--simd', 'c'],
         'asm-dyn': ['--simd', 'asm', '--dyn-base'], 'c-dyn': ['--simd', 'c', '--dyn-base']}
PY_SOURCES = ['gen.py', 'lifter.py', 'analyze.py', 'nso.py', 'patches.py', 'chunk_bounds.py', 'simd_c.py',
              'game_addresses.py', 'rando/patch_variants.json', '../runtime/hle/hle_symbols.txt',
              '../runtime/game_addresses/versions.h']


def digest(paths):
    h = hashlib.sha256()
    for p in paths:
        with open(p, 'rb') as f:
            h.update(f.read())
    return h.hexdigest()[:16]


def gen_args(module, nso, out, mode):
    a = [module, nso, BASES[module], out] + MODES[mode]
    if module == 'sdk':
        a += ['--hle', os.path.join(ROOT, 'runtime/hle/hle_symbols.txt')]
    return a


def run(cmd, cwd, log):
    t = time.time()
    with open(log, 'w') as f:
        r = subprocess.run(cmd, cwd=cwd, stdout=f, stderr=subprocess.STDOUT)
    if r.returncode:
        sys.exit(f'failed ({r.returncode}): {" ".join(cmd)}; see {log}')
    return time.time() - t


def compare(ref, got):
    """Problems found, as text lines (empty: identical)."""
    out = []
    a, b = set(os.listdir(ref)), set(os.listdir(got))
    for n in sorted(a - b):
        out.append(f'  missing from lasgen: {n}')
    for n in sorted(b - a):
        out.append(f'  only from lasgen: {n}')
    for n in sorted(a & b):
        with open(os.path.join(ref, n), 'rb') as f:
            x = f.read()
        with open(os.path.join(got, n), 'rb') as f:
            y = f.read()
        if x == y:
            continue
        xl, yl = x.split(b'\n'), y.split(b'\n')
        i = next((i for i in range(min(len(xl), len(yl))) if xl[i] != yl[i]), min(len(xl), len(yl)))
        out.append(f'  {n}: line {i + 1} differs')
        out.append(f'    gen.py: {xl[i][:300] if i < len(xl) else b"<end>"}')
        out.append(f'    lasgen: {yl[i][:300] if i < len(yl) else b"<end>"}')
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('lasgen')
    ap.add_argument('games', nargs='+')
    ap.add_argument('--modes', default='asm,c,asm-dyn,c-dyn')
    ap.add_argument('--modules', default='main,sdk')
    ap.add_argument('--python', default=sys.executable, help='the Python gen.py runs with (needs unicorn)')
    ap.add_argument('--ref-cache')
    ap.add_argument('--keep', help='keep both outputs here instead of a temporary folder')
    args = ap.parse_args()
    lasgen = os.path.abspath(args.lasgen)
    src = digest([os.path.join(TOOLS, p) for p in PY_SOURCES])
    work = args.keep or tempfile.mkdtemp(prefix='gencmp-')
    failed = 0
    for game in args.games:
        for module in args.modules.split(','):
            nso = os.path.join(game, 'exefs', module)
            key = digest([nso])
            for mode in args.modes.split(','):
                tag = f'{os.path.basename(os.path.normpath(game))} {module} {mode}'
                ref = os.path.join(args.ref_cache, f'{key}-{module}-{mode}-{src}') if args.ref_cache else \
                    os.path.join(work, f'{key}-{module}-{mode}-py')
                got = os.path.join(work, f'{key}-{module}-{mode}-cpp')
                tp = 0.0
                if not (args.ref_cache and os.path.exists(os.path.join(ref, '.done'))):
                    shutil.rmtree(ref, ignore_errors=True)
                    tp = run([args.python, 'gen.py'] + gen_args(module, nso, ref, mode), TOOLS, ref + '.log')
                    open(os.path.join(ref, '.done'), 'w').close()
                shutil.rmtree(got, ignore_errors=True)
                # (from the repository root: lasgen reads tools/rando/patch_variants.json from there, as gen.py does)
                tc = run([lasgen] + gen_args(module, nso, got, mode), ROOT, got + '.log')
                open(os.path.join(got, '.done'), 'w').close()
                problems = compare(ref, got)
                files = len(os.listdir(ref)) - 1
                if problems:
                    failed += 1
                    print(f'DIFF {tag}: {files} files')
                    print('\n'.join(problems[:12]))
                else:
                    print(f'same {tag}: {files} files (gen.py {tp:.0f}s, lasgen {tc:.1f}s)', flush=True)
    if not args.keep:
        shutil.rmtree(work, ignore_errors=True)
    print('all identical' if not failed else f'{failed} differ')
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
