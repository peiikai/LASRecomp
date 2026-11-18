#!/bin/bash
# Puts the release notes into the macOS update feed (appcast-macOS.xml, tools/sparkle_appcast.sh), so Sparkle's update
# window shows them instead of loading the GitHub release page. The notes are rendered to HTML by GitHub's own
# markdown renderer (they look as on the release page) and go in the item's <description>; the release page stays as
# Sparkle's "Version History" button (fullReleaseNotesLink). Sparkle prefers a releaseNotesLink over a <description>,
# so the link goes once the notes are in; without notes (none written yet, or the renderer failed) the feed keeps the
# link and the window shows the release page as before. The EdDSA signature covers the .dmg, not the feed, so the feed
# can be rewritten at any time.
# Usage:
#   tools/sparkle_notes.sh <appcast.xml> release|nightly <release body.md>
#     rewrites the feed in place with the notes taken from that release body. release: the body without the build
#     lines release.yml adds (the "being built" line, "Built from", the sha256 table). nightly: its "## Changes".
#   tools/sparkle_notes.sh --publish <tag>   (GH_TOKEN, GH_REPO set)
#     a published release: its feed and body from GitHub, rewritten, the feed uploaded again. Nothing to do for a
#     draft or a release without a feed. .github/workflows/appcast_notes.yml runs it when a release is published or
#     its notes are edited.
# (test) SPARKLE_NOTES_HTML=<file>: render from that HTML instead of calling GitHub's renderer.
set -euo pipefail
REPO=${GH_REPO:-peiikai/LASRecomp}

if [ "${1:-}" = --publish ]; then
    [ $# = 2 ] || { echo "usage: tools/sparkle_notes.sh --publish <tag>" >&2; exit 2; }
    TAG=$2
    case $TAG in nightly) echo "nightly: its feed gets the notes in nightly.yml"; exit 0 ;; esac
    W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
    gh release view "$TAG" --json isDraft,body,assets > "$W/rel.json"
    [ "$(jq -r .isDraft "$W/rel.json")" = false ] || { echo "$TAG is a draft: its notes aren't final"; exit 0; }
    jq -e '.assets[] | select(.name == "appcast-macOS.xml")' "$W/rel.json" >/dev/null ||
        { echo "$TAG has no appcast-macOS.xml (yet): nothing to do"; exit 0; }
    jq -r .body "$W/rel.json" > "$W/body.md"
    gh release download "$TAG" -p appcast-macOS.xml -D "$W"
    "$0" "$W/appcast-macOS.xml" release "$W/body.md"
    gh release upload "$TAG" --clobber "$W/appcast-macOS.xml"
    echo "$TAG: appcast-macOS.xml updated"
    exit 0
fi

[ $# = 3 ] || { echo "usage: tools/sparkle_notes.sh <appcast.xml> release|nightly <body.md> | --publish <tag>" >&2; exit 2; }
FEED=$1 KIND=$2 BODY=$3
[ -f "$FEED" ] || { echo "no $FEED (no signing key?): no notes to add"; exit 0; }
NOTES=$(mktemp); HTML=$(mktemp); trap 'rm -f "$NOTES" "$HTML"' EXIT
# (the notes' own text, CRLF as LF (GitHub's editor saves CRLF): HTML comments out - the nightly round markers - and the build lines)
perl -0pe 's/\r\n?/\n/g; s/<!--.*?-->[ \t]*\n?//gs' "$BODY" | case $KIND in
    release)
        awk '/^> The download files (are being built|will be added here)/ { next }
             /^Release notes: \(to be written\)$/ { next }
             /^Built from [0-9a-f]+\.?$/ || /^[|] File [|] sha256 [|]$/ || /^The linux-aarch64 file is the nightly build/ { exit }
             { print }' ;;
    nightly)
        awk '/^## Changes[ \t]*$/ { on = 1 } on && /^To report a problem/ { exit } on { print }' ;;
    *) echo "kind: release or nightly" >&2; exit 2 ;;
esac > "$NOTES"
# (trailing blank lines and a closing "---" rule out)
perl -0pi -e 's/(\s*\n---\s*)?\s*\z/\n/' "$NOTES"
if ! grep -q '[^[:space:]]' "$NOTES"; then
    echo "no release notes in $BODY: the feed keeps the release page link"
    exit 0
fi
if [ -n "${SPARKLE_NOTES_HTML:-}" ]; then
    cp "$SPARKLE_NOTES_HTML" "$HTML"
elif ! gh api markdown -F text=@"$NOTES" -f mode=gfm -f context="$REPO" > "$HTML" || ! [ -s "$HTML" ]; then
    echo "::warning::GitHub's markdown renderer failed: the macOS feed keeps the release page link"
    exit 0
fi
python3 -I - "$FEED" "$HTML" <<'EOF_PY'
import re, sys
feed, html_path = sys.argv[1], sys.argv[2]
xml = open(feed, encoding="utf-8").read()
html = open(html_path, encoding="utf-8").read()
# (Sparkle's web view brings the font and its own dark-mode colors; this only keeps GitHub's markup tidy in it)
style = ("<style>h1{font-size:1.4em;margin:.2em 0 .6em}h2{font-size:1.15em;margin:1.1em 0 .4em}"
         "ul,ol{padding-left:1.4em}li{margin:.15em 0}a.anchor{display:none}"
         "code{font-family:ui-monospace,Menlo,monospace;font-size:.92em}"
         "table{border-collapse:collapse}td,th{border:1px solid rgba(128,128,128,.4);padding:2px 6px}"
         "blockquote,.markdown-alert{margin:.6em 0;padding-left:.8em;border-left:3px solid rgba(128,128,128,.5)}"
         ".markdown-alert-title{font-weight:600}svg{display:none}</style>\n")
cdata = (style + html).replace("]]>", "]]]]><![CDATA[>")
m = re.search(r"<sparkle:(?:fullR|r)eleaseNotesLink>([^<]*)</sparkle:(?:fullR|r)eleaseNotesLink>", xml)
if not m:
    sys.exit("sparkle_notes.sh: no release notes link in " + feed)
page = m.group(1)
# (a feed that already has notes: they're replaced)
xml = re.sub(r"[ \t]*<description\b[^>]*>.*?</description>\n", "", xml, flags=re.S)
xml = re.sub(r"[ \t]*<sparkle:(?:fullR|r)eleaseNotesLink>[^<]*</sparkle:(?:fullR|r)eleaseNotesLink>\n", "", xml)
def add(m):
    indent = m.group(1)
    return (m.group(0) + f'{indent}<description sparkle:format="html"><![CDATA[{cdata}]]></description>\n'
            f"{indent}<sparkle:fullReleaseNotesLink>{page}</sparkle:fullReleaseNotesLink>\n")
xml, n = re.subn(r"<item>[ \t]*\n([ \t]*)<title>[^<]*</title>\n", add, xml, count=1)
if n != 1:
    sys.exit("sparkle_notes.sh: no item title in " + feed)
open(feed + ".part", "w", encoding="utf-8").write(xml)
EOF_PY
mv "$FEED.part" "$FEED"
echo "$FEED: release notes added ($(wc -c < "$HTML" | tr -d ' ') bytes of HTML)"
