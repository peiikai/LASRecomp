#!/usr/bin/env python3
"""Writes runtime/rando/apply/las_world_data.cpp: what the ported world steps (las_world.cpp; docs/randomizer.md,
stage 3) need besides the seed's decisions:

  - LAS-Randomizer's code patches (RandomizerCore/ASM/Patches/*.asm, MIT, Owen-Splat and contributors), each block
    already assembled, as mod_generator.makeExefsPatches writes them into the .pchtxt files: the setting that turns it
    on, its title, its address and its bytes (a block that takes the Chest_Enemies pick: one per pick). The C++ has
    no assembler: the words are assembled here with LAS's own (keystone), at the base LAS gives them.
  - the game's level folders (region_common/level), which makeMusicChanges / changeLevelConfigs walk.

    <LAS venv python> tools/rando/export_las_world_tables.py <romfs>          # rewrite the file
    <LAS venv python> tools/rando/export_las_world_tables.py <romfs> --check  # exit 1 when it's out of date, or when
                                                                             # the table doesn't give LAS's own
                                                                             # .pchtxt for every setting tried

The .asm files are read in the order os.listdir gives them, as assemble.createRandomizerPatches does (the order the
patches appear in the .pchtxt): the order is part of the table. Declarations: runtime/rando/apply/las_world_data.h.
"""
import copy
import os
import random
import sys
import types

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
LAS = os.path.join(ROOT, 'third_party', 'LAS-Randomizer')
OUT = os.path.join(ROOT, 'runtime', 'rando', 'apply', 'las_world_data.cpp')

sys.modules.setdefault('PySide6', types.ModuleType('PySide6'))
sys.modules.setdefault('PySide6.QtCore', types.ModuleType('PySide6.QtCore'))
sys.path.insert(0, LAS)
sys.argv[0] = os.path.join(LAS, 'randomizer.py')  # (LAS finds its data next to argv[0] when run from source)

from keystone import Ks, KS_ARCH_ARM64, KS_MODE_LITTLE_ENDIAN  # noqa: E402
from RandomizerCore.ASM import assemble  # noqa: E402
from RandomizerCore.Tools.exefs_editor.patcher import Patcher  # noqa: E402
from RandomizerCore.randomizer_data import ENEMY_DATA  # noqa: E402

assemble.IS_RUNNING_FROM_SOURCE = False  # (quiet)
KS = Ks(KS_ARCH_ARM64, KS_MODE_LITTLE_ENDIAN)
BUILD_IDS = (('1.0.0.pchtxt', 'AE16F71E002AF8CB059A9A74C4D90F34BA984892'),
             ('1.0.1.pchtxt', '909E904AF78AC1B8DEEFE97AB2CCDB51968F0EC7'))


def symbolic_asm(asm, asm_data):
    """assemble.readASM with the setting left open: each patch with the setting (name, state) it needs (None:
    always). The same steps as there, line for line; it holds as long as a ';settings' line only starts a block (after
    a blank line), which it asserts - then readASM skips exactly the patches whose setting isn't met."""
    with open(f'{assemble.ASM_PATH}/{asm}', 'r') as f:
        asm_lines = f.read().splitlines()
    patches = []
    offset, instruction, patch_info, cond, data = 0, '', '', None, None
    for line in asm_lines:
        line = line.strip()
        if line.startswith(';*'):
            if len(patch_info) > 0:
                patch_info += '\n'
            patch_info += line.replace(';*', '//')
            continue
        elif line.startswith('; '):
            continue
        if line.startswith(';settings'):
            assert offset == 0 and instruction == '', f'{asm}: a setting inside a block'
            condition = line.split(' ')[1]
            state = True
            if condition.startswith('!'):
                state = False
                condition = condition.split('!')[1]
            cond = (condition, state)
            continue
        if len(line) == 0:
            if offset > 0 and len(instruction) > 0:
                patches.append((offset, instruction, patch_info, cond, data))
            cond, instruction, patch_info, offset, data = None, '', '', 0, None
            continue
        if ';data' in line:
            line_data = line.split(';data ')
            data = line_data[1]
            needed_data = asm_data[data]
            line = line_data[0].strip()
            register = line[-3:].strip()
            line = line.replace(register, needed_data)
        if ';' in line:
            line = line.split(';')[0]
            line = line.strip()
        if line.startswith('.offset'):
            if offset > 0 and len(instruction) > 0:
                patches.append((offset, instruction, patch_info, cond, data))
                instruction = ''
                patch_info = ''
                data = None
            offset = int(line.split(' ')[1][2:], 16)
            continue
        mnemonic = line.split(' ')[0]
        if mnemonic in ('cbz', 'cbnz', 'tbz', 'tbnz') and line.split(', ')[-1].startswith('0x'):
            operands = line[len(mnemonic):].strip().split(', ')
            diff = int(operands[-1], 16) - offset
            operands[-1] = ('+' if diff >= 0 else '') + hex(diff)
            line = f"{mnemonic} {', '.join(operands)}"
        elif line.startswith('b'):
            line_data = line.split(' ')
            branch_offset = int(line_data[1][2:], 16)
            diff = branch_offset - offset
            symbol = '+'
            if diff < 0:
                symbol = ''
            line = f"{line_data[0]} {symbol}{hex(diff)}"
        if line.startswith('adrp'):
            line_data = line.split(', ')
            data_offset = line_data[1]
            line_data = line_data[0]
            data_offset = data_offset.replace(data_offset[-3:], '000', -1)
            page_offset = hex(offset)
            page_offset = page_offset.replace(page_offset[-3:], '000', -1)
            diff = int(data_offset, 16) - int(page_offset, 16)
            line = f"{line_data}, {hex(diff)}"
        line = line.strip()
        instruction += line + '; '
    if offset > 0 and len(instruction) > 0:
        patches.append((offset, instruction, patch_info, cond, data))
    return patches


