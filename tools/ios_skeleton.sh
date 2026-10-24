#!/bin/bash
# The iOS skeleton zip: build/dist/LAS Recomp-<version>-ios-skeleton.zip, what the start screen on any desktop turns
# into the iOS app with the player's own game files (runtime/iosexport.c). Unpacked, the folder holds:
#   LAS Recomp.app/      the app without game code (tools/bundle_ios.sh SKELETON=1; symlinks as .symlink files)
#   lld/<arch>-<os>/     the Mach-O linker for each desktop (tools/fetch_lld.sh: rust-lld, bin/ + lib/)
#   include/any-darwin-any/  zig's Darwin libc headers (the bundled zig compiles for iOS with these; its own copy
#                        is trimmed to the host's, tools/fetch_zig.sh)
#   libSystem.B.tbd      the libSystem stub the link needs (runtime/platform/ios-sdk)
#   README.txt
# Run after `make ios-skeleton` (the app) and tools/fetch_lld.sh for every LLD_TARGET (the linkers); needs the
# zig release archive of this host in build/zig-dl (tools/fetch_zig.sh fetched it) for the untrimmed headers.
set -euo pipefail
cd "$(dirname "$0")/.."
APP=${APP:-build/ios-skeleton/LAS Recomp.app}
VERSION=$(sed -n 's/^#define LARECOMP_VERSION "\(.*\)"/\1/p' runtime/ui/launcher.mm)
[ -x "$APP/lasrecomp" ] || { echo "no $APP (make ios-skeleton)"; exit 1; }
[ -e "$APP/Frameworks/LASGame.framework/LASGame" ] && { echo "$APP holds game code"; exit 1; }
if find "$APP" -type l | grep . >/dev/null; then echo "$APP has symlinks (SKELETON=1 writes .symlink files)"; exit 1; fi
for t in aarch64-macos x86_64-linux aarch64-linux x86_64-windows; do
    [ -d "build/lld/$t/bin" ] || { echo "no build/lld/$t (LLD_TARGET=$t tools/fetch_lld.sh)"; exit 1; }
done

STAGE=build/ios-skeleton-zip
NAME="LAS Recomp-$VERSION-ios-skeleton"
rm -rf "$STAGE" && mkdir -p "$STAGE/$NAME"
S="$STAGE/$NAME"
cp -R "$APP" "$S/LAS Recomp.app"
cp -R build/lld "$S/lld"
cp runtime/platform/ios-sdk/libSystem.B.tbd "$S/"
# the Darwin headers, from the zig archive (build/zig keeps only the host's set)
ZVER=$(sed -n 's/^VER=//p' tools/fetch_zig.sh)
ZA=$(find build/zig-dl -maxdepth 1 \( -name "zig-*-$ZVER.tar.xz" -o -name "zig-*-$ZVER.zip" \) | head -1)
[ -n "$ZA" ] || { echo "no zig archive in build/zig-dl (tools/fetch_zig.sh)"; exit 1; }
mkdir -p "$S/include"
ZN=$(basename "$ZA"); ZN=${ZN%.tar.xz}; ZN=${ZN%.zip}
case "$ZA" in
    *.zip) unzip -q "$ZA" "$ZN/lib/libc/include/any-darwin-any/*" -d "$STAGE/zig-tmp"
           mv "$STAGE/zig-tmp/$ZN/lib/libc/include/any-darwin-any" "$S/include/"; rm -rf "$STAGE/zig-tmp" ;;
    *) tar -xJf "$ZA" -C "$S/include" --strip-components 4 "$ZN/lib/libc/include/any-darwin-any" ;;
esac
cat > "$S/README.txt" <<EOF
LAS Recomp $VERSION - iOS skeleton

Not an app you can install: the iOS app without the game code. LAS Recomp on your Mac, Linux or Windows machine
(the start screen's "Recomp for iOS") builds the game code from your own game files with the tools in here and
saves an .ipa for your own sideloading tool and Apple ID (Impactor, SideStore, AltStore...). The app needs one
entitlement, com.apple.developer.kernel.increased-memory-limit, which a free developer account grants; it is in the
executable's signature, where those tools read it. On the device, put your extracted/ folder (exefs and romfs) in
the app's Documents through the Files app or Finder.

Contents: LAS Recomp.app (the app), lld/ (the linker for each desktop OS, from Rust's rust-lld: Apache-2.0 with LLVM
exception), include/ (zig's Darwin C headers: MIT / Apple's APSL), libSystem.B.tbd (the link stub).
EOF
mkdir -p build/dist
OUT="build/dist/$NAME.zip"
rm -f "$OUT"
(cd "$STAGE" && zip -qr -X "../../$OUT" "$NAME")
rm -rf "$STAGE"
echo "$OUT ($(du -h "$OUT" | cut -f1))"
