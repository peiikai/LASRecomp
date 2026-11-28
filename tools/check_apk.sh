#!/usr/bin/env bash
# tools/check_apk.sh <apk>: the APK holds only what the app ships - the runtime, SDL, shaderc, FFmpeg, the shipped fonts
# and icon with their licences, and the game code builder's files in assets/gen.zip (zig's lib/ and the headers),
# checked file by file. Nothing of the game. (.github/workflows/ci.yml, release.yml, nightly.yml)
set -eu
APK=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
cd "$T"
unzip -Z1 "$APK" |
  grep -vE '^(META-INF/|res/|classes[0-9]*\.dex$|AndroidManifest\.xml$|resources\.arsc$|lib/arm64-v8a/lib(main|SDL3|shaderc_shared|hook_impl|main_hook|file_redirect_hook|gsl_alloc_hook|avcodec|avformat|avutil|swscale|zig)\.so$|assets/gen\.zip$|THIRD-PARTY-NOTICES\.txt$|assets/res/(Baloo2-SemiBold\.ttf|Baloo2-OFL\.txt|BIZUDPGothic-Regular\.ttf|BIZUDPGothic-OFL\.txt|DejaVuSans(-Bold)?\.ttf|DejaVu-LICENSE\.txt|icon\.png|libadrenotools-LICENSE\.txt|liblinkernsbypass-LICENSE\.txt|volk-LICENSE\.md)$)' \
  > unexpected.txt || true
unzip -p "$APK" assets/gen.zip > gen.zip && unzip -Z1 gen.zip |
  grep -vE '^(zig/|gamelib-src/(cpu|simd|simd_fp|gamelib)\.h$|gamelib-src/gamelib_entry\.c$|gamelib-src/hot_functions(_v[0-9_]+)?\.txt$|gamelib-src/$)' \
  | sed 's#^#gen.zip: #' >> unexpected.txt || true
if [ -s unexpected.txt ]; then echo "unexpected files in the APK:"; cat unexpected.txt; exit 1; fi
