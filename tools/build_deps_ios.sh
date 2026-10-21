#!/bin/bash
# make deps-ios: SDL3 and FFmpeg (the same LGPL subset as macOS: the game's movies) as static libraries for iOS
# (arm64, IOS_MIN = 16.0 by default), from the sources pinned in tools/build_deps_macos.sh, into build/deps-ios.
# Static: an .ipa then needs no embedded frameworks to sign. Each step is skipped when its output exists.
# Needs: Xcode (iOS SDK), cmake, ninja.
set -euo pipefail
cd "$(dirname "$0")/.."
MIN=${IOS_MIN:-16.0}
P=$PWD/build/deps-ios
S=$PWD/build/deps/src  # (shared with the macOS deps: the same tarballs)
mkdir -p "$S" "$P/lib"
SDK=$(xcrun --sdk iphoneos --show-sdk-path)
JOBS=$(sysctl -n hw.ncpu)

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
    unpack SDL3-3.4.18.tar.gz sdl-ios
    cmake -S "$S/sdl-ios" -B "$S/sdl-ios/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$P" \
          -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos -DCMAKE_OSX_ARCHITECTURES=arm64 \
          -DCMAKE_OSX_DEPLOYMENT_TARGET="$MIN" -DSDL_SHARED=OFF -DSDL_STATIC=ON \
          -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_INSTALL_DOCS=OFF
    cmake --build "$S/sdl-ios/build" -j"$JOBS" && cmake --install "$S/sdl-ios/build"
fi

if [ ! -f "$P/lib/libavcodec.a" ]; then
    unpack ffmpeg-9.0.2.tar.xz ffmpeg-ios
    (cd "$S/ffmpeg-ios" && ./configure --prefix="$P" --enable-cross-compile --target-os=darwin --arch=arm64 \
        --cc="xcrun -sdk iphoneos clang" --as="xcrun -sdk iphoneos clang" \
        --sysroot="$SDK" --extra-cflags="-arch arm64 -miphoneos-version-min=$MIN" \
        --extra-ldflags="-arch arm64 -miphoneos-version-min=$MIN" \
        --enable-static --disable-shared --enable-pic \
        --disable-autodetect --disable-programs --disable-doc --disable-network --disable-avdevice --disable-avfilter \
        --disable-swresample --disable-everything \
        --enable-protocol=file --enable-demuxer=matroska,mov \
        --enable-decoder=vp9,vp8,vorbis,opus,h264,aac --enable-parser=vp9,vp8,vorbis,opus,h264,aac \
        && make -j"$JOBS" && make install)
fi

# ---- the embedded CPython and the seed generator's packages (runtime/platform/ios_python.m)
# BeeWare's Python.xcframework (CPython 3.13 built for iOS, with the stdlib's C extensions); keystone (cross-built,
# the ELF-only -z,defs link flag patched out); oead (pybind11, built as a wheel from BeeWare's cross venv); PyYAML
# without its C extension; evfl and appdirs (pure Python). Result: $P/Python.xcframework and $P/python/site-packages.
PYV=3.13
PYB=b16
fetch "https://github.com/beeware/Python-Apple-support/releases/download/$PYV-$PYB/Python-$PYV-iOS-support.$PYB.tar.gz" \
      22b96f3d0c3663225f078556b036d2ee6ce3ff98c885706585240681335c71ea Python-$PYV-iOS-support.$PYB.tar.gz
fetch https://github.com/keystone-engine/keystone/archive/refs/tags/0.9.2.tar.gz \
      c9b3a343ed3e05ee168d29daf89820aff9effb2c74c6803c2d9e21d55b5b7c24 keystone-0.9.2.tar.gz
fetch https://files.pythonhosted.org/packages/source/o/oead/oead-1.3.0.tar.gz \
      "$(curl -fsSL https://pypi.org/pypi/oead/1.3.0/json | python3 -c 'import json,sys; print([u for u in json.load(sys.stdin)["urls"] if u["packagetype"]=="sdist"][0]["digests"]["sha256"])')" oead-1.3.0.tar.gz
