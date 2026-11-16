#!/bin/bash
# The self-updater's signed manifest for one release file (runtime/update_core.h): build/dist/update-<platform>.txt.
# release.yml and nightly.yml run it after each platform's build, with the UPDATE_SIGNING_KEY secret in the
# environment. Without the key (a fork, or a run before the key existed) it only warns: the release still ships, and
# the start screen's Update needs the manifest, so players there use Download.
# Usage: tools/update_manifest.sh <version> <platform> <file> [<folder the zip holds>]
set -euo pipefail
cd "$(dirname "$0")/.."
[ $# -ge 3 ] || { echo "usage: tools/update_manifest.sh <version> <platform> <file> [<folder>]" >&2; exit 2; }
out=build/dist/update-$2.txt
rm -f "$out"
if [ -z "${UPDATE_SIGNING_KEY:-}" ]; then
    echo "::warning::UPDATE_SIGNING_KEY is not set: no update manifest for $2 (players there update with Download)"
    exit 0
fi
mkdir -p build
"${CXX_UPDATE:-clang++}" -std=c++17 -O2 -Iruntime tools/update_sign.cpp -o build/update_sign
build/update_sign manifest "$@" > "$out.part"
mv "$out.part" "$out"
head -4 "$out"
echo "$(grep -c '^file ' "$out" || true) files; $out"
