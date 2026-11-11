"""Text speed hook checks (#681), without game files."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as directory:
    exe = Path(directory) / ('text-speed-test.exe' if os.name == 'nt' else 'text-speed-test')
    subprocess.run([*shlex.split(os.environ.get('CC', 'clang')), '-std=gnu11', '-O2', '-Iruntime',
                    'tools/test_text_speed.c', '-o', str(exe)], cwd=root, check=True)
    subprocess.run([str(exe)], check=True, timeout=30)
