#!/bin/bash
# The self-updater end to end on its AppImage path (runtime/update.cpp), on macOS or Linux, with no network beyond
# this machine: a throwaway signing key, a local web server (python3 -m http.server) holding a signed manifest and a
# fake AppImage (a shell script), and tools/test_update_e2e.cpp standing in for the app. Covers: an update and its
# restart, the first start and the start screen keeping it, a new version that never gets there (rolled back), and
# the failures that must leave the app as it was: bad signature, truncated or damaged download, a manifest for
# another platform or an older nightly, no manifest, a folder that can't be written.
# Usage: tools/test_update_e2e.sh   (from anywhere; prints "all passed" and exits 0)
set -uo pipefail
cd "$(dirname "$0")/.."
ROOT=$(pwd)
T=$(mktemp -d "${TMPDIR:-/tmp}/lasrecomp-e2e.XXXXXX")
PORT=$((20000 + RANDOM % 20000))
SERVER=
cleanup() { [ -n "$SERVER" ] && kill "$SERVER" 2>/dev/null; chmod -R u+w "$T" 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT
fails=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }

CXX=${CXX:-clang++}
CC=${CC:-clang}
mkdir -p "$T/obj"
$CXX -std=c++17 -O1 -Iruntime -o "$T/update_sign" tools/update_sign.cpp || exit 1
for f in update_crypto update_crypto_ed25519; do
    $CC -O1 -DLARECOMP_SELF_UPDATE_TEST -Iruntime -c runtime/$f.c -o "$T/obj/$f.o" || exit 1
done
$CXX -std=c++17 -O1 -DLARECOMP_SELF_UPDATE_TEST -Iruntime -Iruntime/nvn -o "$T/app" tools/test_update_e2e.cpp \
    runtime/update.cpp "$T/obj/update_crypto.o" "$T/obj/update_crypto_ed25519.o" -lpthread || exit 1
"$T/update_sign" keygen "$T/key" > "$T/pub" || exit 1
export UPDATE_SIGNING_KEY=$(cat "$T/key")
export LARECOMP_UPDATE_KEY=$(awk '/^public /{print $2}' "$T/pub")
export LARECOMP_UPDATE_URL=http://127.0.0.1:$PORT
export LARECOMP_UPDATE_NO_RESTART=1
PLAT=linux-x86_64
NEW=nightly-202611171000-abc1234

# the "AppImages": scripts that say which version they are (and leave a mark when started)
appimage() { printf '#!/bin/sh\necho %s > "%s/started"\n' "$1" "$T"; }
mkdir -p "$T/srv/nightly" "$T/app1"
appimage v2 > "$T/srv/nightly/LAS.Recomp-$NEW-$PLAT.AppImage"
manifest() {  # <version> <platform> -> srv/nightly/update-<platform>.txt
    "$T/update_sign" manifest "$1" "$2" "$T/srv/nightly/LAS.Recomp-$NEW-$PLAT.AppImage" > "$T/srv/nightly/update-$2.txt"
}
manifest "$NEW" "$PLAT"
(cd "$T/srv" && exec python3 -m http.server "$PORT" --bind 127.0.0.1 >/dev/null 2>&1) &
SERVER=$!
for _ in $(seq 50); do curl -fs "http://127.0.0.1:$PORT/nightly/update-$PLAT.txt" >/dev/null && break; sleep 0.1; done

fresh() { rm -rf "$T/app1"; mkdir -p "$T/app1"; appimage v1 > "$T/app1/LAS.AppImage"; chmod 755 "$T/app1/LAS.AppImage"; rm -f "$T/started"; }
export APPIMAGE=$T/app1/LAS.AppImage
is_v1() { grep -q 'echo v1' "$APPIMAGE"; }
is_v2() { grep -q 'echo v2' "$APPIMAGE"; }
app() { TEST_VERSION=${V:-nightly-202611151000-0000000} "$T/app" "$@" > "$T/out" 2>&1; }