def encode(instruction):
    """What Patcher puts in the .pchtxt for one patch: (kind, text) - 0 code (hex), 1 string (as written), 2 short."""
    if instruction.startswith('.string'):
        return 1, instruction.split('.string ')[1].split('; ')[0]
    if instruction.startswith('.short'):
        v = int(instruction.split('.short ')[1].split('; ')[0].replace('#', ''))
        return 2, v.to_bytes(1, 'little', signed=True).hex().upper()
    return 0, KS.asm(instruction, as_bytes=True)[0].hex().upper()


def patch_table():
    """[(file, cond, comment, address, kind, text, data, {pick: text})] in the .pchtxt's order."""
    picks = list(ENEMY_DATA['Chest_Enemies'])
    files = [f for f in os.listdir(assemble.ASM_PATH) if f.endswith('.asm')]
    rows = []
    for asm in files:
        runs = [symbolic_asm(asm, {'CHEST_ENEMY': f'#{p}'}) for p in picks]
        for i, (addr, ins, comment, cond, data) in enumerate(runs[0]):
            if data is None:
                kind, text = encode(ins)
                rows.append((asm, cond, comment, addr, kind, text, None, None))
            else:
                assert data == 'CHEST_ENEMY', data
                variants = {}
                for p, run in zip(picks, runs):
                    kind, variants[p] = encode(run[i][1])
                rows.append((asm, cond, comment, addr, kind, None, data, variants))
    return rows


def setting_met(settings, name, state):
    """The C++ rule (las_world.cpp cond_met): settings[name] == state, after preSetup's stealing mapping."""
    if name not in settings:
        return False
    v = settings[name]
    if name == 'stealing' and v in ('always', 'never'):
        v = v == 'always'
    return v == state if isinstance(v, (bool, int, float)) else False


def model_pchtxt(rows, settings, pick, build_id):
    """What las_world.cpp writes, from the table (the C++ does the same walk)."""
    version = '1.0.0' if build_id.startswith('A') else '1.0.1'
    out = f'@nsobid-{build_id}\n# Zelda: Link\'s Awakening [01006BB00C6F0000] {version}\n\n@flag offset_shift 0x100\n'
    for asm, cond, comment, addr, kind, text, data, variants in rows:
        if cond is not None and not setting_met(settings, *cond):
            continue
        if data is not None:
            text = variants[pick]
        if comment:
            out += f'\n{comment}\n@enabled\n'
        out += f'{addr:08X} {text}\n'
    return out.encode('ascii')


def las_pchtxt(settings, seed):
    """LAS's own: mod_generator.makeExefsPatches with a random state, and the chest enemy it picks."""
    random.seed(seed)
    state = random.getstate()
    s = copy.deepcopy(settings)
    patcher = assemble.createRandomizerPatches(state, s)
    pick = None
    if settings.get('randomize-enemies'):
        r = random.Random()
        r.setstate(state)
        pick = r.choice(ENEMY_DATA['Chest_Enemies'])
    return [(patcher.generatePCHTXT(bid)) for _, bid in BUILD_IDS], pick


