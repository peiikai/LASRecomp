#!/bin/bash
# make deps (macOS): the libraries and tools the shipped .app contains, built from pinned sources for the app's
# minimum macOS (MACOS_MIN, default 15.0) into build/deps. Homebrew's bottles target the macOS they were built on, so
# they can't ship. Plain `make` (dev builds) keeps using Homebrew.
#   SDL3; FFmpeg (LGPL only, no GPL parts: just what the game's movies need - WebM: VP9 video + Vorbis audio, plus
#   VP8 / Opus / H.264 / AAC in WebM or MP4 - and swscale).
# Needs: Xcode command line tools, cmake. Each step is skipped when its output exists (delete build/deps/<x> to redo).
# No path of this machine may end up in the shipped libraries: FFmpeg stores its configure line (with --prefix) in
# them, so it is configured for a neutral prefix and installed into build/deps with DESTDIR; the CMake builds map
# their source folder away (-ffile-prefix-map).
set -euo pipefail
cd "$(dirname "$0")/.."
MIN=${MACOS_MIN:-15.0}
P=$PWD/build/deps
S=$P/src
mkdir -p "$S" "$P/bin" "$P/lib"
export MACOSX_DEPLOYMENT_TARGET=$MIN
JOBS=$(sysctl -n hw.ncpu)
NEUTRAL=/opt/las-recomp-deps  # (FFmpeg's --prefix: never used at run time, the dylibs load by @rpath)
MAPFLAGS="-ffile-prefix-map=$S=deps-src"

fetch() {  # url sha256 file
    [ -f "$S/$3" ] || curl -fsSL -o "$S/$3.part" "$1" && { [ -f "$S/$3" ] || mv "$S/$3.part" "$S/$3"; }
    echo "$2  $S/$3" | shasum -a 256 -c --quiet
}
unpack() { rm -rf "$S/$2" && mkdir -p "$S/$2" && tar -xf "$S/$1" -C "$S/$2" --strip-components 1; }

fetch https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz \
      9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3 SDL3-3.4.18.tar.gz
fetch https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz \
      8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e ffmpeg-9.0.2.tar.xz

if [ ! -f "$P/lib/libSDL3.0.dylib" ]; then
    unpack SDL3-3.4.18.tar.gz sdl
    cmake -S "$S/sdl" -B "$S/sdl/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$P" \
          -DCMAKE_OSX_DEPLOYMENT_TARGET="$MIN" -DCMAKE_OSX_ARCHITECTURES=arm64 -DSDL_SHARED=ON -DSDL_STATIC=OFF \
          -DCMAKE_C_FLAGS="$MAPFLAGS" -DCMAKE_OBJC_FLAGS="$MAPFLAGS" -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_INSTALL_DOCS=OFF
    cmake --build "$S/sdl/build" -j"$JOBS" && cmake --install "$S/sdl/build"
fi

if [ ! -f "$P/lib/libavcodec.dylib" ]; then
    unpack ffmpeg-9.0.2.tar.xz ffmpeg
    (cd "$S/ffmpeg" && ./configure --prefix="$NEUTRAL" --cc=clang --arch=arm64 --enable-shared --disable-static \
        --extra-cflags="-mmacosx-version-min=$MIN" --extra-ldflags="-mmacosx-version-min=$MIN" \
        --disable-autodetect --disable-programs --disable-doc --disable-network --disable-avdevice --disable-avfilter \
        --disable-swresample --disable-everything \
        --enable-protocol=file --enable-demuxer=matroska,mov \
        --enable-decoder=vp9,vp8,vorbis,opus,h264,aac --enable-parser=vp9,vp8,vorbis,opus,h264,aac \
        --install-name-dir='@rpath' && make -j"$JOBS" && rm -rf "$S/ffmpeg-root" && make install DESTDIR="$S/ffmpeg-root")
    cp -R "$S/ffmpeg-root$NEUTRAL"/. "$P"/
    sed -i '' "s|$NEUTRAL|$P|g" "$P"/lib/pkgconfig/lib{avformat,avcodec,avutil,swscale}.pc  # (pkg-config: the real place)
fi

# no path of this machine inside a library (FFmpeg's configure line, source file names); not on CI (no one's paths)
if [ "${CI:-}" != true ]; then
    case "$(basename "$HOME")" in runner|runneradmin) home=() ;; *) home=(-e "$HOME") ;; esac
    for f in "$P"/lib/*.dylib; do
        [ -L "$f" ] || ! grep -qaF -e "$PWD" ${home[@]+"${home[@]}"} "$f" || { echo "$f holds a path of this machine"; exit 1; }
    done
fi

echo "build/deps ready (macOS $MIN):"
for f in "$P"/lib/*.dylib; do
    [ -L "$f" ] && continue
    printf '  %-28s minos %s\n' "$(basename "$f")" "$(otool -l "$f" | awk '/LC_BUILD_VERSION/ {b=1} b && /minos/ {print $2; exit}')"
done
