#!/bin/bash
# tools/linux/check_portable.sh <x86_64|aarch64> (after make appimage + make tarball, on a machine of that arch): the
# data folder rules of runtime/paths.c, checked with the shipped files. No window or GPU: --list-settings reads the
# settings from the data folder and exits (its log line "[paths] data: <folder>" says which one it used).
#   - the tarball: its own folder (portable.txt), else ~/.local/share/lasrecomp once portable.txt is gone
#   - the AppImage with <file>.AppImage.home next to it: <file>.AppImage.home/.local/share/lasrecomp
#   - --data-dir: that folder
set -euo pipefail
cd "$(dirname "$0")/../.."
ARCH=${1:?usage: tools/linux/check_portable.sh x86_64|aarch64}
TAR=$(ls build/dist/*-linux-"$ARCH".tar.gz)
APP=$(ls build/dist/*-linux-"$ARCH".AppImage)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
export HOME="$T/home" XDG_DATA_HOME="$T/xdg"  # (never the machine's own data folder)
unset LARECOMP_DATA_ROOT APPIMAGE APPDIR
mkdir -p "$HOME"
data_of() {  # command... -> the data folder it used
    local log="$T/run.log"
    "$@" --list-settings >/dev/null 2>"$log" || true
    # (without a terminal the log goes to the data folder's lasrecomp.log: look there too)
    local logs=("$T"/*/lasrecomp*.log "$T"/*/*/lasrecomp*.log "$T"/*/*/.local/share/lasrecomp/lasrecomp*.log)
    { cat "$log"; cat "${logs[@]}" 2>/dev/null; } | sed -n 's/^\[paths\] data: //p' | tail -1
    rm -f "${logs[@]}"
}
expect() {  # what, got, want
    if [ "$2" = "$3" ]; then echo "ok: $1 -> $2"; else echo "::error::$1: data folder '$2', expected '$3'"; exit 1; fi
}

tar -xzf "$TAR" -C "$T"
D="$T/LAS Recomp"
[ -f "$D/portable.txt" ] && [ -x "$D/AppRun" ] && [ -L "$D/lasrecomp" ] || { echo "::error::tarball layout"; exit 1; }
expect "tarball" "$(cd "$T" && data_of "$D/lasrecomp")" "$D"
expect "tarball --data-dir" "$(data_of "$D/lasrecomp" --data-dir "$T/chosen")" "$T/chosen"
rm "$D/portable.txt"
expect "tarball without portable.txt" "$(data_of "$D/lasrecomp")" "$T/xdg/lasrecomp"

mkdir "$T/ai"
cp "$APP" "$T/ai/game.AppImage"
chmod +x "$T/ai/game.AppImage"
export APPIMAGE_EXTRACT_AND_RUN=1  # (no FUSE needed)
"$T/ai/game.AppImage" --appimage-portable-home >/dev/null 2>&1
[ -d "$T/ai/game.AppImage.home" ] || { echo "::error::--appimage-portable-home made no folder"; exit 1; }
expect "AppImage with .home" "$(data_of "$T/ai/game.AppImage")" "$T/ai/game.AppImage.home/.local/share/lasrecomp"
echo "data folder rules: ok"
