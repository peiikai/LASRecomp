#!/bin/bash
# tools/check_notices.sh <package>...: fails unless each package holds THIRD-PARTY-NOTICES.txt at its root, the same
# as the repository's (tools/third_party_notices.sh), and nothing named after the dev-only Python libraries (capstone,
# unicorn: tools/ and the tests only). A package is a release file (.zip, .tar.gz, .dmg, .AppImage, .apk, .laskit,
# .ipa) or the folder it is made from (the Windows "LAS Recomp" folder, a .app, the Linux AppDir).
# Where the notices go: the top of the .zip / .tar.gz / AppDir / .dmg / .laskit; Contents/Resources in the macOS app
# (its root can't hold files); the root of the iOS app; assets/ in the .apk.
set -euo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd)
WANT=$REPO/THIRD-PARTY-NOTICES.txt
N=THIRD-PARTY-NOTICES.txt
[ $# -gt 0 ] || { echo "usage: tools/check_notices.sh <package>..."; exit 2; }
[ -f "$WANT" ] || { echo "no $WANT"; exit 1; }
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail=0
bad() { echo "check_notices: $1"; fail=1; }

same() {  # <package> <where in it> <file or -> : the notices there are the repository's
    if [ ! -s "$3" ]; then bad "$1: no $N at ${2:-its top}"
    elif [ "$(cksum < "$3")" != "$(cksum < "$WANT")" ]; then bad "$1: $2$N differs from the repository's (tools/third_party_notices.sh)"
    fi
}
no_dev_libs() {  # <package> <list of its file names>
    local hit
    hit=$(grep -iE 'capstone|unicorn' <<<"$2" || true)
    [ -z "$hit" ] || bad "$1 holds files of the dev-only Python libraries: $hit"
}
from_list() {  # <package> <list of names> <path in it>: the archive has it?
    grep -qxF "$3" <<<"$2" || { bad "$1: no $3"; return 1; }
}

for p in "$@"; do
    case $p in
    *.app|*.app/)
        if [ -d "$p/Contents" ]; then same "$p" Contents/Resources/ "$p/Contents/Resources/$N"
        else same "$p" "" "$p/$N"; fi
        no_dev_libs "$p" "$(cd "$p" && find . -print)" ;;
    *.zip|*.laskit|*.ipa|*.apk)
        list=$(zipinfo -1 "$p")
        no_dev_libs "$p" "$list"
        case $p in
        *.apk) where=("assets/$N") ;;
        *.laskit) where=("$N" "Payload/LAS Recomp.app/$N") ;;
        *.ipa) where=("Payload/LAS Recomp.app/$N") ;;
        *) where=("$(grep -m1 -E "^([^/]+/)?$N\$" <<<"$list" || echo "LAS Recomp/$N")") ;;
        esac
        for w in "${where[@]}"; do
            from_list "$p" "$list" "$w" || continue
            unzip -p "$p" "$w" > "$TMP/n"; same "$p" "$(dirname "$w")/" "$TMP/n"
        done ;;
    *.tar.gz)
        list=$(tar -tzf "$p")
        no_dev_libs "$p" "$list"
        w=$(grep -m1 -E "^([^/]+/)?$N\$" <<<"$list" || true)
        if [ -z "$w" ]; then bad "$p: no $N at its top"
        else tar -xzOf "$p" "$w" > "$TMP/n"; same "$p" "$(dirname "$w")/" "$TMP/n"; fi ;;
    *.dmg)
        hdiutil attach -readonly -nobrowse -mountpoint "$TMP/mnt" "$p" >/dev/null 2>&1
        same "$p" "" "$TMP/mnt/$N"
        for a in "$TMP/mnt"/*.app; do same "$p" "$(basename "$a")/Contents/Resources/" "$a/Contents/Resources/$N"; done
        no_dev_libs "$p" "$(cd "$TMP/mnt" && find . -print)"
        hdiutil detach "$TMP/mnt" >/dev/null 2>&1 ;;
    *.AppImage)
        # (the squashfs follows the runtime, a 64-bit ELF ending with its section headers: e_shoff + e_shentsize *
        # e_shnum. Read without running it, so any arch's AppImage can be checked)
        command -v unsquashfs >/dev/null || { bad "$p: needs unsquashfs (squashfs-tools); or check its AppDir"; continue; }
        le() { od -An -t u"$2" -j "$1" -N "$2" "$p" | tr -d ' '; }
        off=$(( $(le 40 8) + $(le 58 2) * $(le 60 2) ))
        no_dev_libs "$p" "$(unsquashfs -o "$off" -l "$p" 2>/dev/null)"
        unsquashfs -o "$off" -cat "$p" "$N" > "$TMP/n" 2>/dev/null || true
        same "$p" "" "$TMP/n" ;;
    *)
        [ -d "$p" ] || { bad "$p: not a package this knows"; continue; }
        same "$p" "" "$p/$N"
        no_dev_libs "$p" "$(cd "$p" && find . -print)" ;;
    esac
done
[ $fail = 0 ] && echo "check_notices: $N in $*"
exit $fail
