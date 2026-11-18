"""The command line for launchers (runtime/main.c, docs/debug-hooks.md "Command line") and, on Linux, the data folder
rules for the tarball and the AppImage's portable folder (runtime/paths.c). Runs the built app (build/lasrecomp, or
LARECOMP_EXE) with --list-settings, which needs no window, GPU or game files; scratch folders only.
--prepare and --play need the game files and a GPU: they are tested by hand (docs/debug-hooks.md)."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
EXE = Path(os.environ.get('LARECOMP_EXE') or ROOT / 'build' / ('lasrecomp.exe' if os.name == 'nt' else 'lasrecomp'))
failures = []


def check(what, ok, detail=''):
    print(('ok: ' if ok else 'FAIL: ') + what + (f' ({detail})' if detail and not ok else ''))
    if not ok:
        failures.append(what)


with tempfile.TemporaryDirectory() as t:
    t = Path(t).resolve()
    home, xdg = t / 'home', t / 'xdg'
    home.mkdir()
    base = {k: v for k, v in os.environ.items()
            if not k.startswith(('LARECOMP_', 'RECOMPLAUNCHER', 'APPIMAGE', 'APPDIR', 'LASRECOMP_'))}
    base.update(HOME=str(home), XDG_DATA_HOME=str(xdg), LARECOMP_DATA_HOME=str(t / 'platform-home'))

    def run(args, env=None, exe=EXE, cwd=None):
        e = dict(base, **(env or {}))
        return subprocess.run([str(exe)] + args, env=e, cwd=cwd or t, capture_output=True, text=True, timeout=120)

    def data_of(args, env=None, exe=EXE):
        """The data folder a run used: its "[paths] data:" line (stderr, or the data folder's log without one)"""
        r = run(args + ['--list-settings'], env, exe)
        lines = r.stderr.splitlines()
        for log in list(t.rglob('lasrecomp.log')):
            lines += log.read_text(errors='replace').splitlines()
            log.unlink()
        found = [line[len('[paths] data: '):] for line in lines if line.startswith('[paths] data: ')]
        return found[-1] if found else None, r

    def settings(stdout):
        out = {}
        for line in stdout.splitlines():
            if '=' in line and not line.startswith('#'):
                k, v = line.split('=', 1)
                out[k.strip()] = v.strip()
        return out

    r = run(['--version'])
    check('--version prints the version', r.returncode == 0 and r.stdout.startswith('LAS Recomp '), r.stdout + r.stderr)
    r = run(['--help'])
    check('--help lists the options', r.returncode == 0 and all(o in r.stdout for o in
          ['--data-dir', '--game-files', '--prepare', '--set', '--list-settings', '--no-update-check']))
    # the RecompLauncher profile we ship (tools/recomplauncher.json): JSON, and only options this app knows
    import json
    profile = json.loads((ROOT / 'tools' / 'recomplauncher.json').read_text())
    used = {a for part in ('start', 'prepare') for a in profile[part]['args'] if a.startswith('--')}
    used |= {a for row in profile.get('settings', []) for c in row.get('choices', []) for a in c.get('args', [])
             if a.startswith('--')}
    help_text = run(['--help']).stdout
    check('the RecompLauncher profile uses known options', all(a in help_text for a in used),
          sorted(a for a in used if a not in help_text))
    r = run(['--set'])
    check('an option without its value fails', r.returncode == 2)

    d = t / 'data'
    d.mkdir()
    ini = d / 'larecomp.ini'
    ini.write_text('fullscreen = 0\nvolume = 0.50\nresolution = 2\n')
    before = ini.read_bytes()
    folder, r = data_of(['--data-dir', str(d), '--fullscreen', '--set', 'volume=1.5', '--set', 'bogus=1'])
    s = settings(r.stdout)
    check('--data-dir is the data folder', folder == str(d), folder)
    check('--fullscreen and --set apply', s.get('fullscreen') == '1' and s.get('volume') == '1.50', s)
    check('the file keeps its own values', ini.read_bytes() == before)
    check('the rest comes from the file', s.get('resolution') == '2x', s)
    s = settings(run(['--list-settings'], {'LARECOMP_DATA_ROOT': str(d), 'LARECOMP_SET': 'volume=0.75; fullscreen = 1'}).stdout)
    check('LARECOMP_DATA_ROOT and LARECOMP_SET', s.get('volume') == '0.75' and s.get('fullscreen') == '1', s)
    s = settings(run(['--data-dir', str(d), '--set', 'volume=0.6', '--set', 'volume=0.7', '--list-settings']).stdout)
    check('the last --set of a key wins', s.get('volume') == '0.70', s)
    rl = {'RECOMPLAUNCHER': '1', 'RECOMPLAUNCHER_MODE': 'bigpicture'}
    s = settings(run(['--data-dir', str(d), '--list-settings'], rl).stdout)
    check('RecompLauncher Big Picture: fullscreen', s.get('fullscreen') == '1', s)
    s = settings(run(['--data-dir', str(d), '--windowed', '--list-settings'], rl).stdout)
    check('... unless --windowed', s.get('fullscreen') == '0', s)
    s = settings(run(['--data-dir', str(d), '--list-settings'], dict(rl, RECOMPLAUNCHER_MODE='desktop')).stdout)
    check('RecompLauncher desktop: the file\'s setting', s.get('fullscreen') == '0', s)

    if sys.platform.startswith('linux'):
        # the tarball: <folder>/usr/bin/lasrecomp with <folder>/portable.txt (an unpacked AppImage the same)
        tb = t / 'LAS Recomp'
        (tb / 'usr' / 'bin').mkdir(parents=True)
        exe = tb / 'usr' / 'bin' / 'lasrecomp'
        shutil.copy2(EXE, exe)
        (tb / 'portable.txt').write_text('portable\n')
        folder, _ = data_of([], exe=exe)
        check('tarball: its own folder', folder == str(tb), folder)
        folder, _ = data_of([], {'APPDIR': str(tb)}, exe=exe)
        check('an AppImage a launcher unpacked (APPDIR, no APPIMAGE): its own folder', folder == str(tb), folder)
        folder, _ = data_of([], {'APPDIR': str(tb), 'APPIMAGE': str(t / 'x.AppImage')}, exe=exe)
        check('a mounted AppImage never writes into itself', folder != str(tb), folder)
        folder, _ = data_of(['--data-dir', str(d)], exe=exe)
        check('tarball --data-dir', folder == str(d), folder)
        (tb / 'portable.txt').unlink()
        folder, _ = data_of([], {'LARECOMP_DATA_HOME': ''}, exe=exe)
        check('tarball without portable.txt: XDG_DATA_HOME', folder == str(xdg / 'lasrecomp'), folder)
        # the AppImage's portable folder: <file>.AppImage.home next to it ($APPIMAGE names the file)
        ai = t / 'game.AppImage'
        ai.write_bytes(b'')
        (t / 'game.AppImage.home').mkdir()
        folder, _ = data_of([], {'APPIMAGE': str(ai), 'LARECOMP_DATA_HOME': ''})
        check('AppImage with .home', folder == str(t / 'game.AppImage.home' / '.local' / 'share' / 'lasrecomp'), folder)
        (t / 'game.AppImage.home').rmdir()
        folder, _ = data_of([], {'APPIMAGE': str(ai), 'LARECOMP_DATA_HOME': ''})
        check('AppImage without .home: XDG_DATA_HOME', folder == str(xdg / 'lasrecomp'), folder)

if failures:
    sys.exit(f'{len(failures)} command line check(s) failed')
print('command line: all checks passed')
