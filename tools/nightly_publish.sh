#!/bin/bash
# Puts one platform's nightly file on the rolling "nightly" pre-release (.github/workflows/nightly.yml), replacing that
# platform's older nightly file. Each platform's job calls it as soon as its own build is done, so a platform that is
# slow or fails never holds the others back.
# Usage: tools/nightly_publish.sh <file> <platform suffix, e.g. macOS.dmg>   (GH_TOKEN, GH_REPO set)
set -euo pipefail
f=$1
suffix=$2
[ -f "$f" ] || { echo "nightly_publish.sh: no file $f" >&2; exit 1; }
new=$(basename "$f" | tr ' ' .)  # (GitHub stores "LAS Recomp-..." as "LAS.Recomp-...")
case $new in LAS.Recomp-nightly-*-"$suffix") ;; *) echo "nightly_publish.sh: $new is not a nightly $suffix file" >&2; exit 1 ;; esac
gh release upload nightly "$f" --clobber
# (this platform's older nightly files out, after the new one is up: the page never lacks this platform)
gh release view nightly --json assets -q '.assets[].name' | while read -r old; do
    case $old in LAS.Recomp-nightly-*-"$suffix") [ "$old" = "$new" ] || gh release delete-asset nightly "$old" -y ;; esac
done
echo "nightly: $new"
