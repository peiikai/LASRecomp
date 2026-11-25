#!/bin/bash
# Puts one platform's nightly file on the rolling "nightly" pre-release (.github/workflows/nightly.yml), replacing that
# platform's older nightly file. Each platform's job calls it as soon as its own build is done, so a platform that is
# slow or fails never holds the others back.
# The platform's update manifest (build/dist/update-<platform>.txt, tools/update_manifest.sh) or Sparkle feed
# (appcast-macOS.xml, tools/sparkle_appcast.sh) goes up right after the file and before the older file goes, so it
# always names a file that is there; without one (no signing key) the old one is removed, so Update never fetches a
# file of an older round.
# Usage: tools/nightly_publish.sh <file> <platform suffix, e.g. macOS.dmg> [manifest platform]   (GH_TOKEN, GH_REPO set)
# The manifest platform defaults to the suffix without its extension; Windows x64 passes "windows" (its files say
# windows-x64, its manifest stays update-windows.txt); "-": no manifest (a second name for the same file).
set -euo pipefail
f=$1
suffix=$2
[ -f "$f" ] || { echo "nightly_publish.sh: no file $f" >&2; exit 1; }
new=$(basename "$f" | tr ' ' .)  # (GitHub stores "LAS Recomp-..." as "LAS.Recomp-...")
case $new in LAS.Recomp-nightly-*-"$suffix") ;; *) echo "nightly_publish.sh: $new is not a nightly $suffix file" >&2; exit 1 ;; esac
gh release upload nightly "$f" --clobber
platform=${3:-${suffix%.*}}  # (linux-x86_64.AppImage -> linux-x86_64)
[ "$platform" = - ] || for m in "update-$platform.txt" "appcast-$platform.xml"; do
    if [ -f "$(dirname "$f")/$m" ]; then gh release upload nightly "$(dirname "$f")/$m" --clobber
    else gh release delete-asset nightly "$m" -y 2>/dev/null || true; fi
done
# (this platform's older nightly files out, after the new one is up: the page never lacks this platform)
gh release view nightly --json assets -q '.assets[].name' | while read -r old; do
    case $old in LAS.Recomp-nightly-*-"$suffix") [ "$old" = "$new" ] || gh release delete-asset nightly "$old" -y ;; esac
done
echo "nightly: $new"
