"""The data folder's game-file links never change the player's own files (runtime/romfs_build.c, plat_remove_tree).
Builds tools/test_game_file_links.c with the real romfs_build.c, paths.c and this platform's file; scratch folders only."""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
plat = 'windows' if os.name == 'nt' else 'macos' if sys.platform == 'darwin' else 'linux'
flags = []
deps = os.environ.get('DEPS')  # (a prefix with SDL3, as for make DEPS=)
if deps:
    flags = [f'-I{deps}/include', f'-L{deps}/lib', f'-Wl,-rpath,{deps}/lib', '-lSDL3']
elif shutil.which('pkg-config') and subprocess.run(['pkg-config', '--exists', 'sdl3']).returncode == 0:
    flags = shlex.split(subprocess.run(['pkg-config', '--cflags', '--libs', 'sdl3'], capture_output=True, text=True,
                                       check=True).stdout)
elif sys.platform == 'darwin':
    brew = subprocess.run(['brew', '--prefix'], capture_output=True, text=True).stdout.strip() or '/opt/homebrew'
    flags = [f'-I{brew}/include', f'-L{brew}/lib', '-lSDL3']
extra = {'macos': ['-framework', 'CoreFoundation'], 'linux': ['-pthread', '-ldl'],
         'windows': ['-D_FILE_OFFSET_BITS=64', '-D_POSIX_THREAD_SAFE_FUNCTIONS', '-D_CRT_RAND_S',
                     '-DWIN32_LEAN_AND_MEAN', '-DNOMINMAX', '-lwinmm', '-lole32', '-lshell32', '-lcomdlg32', '-luuid', '-ladvapi32']}[plat]
with tempfile.TemporaryDirectory() as directory:
    directory = Path(directory)
    exe = directory / ('game-file-links-test.exe' if os.name == 'nt' else 'game-file-links-test')
    subprocess.run(shlex.split(os.environ.get('CC', 'clang')) +
                   ['-std=gnu11', '-Iruntime', 'runtime/romfs_build.c', 'runtime/paths.c',
                    f'runtime/platform/{plat}.c',
                    'tools/test_game_file_links.c', '-o', str(exe)] + flags + extra, cwd=ROOT, check=True)
    scratch = directory / 'scratch'
    scratch.mkdir()
    run = subprocess.run([str(exe), str(scratch)], capture_output=True, text=True)
    print(run.stderr + run.stdout, end='')
    assert run.returncode == 0 and 'all checks passed' in run.stdout, f'{plat}: game file links test failed'
    print(f"{plat}: the data folder's game-file links never change the player's files")
