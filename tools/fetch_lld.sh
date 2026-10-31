#!/bin/bash
# The Mach-O linker the start screen links the iOS game code library with on the player's machine (runtime/gamebuild.c
# GAMEBUILD_IOS): LLVM's ld64.lld (Apache-2.0 with LLVM exception), as `rust-lld` from the pinned Rust release for
# this OS / CPU (or LLD_TARGET=<arch>-<os>) into build/lld/<arch>-<os>/. The bundled zig compiles for iOS but can't
# link for it, and Apple's ld needs Xcode or the Command Line Tools; with this, exporting the iOS app needs neither
# (and works from Linux / Windows). Shipped in the iOS kit, for every desktop (tools/ios_kit.sh).
# Rust's rustc component (60-70 MB, a .sha256 next to each) rather than an LLVM release (1-2 GB, and its Linux lld
# needs the host's libxml2 / ICU): rust-lld depends only on libc, libz (and on Linux its own lib/libLLVM.so, found by
# RUNPATH $ORIGIN/../lib - so the bin/ + lib/ layout is kept). Each archive is fetched once (build/lld-dl/).
set -euo pipefail
cd "$(dirname "$0")/.."
VER=1.91.0
case "$(uname -s)" in Darwin) OS=macos;; Linux) OS=linux;; *) OS=windows;; esac
# (MSYS2 on Windows ARM64: uname -m is the emulated x86_64 of its shell, MSYSTEM_CARCH the environment's real arch)
case "${MSYSTEM_CARCH:-$(uname -m)}" in arm64|aarch64) ARCH=aarch64;; *) ARCH=x86_64;; esac
TARGET=${LLD_TARGET:-$ARCH-$OS}
# the Rust triple, the archive's sha256 (static.rust-lang.org/dist/<archive>.sha256) and, on Linux, the LLVM dylib
case "$TARGET" in
    aarch64-macos)  TRIPLE=aarch64-apple-darwin;      SHA=a68e3ada151e6eee10374529161d35dc26e8a185bddb14eabe6133f37144e02f; DYLIB= ;;
    x86_64-linux)   TRIPLE=x86_64-unknown-linux-gnu;  SHA=a7169e8cb6174af2f45717703370363d8de82ce55f6ccba185893045b9370874; DYLIB=libLLVM.so.21.1-rust-$VER-stable ;;
    aarch64-linux)  TRIPLE=aarch64-unknown-linux-gnu; SHA=f3ea3c964b7f3b884337f2d411764032bbd1722d7f55592a547cbb29afd87c03; DYLIB=libLLVM.so.21.1-rust-$VER-stable ;;
    x86_64-windows) TRIPLE=x86_64-pc-windows-msvc;    SHA=fe4fb6431282c714434fee3a8cb268961b88ba1c29122b42b3de97f571dba354; DYLIB= ;;
    aarch64-windows) TRIPLE=aarch64-pc-windows-msvc;  SHA=582b61e4752ec6e7aaf78ec9eb91d0f50e85f14d1cbae4f089f6a439d3a03f83; DYLIB= ;;
    *) echo "no lld for $TARGET"; exit 1;;
esac
EXE=; [ "${TARGET#*-}" = windows ] && EXE=.exe
NAME=rustc-$VER-$TRIPLE.tar.xz
IN=rustc-$VER-$TRIPLE/rustc

DL=build/lld-dl
mkdir -p "$DL"
URL="https://static.rust-lang.org/dist/$NAME"
[ -f "$DL/$NAME" ] || curl -fL --proto '=https' -o "$DL/$NAME.part" "$URL" && { [ -f "$DL/$NAME" ] || mv "$DL/$NAME.part" "$DL/$NAME"; }
GOT=$( (shasum -a 256 2>/dev/null || sha256sum) < "$DL/$NAME" | cut -d' ' -f1)
[ "$GOT" = "$SHA" ] || { echo "$NAME: sha256 $GOT, expected $SHA"; exit 1; }
OUT=build/lld/$TARGET
rm -rf "$OUT" && mkdir -p "$OUT/bin"
tar -xJf "$DL/$NAME" -O "$IN/lib/rustlib/$TRIPLE/bin/rust-lld$EXE" > "$OUT/bin/lld$EXE"
chmod +x "$OUT/bin/lld$EXE"
if [ -n "$DYLIB" ]; then
    mkdir -p "$OUT/lib"
    tar -xJf "$DL/$NAME" -O "$IN/lib/$DYLIB" > "$OUT/lib/$DYLIB"
fi
# (the runtime calls `lld -flavor darwin`, runtime/gamebuild.c: no ld64.lld link, which a zip for Windows can't hold)
echo "$OUT: lld (rust $VER) for $TARGET, $(du -sh "$OUT" | cut -f1)"
