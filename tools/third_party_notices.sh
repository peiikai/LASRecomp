#!/bin/bash
# THIRD-PARTY-NOTICES.txt (repository root, copied to the root of every package): licenses/components.txt (what the
# packages bundle, with versions and sources) followed by the full text of every licence in licenses/.
# tools/third_party_notices.sh        writes it
# tools/third_party_notices.sh --check  fails if it is out of date, or if a "Text:" name in components.txt has no
#                                       licenses/<name>.txt or a licenses/ file is named by no component
# After changing a bundled library or its version: update licenses/ and components.txt, then run this. Packages are
# checked with tools/check_notices.sh.
set -euo pipefail
cd "$(dirname "$0")/.."
OUT=THIRD-PARTY-NOTICES.txt

names=$(sed -n 's/^  Text: *//p' licenses/components.txt | tr ',' '\n' | sed 's/^ *//; s/ *$//' | awk 'NF && !seen[$0]++')
bad=0
for n in $names; do [ -f "licenses/$n.txt" ] || { echo "components.txt names $n, but there is no licenses/$n.txt"; bad=1; }; done
for f in licenses/*.txt; do
    n=$(basename "$f" .txt)
    [ "$n" = components ] && continue
    grep -qxF "$n" <<<"$names" || { echo "licenses/$n.txt is not named by any component in components.txt"; bad=1; }
done
[ $bad = 0 ] || exit 1

make_notices() {
    cat licenses/components.txt
    printf '\n\nLicence texts\n-------------\n'
    for n in $names; do
        printf '\n\n========================================================================\n%s\n' "$n"
        printf '========================================================================\n\n'
        cat "licenses/$n.txt"
    done
}

if [ "${1:-}" = --check ]; then
    if ! make_notices | cmp -s - "$OUT"; then
        echo "$OUT is out of date: run tools/third_party_notices.sh"; exit 1
    fi
    echo "$OUT: up to date ($(wc -w <<<"$names" | tr -d ' ') licence texts)"
else
    make_notices > "$OUT"
    echo "$OUT: $(wc -w <<<"$names" | tr -d ' ') licence texts"
fi
