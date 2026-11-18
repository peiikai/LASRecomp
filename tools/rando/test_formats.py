#!/usr/bin/env python3
"""The randomizer's native file-format code (runtime/rando/fmt/) against the Python it replaces, on the player's own
extracted game files (docs/randomizer.md, stage 2).

For each kind, every game file of that kind is read and written back by `build/lasrando repack` and by the Python code
seeds are made with today; the two must be byte-identical (seeds made either way must be the same files). It also
reports how many come out identical to the file they read (the Python writers normalise some layouts; that is
informational). Point it at the game files, or at a seed's romfs/ to check the files a seed writes.

usage: third_party/LAS-Randomizer/venv/bin/python tools/rando/test_formats.py <romfs> [kind ...]
kinds: leb (room files as LAS repacks them), leb-grid (the same with the tile grid read and rewritten), lvb (level
configs), gsheet (data sheets, as oead writes them), msbt
(messages), bfevfl (event flows, as evfl writes them)
Exit status 1 if any file differs between the two. Needs `make lasrando` first.
"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
LAS = os.path.join(ROOT, 'third_party', 'LAS-Randomizer')
sys.path.insert(0, LAS)
TOOL = os.path.join(ROOT, 'build', 'lasrando.exe' if os.name == 'nt' else 'lasrando')

EXT = {'leb': '.leb', 'leb-grid': '.leb', 'lvb': '.lvb', 'gsheet': '.gsheet', 'msbt': '.msbt', 'sarc': '.arc', 'bfevfl': '.bfevfl'}


def python_repack(kind, data):
    from RandomizerCore.Tools import leb, lvb
    if kind in ('leb', 'leb-grid'):
        return leb.Room(data, edit_grid=kind == 'leb-grid').repack()
    if kind == 'lvb':
        return lvb.Level(data).repack()
    if kind == 'gsheet':  # (as oead_tools.readSheet / writeSheet: parsed, copied into a new sheet, written)
        import oead
        src = oead.gsheet.parse(oead.Bytes(data))
        sheet = oead.gsheet.Sheet()
        sheet.alignment, sheet.hash, sheet.name = src.alignment, src.hash, src.name
        sheet.root_fields, sheet.values = src.root_fields, src.values
        return bytes(sheet.to_binary())
    if kind == 'sarc':  # (as dungeon_gen rewrites DgnTex.arc)
        import oead
        return bytes(oead.SarcWriter.from_sarc(oead.Sarc(data)).write()[1])
    if kind == 'msbt':
        sys.path.insert(0, HERE)
        import msbt
        return msbt.Msbt(data).pack()
    if kind == 'bfevfl':  # (as event_tools.readFlow / writeFlow)
        import io
        import evfl
        flow = evfl.EventFlow()
        flow.read(data)
        out = io.BytesIO()
        flow.write(out)
        return out.getvalue()
    raise SystemExit(f'unknown kind {kind}')


def files_of(romfs, ext):
    out = []
    for d, _, names in os.walk(romfs):
        out += [os.path.join(d, n) for n in names if n.endswith(ext) and not n.startswith('._')]
    return sorted(out)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    romfs = os.path.abspath(sys.argv[1])
    kinds = sys.argv[2:] or list(EXT)
    if not os.path.exists(TOOL):
        sys.exit(f'{TOOL} missing: make lasrando')
    failed = False
    for kind in kinds:
        paths = files_of(romfs, EXT[kind])
        with tempfile.TemporaryDirectory() as out:
            subprocess.run([TOOL, 'repack', kind, out], input='\n'.join(paths).encode(), check=True)
            same = differ = both_err = same_as_game = py_err = 0
            examples = []
            for p in paths:
                data = open(p, 'rb').read()
                try:
                    want = python_repack(kind, data)
                except Exception as e:  # (the Python can't read it: the native code must refuse it too)
                    want = None
                    py_err += 1
                got = open(os.path.join(out, p.lstrip('/')), 'rb').read()
                native_err = got.startswith(b'ERROR ')
                if want is None and native_err:
                    both_err += 1
                    continue
                if want is not None and not native_err and got == want:
                    same += 1
                    same_as_game += got == data
                    continue
                differ += 1
                if len(examples) < 5:
                    rel = os.path.relpath(p, romfs)
                    if native_err:
                        examples.append(f'{rel}: native {got.decode(errors="replace")}')
                    elif want is None:
                        examples.append(f'{rel}: Python fails, native reads it')
                    else:
                        at = next((i for i in range(min(len(got), len(want))) if got[i] != want[i]), min(len(got), len(want)))
                        examples.append(f'{rel}: differs at byte {at:#x} (native {len(got)}, Python {len(want)} bytes)')
            print(f'{kind}: {len(paths)} files: {same} identical to the Python ({same_as_game} of them also to the file'
                  f' read), {both_err} refused by both, {differ} different'
                  + (f' (the Python fails on {py_err})' if py_err else ''))
            for e in examples:
                print('   ', e)
            failed |= differ > 0
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
