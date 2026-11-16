#!/bin/bash
# The macOS update feed for Sparkle (runtime/ui/sparkle.mm): build/dist/appcast-macOS.xml with one item, this build's
# .dmg, signed with the UPDATE_SIGNING_KEY secret (EdDSA, the same key as the Windows and Linux manifests:
# tools/update_sign.cpp). release.yml and nightly.yml put it next to the .dmg; the apps read it from
# releases/latest/download/ (releases) or releases/download/nightly/ (nightlies), tools/bundle_macos.sh. Without the
# key it only warns: no feed, and the app's Download still works.
# Usage: tools/sparkle_appcast.sh <tag the release is published under: v0.7.0 or nightly> <dmg>
set -euo pipefail
cd "$(dirname "$0")/.."
[ $# = 2 ] || { echo "usage: tools/sparkle_appcast.sh <release tag> <dmg>" >&2; exit 2; }
REL=$1
DMG=$2
OUT=build/dist/appcast-macOS.xml
rm -f "$OUT"
if [ -z "${UPDATE_SIGNING_KEY:-}" ]; then
    echo "::warning::UPDATE_SIGNING_KEY is not set: no Sparkle feed for macOS (players there update with Download)"
    exit 0
fi
TAG=$(sed -n 's/^#define LARECOMP_VERSION "\(.*\)"/\1/p' runtime/ui/launcher.mm)
TEST_OF=$(sed -n 's/^#define LARECOMP_TEST_OF "\(.*\)"/\1/p' runtime/ui/launcher.mm)
read -r SHORT BUILD <<EOF_V
$(tools/bundle_version.sh "$TAG" "$TEST_OF")
EOF_V
mkdir -p build
"${CXX_UPDATE:-clang++}" -std=c++17 -O2 -Iruntime tools/update_sign.cpp -o build/update_sign
SIG=$(build/update_sign sparkle "$DMG")
LEN=$(wc -c < "$DMG" | tr -d ' ')
NAME=$(basename "$DMG" | tr ' ' .)  # (GitHub stores "LAS Recomp-..." as "LAS.Recomp-...")
URL=https://github.com/peiikai/LASRecomp/releases/download/$REL/$NAME
TITLE=$TAG
# (a nightly's short version is 0.0.1: Sparkle's window names it by its date instead)
case $TAG in nightly-*) TITLE="Nightly build ${TAG:8:4}-${TAG:12:2}-${TAG:14:2}"; SHORT=$TITLE ;; esac
cat > "$OUT.part" <<EOF
<?xml version="1.0" encoding="utf-8"?>
<rss version="2.0" xmlns:sparkle="http://www.andymatuschak.org/xml-namespaces/sparkle">
  <channel>
    <title>LAS Recomp</title>
    <item>
      <title>$TITLE</title>
      <pubDate>$(LC_ALL=C date -u '+%a, %d %b %Y %H:%M:%S +0000')</pubDate>
      <sparkle:version>$BUILD</sparkle:version>
      <sparkle:shortVersionString>$SHORT</sparkle:shortVersionString>
      <sparkle:minimumSystemVersion>15.0</sparkle:minimumSystemVersion>
      <sparkle:releaseNotesLink>https://github.com/peiikai/LASRecomp/releases/tag/$REL</sparkle:releaseNotesLink>
      <enclosure url="$URL" length="$LEN" type="application/octet-stream" sparkle:edSignature="$SIG"/>
    </item>
  </channel>
</rss>
EOF
mv "$OUT.part" "$OUT"
cat "$OUT"
