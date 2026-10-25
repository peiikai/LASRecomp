#!/bin/bash
# make deps-ios: SDL3 and FFmpeg (the same LGPL subset as macOS: the game's movies) as static libraries for iOS
# (arm64, IOS_MIN = 16.0 by default), from the sources pinned in tools/build_deps_macos.sh, into build/deps-ios.
# Static: an .ipa then needs no embedded frameworks to sign. Each step is skipped when its output exists.
# SIM=1: the same for the iOS Simulator (arm64, into build/deps-ios-sim), the CI's run of the start screen and the
# embedded Python; only the SDK, the version-min flag and the xcframework slice differ.
# Needs: Xcode (iOS SDK), cmake and ninja (brew), a host python of the embedded version PYV (brew python@<PYV>).
set -euo pipefail
cd "$(dirname "$0")/.."
MIN=${IOS_MIN:-16.0}
if [ -n "${SIM:-}" ]; then
    SDKNAME=iphonesimulator; MINFLAG="-mios-simulator-version-min=$MIN"; SLICENAME=ios-arm64_x86_64-simulator; TAG=ios-sim
else
    SDKNAME=iphoneos; MINFLAG="-miphoneos-version-min=$MIN"; SLICENAME=ios-arm64; TAG=ios
fi
P=$PWD/build/deps-$TAG
S=$PWD/build/deps/src  # (shared with the macOS deps: the same tarballs)
mkdir -p "$S" "$P/lib"
SDK=$(xcrun --sdk $SDKNAME --show-sdk-path)
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
    unpack SDL3-3.4.18.tar.gz sdl-$TAG
    cmake -S "$S/sdl-$TAG" -B "$S/sdl-$TAG/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$P" \
          -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=$SDKNAME -DCMAKE_OSX_ARCHITECTURES=arm64 \
          -DCMAKE_OSX_DEPLOYMENT_TARGET="$MIN" -DSDL_SHARED=OFF -DSDL_STATIC=ON \
          -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_INSTALL_DOCS=OFF
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

# ---- the embedded CPython and the seed generator's packages (runtime/platform/ios_python.m)
# BeeWare's Python.xcframework (CPython built for iOS, with the stdlib's C extensions); keystone (cross-built, its
# build files patched: tools/patches/keystone-0.9.2.patch); oead (pybind11, built as a wheel from BeeWare's cross venv); PyYAML
# without its C extension; evfl and appdirs (pure Python). Result: $P/Python.xcframework and $P/python/site-packages.
# PYV comes from the Makefile (the one place the version is spelled); PYB is BeeWare's build of it, which fixes the
# ABI every extension here is built against (a new b-release: rebuild them all).
PYV=${PYV:-3.13}
PYB=b16
fetch "https://github.com/beeware/Python-Apple-support/releases/download/$PYV-$PYB/Python-$PYV-iOS-support.$PYB.tar.gz" \
      22b96f3d0c3663225f078556b036d2ee6ce3ff98c885706585240681335c71ea Python-$PYV-iOS-support.$PYB.tar.gz
fetch https://github.com/keystone-engine/keystone/archive/refs/tags/0.9.2.tar.gz \
      c9b3a343ed3e05ee168d29daf89820aff9effb2c74c6803c2d9e21d55b5b7c24 keystone-0.9.2.tar.gz
fetch https://files.pythonhosted.org/packages/source/o/oead/oead-1.3.0.tar.gz \
      87145c68dcddec8967fd56565e057cbbb83382554d744fbb5b38295b0dfa615f oead-1.3.0.tar.gz
fetch https://files.pythonhosted.org/packages/source/P/PyYAML/pyyaml-6.0.2.tar.gz \
      d584d9ec91ad65861cc08d42e834324ef890a082e591037abe114850ff7bbc3e pyyaml-6.0.2.tar.gz

SLICE=$P/Python.xcframework/$SLICENAME
if [ ! -f "$SLICE/Python.framework/Python" ]; then
    rm -rf "$P/Python.xcframework" "$S/py-$TAG" && mkdir -p "$S/py-$TAG"
    tar -xf "$S/Python-$PYV-iOS-support.$PYB.tar.gz" -C "$S/py-$TAG"
    mv "$S/py-$TAG/Python.xcframework" "$P/Python.xcframework"
