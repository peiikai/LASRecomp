#!/bin/bash
# (Link is warped to the dungeon's entrance (ENTRANCE overrides), then placed at the doorway: a cell's centre can be a pit)
# Walk every mismatched doorway of a generated-dungeon seed (its door jumps) and check Link ends up in
# the room it leads to: tools/doorjump_test.sh <seed> [first] [count]. One game run per doorway (warp into the
# cell, LARECOMP_DOORJUMP_TEST starts after the dungeon title card (Link is held while it shows: jumps undone), puts Link 1.2 inside the doorway and pushes the stick outward). Fresh scratch saves only.
# Same-side doorways (every doorway on one wall of a single-screen room leads to the same place) are checked first,
# without the game: a wall whose doorways lead to different rooms is a GROUP FAIL. The cases: `lasrando room-cases
# doors` (the seed's door jumps; its rooms built from its record, as the game does).
# DJ_GROUPS=1: walk only those walls' doorways.
# Results: build/doorjump_<seed>.txt (+ a screenshot per doorway in build/doorjump_<seed>/).
SEED=$1; FIRST=${2:-1}; COUNT=${3:-999}
JOB=${DJ_JOB:-$SEED}  # DJ_JOB=<name>: several jobs of one seed in parallel (own saves, shots, results)
OUT=build/doorjump_$JOB.txt; SHOTS=build/doorjump_$JOB
rm -rf "$SHOTS"; mkdir -p "$SHOTS"; : > "$OUT"
JOBS=$SHOTS/jobs.tsv
# the doorways to walk (level, cell, side, centre, cell he must end up in, floor) + the same-side check
make -s lasrando >/dev/null || exit 1
build/lasrando room-cases doors extracted/romfs "rando/$SEED" "$JOBS" "${DJ_GROUPS:-0}" 2>/dev/null | tee -a "$OUT"
n=0
while IFS=$'\t' read -r level cell side slot expect fl; do
    n=$((n + 1))
    [ $n -lt "$FIRST" ] && continue
    [ $n -ge $((FIRST + COUNT)) ] && break
    case $level in Lv02*|Lv10*) ent=08C;; Lv03*|Lv07*) ent=08B;; Lv05*) ent=08H;; *) ent=08D;; esac  # dungeon_gen ENTRANCES
    tools/fresh_save.sh "djtest_$JOB" >/dev/null 2>&1  # (per seed: suites of different seeds run in parallel)
    LARECOMP_DOORJUMP_TEST="3100:$cell:$side:$slot:$fl" LARECOMP_CHEATS=hearts LARECOMP_RANDO_SEED=$SEED \
    LARECOMP_SAVE_DIR="build/djtest_$JOB" LARECOMP_WINDOW=640x360 SHOT_ID="dj_$JOB" \
    LARECOMP_INPUT="400:A:6,520:A:6,900:ZL:8,900:ZR:8,1300:A:6,1600:A:6,1900:A:6,2200:A:6" \
    LARECOMP_WARP_AT="2700:$level:${level}_${ENTRANCE:-$ent}" tools/shot.sh 4000 600 </dev/null 2>/dev/null
    cp "build/window_dj_$JOB.png" "$SHOTS/${level:2:2}-$cell$side$slot.png"
    res=$(grep '\[doorjump-test\].*->' "build/run_shot_dj_$JOB.log" | tail -1)
    [ -z "$res" ] && res="[doorjump-test] $cell $side -> no result (crash? $(grep -c '\*\*\* signal' "build/run_shot_dj_$JOB.log") signals)"
    echo "$level $slot (into $expect) $res" | tee -a "$OUT"
done < "$JOBS"
# contact sheet: every doorway's last frame (Link must be visible in the room he walked into)
python3 - "$SHOTS" <<'PY'
import os, sys
from PIL import Image, ImageDraw
d = sys.argv[1]
fs = sorted(f for f in os.listdir(d) if f.endswith('.png') and f != 'sheet.png')
if fs:
    w, h, cols = 480, 270, 3
    sheet = Image.new('RGB', (w * cols, h * ((len(fs) + cols - 1) // cols)))
    for i, f in enumerate(fs):
        im = Image.open(os.path.join(d, f)).resize((w, h))
        ImageDraw.Draw(im).text((8, 30), f[:-4], fill=(255, 255, 0))
        sheet.paste(im, ((i % cols) * w, (i // cols) * h))
    sheet.save(os.path.join(d, 'sheet.png'))
PY
walked=$(grep -c '^Lv.*doorjump-test' "$OUT"); pass=$(grep -c 'doorjump-test.*PASS' "$OUT")
echo "$pass pass / $((walked - pass)) other; $(grep -c 'GROUP FAIL' "$OUT") wall(s) leading to more than one place -> $OUT"
