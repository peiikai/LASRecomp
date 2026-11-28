#!/bin/bash
# The data folder's game files are hard links to the player's own (runtime/romfs_build.c): nothing
# LAS Recomp does may write through them. Runs ./build/lasrecomp under tools/write_guard.c, which logs any open for writing,
# truncate or rename of an existing file in <data>/extracted/{exefs,romfs}, through:
#   setup   the start screen's Set up (LARECOMP_LAUNCHER_EXTRACT): copy, game code build, shaders
#   play    the game from boot for PLAY_FRAMES frames (default 1800), with LARECOMP_INPUT if you give one
#   delete  the start screen's Delete game files (LARECOMP_LAUNCHER_DELETE)
# with the data folder's files hard links to a scratch copy of the game files (made here, cloned where the file system
# can). Fails if the guard logged anything or the scratch copy changed (bytes of exefs, size / time / mode of all).
# usage: tools/test_game_files_read_only.sh <game files folder (exefs/ and romfs/)> [phases, default "setup play delete"]
# macOS and Linux; needs a built ./build/lasrecomp. Muted, windowed, scratch save and settings; WG_DIR: scratch folder.
set -e
cd "$(dirname "$0")/.."
GAME=${1:?usage: $0 <game files folder> [phases]}
PHASES=${2:-setup play delete}
WG=${WG_DIR:-build/write-guard-test}
rm -rf "$WG"
mkdir -p "$WG/data/extracted" "$WG/save"
WG=$(cd "$WG" && pwd -P)
PLAYER=$WG/player DATA=$WG/data LOG=$WG/guard.log
if [ "$(uname)" = Darwin ]; then
    LIB=build/write_guard.dylib
    cc -shared -fPIC -O1 tools/write_guard.c -o $LIB
    export_guard() { export DYLD_INSERT_LIBRARIES=$PWD/$LIB; }
    mkdir -p "$PLAYER"
    cp -cR "$GAME/exefs" "$GAME/romfs" "$PLAYER/"
else
    LIB=build/write_guard.so
    cc -shared -fPIC -O1 tools/write_guard.c -o $LIB -ldl
    export_guard() { export LD_PRELOAD=$PWD/$LIB; }
    mkdir -p "$PLAYER"
    cp -r --reflink=auto "$GAME/exefs" "$GAME/romfs" "$PLAYER/"
fi
fingerprint() {  # (ctime left out: unlinking a name changes the link count, and that is allowed)
    (cd "$PLAYER" && find . -type f | LC_ALL=C sort | while IFS= read -r f; do
        if [ "$(uname)" = Darwin ]; then stat -f '%z %m %p %N' "$f"; else stat -c '%s %Y %a %n' "$f"; fi
    done; cat exefs/* | shasum -a 256)
}
fingerprint > "$WG/before.txt"
# the data folder's copy as hard links to the player's (setup clones on APFS instead: the play and delete phases
# relink it so a write would reach the player's files)
relink() {
    rm -rf "$DATA/extracted/exefs" "$DATA/extracted/romfs"
    (cd "$PLAYER" && find exefs romfs -type d -exec mkdir -p "$DATA/extracted/{}" \; &&
     find exefs romfs -type f -exec ln {} "$DATA/extracted/{}" \;)
}
export LARECOMP_WRITE_GUARD="$DATA/extracted/exefs:$DATA/extracted/romfs" LARECOMP_WRITE_GUARD_LOG=$LOG
export LARECOMP_DATA_ROOT=$DATA LARECOMP_SAVE_DIR=$WG/save LARECOMP_NO_AUDIO=1 LARECOMP_NO_MOVIE_AUDIO=1
printf 'fullscreen = 0\n' > "$WG/settings.ini"
export LARECOMP_SETTINGS=$WG/settings.ini
run() {  # <what> <env...>: one copy of the game under the guard (tools/game_slot.sh: memory, one launch at a time)
    local what=$1; shift
    ( . tools/game_slot.sh
      export_guard
      env "$@" ./build/lasrecomp > "$WG/$what.out" 2>&1 ) || true
    # (the app's log is the data folder's lasrecomp.log outside a source checkout; stdout is mostly empty)
    cp "$DATA/lasrecomp.log" "$WG/$what.log" 2>/dev/null || cp "$WG/$what.out" "$WG/$what.log"
    echo "$what: $(grep -c 'WRITE-GUARD' "$LOG" 2>/dev/null || echo 0) guard lines so far ($WG/$what.log)"
}
for p in $PHASES; do
    case $p in
    setup)  run setup LARECOMP_NO_LAUNCHER=0 LARECOMP_LAUNCHER_EXTRACT="$PLAYER"
            grep -q 'setup: .* -> READY' "$WG/setup.log" || echo "setup didn't finish: see $WG/setup.log" ;;
    play)   relink
            run play LARECOMP_WINDOW_SHOT=${PLAY_FRAMES:-1800} LARECOMP_EXIT_AFTER_SHOT=1 LARECOMP_SHOT_PATH="$WG/play.png" ;;
    delete) relink
            run delete LARECOMP_NO_LAUNCHER=0 LARECOMP_LAUNCHER_DELETE=1 LARECOMP_LAUNCHER_SHOT=60
            grep -q '\[launcher\] delete .*: ok' "$WG/delete.log" || echo "delete didn't run: see $WG/delete.log" ;;
    esac
done
fingerprint > "$WG/after.txt"
fail=0
if [ -s "$LOG" ]; then echo "FAIL: LAS Recomp wrote to the game files' links:"; sort -u "$LOG" | sed 's/ (pid.*//' | head -20; fail=1; fi
if ! cmp -s "$WG/before.txt" "$WG/after.txt"; then echo "FAIL: the player's copy changed:"; diff "$WG/before.txt" "$WG/after.txt" | head -20; fail=1; fi
[ $fail = 0 ] && echo "OK: nothing wrote to the game files ($PHASES)" && rm -rf "$PLAYER" "$DATA/extracted"
exit $fail
