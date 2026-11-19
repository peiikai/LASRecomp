#!/bin/bash
# tools/linux/gen_helper.sh <x86_64|aarch64> (inside the build container of that arch, tools/linux/appimage.sh): the
# game code generator frozen by PyInstaller (tools/lasgen.spec, as `make gen-helper`) with the container's pinned
# standalone Python 3.12 (tools/linux/Containerfile), into build/linux/<arch>/helper/lasgen. The wheels pip takes or
# builds are kept in build/linux/<arch>/wheels for the next run.
set -euo pipefail
cd "$(dirname "$0")/../.."
L=$PWD/build/linux/$1
V=$L/gen-venv
REQS=(pyinstaller==6.22.3 capstone==5.0.9 unicorn==2.1.4 lz4==4.4.5)
if [ ! -f "$V/.ok" ]; then
    rm -rf "$V" && python3.12 -m venv "$V"
    "$V/bin/python" -m pip install -q --upgrade pip
    "$V/bin/python" -m pip wheel -q -w "$L/wheels" --find-links "$L/wheels" "${REQS[@]}"
    "$V/bin/python" -m pip install -q --no-index --find-links "$L/wheels" "${REQS[@]}"
    touch "$V/.ok"
fi
"$V/bin/python" -m PyInstaller --noconfirm --clean --log-level WARN --distpath "$L/helper" --workpath "$L/pyinstaller" \
    tools/lasgen.spec
du -sh "$L/helper/lasgen"
