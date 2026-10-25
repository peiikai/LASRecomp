#!/bin/bash
# The embedded Python on an iOS device: tools/test_ios_python.sh <devicectl id> <udid> [app]
# Launches the installed app with LARECOMP_IOS_PYTEST=1 (runtime/platform/ios.c plat_init: the self-test in
# ios_python.m imports every package and runs the seed generator on Documents/pytest-seed with
# {"seed": "pytest", "create-spoiler": true}, then once more into pytest-seed2 and compares the two), pulls
# Documents/lasrecomp.log and checks for `[py] selftest ok`, two `generator exit 0` and `second run: … identical`;
# then generates the same seed here (third_party/LAS-Randomizer/venv, PYTHONHASHSEED=0 as the
# runtime sets it) and compares the two outputs byte for byte - the seed must not depend on the platform.
# The device's Documents/extracted/romfs must be the same romfs as extracted/romfs here (a missing file changes the
# output without an error: loc_text.Names falls back to English names when a language's Place.msbt is absent).
# Needs: xcrun devicectl (Xcode), go-ios (`ios`: npm i -g go-ios) for the app's container, the randomizer venv.
# Takes several minutes: go-ios opens a connection per file for the ~290 files of a seed.
# <devicectl id>: `xcrun devicectl list devices`; <udid>: `ios list`. BUNDLE_ID: the app's (default the Makefile's).
set -euo pipefail
cd "$(dirname "$0")/.."
DEVICE=$1; UDID=$2
BUNDLE_ID=${BUNDLE_ID:-com.peiikai.lasrecomp.ios}
PY=third_party/LAS-Randomizer/venv/bin/python
ROMFS=${ROMFS:-extracted/romfs}
W=build/ios-pytest; rm -rf "$W"; mkdir -p "$W/mac" "$W/ios"

xcrun devicectl device process launch --device "$DEVICE" --terminate-existing \
    --environment-variables '{"LARECOMP_IOS_PYTEST":"1"}' "$BUNDLE_ID" >/dev/null
# (the self-test logs `generator exit` when the generator is done: poll the log for it)
for i in $(seq 1 60); do
    sleep 2
    rm -rf "$W/log"; ios fsync --udid="$UDID" --app="$BUNDLE_ID" pull --srcPath=Documents/lasrecomp.log --dstPath="$W/log" >/dev/null 2>&1 || true
    LOG=$(find "$W/log" -name lasrecomp.log 2>/dev/null | head -1)
    [ -n "$LOG" ] && grep -q "^\[py\] second run\|^\[py\] generator exit [1-9]\|^\[py\] selftest FAILED\|^\[py\] second-run compare FAILED" "$LOG" && break
done
[ -n "${LOG:-}" ] || { echo "no Documents/lasrecomp.log on the device"; exit 1; }
grep "^\[py\]" "$LOG"
grep -q "^\[py\] selftest ok" "$LOG" || { echo "FAIL: self-test"; exit 1; }
[ "$(grep -c "^\[py\] generator exit 0" "$LOG")" = 2 ] || { echo "FAIL: generator"; exit 1; }
grep -q "^\[py\] second run: .* identical" "$LOG" || { echo "FAIL: the second run in one interpreter differs"; exit 1; }

# the device's seed (go-ios pulls one file at a time, into <dst>/<name>)
ios fsync --udid="$UDID" --app="$BUNDLE_ID" tree --path=Documents/pytest-seed --nojson 2>/dev/null \
    | python3 -c '
import re, sys
rows = [(len(m.group(1)) // 3, m.group(2)) for m in (re.match(r"^((?:\|  )*)\|-(.*)$", l) for l in sys.stdin) if m]
base = min(d for d, _ in rows); stack = []
for d, n in rows:
    stack = stack[:d - base]
    if n.endswith("/"): stack.append(n[:-1])
    else: print("/".join(stack + [n]))' > "$W/ios-files.txt"
while IFS= read -r rel; do
    mkdir -p "$W/ios/$(dirname "$rel")"
    ios fsync --udid="$UDID" --app="$BUNDLE_ID" pull --srcPath="Documents/pytest-seed/$rel" --dstPath="$W/pull" >/dev/null 2>&1
    mv "$W/pull/$(basename "$rel")" "$W/ios/$rel"; rm -rf "$W/pull"
done < "$W/ios-files.txt"
echo "$(wc -l < "$W/ios-files.txt" | tr -d ' ') files from the device"

# the same seed here
[ -x "$PY" ] || { echo "$PY: the randomizer venv is needed for the comparison (README.md: Randomizer)"; exit 1; }
echo '{"seed": "pytest", "create-spoiler": true}' > "$W/request.json"
(cd tools/rando && PYTHONHASHSEED=0 "../../$PY" las_headless.py "$PWD/../../$ROMFS" "../../$W/mac" "../../$W/request.json" > "../../$W/mac-generate.log" 2>&1)

# compare (generate.log and request.json: the device's own paths; spoiler_view.txt's first line names the folder)
fail=0
while IFS= read -r rel; do
    case "$rel" in generate.log|request.json) continue ;; esac
    if [ ! -f "$W/mac/$rel" ]; then echo "only on the device: $rel"; fail=1
    elif [ "$rel" = spoiler_view.txt ]; then
        cmp -s <(tail -n +2 "$W/mac/$rel") <(tail -n +2 "$W/ios/$rel") || { echo "DIFF $rel"; fail=1; }
    elif ! cmp -s "$W/mac/$rel" "$W/ios/$rel"; then echo "DIFF $rel"; fail=1; fi
done < "$W/ios-files.txt"
(cd "$W/mac" && find . -type f | sed 's|^\./||') | while IFS= read -r rel; do
    [ -f "$W/ios/$rel" ] || case "$rel" in generate.log) ;; *) echo "only here: $rel"; exit 1 ;; esac
done || fail=1
[ $fail = 0 ] && echo "ok: the device's seed equals this machine's" || { echo "FAIL: the seeds differ ($W/mac vs $W/ios)"; exit 1; }