fetch https://files.pythonhosted.org/packages/source/P/PyYAML/pyyaml-6.0.2.tar.gz \
      d584d9ec91ad65861cc08d42e834324ef890a082e591037abe114850ff7bbc3e pyyaml-6.0.2.tar.gz

SLICE=$P/Python.xcframework/ios-arm64
if [ ! -f "$SLICE/Python.framework/Python" ]; then
    rm -rf "$P/Python.xcframework" "$S/py-ios" && mkdir -p "$S/py-ios"
    tar -xf "$S/Python-$PYV-iOS-support.$PYB.tar.gz" -C "$S/py-ios"
    mv "$S/py-ios/Python.xcframework" "$P/Python.xcframework"
fi
HOSTPY=${HOSTPY:-$(command -v python$PYV || true)}
[ -x "$HOSTPY" ] || { echo "a host python$PYV is needed to cross-build the Python packages (brew install python@$PYV)"; exit 1; }
if [ ! -x "$P/pyvenv/bin/python" ]; then  # the cross venv: pip builds wheels for iOS from it
    "$HOSTPY" -m venv "$P/pyvenv"
    "$P/pyvenv/bin/pip" install -q --upgrade pip setuptools wheel "scikit-build-core>=0.10" "setuptools-scm>=8" pybind11
    "$P/pyvenv/bin/python" "$SLICE/platform-config/arm64-iphoneos/make_cross_venv.py" "$P/pyvenv"
fi

if [ ! -f "$P/lib/libkeystone.0.dylib" ]; then
    unpack keystone-0.9.2.tar.gz keystone-ios
    sed -i '' 's/cmake_policy(SET CMP0051 OLD)//' "$S/keystone-ios/CMakeLists.txt" "$S/keystone-ios/llvm/CMakeLists.txt"  # (CMake 4)
    sed -i '' 's/MATCHES "Darwin" OR WIN32/MATCHES "Darwin" OR ${CMAKE_SYSTEM_NAME} MATCHES "iOS" OR WIN32/' \
        "$S/keystone-ios/llvm/cmake/modules/HandleLLVMOptions.cmake"  # (-Wl,-z,defs is ELF-only)
    cmake -S "$S/keystone-ios" -B "$S/keystone-ios/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$P" \
          -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos -DCMAKE_OSX_ARCHITECTURES=arm64 \
          -DCMAKE_OSX_DEPLOYMENT_TARGET="$MIN" -DBUILD_SHARED_LIBS=ON -DBUILD_LIBS_ONLY=ON \
          -DLLVM_TARGETS_TO_BUILD="AArch64" -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
          -DCMAKE_CXX_FLAGS="-Wno-everything" -DCMAKE_C_FLAGS="-Wno-everything" \
          -DPYTHON_EXECUTABLE="$HOSTPY" -DPYTHON_VERSION_STRING=$PYV -DPYTHONINTERP_FOUND=ON
    cmake --build "$S/keystone-ios/build" -j"$JOBS" && cmake --install "$S/keystone-ios/build"
fi

mkdir -p "$P/wheels"
if ! ls "$P"/wheels/oead-1.3.0-*_iphoneos.whl >/dev/null 2>&1; then  # (…-cp313-cp313-ios_13_0_arm64_iphoneos.whl)
    unpack oead-1.3.0.tar.gz oead-ios
    (cd "$S/oead-ios" && git init -q && PATH="$SLICE/bin:$PATH" IPHONEOS_DEPLOYMENT_TARGET=$MIN \
        SETUPTOOLS_SCM_PRETEND_VERSION=1.3.0 CMAKE_GENERATOR=Ninja \
        "$P/pyvenv/bin/pip" wheel . --no-build-isolation --no-deps -w "$P/wheels")
    ls "$P"/wheels/oead-1.3.0-*_iphoneos.whl >/dev/null
fi

