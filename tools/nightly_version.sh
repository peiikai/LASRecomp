#!/bin/sh
# A nightly build's version (.github/workflows/nightly.yml): LARECOMP_VERSION = nightly-<YYYYMMDDHHMM>-<commit> and
# LARECOMP_NIGHTLY_STAMP = that time, so its update check only offers a newer nightly of its own platform.
# Usage: tools/nightly_version.sh <version> <stamp>
set -eu
v=$1
s=$2
case $v in nightly-[0-9]*-*) ;; *) echo "nightly_version.sh: not a nightly version: $v" >&2; exit 1 ;; esac
case $s in [0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9]) ;; *) echo "nightly_version.sh: bad stamp: $s" >&2; exit 1 ;; esac
F=runtime/ui/launcher.mm
sed -i.bak "s/^#define LARECOMP_VERSION \".*\"/#define LARECOMP_VERSION \"$v\"/; s/^#define LARECOMP_NIGHTLY_STAMP .*/#define LARECOMP_NIGHTLY_STAMP ${s}LL/" "$F"
rm -f "$F.bak"
grep -q "^#define LARECOMP_VERSION \"$v\"" "$F" && grep -q "^#define LARECOMP_NIGHTLY_STAMP ${s}LL" "$F" ||
    { echo "nightly_version.sh: $F: LARECOMP_VERSION / LARECOMP_NIGHTLY_STAMP not set" >&2; exit 1; }
echo "nightly version $v (stamp $s)"
