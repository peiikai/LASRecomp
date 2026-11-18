#!/bin/bash
# tools/linux/check_no_game_code.sh <folder>: fails if a Linux build's files (an AppDir, an unpacked AppImage, the
# tarball's folder) hold anything of the game: game files, generated game code, a built game library, or an ELF file
# with game functions in it (readelf: any arch). release.yml and nightly.yml run it on everything they ship.
set -euo pipefail
A=${1:?usage: tools/linux/check_no_game_code.sh <folder>}
[ -d "$A" ] || { echo "::error::no folder $A"; exit 1; }
bad=$(find "$A" -type d \( -name extracted -o -name 'generated*' -o -name exefs -o -name romfs \);
      find "$A" -name '*.c' ! -path '*/zig/lib/*' ! -path '*/gamelib-src/gamelib_entry.c'; find "$A" -name 'lasgame*')
[ -z "$bad" ] || { echo "::error::must not ship: $bad"; exit 1; }
find "$A" -type f \( -perm /111 -o -name '*.so*' \) | while read -r f; do
    if readelf -Ws --wide "$f" 2>/dev/null | grep ' f_70' >/dev/null; then echo "::error::game code in $f"; exit 1; fi
done
echo "no game code in $A"
