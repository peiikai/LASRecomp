#!/bin/bash
# miniz (MIT: deflate and zip archives) for runtime/zipfile.c - the iOS kit the desktop reads and the .ipa it writes.
# Fetches the pinned release into build/miniz/ (miniz.c, miniz.h, LICENSE) and checks its hash. The Makefile runs
# this when build/miniz/.fetched is missing. The build needs network once, as for zig (tools/fetch_zig.sh).
set -euo pipefail
cd "$(dirname "$0")/.."
VER=3.0.2
SHA=ada38db0b703a56d3dd6d57bf84a9c5d664921d870d8fea4db153979fb5332c5
DL=build/miniz-dl
OUT=build/miniz
mkdir -p "$DL"
[ -f "$DL/miniz-$VER.zip" ] || curl -fsSL --proto '=https' -o "$DL/miniz-$VER.zip" \
    "https://github.com/richgel999/miniz/releases/download/$VER/miniz-$VER.zip"
GOT=$( (shasum -a 256 2>/dev/null || sha256sum) < "$DL/miniz-$VER.zip" | cut -d' ' -f1)
[ "$GOT" = "$SHA" ] || { echo "miniz-$VER.zip: sha256 $GOT, expected $SHA"; exit 1; }
rm -rf "$OUT" && mkdir -p "$OUT"
unzip -q "$DL/miniz-$VER.zip" miniz.c miniz.h LICENSE -d "$OUT"
touch "$OUT/.fetched"
echo "$OUT: miniz $VER"
