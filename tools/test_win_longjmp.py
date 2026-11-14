"""Windows, x64 Python with unicorn (what lasgen is built from): unicorn's emulation with VCRUNTIME140's longjmp and
with tools/win_longjmp.py's (#779). The first may be stopped (0xC0000409) under x64 emulation on Windows on ARM: that is
reported, not failed. The second must work everywhere, including emulation errors (also a longjmp).
Run: python tools/test_win_longjmp.py (with `pip install unicorn==2.1.4`, as the Makefile's gen-helper)."""
import os
import subprocess
import sys

if os.name != 'nt':
    raise SystemExit('Windows-only test')

TOOLS = os.path.dirname(os.path.abspath(__file__))


def child(fixed):
    sys.path.insert(0, TOOLS)
    import unicorn as uc
    from unicorn import arm64_const as A
    import win_longjmp
    print('own' if fixed else 'VCRUNTIME140', 'longjmp:', win_longjmp.describe(), flush=True)
    if fixed:
        r = win_longjmp.apply()
        assert r == 'own longjmp', r
    base = 0x7004000000
    mu = uc.Uc(uc.UC_ARCH_ARM64, uc.UC_MODE_ARM)
    mu.mem_map(base, 0x1000)
    # add x0, x0, #1 ; add x1, x1, #2 ; ldr x2, [x3] ; br x4
    mu.mem_write(base, bytes.fromhex('00040091 21080091 620040f9 80001fd6'))
    for i in range(2000):  # (one instruction at a time, as gen.py's jump tables: every stop leaves by longjmp)
        mu.reg_write(A.UC_ARM64_REG_X0, i)
        mu.emu_start(base, 0, count=1)
        assert mu.reg_read(A.UC_ARM64_REG_X0) == i + 1
    mu.reg_write(A.UC_ARM64_REG_X1, 5)
    mu.emu_start(base, base + 8)
    assert mu.reg_read(A.UC_ARM64_REG_X1) == 7
    mu.reg_write(A.UC_ARM64_REG_X3, 0x123450000)  # (unmapped: the error path)
    for _ in range(100):
        try:
            mu.emu_start(base + 8, 0, count=1)
            raise AssertionError('read of unmapped memory did not fail')
        except uc.UcError as e:
            assert e.errno == uc.UC_ERR_READ_UNMAPPED, e
    print('ok', flush=True)


if len(sys.argv) > 1:
    child(sys.argv[1] == 'fixed')
    sys.exit(0)

for fixed in (False, True):
    r = subprocess.run([sys.executable, __file__, 'fixed' if fixed else 'plain'], capture_output=True, text=True,
                       timeout=120)
    sys.stdout.write(r.stdout + r.stderr)
    code = r.returncode & 0xFFFFFFFF
    if fixed:
        assert code == 0 and r.stdout.rstrip().endswith('ok'), f'own longjmp: exit {code:#x}'
    elif code:
        print(f'::notice::unicorn with VCRUNTIME140 longjmp: exit {code:#x}'
              + (' (fast fail: the #779 case)' if code == 0xC0000409 else ''))
print("unicorn's longjmp: emulation and its errors with tools/win_longjmp.py passed")
