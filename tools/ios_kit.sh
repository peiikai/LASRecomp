#!/bin/bash
# The iOS kit: build/dist/LAS Recomp-<version>-iOS.laskit, the one file a player downloads so that LAS Recomp on
# any desktop (the start screen's "Recomp for iOS", runtime/iosexport.c) turns their own game files into the iOS app.
# It is a zip (read in place by the runtime, never unpacked by the player) holding:
#   Payload/LAS Recomp.app/  the app without game code (tools/bundle_ios.sh SKELETON=1); the .ipa is these entries
#                            copied as they are plus Frameworks/LASGame.framework
#   lld/<arch>-<os>/         the Mach-O linker for each desktop (tools/fetch_lld.sh: rust-lld, bin/ + lib/); the
#                            runtime extracts its host's set once (<data>/ios-kit/<kit id>/)
#   include/any-darwin-any/  zig's Darwin libc headers (the bundled zig compiles for iOS with these; its own copy
#                            is trimmed to the host's, tools/fetch_zig.sh)
#   libSystem.B.tbd          the libSystem stub the link needs (runtime/platform/ios-sdk)
#   README.txt
# Run after `make ios-skeleton` (the app) and tools/fetch_lld.sh for every LLD_TARGET (the linkers); needs the
# zig release archive of this host in build/zig-dl (tools/fetch_zig.sh fetched it) for the untrimmed headers.
set -euo pipefail
cd "$(dirname "$0")/.."
APP=${APP:-build/ios-skeleton/LAS Recomp.app}
VERSION=$(sed -n 's/^#define LARECOMP_VERSION "\(.*\)"/\1/p' runtime/ui/launcher.mm)
[ -x "$APP/lasrecomp" ] || { echo "no $APP (make ios-skeleton)"; exit 1; }
[ -e "$APP/Frameworks/LASGame.framework/LASGame" ] && { echo "$APP holds game code"; exit 1; }
for t in aarch64-macos x86_64-linux aarch64-linux x86_64-windows; do
    [ -d "build/lld/$t/bin" ] || { echo "no build/lld/$t (LLD_TARGET=$t tools/fetch_lld.sh)"; exit 1; }
done

STAGE=build/ios-kit
rm -rf "$STAGE" && mkdir -p "$STAGE/Payload"
cp -R "$APP" "$STAGE/Payload/LAS Recomp.app"
cp -R build/lld "$STAGE/lld"
cp runtime/platform/ios-sdk/libSystem.B.tbd "$STAGE/"
# the Darwin headers, from the zig archive (build/zig keeps only the host's set)
ZVER=$(sed -n 's/^VER=//p' tools/fetch_zig.sh)
ZA=$(find build/zig-dl -maxdepth 1 \( -name "zig-*-$ZVER.tar.xz" -o -name "zig-*-$ZVER.zip" \) | head -1)
[ -n "$ZA" ] || { echo "no zig archive in build/zig-dl (tools/fetch_zig.sh)"; exit 1; }
mkdir -p "$STAGE/include"
ZN=$(basename "$ZA"); ZN=${ZN%.tar.xz}; ZN=${ZN%.zip}
case "$ZA" in
    *.zip) unzip -q "$ZA" "$ZN/lib/libc/include/any-darwin-any/*" -d "$STAGE/zig-tmp"
           mv "$STAGE/zig-tmp/$ZN/lib/libc/include/any-darwin-any" "$STAGE/include/"; rm -rf "$STAGE/zig-tmp" ;;
    *) tar -xJf "$ZA" -C "$STAGE/include" --strip-components 4 "$ZN/lib/libc/include/any-darwin-any" ;;
esac
cat > "$STAGE/README.txt" <<EOT
LAS Recomp $VERSION - iOS kit

Not an app you can install, and nothing to unpack: LAS Recomp on your Mac, Linux or Windows machine (the start
screen's "Recomp for iOS") takes this file as it is, builds the game code from your own game files with the tools in
it and saves an .ipa for your own sideloading tool and Apple ID (Impactor, SideStore, AltStore...). The app needs
one entitlement, com.apple.developer.kernel.increased-memory-limit, which a free developer account grants; it is in
the executable's signature, where those tools read it. On the device, put your extracted/ folder (exefs and romfs)
in the app's Documents through the Files app or Finder.

Contents: Payload/LAS Recomp.app (the app without game code), lld/ (the linker for each desktop OS, from Rust's
rust-lld: Apache-2.0 with LLVM exception), include/ (zig's Darwin C headers: MIT / Apple's APSL), libSystem.B.tbd
(the link stub).
EOT
mkdir -p build/dist
OUT="build/dist/LAS Recomp-$VERSION-iOS.laskit"
rm -f "$OUT"
# (no symlinks in the app: an iOS install drops them; -y would keep any as entries) -X: no extra fields beyond the modes
[ -z "$(find "$STAGE/Payload" -type l)" ] || { echo "symlinks in the app: $(find "$STAGE/Payload" -type l)"; exit 1; }
(cd "$STAGE" && zip -qry -X "../../$OUT" Payload lld include libSystem.B.tbd README.txt)
rm -rf "$STAGE"
echo "$OUT ($(du -h "$OUT" | cut -f1))"
