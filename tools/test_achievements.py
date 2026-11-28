#!/usr/bin/env python3
"""Exercise the production achievement tracker with synthetic saves, without game files."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="las-achievements-") as folder:
    exe = Path(folder) / "test"
    subprocess.run([os.environ.get("CXX", "clang++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-g", str(root / "runtime/achievements.cpp"),
                    str(root / "tools/test_achievements.cpp"), "-o", str(exe)], check=True)
    subprocess.run([str(exe), str(Path(folder) / "unlocks.log")], check=True)
