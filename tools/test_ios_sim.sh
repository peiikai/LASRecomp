#!/bin/bash
# The embedded Python's self-test in the iOS Simulator: tools/test_ios_sim.sh <app> [simulator name]
# (make ios-sim-test). Boots a simulator (SIM_DEVICE, default "LAS Recomp test": created from the newest iPhone type
# and iOS runtime when missing), installs the app, launches it with LARECOMP_IOS_PYTEST=1 and reads
# Documents/lasrecomp.log from the app's data container: `[py] selftest ok` must be there (every native package
# imported and exercised) and, with no game files in the simulator, `[py] generator skipped`. SIM_ROMFS=<romfs dir>
# links that romfs into the app's Documents first, and then the generator's two runs and their comparison are
# required too (as on a device). A screenshot of the start screen goes to build/ios-sim/start-screen.png. The game
# itself is not run in the simulator (its GPU is the Mac's; the macOS build covers the game code).
set -euo pipefail
cd "$(dirname "$0")/.."
APP=${1:-build/ios-sim/LAS Recomp.app}
NAME=${2:-${SIM_DEVICE:-LAS Recomp test}}
BUNDLE_ID=$(plutil -extract CFBundleIdentifier raw -o - "$APP/Info.plist")
OUT=$(dirname "$APP")

UDID=$(xcrun simctl list devices available -j | python3 -c '
import json, sys
name = sys.argv[1]
for devs in json.load(sys.stdin)["devices"].values():
    for d in devs:
        if d["name"] == name: print(d["udid"]); sys.exit()' "$NAME")
if [ -z "$UDID" ]; then
    # the newest iOS runtime, and the last iPhone it supports (the runtime lists its own device types)
    read -r RUNTIME TYPE < <(xcrun simctl list runtimes -j | python3 -c '
import json, sys
rts = [r for r in json.load(sys.stdin)["runtimes"] if r["isAvailable"] and r["platform"] == "iOS"]
rt = max(rts, key=lambda r: [int(x) for x in r["version"].split(".")])
import re
phones = [t for t in rt["supportedDeviceTypes"] if t["productFamily"] == "iPhone" and re.search(r"iPhone \d+", t["name"])]
phones.sort(key=lambda t: (int(re.search(r"iPhone (\d+)", t["name"]).group(1)), "Pro" in t["name"]))
print(rt["identifier"], phones[-1]["identifier"])')
    UDID=$(xcrun simctl create "$NAME" "$TYPE" "$RUNTIME")
    echo "created simulator $NAME ($TYPE, $RUNTIME)"
fi
xcrun simctl bootstatus "$UDID" -b >/dev/null
xcrun simctl uninstall "$UDID" "$BUNDLE_ID" 2>/dev/null || true
xcrun simctl install "$UDID" "$APP"
DATA=$(xcrun simctl get_app_container "$UDID" "$BUNDLE_ID" data)
LOG=$DATA/Documents/lasrecomp.log
if [ -n "${SIM_ROMFS:-}" ]; then
    mkdir -p "$DATA/Documents/extracted" && ln -sfn "$(cd "$SIM_ROMFS" && pwd)" "$DATA/Documents/extracted/romfs"
fi
# (SIMCTL_CHILD_* reaches the app as its environment)
SIMCTL_CHILD_LARECOMP_IOS_PYTEST=1 xcrun simctl launch "$UDID" "$BUNDLE_ID" >/dev/null
for i in $(seq 1 60); do
    sleep 2
    [ -f "$LOG" ] && grep -q "^\[py\] \(generator skipped\|second run\|generator exit [1-9]\|selftest FAILED\|second-run compare FAILED\)" "$LOG" && break
done
xcrun simctl io "$UDID" screenshot "$OUT/start-screen.png" >/dev/null 2>&1 || true
xcrun simctl terminate "$UDID" "$BUNDLE_ID" 2>/dev/null || true
[ -f "$LOG" ] || { echo "no $LOG"; exit 1; }
grep "^\[py\]\|FATAL\|Traceback" "$LOG" || true
grep -q "^\[py\] selftest ok" "$LOG" || { echo "FAIL: self-test"; exit 1; }
if [ -n "${SIM_ROMFS:-}" ]; then
    [ "$(grep -c "^\[py\] generator exit 0" "$LOG")" = 2 ] || { echo "FAIL: generator"; exit 1; }
    grep -q "^\[py\] second run: .* identical" "$LOG" || { echo "FAIL: the second run differs"; exit 1; }
else
    grep -q "^\[py\] generator skipped" "$LOG" || { echo "FAIL: the generator neither ran nor was skipped"; exit 1; }
fi
echo "ok: the embedded Python works in the simulator ($NAME)"
