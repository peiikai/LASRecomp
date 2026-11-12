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
if [ ! -f "$DL/miniz-$VER.zip" ]; then  # (a .part file until it's whole: a download cut off partway is fetched again)
    curl -fsSL --proto '=https' -o "$DL/miniz-$VER.zip.part" \
        "https://github.com/richgel999/miniz/releases/download/$VER/miniz-$VER.zip"
    mv "$DL/miniz-$VER.zip.part" "$DL/miniz-$VER.zip"
fi
GOT=$( (shasum -a 256 2>/dev/null || sha256sum) < "$DL/miniz-$VER.zip" | cut -d' ' -f1)
# (a zip with the wrong hash is deleted, so the next run downloads it again instead of failing the same way: #737)
[ "$GOT" = "$SHA" ] || { echo "miniz-$VER.zip: sha256 $GOT, expected $SHA"; rm -f "$DL/miniz-$VER.zip"; exit 1; }
# Git Bash on Windows has no unzip, and its own tar is GNU tar, which can't read zips (Windows' bsdtar can). So try
# each extractor in turn and keep the first that produces all three files.
FILES="miniz.c miniz.h LICENSE"
ZIP="$DL/miniz-$VER.zip"
TMP="$DL/x"
extract() {
    case "$1" in
        unzip) unzip -q "$ZIP" $FILES -d "$TMP" ;;
        tar) tar -xf "$ZIP" -C "$TMP" $FILES ;;
        python3|python) "$1" -m zipfile -e "$ZIP" "$TMP" ;;
    esac
}
ok=
for tool in unzip tar python3 python; do
    command -v "$tool" >/dev/null 2>&1 || continue
    rm -rf "$TMP" && mkdir -p "$TMP"
    extract "$tool" >/dev/null 2>&1 || continue
    ok=1
    for f in $FILES; do [ -f "$TMP/$f" ] || ok=; done
    [ -n "$ok" ] && break
done
[ -n "$ok" ] || { echo "can't extract $ZIP: install unzip, a tar that reads zips (bsdtar) or Python 3"; exit 1; }
rm -rf "$OUT" && mkdir -p "$OUT"
for f in $FILES; do mv "$TMP/$f" "$OUT/"; done
rm -rf "$TMP"
touch "$OUT/.fetched"
echo "$OUT: miniz $VER"
