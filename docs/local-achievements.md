# Local achievements (development)

This branch adds 80 local achievements for vanilla Link's Awakening.
The five-shell unlock and the eight activity rules have been smoke-tested in gameplay on macOS/Metal. No account or network service is involved.

## Behavior

The renderer polls `GA(LIVE_SAVE)` ten times per second, evaluates the catalog, and queues newly earned
achievements. It does not wait for the game to save. Unlocks are written to `achievements-las-v1.log` in the
data folder, separate from Nintendo saves. They apply to this game's local installation across its save slots.
A history read failure disables tracking for the session rather than replacing unreadable progress. Failed
writes are logged and remain eligible for another attempt; no popup is shown until the write succeeds.

Opening a save establishes a baseline: existing items do not unlock achievements. A save's UUID identifies a
new playthrough; full save reads are matched to the live UUID, body CRC and achievement progress to recognize
reloads of the same playthrough. Tracking also rebases when leaving gameplay or rebooting. Already satisfied
baseline rules stay suppressed for that session, even if an item disappears and is reacquired. Earned IDs
persist across restarts and cannot award twice. Loading a new game allows previously unearned rules again.

The popup is 25% larger than the first gameplay preview and lasts seven seconds: circle entrance, swirling ring, horizontal expansion, reading time, contraction,
then disappearance. Whole letters disappear from right to left when the inner edge reaches them. Text is never
resized or wrapped during contraction. Popups queue and pause while the host menu or keyboard is open.
The menu and popup share original badges for each achievement category. Locked badges use muted colors,
and a lock badge keeps the hidden story achievement a surprise.

## Implemented rules

Stable IDs LA-001 through LA-080 are in `runtime/achievement_rules.inc`:

- Heart Pieces: 1, 4, 8, 12, 16, 20, 24, 28, 32.
- Maximum hearts: 5, 8, 10, 12, 15, 18, 20.
- Seashells: 1, then every 5 through 50.
- Each figurine and all 10; each instrument and all 8; each song and all 3.
- Eleven equipment acquisitions/upgrades.
- Four trading-item acquisitions (current item, not proof of completing a particular quest event).
- Three capacity upgrades and owning 1, 2 and 3 bottles.

Open **Achievements** from the LAS menu to see the total unlocked and filter the catalog by locked or unlocked.
When a vanilla save is loaded, collection goals show current progress in that save. The final story achievement
stays hidden until earned. Importing existing progress and disabling popups are still pending. Randomizer sessions
are excluded because collection totals and acquisition semantics can differ. Save-editor changes and cheats
can satisfy these local rules; this is not a competitive or anti-cheat system.

The eight activity and story rules reward meeting the Color Dungeon guards, claiming the Fairy Queen's reward,
landing a small or big fish, earning the 150 cm fishing reward, winning the Yoshi Doll, clearing Dampé's first
Chamber Dungeon challenge, earning the one-minute Rapids Ride reward, and awakening the Wind Fish.

Activity flags come from the vanilla game’s event table. The Trendy Game rule uses Yoshi Doll acquisition
as evidence of winning that specific prize. The story rule uses the Wind Fish event’s completion flag,
which marks the post-boss sequence. Deathless, speedrun and no-damage challenges are outside this set.

Inventory fields follow `docs/save-format.md`. Some are code-mapped but not gameplay-validated. Figurine
ownership versus placement/lifetime collection, transient trading items, load timing and simultaneous
acquisitions require targeted in-game checks. Polling may miss an item acquired and consumed between samples.
Guest-state reads follow existing overlays' render-thread convention and are not a transactional game snapshot.

## Validation

The macOS host executable builds successfully with `IOSEXPORT=0` and reports its version on launch.
The tracker sanitizer tests pass. On Apple M1/macOS with game v1.0.0, isolated gameplay tests confirmed:

- Loading a completed save enters gameplay without awarding its existing accomplishments.
- Calling the game’s own AddItem function for a fifth seashell produces LA-018 before the subsequent autosave.
- Captured frames show the circle, expanded banner and whole-letter removal during contraction.
- Restarting and repeating that grant does not award the achievement again. Its journal has one entry.
- A scripted Yoshi Doll grant and seven live event-flag changes produce exactly the eight expected activity unlocks.

These gameplay smoke tests use scripted item grants and flag changes. Natural activity playthroughs and
full gameplay coverage of the catalog are still pending. Vulkan, individual figurine semantics, remaining acquisition paths and same-session reload
edge cases still need gameplay coverage.

`python3 tools/test_achievements.py` builds the production tracker with AddressSanitizer and UBSan and checks
all 80 rules, multi-milestone unlocks, deduplication, reload baselines, UUID changes, ignored high bits, bottles,
persistence, partial journal writes and I/O failures. No game files or real saves are used.

Build with `make -j6 IOSEXPORT=0` to omit the unrelated iOS export dependency. The source-only host build still
requires the player's game files to play. Test both rendering backends before distributing a release.

For gameplay tests, use isolated `LARECOMP_SAVE_DIR` and `LARECOMP_SETTINGS` paths and set
`LARECOMP_ACHIEVEMENTS_FILE` to an isolated history file. Mute with `LARECOMP_NO_AUDIO=1` and
`LARECOMP_NO_MOVIE_AUDIO=1`. Do not use the real progress file for synthetic grant tests.

Validate a completed-save load (no popups), a fresh-file pickup, one acquisition crossing multiple thresholds,
a same-slot reload, a different-slot load, menu pause/resume, resizing, and restart persistence. Existing
`LARECOMP_GIVE_ITEM`, `LARECOMP_POKE_SAVE` and screenshot hooks can help with isolated test cases; they do not
replace confirming natural acquisition events.