# quicktex (pybind11 2.x, its setup.py can't cross-build: the module straight from CMake, against the iOS Python)
if [ ! -f "$P/wheels/_quicktex.cpython-313-iphoneos.so" ]; then
    "$HOSTPY" -m pip download -q --no-deps --no-binary :all: -d "$P/wheels" "quicktex~=0.2.1"
    rm -rf "$S/quicktex-ios" && mkdir -p "$S/quicktex-ios"
    tar -xf "$P"/wheels/quicktex-0.2.1.tar.gz -C "$S/quicktex-ios" --strip-components 1
    PB=$("$P/pyvenv/bin/python" -c "import pybind11; print(pybind11.get_cmake_dir())")
    cmake -S "$S/quicktex-ios" -B "$S/quicktex-ios/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos -DCMAKE_OSX_ARCHITECTURES=arm64 \
          -DCMAKE_OSX_DEPLOYMENT_TARGET="$MIN" -Dpybind11_DIR="$PB" -DPYBIND11_FINDPYTHON=OFF \
          -DPYTHON_EXECUTABLE="$P/pyvenv/bin/python" -DPYTHON_INCLUDE_DIR="$SLICE/include/python3.13" \
          -DPYTHON_LIBRARY="$SLICE/Python.framework/Python" -DPYTHON_MODULE_EXTENSION=.cpython-313-iphoneos.so \
          -DQUICKTEX_MODULE_ONLY=TRUE -DQUICKTEX_VERSION_INFO=0.2.1 -DCMAKE_LIBRARY_OUTPUT_DIRECTORY="$S/quicktex-ios/build/out" \
          "-DCMAKE_MODULE_LINKER_FLAGS=-undefined dynamic_lookup"  # (the Python symbols: the app's framework at run time)
    cmake --build "$S/quicktex-ios/build" -j"$JOBS"
    cp "$S/quicktex-ios/build/out/_quicktex.cpython-313-iphoneos.so" "$P/wheels/"
fi
# Pillow: BeeWare's prebuilt iOS wheel (its image codecs linked in statically)
ls "$P"/wheels/pillow-10.4.0-*_iphoneos.whl >/dev/null 2>&1 || \
    "$HOSTPY" -m pip download -q --no-deps --only-binary :all: --platform ios_13_0_arm64_iphoneos --python-version 3.13 \
        --index-url https://pypi.anaconda.org/beeware/simple --extra-index-url https://pypi.org/simple -d "$P/wheels" "Pillow~=10.4.0"

SP=$P/python/site-packages
if [ ! -f "$SP/oead.cpython-313-iphoneos.so" ]; then
    rm -rf "$SP" && mkdir -p "$SP"
    unzip -qo "$P"/wheels/oead-1.3.0-*_iphoneos.whl -d "$SP"
    unzip -qo "$P"/wheels/pillow-10.4.0-*_iphoneos.whl -d "$SP"
    rm -f "$SP"/PIL/_imagingtk*.so  # (Tk: not there)
    "$HOSTPY" -m pip download -q --no-deps -d "$P/wheels" "evfl~=1.2.0" "appdirs~=1.4.4"
    for w in "$P"/wheels/evfl-*.whl "$P"/wheels/appdirs-*.whl; do unzip -qo "$w" -d "$SP"; done
    unpack pyyaml-6.0.2.tar.gz pyyaml && cp -R "$S/pyyaml/lib/yaml" "$SP/yaml"
    unpack keystone-0.9.2.tar.gz keystone-py && cp -R "$S/keystone-py/bindings/python/keystone" "$SP/keystone"
    cp "$P/lib/libkeystone.0.dylib" "$SP/keystone/libkeystone.dylib"  # (keystone.py loads it from its own folder)
    cp -R "$S/quicktex-ios/quicktex" "$SP/quicktex" && rm -rf "$SP/quicktex/cli"
    cp "$P/wheels/_quicktex.cpython-313-iphoneos.so" "$SP/"  # (a top-level module, as its setup.py installs it)
    rm -rf "$SP"/*.dist-info "$SP"/oead-stubs
fi

echo "build/deps-ios ready (iOS $MIN):"
ls -la "$P"/lib/*.a "$P"/lib/libkeystone.0.dylib
ls "$SP"
