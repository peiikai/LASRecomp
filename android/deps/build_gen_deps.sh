#!/bin/bash
# android/deps/build_gen_deps.sh: what the app needs to build the game code on the device from the player's own
# extracted game files, as the desktop's start screen does, into android/deps/out/gen, for arm64-v8a. The generator
# itself is part of the app (runtime/gen/); this is the compiler:
#   zig/      zig for aarch64 Linux (a static binary: it runs on Android, as libzig.so) and what compiling the game's
#             C for aarch64 needs (its clang headers, musl's headers, its std for compiler_rt)
# Android runs programs only from the app's native library folder: zig ships as libzig.so there.
# Skipped when its output exists (delete out/gen to redo). From LASRecompAndroid by mamcjr (MIT).
set -euo pipefail
cd "$(dirname "$0")"
D=$PWD
S=$D/src
P=$D/out/gen
W=$D/work
mkdir -p "$S" "$P" "$W"

sha256() { if command -v sha256sum >/dev/null; then sha256sum "$1"; else shasum -a 256 "$1"; fi | cut -d' ' -f1; }
fetch() {  # url sha256 file
    [ -f "$S/$3" ] || { curl -fsSL -o "$S/$3.part" "$1" && mv "$S/$3.part" "$S/$3"; }
    [ "$(sha256 "$S/$3")" = "$2" ] || { echo "checksum mismatch: $3" >&2; exit 1; }
}

fetch https://ziglang.org/download/0.17.0/zig-aarch64-linux-0.17.0.tar.xz \
      9e8d11661d4ae3bd57702a3832781e23ad151dde5798e16a5ccd503f65234ff8 zig-aarch64-linux-0.17.0.tar.xz

# (what older versions of this script put here for the Python generator: the generator is in the app now)
rm -rf "$P/python" "$P/lib" "$P/site"

# zig: the binary, its clang headers, musl's headers for aarch64 and its std (compiler_rt is built from it)
if [ ! -f "$P/zig/libzig.so" ]; then
    rm -rf "$W/zig" && mkdir -p "$W/zig" && tar -xJf "$S/zig-aarch64-linux-0.17.0.tar.xz" -C "$W/zig" --strip-components 1
    rm -rf "$P/zig" && mkdir -p "$P/zig/lib/libc/include"
    cp "$W/zig/zig" "$P/zig/libzig.so"
    cp -R "$W/zig/lib/include" "$W/zig/lib/std" "$W/zig/lib/compiler_rt" "$P/zig/lib/"
    cp "$W/zig/lib/compiler_rt.zig" "$W/zig/lib/c.zig" "$P/zig/lib/"
    for d in aarch64-linux-musl aarch64-linux-any any-linux-any generic-musl; do cp -R "$W/zig/lib/libc/include/$d" "$P/zig/lib/libc/include/"; done
    cp "$W/zig/LICENSE" "$P/zig/LICENSE-zig.txt"
fi
echo "android/deps/out/gen: $(du -sh "$P" | cut -f1)"