fi
HOSTPY=${HOSTPY:-$(command -v python$PYV || true)}
[ -x "$HOSTPY" ] || { echo "a host python$PYV is needed to cross-build the Python packages (brew install python@$PYV)"; exit 1; }
HOSTPYV=$("$HOSTPY" -c 'import sys; print("%d.%d" % sys.version_info[:2])')
[ "$HOSTPYV" = "$PYV" ] || { echo "$HOSTPY is Python $HOSTPYV, the embedded one is $PYV: wheels would carry the wrong ABI"; exit 1; }
if [ ! -x "$P/pyvenv/bin/python" ]; then  # the cross venv: pip builds wheels for iOS from it
    "$HOSTPY" -m venv "$P/pyvenv"
    "$P/pyvenv/bin/pip" install -q --upgrade pip setuptools wheel "scikit-build-core>=0.10" "setuptools-scm>=8" pybind11
    "$P/pyvenv/bin/python" "$SLICE/platform-config/arm64-$SDKNAME/make_cross_venv.py" "$P/pyvenv"
fi
EXT=$("$P/pyvenv/bin/python" -c 'import sysconfig; print(sysconfig.get_config_var("EXT_SUFFIX"))')  # .cpython-313-iphoneos.so
WHEELTAG=$("$P/pyvenv/bin/python" -c 'import sysconfig; print(sysconfig.get_platform().replace("-", "_").replace(".", "_"))')  # ios_13_0_arm64_iphoneos

# The randomizer's requirements.txt is what the generator is written against; the natively built packages below are
# pinned here by hand (each needs its own recipe), so a bump there must not go unnoticed: every pin is checked
# against the file's specifier, and the pure-Python ones are taken from the file as they are.
REQ=third_party/LAS-Randomizer/requirements.txt
req() {  # name [version]: the file's line for name; with a version, fail unless it satisfies the line
    "$HOSTPY" - "$REQ" "$@" <<'EOF'
import sys
from pip._vendor.packaging.requirements import Requirement
path, name, ver = (sys.argv[1:] + [None])[:3]
norm = lambda s: s.lower().replace('_', '-')
for line in open(path):
    line = line.split('#')[0].strip()
    if line and norm(Requirement(line).name) == norm(name):
        if ver and ver not in Requirement(line).specifier:
            sys.exit(f"{path} wants {line}; tools/build_deps_ios.sh builds {name} {ver}: move its pin (and checksum)")
        print(line)
        break
else:
    sys.exit(f"{name} is not in {path}")
EOF
}
req keystone-engine 0.9.2 >/dev/null
req oead 1.3.0 >/dev/null
req PyYAML 6.0.2 >/dev/null
req quicktex 0.2.1 >/dev/null
req Pillow 10.4.0 >/dev/null

if [ ! -f "$P/lib/libkeystone.0.dylib" ]; then
    unpack keystone-0.9.2.tar.gz keystone-$TAG
    patch -s -p1 -d "$S/keystone-$TAG" < tools/patches/keystone-0.9.2.patch
    cmake -S "$S/keystone-$TAG" -B "$S/keystone-$TAG/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$P" \
          -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=$SDKNAME -DCMAKE_OSX_ARCHITECTURES=arm64 \
          -DCMAKE_OSX_DEPLOYMENT_TARGET="$MIN" -DBUILD_SHARED_LIBS=ON -DBUILD_LIBS_ONLY=ON \
          -DLLVM_TARGETS_TO_BUILD="AArch64" -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
          -DCMAKE_CXX_FLAGS="-Wno-everything" -DCMAKE_C_FLAGS="-Wno-everything" \
          -DPYTHON_EXECUTABLE="$HOSTPY" -DPYTHON_VERSION_STRING=$PYV -DPYTHONINTERP_FOUND=ON
    cmake --build "$S/keystone-$TAG/build" -j"$JOBS" && cmake --install "$S/keystone-$TAG/build"
fi

mkdir -p "$P/wheels"
OEAD_WHL=oead-1.3.0-cp${PYV/./}-cp${PYV/./}-$WHEELTAG.whl
if [ ! -f "$P/wheels/$OEAD_WHL" ]; then
    unpack oead-1.3.0.tar.gz oead-$TAG
    (cd "$S/oead-$TAG" && git init -q && PATH="$SLICE/bin:$PATH" IPHONEOS_DEPLOYMENT_TARGET=$MIN \
        SETUPTOOLS_SCM_PRETEND_VERSION=1.3.0 CMAKE_GENERATOR=Ninja \
        "$P/pyvenv/bin/pip" wheel . --no-build-isolation --no-deps -w "$P/wheels")
    [ -f "$P/wheels/$OEAD_WHL" ]
fi

