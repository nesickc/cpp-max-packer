# Scoreboard history

Rows appended by `python tools/scoreboard.py --record`. `!` marks a case below its
floor or not `valid`. Compare repeated runs before reading much into a change of one
or two copies.

| Date | Commit | Results |
| --- | --- | --- |
| 2026-10-17 | 07349c8 (engine from PR #16) | cube64=64 \| lprism2=1 \| pryanik2=340 \| pryanik2-0=1 \| ulamok=36 |
| 2026-10-17 | 5f67ba3 | pryanik2-0=330 \| cube64=64 \| lprism2=1 \| pryanik2=340 \| ulamok=36 |
| 2026-10-17 | 7a057d0 | pryanik2-0=360 \| cube64=64 \| lprism2=1 \| pryanik2=340 \| ulamok=36 |
| 2026-10-17 | 1db851c | pryanik2-0=360 \| cube64=64 \| lprism2=1 \| pryanik2=340 \| ulamok=36 |
| 2026-10-18 | e9904ba | cube64=64 \| lprism2=1 \| pryanik2=340 \| pryanik2-0=360 \| ulamok=36 |
| 2026-10-18 | f8e16fd | cube64=64 \| lprism2=1 \| pryanik2=343 \| pryanik2-0=360 \| ulamok=36 |
| 2026-10-18 | bee20f2 | cube64=64 \| lprism2=1 \| pryanik2=350 \| pryanik2-0=360 \| ulamok=36 |
| 2026-10-18 | aa97af9 | cube64=64 \| lprism2=1 \| pryanik2=425 \| pryanik2-0=416 \| ulamok=36 |
| 2026-10-18 | 185015f | cube64=64 \| lprism2=1 \| pryanik2=425 \| pryanik2-0=416 \| ulamok=36 |
| 2026-10-18 | a42c584 | cube64=64 \| lprism2=1 \| pryanik2=435 \| pryanik2-0=416 \| ulamok=36 |
| 2026-10-18 | 7f6c4c9 + import-repair worktree | cube64=64 \| lprism2=1 \| pryanik2=435 \| pryanik2-0=447 \| ulamok=36 |
| 2026-10-19 | 65b71d4 | cube64=64 \| lprism2=1 \| pryanik2=435 \| pryanik2-0=447 \| ulamok=36 |
| 2026-10-19 | 710c4dc | cube64=64 \| lprism2=1 \| pryanik2=435 \| pryanik2-0=447 \| ulamok=36 |
| 2026-10-19 | 72f7e9a | cube64=64 \| lprism2=1 \| pryanik2-0=450 \| ulamok=36 |
| 2026-10-19 | 72f7e9a | pryanik2=435 |
| 2026-10-19 | e4643eb + compaction | cube64=64 \| lprism2=1 \| lprism-gap=2 \| pryanik2=435 \| pryanik2-0=450 \| ulamok=36 |
| 2026-10-19 | e4643eb + compaction, low RAM | cube64=64 \| lprism2=1 \| lprism-gap=2 \| pryanik2=-! \| pryanik2-0=-! \| ulamok=36 |
| 2026-10-20 | befd9a1 + automatic envelopes / validator v7, concurrent Python tests | cube64=64 \| lprism2=1 \| lprism-gap=2 \| pryanik2=435 \| pryanik2-0=447 \| ulamok=36 |
| 2026-10-20 | befd9a1 + automatic envelopes / validator v7, isolated repeat | pryanik2-0=450 |
| 2026-10-20 | 7361287 + diagonal compaction | cube64=64 \| lprism2=1 \| lprism-gap=2 \| pryanik2=435 \| pryanik2-0=450 \| ulamok=36 |
| 2026-10-21 | ee08c71 (cube) | cube64=64 \| lprism2=1 \| lprism-gap=2 \| pryanik2=429 \| pryanik2-0=450 \| ulamok=36 |
| 2026-10-21 | ee08c71 (cube45) | cube64=64 \| lprism2=1 \| lprism-gap=2 \| pryanik2=425 \| pryanik2-0=416 \| ulamok=36 |

The low-RAM repeat had about 1.1 GiB available physical memory. Both Pryanik runs
were refused before search (`IMPORT_WORKING_BYTES`); the previous engine also
refused the bracket import under the same pressure. The earlier compaction run
completed all six cases. Logs: `.local/compaction/`.

The v7 zero-clearance repeat followed an isolated old-engine run; both reached
450 at the unchanged 60-second budget. Bulba additionally improved from 22 to
24 native-valid copies in 400 x 340 x 285 mm, with 0/0 mm clearance, cube rotations,
2 mm pitch and a 60-second search budget. Its 408 x 348 x 293 mm run retained 24;
32 is unverified. No model-specific search changes were retained. Logs and saved
results: `.local/bulba/`.

Diagonal refinement preserves all six counts. Bulba improves from 24 after 360 seconds
to 30 after 90 seconds in 410 x 350 x 285 mm, with four threads, 2 mm pitch, cube
rotations and 0/0 mm clearance. Both results pass native validation. Source meshes
and physical constraints are unchanged. The final desktop result occupies
281.605 mm in Z and retains identical displayed/saved/restored/exported poses.
Logs and results: `.local/diagonal/`;
scoreboard artifacts: `.local/scoreboard/20261020-182445/`.

The 45-degree catalog retains validity in all six cases but lowers the two Pryanik
counts at the same 60-second budget. It is optional; the desktop default remains
the 24 cube rotations. A separate diagonal-only bar fixture improves from zero to
a validated fit with 45-degree turns and survives restore/export. Logs:
`.local/rotations45/`; scoreboard artifacts: `.local/scoreboard/20261021-221507/`
and `.local/scoreboard/20261021-221827/`.
