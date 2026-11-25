#!/bin/bash
# make deps-ios: SDL3 and FFmpeg (the same LGPL subset as macOS: the game's movies) as static libraries for iOS
# (arm64, the Makefile's IOS_MIN), from the sources pinned in tools/build_deps_macos.sh, into build/deps-ios.
# Static: an .ipa then needs no embedded frameworks to sign. Each step is skipped when its output exists.
# SIM=1: the same for the iOS Simulator (arm64, into build/deps-ios-sim), the CI's build of the simulator app; only
# the SDK and the version-min flag differ.
# Needs: Xcode (iOS SDK), cmake and ninja (brew).
set -euo pipefail
cd "$(dirname "$0")/.."
MIN=${IOS_MIN:?the Makefile sets IOS_MIN - run as make deps-ios}
if [ -n "${SIM:-}" ]; then
    SDKNAME=iphonesimulator; MINFLAG="-mios-simulator-version-min=$MIN"; TAG=ios-sim
else
    SDKNAME=iphoneos; MINFLAG="-miphoneos-version-min=$MIN"; TAG=ios
fi
P=$PWD/build/deps-$TAG
S=$PWD/build/deps/src  # (shared with the macOS deps: the same tarballs)
mkdir -p "$S" "$P/lib"
SDK=$(xcrun --sdk $SDKNAME --show-sdk-path)
JOBS=$(sysctl -n hw.ncpu)
# No path of this machine in what ships (tools/build_deps_macos.sh does the same): the CMake builds map their source
# folder away. FFmpeg is static here and its configure line isn't kept in a static archive's objects.
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

if [ ! -f "$P/lib/libSDL3.a" ]; then
    unpack SDL3-3.4.18.tar.gz sdl-$TAG
    # iOS 27: the window's view kept a portrait frame after the rotation to landscape (SDL issue 16436) - SDL's
    # own fixes from its release-3.4.x branch (commits ca97578, 2ee7eb6; in 3.4.20 once that is out)
    patch -s -p1 -d "$S/sdl-$TAG" < tools/patches/SDL3-3.4.18-ios27-orientation.patch
    cmake -S "$S/sdl-$TAG" -B "$S/sdl-$TAG/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$P" \
          -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=$SDKNAME -DCMAKE_OSX_ARCHITECTURES=arm64 \
          -DCMAKE_OSX_DEPLOYMENT_TARGET="$MIN" -DSDL_SHARED=OFF -DSDL_STATIC=ON \
          -DCMAKE_C_FLAGS="$MAPFLAGS" -DCMAKE_OBJC_FLAGS="$MAPFLAGS" -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_INSTALL_DOCS=OFF
    cmake --build "$S/sdl-$TAG/build" -j"$JOBS" && cmake --install "$S/sdl-$TAG/build"
fi

if [ ! -f "$P/lib/libavcodec.a" ]; then
    unpack ffmpeg-9.0.2.tar.xz ffmpeg-$TAG
    (cd "$S/ffmpeg-$TAG" && ./configure --prefix="$P" --enable-cross-compile --target-os=darwin --arch=arm64 \
        --cc="xcrun -sdk $SDKNAME clang" --as="xcrun -sdk $SDKNAME clang" \
        --sysroot="$SDK" --extra-cflags="-arch arm64 $MINFLAG" \
        --extra-ldflags="-arch arm64 $MINFLAG" \
        --enable-static --disable-shared --enable-pic \
        --disable-autodetect --disable-programs --disable-doc --disable-network --disable-avdevice --disable-avfilter \
        --disable-swresample --disable-everything \
        --enable-protocol=file --enable-demuxer=matroska,mov \
        --enable-decoder=vp9,vp8,vorbis,opus,h264,aac --enable-parser=vp9,vp8,vorbis,opus,h264,aac \
        && make -j"$JOBS" && make install)
fi

# The licence texts of what the app ships statically, for the app's LICENSES/ (tools/bundle_ios.sh; the README's
# License section names each with its version): from the tarballs above
L=$P/licenses
rm -rf "$L" && mkdir -p "$L"
unpack SDL3-3.4.18.tar.gz sdl-lic && cp "$S/sdl-lic/LICENSE.txt" "$L/SDL3-LICENSE.txt"
unpack ffmpeg-9.0.2.tar.xz ffmpeg-lic && cp "$S/ffmpeg-lic/COPYING.LGPLv2.1" "$L/FFmpeg-COPYING.LGPLv2.1.txt"
rm -rf "$S"/{sdl,ffmpeg}-lic
# (what an older run built for the embedded Python, which the app no longer has)
rm -rf "$P/Python.xcframework" "$P/pyvenv" "$P/wheels" "$P/python" "$P/python-build" "$P"/lib/libkeystone*
ls "$L"

echo "build/deps-$TAG ready ($SDKNAME $MIN):"
ls -la "$P"/lib/*.a
