#!/bin/sh
# Control-flow regression test for runtime/nvn/maxwell/translate.c (see tools/test_translate_flow.c): translates small
# branchy fragment shaders as MSL, GLSL (state machine) and GLSL (structured), runs their bodies on the CPU and checks
# the results.
# usage: tools/test_translate_flow.sh   (needs clang / clang++)
set -e
cd "$(dirname "$0")/.."
OUT=build/test_translate_flow
mkdir -p "$OUT"
CC=${CC:-clang}
CXX=${CXX:-clang++}
$CC -O1 -Wall tools/test_translate_flow.c runtime/nvn/maxwell/translate.c -o "$OUT/gen" -lm
for mode in msl glsl structured; do
    "$OUT/gen" "$mode" "$OUT/$mode.cpp"
    $CXX -std=c++17 -O1 -w "$OUT/$mode.cpp" -o "$OUT/$mode"
    "$OUT/$mode"
done
