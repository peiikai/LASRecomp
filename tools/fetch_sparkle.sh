#!/bin/bash
# Sparkle 2 (MIT: https://sparkle-project.org), the macOS app's updater (runtime/ui/sparkle.mm), into
# build/sparkle/ (Sparkle.framework, LICENSE). tools/bundle_macos.sh puts the framework in the app. Fetches the pinned
# release and checks its hash; needs network once, as tools/fetch_zig.sh does.
set -euo pipefail
cd "$(dirname "$0")/.."
VER=2.10.1
SHA=1a856d2a9edb8e628de88121d7be04cf0e2167f8641650b78fca00b14244cf27
DL=build/sparkle-dl
OUT=build/sparkle
[ -f "$OUT/.ok-$VER" ] && exit 0
mkdir -p "$DL"
TAR="$DL/Sparkle-$VER.tar.xz"
if [ ! -f "$TAR" ]; then  # (a .part file until it's whole)
    curl -fsSL --proto '=https' -o "$TAR.part" "https://github.com/sparkle-project/Sparkle/releases/download/$VER/Sparkle-$VER.tar.xz"
    mv "$TAR.part" "$TAR"
fi
GOT=$(shasum -a 256 < "$TAR" | cut -d' ' -f1)
[ "$GOT" = "$SHA" ] || { echo "Sparkle-$VER.tar.xz: sha256 $GOT, expected $SHA"; rm -f "$TAR"; exit 1; }
rm -rf "$OUT" "$DL/x" && mkdir -p "$DL/x" "$OUT"
tar -xJf "$TAR" -C "$DL/x" Sparkle.framework LICENSE
ditto "$DL/x/Sparkle.framework" "$OUT/Sparkle.framework"  # (ditto: links and signatures as they are)
cp "$DL/x/LICENSE" "$OUT/LICENSE"
touch "$OUT/.ok-$VER"
echo "build/sparkle: Sparkle $VER"