# quicktex (pybind11 2.x): its own setup.py drives CMake and takes the cross venv's Python, as oead's does; it runs
# `cmake` by name, so cmake's folder goes on PATH (the cross venv's PATH is the slice's bin + the system's). Its
# CMake gets no sysroot from setup.py, so the compiler is the slice's wrapper (xcrun with the right SDK and -target)
# through CC / CXX, which CMake honours.
QUICKTEX_WHL=quicktex-0.2.1-cp${PYV/./}-cp${PYV/./}-$WHEELTAG.whl
if [ ! -f "$P/wheels/$QUICKTEX_WHL" ]; then
    "$HOSTPY" -m pip download -q --no-deps --no-binary :all: -d "$P/wheels" "quicktex~=0.2.1"
    rm -rf "$S/quicktex-$TAG" && mkdir -p "$S/quicktex-$TAG"
    tar -xf "$P"/wheels/quicktex-0.2.1.tar.gz -C "$S/quicktex-$TAG" --strip-components 1
    CROSS_CC=$("$P/pyvenv/bin/python" -c 'import sysconfig; print(sysconfig.get_config_var("CC"))')  # arm64-apple-ios[-simulator]-clang
    (cd "$S/quicktex-$TAG" && PATH="$SLICE/bin:$(dirname "$(command -v cmake)"):$(dirname "$(command -v ninja)"):$PATH" \
        CC=$CROSS_CC CXX=${CROSS_CC}++ \
        IPHONEOS_DEPLOYMENT_TARGET=$MIN SETUPTOOLS_SCM_PRETEND_VERSION=0.2.1 CMAKE_GENERATOR=Ninja \
        "$P/pyvenv/bin/pip" wheel . --no-build-isolation --no-deps -w "$P/wheels")
    [ -f "$P/wheels/$QUICKTEX_WHL" ]
fi
# Pillow: BeeWare's prebuilt iOS wheel (its image codecs linked in statically), pinned by checksum like the sources
PILLOW_WHL=pillow-10.4.0-cp${PYV/./}-cp${PYV/./}-$WHEELTAG.whl
if [ -n "${SIM:-}" ]; then PILLOW_SHA=35be44fe3efe66d129d3e9800cdaa3773d752fececbbc3a6fa073767a15dfb64
else PILLOW_SHA=8e2cd37b032d9246fac4acf5202bdcdfa95269c5f26cc3fcb4d1e1ece5d54c47; fi
if [ ! -f "$P/wheels/$PILLOW_WHL" ]; then
    "$HOSTPY" -m pip download -q --no-deps --only-binary :all: --platform "$WHEELTAG" --python-version "$PYV" \
        --index-url https://pypi.anaconda.org/beeware/simple --extra-index-url https://pypi.org/simple -d "$P/wheels" "Pillow==10.4.0"
    echo "$PILLOW_SHA  $P/wheels/$PILLOW_WHL" | shasum -a 256 -c --quiet || { rm -f "$P/wheels/$PILLOW_WHL"; exit 1; }
fi

SP=$P/python/site-packages
if [ ! -f "$SP/oead$EXT" ]; then
    rm -rf "$SP" && mkdir -p "$SP"
    unzip -qo "$P/wheels/$OEAD_WHL" -d "$SP"
    unzip -qo "$P/wheels/$PILLOW_WHL" -d "$SP"
    rm -f "$SP"/PIL/_imagingtk*.so  # (Tk: not there)
    "$HOSTPY" -m pip download -q --no-deps -d "$P/wheels" "$(req evfl)" "$(req appdirs)"
    for w in "$P"/wheels/evfl-*.whl "$P"/wheels/appdirs-*.whl; do unzip -qo "$w" -d "$SP"; done
    unpack pyyaml-6.0.2.tar.gz pyyaml && cp -R "$S/pyyaml/lib/yaml" "$SP/yaml"
    unpack keystone-0.9.2.tar.gz keystone-py && cp -R "$S/keystone-py/bindings/python/keystone" "$SP/keystone"
    cp "$P/lib/libkeystone.0.dylib" "$SP/keystone/libkeystone.dylib"  # (keystone.py loads it from its own folder)
    unzip -qo "$P/wheels/$QUICKTEX_WHL" -d "$SP" && rm -rf "$SP/quicktex/cli"  # (_quicktex: a top-level module)
    rm -rf "$SP"/*.dist-info "$SP"/oead-stubs
fi

echo "build/deps-$TAG ready ($SDKNAME $MIN):"
ls -la "$P"/lib/*.a "$P"/lib/libkeystone.0.dylib
ls "$SP"
