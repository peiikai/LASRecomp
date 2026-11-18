#!/usr/bin/env python3
"""Golden vectors for runtime/rando/pyrandom.h (the native solver's copy of Python's random.Random).

Writes tools/rando/pyrandom_golden.txt: for many seeds (seed-ID shaped strings, the per-module tags the generator
uses, edge cases, ints), a long run of the random methods the generator calls, each with CPython's result.
tools/test_pyrandom.cpp replays the file and must match every line. Regenerate only with the Python the seeds are made
with (the LAS venv, CPython 3.12): `third_party/LAS-Randomizer/venv/bin/python tools/rando/pyrandom_golden.py`.

Line format: `seed str <hex of UTF-8>` / `seed int <n>`, then `<op> <args> = <result>`; floats as the hex of their
IEEE-754 bits.
"""
import os
import random
import struct
import sys

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'pyrandom_golden.txt')

STR_SEEDS = ['', 'a', 'LARecomp', 'MBHMM-13FR6-2000W-40GAZ-X700Z', '5F5HF-AB703-5RKZW-E4VTZ-9D100-0035Z',
             'NRCHK-HDW0F-580GX-WZKYH-DB2B0-000FT', 'NRCHK-HDW0F-580GX-WZKYH-DB2B0-000FT-pots',
             'NRCHK-HDW0F-580GX-WZKYH-DB2B0-000FT-entrances-3', 'NRCHK-HDW0F-580GX-WZKYH-DB2B0-000FT~7',
             'NRCHK-HDW0F-580GX-WZKYH-DB2B0-000FT~7-chaos', 'NRCHK-HDW0F-580GX-WZKYH-DB2B0-000FT-Lv04AnglersTunnel-2',
             'BENCH1', 'Seed (2)', 'éè 日本', 'x' * 300, '\x00lead']
INT_SEEDS = [0, 1, 7, 2**32 - 1, 2**32, -5, 2**63 - 1]


def fbits(x):
    return '%016x' % struct.unpack('<Q', struct.pack('<d', x))[0]


def run(r, meta, out):
    for _ in range(400):
        op = meta.choice(['random', 'getrandbits', 'randbelow', 'randrange1', 'randrange', 'randrange3', 'choice',
                          'shuffle', 'choices', 'choicesu', 'uniform', 'randint'])
        if op == 'random':
            out.append(f'random = {fbits(r.random())}')
        elif op == 'getrandbits':
            k = meta.choice([0, 1, 5, 31, 32, 33, 36, 47, 63, 64])
            out.append(f'getrandbits {k} = {r.getrandbits(k)}')
        elif op == 'randbelow':
            n = meta.choice([1, 2, 3, 7, 8, 9, 100, 315, 6158, 2**31, 2**31 + 1, 2**32 + 3, 2**62 + 5])
            out.append(f'randbelow {n} = {r._randbelow(n)}')
        elif op == 'randrange1':
            n = meta.randrange(1, 5000)
            out.append(f'randrange1 {n} = {r.randrange(n)}')
        elif op == 'randrange':
            a = meta.randrange(-50, 50)
            b = a + meta.randrange(1, 300)
            out.append(f'randrange {a} {b} 1 = {r.randrange(a, b)}')
        elif op == 'randrange3':
            a = meta.randrange(-50, 50)
            s = meta.choice([2, 3, 5, -1, -4])
            b = a + s * meta.randrange(1, 40)
            out.append(f'randrange {a} {b} {s} = {r.randrange(a, b, s)}')
        elif op == 'choice':
            n = meta.randrange(1, 400)
            out.append(f'choice {n} = {r.choice(list(range(n)))}')
        elif op == 'shuffle':
            n = meta.randrange(0, 60)
            x = list(range(n))
            r.shuffle(x)
            out.append(f'shuffle {n} = {",".join(map(str, x))}')
        elif op == 'choices':
            w = [meta.randrange(0, 50) for _ in range(meta.randrange(1, 12))]
            w[meta.randrange(len(w))] += 1
            out.append(f'choices {",".join(map(str, w))} = {r.choices(range(len(w)), w)[0]}')
        elif op == 'choicesu':
            n = meta.randrange(1, 500)
            out.append(f'choicesu {n} = {r.choices(range(n))[0]}')
        elif op == 'uniform':
            a = meta.choice([0.5, -3.25, 0.0, 10.0])
            b = meta.choice([1.5, 2.0, 100.0, -1.0])
            out.append(f'uniform {fbits(a)} {fbits(b)} = {fbits(r.uniform(a, b))}')
        elif op == 'randint':
            a = meta.randrange(-20, 20)
            b = a + meta.randrange(0, 100)
            out.append(f'randint {a} {b} = {r.randint(a, b)}')


def main():
    if sys.version_info[:2] != (3, 12):
        sys.exit(f'run with CPython 3.12 (the generator\'s), not {sys.version.split()[0]}')
    meta = random.Random(20261118)
    out = [f'# CPython {sys.version.split()[0]}: tools/rando/pyrandom_golden.py']
    for s in STR_SEEDS:
        out.append(f'seed str {s.encode().hex()}')
        run(random.Random(s), meta, out)
    for n in INT_SEEDS:
        out.append(f'seed int {n}')
        run(random.Random(n), meta, out)
    with open(OUT, 'w', newline='\n') as f:
        f.write('\n'.join(out) + '\n')
    print(f'{OUT}: {len(out)} lines')


if __name__ == '__main__':
    main()