# an update, its restart into the new file, the first start, the start screen
fresh
check "update available" 'app available'
check "update installs" 'app start nightly $PLAT 202611151000 && grep -q "^done" "$T/out"'
check "new AppImage in place, executable" 'is_v2 && [ -x "$APPIMAGE" ]'
check "old AppImage kept" '[ -f "$T/app1/.lasrecomp-update/old/LAS.AppImage" ]'
check "first start of the new version" 'V=$NEW app boot && grep -q "first start" "$T/out"'
check "start screen keeps it, old gone" 'V=$NEW app commit && is_v2 && [ ! -e "$T/app1/.lasrecomp-update" ]'
fresh
unset LARECOMP_UPDATE_NO_RESTART
check "restart starts the new file" 'app start nightly $PLAT 202611151000 restart; sleep 0.5; [ "$(cat "$T/started" 2>/dev/null)" = v2 ]'
export LARECOMP_UPDATE_NO_RESTART=1
# the new version starts once and never reaches the start screen: the next start puts the old one back
fresh
app start nightly $PLAT 202611151000
V=$NEW app boot
check "never reached the start screen: rolled back" 'V=$NEW app boot; grep -q "previous version is back" "$T/out" && is_v1 && [ ! -e "$T/app1/.lasrecomp-update" ]'

# failures: nothing changes, nothing left behind
fails_clean() { ! app "$@" && grep -q "^failed" "$T/out" && is_v1 && [ ! -e "$T/app1/.lasrecomp-update" ]; }
fresh
check "older nightly refused" 'fails_clean start nightly $PLAT 202611180000'
check "same nightly refused" 'fails_clean start nightly $PLAT 202611171000'
check "no manifest for the platform" 'fails_clean start nightly linux-aarch64 202611151000'
cp "$T/srv/nightly/update-$PLAT.txt" "$T/srv/nightly/update-linux-aarch64.txt"
check "manifest for another platform" 'fails_clean start nightly linux-aarch64 202611151000 && grep -q "another platform" "$T/out"'
check "a release tag gets no nightly" 'fails_clean start v0.7.0 $PLAT 0'
mkdir -p "$T/srv/v0.7.0" && cp "$T/srv/nightly/"* "$T/srv/v0.7.0/"
check "a release whose manifest names another version" 'fails_clean start v0.7.0 $PLAT 0 && grep -q "another version" "$T/out"'
cp "$T/srv/nightly/update-$PLAT.txt" "$T/good"
sed 's/^version .*/version nightly-202611171001-abc1234/' "$T/good" > "$T/srv/nightly/update-$PLAT.txt"
check "changed manifest (bad signature)" 'fails_clean start nightly $PLAT 202611151000 && grep -q "signature does not match" "$T/out"'
LARECOMP_UPDATE_KEY=$(printf '%064d' 1) check "another key" 'fails_clean start nightly $PLAT 202611151000'
cp "$T/good" "$T/srv/nightly/update-$PLAT.txt"
cp "$T/srv/nightly/LAS.Recomp-$NEW-$PLAT.AppImage" "$T/full"
head -c 20 "$T/full" > "$T/srv/nightly/LAS.Recomp-$NEW-$PLAT.AppImage"
check "truncated download" 'fails_clean start nightly $PLAT 202611151000 && grep -q "damaged" "$T/out"'
sed 's/v2/v3/' "$T/full" > "$T/srv/nightly/LAS.Recomp-$NEW-$PLAT.AppImage"
check "damaged download (same size)" 'fails_clean start nightly $PLAT 202611151000 && grep -q "damaged" "$T/out"'
rm "$T/srv/nightly/LAS.Recomp-$NEW-$PLAT.AppImage"
check "download missing" 'fails_clean start nightly $PLAT 202611151000'
cp "$T/full" "$T/srv/nightly/LAS.Recomp-$NEW-$PLAT.AppImage"
if [ "$(id -u)" != 0 ]; then
    chmod 555 "$T/app1"
    check "read-only folder: Update off" '! app available && grep -q "writable" "$T/out"'
    check "read-only folder: refused, unchanged" '! app start nightly $PLAT 202611151000 && is_v1'
    chmod 755 "$T/app1"
fi
unset APPIMAGE
check "not an AppImage: Update off" '! app available && grep -q "AppImage" "$T/out"'

[ $fails = 0 ] && echo "all passed" || echo "$fails FAILED"
exit $((fails != 0))