def check_semantics(rows):
    conds = sorted({r[1][0] for r in rows if r[1]})
    values = (True, False, None, 'normal', 1, 0)
    tries = []
    for v in values:
        tries.append({c: v for c in conds if v is not None})
    for c in conds:
        for v in values:
            for base in (True, False):
                s = {k: base for k in conds}
                if v is None:
                    del s[c]
                else:
                    s[c] = v
                tries.append(s)
    rnd = random.Random(7)
    for _ in range(300):
        s = {}
        for c in conds:
            v = rnd.choice(values)
            if v is not None:
                s[c] = v
        s['stealing'] = rnd.choice((True, False, 'always', 'never', 'normal'))
        tries.append(s)
    bad = 0
    for n, s in enumerate(tries):
        s.setdefault('randomize-enemies', False)
        s.setdefault('stealing', 'normal')
        want, pick = las_pchtxt(s, f'check{n}')
        got = [model_pchtxt(rows, s, pick, bid) for _, bid in BUILD_IDS]
        if want != got:
            bad += 1
            if bad <= 3:
                print('table differs from LAS for settings', s, file=sys.stderr)
    return bad, len(tries)


def c_str(s):
    out = '"'
    for ch in s:
        if ch in '"\\':
            out += '\\' + ch
        elif ch == '\n':
            out += '\\n'
        elif 32 <= ord(ch) < 127:
            out += ch
        else:
            raise ValueError(f'unexpected character {ch!r}')
    return out + '"'


def level_folders(romfs):
    """makeMusicChanges' folders: sorted(os.listdir(levels) without the .ldb file)."""
    return sorted(f for f in os.listdir(os.path.join(romfs, 'region_common', 'level')) if not f.endswith('.ldb'))


def generate(romfs, rows):
    L = ['// Generated by tools/rando/export_las_world_tables.py from LAS-Randomizer\'s RandomizerCore/ASM (MIT, Owen-Splat',
         '// and contributors; assembled with its own assembler) and the game\'s level folder names. Don\'t edit: rerun',
         '// the script.',
         '#include "las_world_data.h"', '', 'namespace rando {', 'namespace las {', 'namespace world {', '']
    var_names = {}
    for i, r in enumerate(rows):
        if r[7] is not None:
            name = f'kVariants{i}'
            var_names[i] = name
            L.append(f'static const std::vector<std::pair<int, const char *>> {name} = {{')
            L += [f'    {{{p}, {c_str(t)}}},' for p, t in r[7].items()]
            L += ['};', '']
    L.append('const std::vector<AsmPatch> ASM_PATCHES = {')
    for i, (asm, cond, comment, addr, kind, text, data, variants) in enumerate(rows):
        cn = c_str(cond[0]) if cond else 'nullptr'
        cs = ('true' if cond[1] else 'false') if cond else 'true'
        tx = c_str(text) if text is not None else 'nullptr'
        dv = c_str(data) if data else 'nullptr'
        vv = f'&{var_names[i]}' if i in var_names else 'nullptr'
        L.append(f'    {{{c_str(asm)}, {cn}, {cs}, {c_str(comment)}, 0x{addr:X}, {kind}, {tx}, {dv}, {vv}}},')
    L += ['};', '']
    L.append('const std::vector<const char *> LEVEL_FOLDERS = {')
    L += [f'    {c_str(f)},' for f in level_folders(romfs)]
    L += ['};', '', '}  // namespace world', '}  // namespace las', '}  // namespace rando', '']
    return '\n'.join(L)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    if len(args) != 1:
        print(__doc__, file=sys.stderr)
        return 2
    rows = patch_table()
    text = generate(args[0], rows)
    if '--check' in sys.argv[1:]:
        old = open(OUT).read() if os.path.exists(OUT) else ''
        bad, n = check_semantics(rows)
        print(f'{n} settings tried, {bad} differ from LAS\'s .pchtxt')
        if old != text:
            print(f'{os.path.relpath(OUT, ROOT)} is out of date: run tools/rando/export_las_world_tables.py',
                  file=sys.stderr)
            return 1
        return 1 if bad else 0
    with open(OUT, 'w') as f:
        f.write(text)
    return 0


if __name__ == '__main__':
    sys.exit(main())
