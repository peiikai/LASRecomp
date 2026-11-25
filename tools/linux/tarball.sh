#!/bin/bash
# make tarball ARCH=<x86_64|aarch64> (after make appimage, which leaves the AppDir): build/dist/ReAwakening-<version>-linux-<arch>.tar.gz
# The same files as the AppImage, unpacked into one "LAS Recomp" folder, for places with no FUSE to mount an
# AppImage (a Flatpak sandbox such as a launcher's). Start it with ./AppRun (or ./lasrecomp, a link to it).
# portable.txt in the folder keeps the saves, settings and game files there, like the Windows zip
# (runtime/paths.c); delete it to use ~/.local/share/lasrecomp. No self-update: the update dialog offers Download.
# See docs/linux.md.
set -euo pipefail
cd "$(dirname "$0")/../.."
ARCH=${1:?usage: tools/linux/tarball.sh x86_64|aarch64}
APPDIR=build/linux/$ARCH/AppDir
[ -x "$APPDIR/AppRun" ] || { echo "no $APPDIR: make appimage ARCH=$ARCH first"; exit 1; }
VERSION=$(sed -n 's/^#define LARECOMP_VERSION "\(.*\)"/\1/p' runtime/ui/launcher.mm)
. tools/dist_name.sh  # (nightlies keep LAS Recomp-...)
OUT="build/dist/$(dist_name "$VERSION")-$VERSION-linux-$ARCH.tar.gz"
STAGE=build/linux/$ARCH/tarball
rm -rf "$STAGE" && mkdir -p "$STAGE/LAS Recomp"
cp -a "$APPDIR/." "$STAGE/LAS Recomp/"
ln -s AppRun "$STAGE/LAS Recomp/lasrecomp"
cat > "$STAGE/LAS Recomp/portable.txt" <<'EOF'
While this file is here, LAS Recomp keeps its saves, settings and game files in this folder.
Delete it to keep them in ~/.local/share/lasrecomp instead.
EOF
tools/linux/check_no_game_code.sh "$STAGE/LAS Recomp"
mkdir -p build/dist && rm -f "$OUT"
# (owner and times fixed: the same AppDir gives the same archive)
tar -C "$STAGE" --owner=0 --group=0 --numeric-owner --sort=name --mtime='@0' -czf "$OUT" "LAS Recomp"
ls -la "$OUT"
