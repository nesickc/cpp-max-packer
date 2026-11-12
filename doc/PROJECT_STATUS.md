# Project status

Updated 2026-11-12. Details belong in PRs and `.local/` logs.

## What works

- **Engine (C++20, CPU):** STL import, independent overlap/containment and pair/wall
  clearance validation (validator v7), mixed-orientation packing, CPU FFT search,
  interlocking twin layers, whole-arrangement compaction and checked export.
- **Desktop:** units and diagnostics, box with size tolerance (default 0.1 mm, ADR
  0022), clearances, 24 cube rotations or 96 with 45° turns, Start/Stop, live validated
  packing, portable Save/Open, JSON and checked STL export. Results keep their settings.
- **Packing envelopes** ([ADR 0021](../spec/decisions/0021-packing-envelopes.md)): every
  desktop import builds a cavity-free CGAL envelope that native validation checks
  against the retained source. Original, Envelope and Overlay views share poses,
  clipping and selection; STL export writes the source and re-verifies enclosure
  after float32 quantization. Exact-valid solids stay available through the CLI.
- **Practical counts** (400 x 340 x 285 mm, 0/0 mm clearance, `best_found`): simplified
  Pryanik 336 in 120 s, Bulba 30 in 90 s at 2 mm pitch, bracket 60. Logs: `.local/`.

## Scoreboard

Release, eight threads; 10-second analytic and 60-second STL budgets; Pryanik in
400 x 340 x 285 mm at 4 mm manual pitch. History: [SCOREBOARD.md](../benchmarks/SCOREBOARD.md).

| Case | Count | Floor | Target |
| --- | --- | --- | --- |
| cube64 | 64 | 64 | 64 |
| lprism2 | 1 | 1 | 2 |
| lprism-gap (0.125/0.125 mm) | **2** | 2 | 2 |
| pryanik2 (1/1 mm) | **435** | 425 | 426 |
| pryanik2-0 (0/1 mm) | **450** (447 beside other jobs) | 416 | 416 |
| ulamok | 36 | 36 | 36 |
| lprism-01 (0.1/0.1 mm, 0.05 mm slack) | 2 | 2 | 2 |
| sphere14 (10 mm, 24.6 mm cube) | 10 | 10 | 14 |
| sphere66 (10 mm, 40.6 mm cube) | 64 (66 once) | 64 | 66 |
| sphere40-np (40 mm, 400 x 340 x 285 mm) | 540 | 540 | 690 |
| tile5, tile10 (45° turns) | 4, 9 | 4, 9 | 5, 10 |
| yasya-0, -1, -wall0 (envelope, 400 x 240 x 200 mm) | **74**, 74, 74 | 74 | 74 |

## Current goal: Core v1.0

In working order; each item is one branch and PR. Spec v0.4 records the decisions behind
it. Not in v1.0: Vulkan, STL containers, free-3D search, search presets, crash recovery,
Windows 10, and the `pack`/`validate`/`benchmark` and stdio surface.

**Daily use**

1. [x] **Drag and drop** (UI-01). Dropping one STL on the window imports it as the
   Import button does; anything else, or a drop during an operation, is refused with a message.
2. [ ] **Box presets** (UI-01). *Done when* a list fills W/D/H and stays editable.
   Nova Post, mm: 0.5 kg 170x120x90, 1 kg 240x170x90, 2 kg 340x240x90, 3 kg 240x240x200,
   5 kg 400x240x200, 10 kg 400x340x285, 15 kg 600x350x280, 20 kg 470x400x420,
   30 kg 700x400x420. Only the 10 kg interior is measured; the rest are published sizes.
3. [ ] **Build output and installer** (SYS-01/02, §4.3). Binaries sit deep in `out/` and
   `desktop/src-tauri/target/`; no installer is built. *Done when* one command leaves
   `bin/release/spectrapack-desktop.exe` with its engine (`bin/debug/` likewise) and
   another builds a versioned NSIS setup with the offline WebView2 installer, in CI too.

**Denser packing**

4. [x] **Density cases** (QA-01). Spheres, tilted tiles and the L pair at 0.1 mm are on
   the scoreboard above. The 17-tile packing needs free rotation: a later target.
5. [ ] **Solver cleanup** (first slice of item 8). Simplify seeding and compaction in
   `pack_solver` (10,500 lines) before extending them; scoreboard unchanged.
6. [ ] **Denser search**, tried in this order, each kept only if the scoreboard moves.
   (a) Repeating patterns: search the three repeat vectors of one copy, or of a copy
   and its twin, on the existing pair no-fit maps, then the placement in the box that
   holds most. It replaces the grid, stagger and twin-layer seeds if it beats them.
   (b) One more copy: insert an overlapping copy and move its neighbours downhill on
   total overlap until none is left, for the last copies and the wall regions. Poses
   are continuous so free rotation can join later. (c) Settling with shaking, only if
   measured cheap enough. *Done when* the small cube reaches 14, the box case 620,
   and no other case loses count or validity.

**Robustness**

7. [ ] **Hostile files** (§10.1). No fuzzing or sanitizer build exists. *Done when* STL
   import, project open and the JSON/protocol readers each have a fuzz target that CI
   runs under AddressSanitizer with time and memory caps, every finding has a
   reproducer test, and an adversarial mesh cannot hang or exhaust the envelope build.
8. [ ] **Review and simplify**, one PR per module: `pack_io` (10,000 lines), CLI (4,600,
   including the stub commands), geometry outside the validator, desktop shell (4,800).
   *Done when* each PR removes more than it adds with tests and scoreboard unchanged.
   Validator, clearance, transform and contract changes get an independent review.

**Finish the desktop**

9. [ ] **Search panel** (UI-01/03, SOL-05/08). *Done when* threads default to the
   shell's value and resolved pitch, threads, estimated RAM, remaining budget and time
   since improvement are shown. Starts at one thread and a fixed 2 mm pitch today.
10. [ ] **Continue** (SOL-06, §8.2, AT-13). Every Start is a fresh job. *Done when*
    Continue adds budget to a stopped, finished or reopened result from its revalidated
    poses, with geometry, box, clearances and rotations locked, and never lowers the count.
11. [ ] **Project and export actions** (§7.1, §8.5). *Done when* Save reuses the current
    path, Save As asks, Export PNG writes the view, and a save or export can reveal
    its folder.
12. [ ] **Long operations** (UI-03, AT-16). Envelope builds (~24 s) and STL export (~95 s
    for 429 Pryanik copies) show no progress and cannot be stopped. *Done when* both
    report progress and cancel cleanly, and a failed write leaves result and project intact.
13. [ ] **Guide and messages** (§2.2, §8.5). *Done when* `docs/DESKTOP.md` is a short user
    guide opened from the app, the results panel states the geometric-only scope, and a
    failed STL export says to raise clearance and that the JSON stays valid.
14. [ ] **Release run** (AT-01, AT-14, AT-15 box journey). From the installer on clean
    Windows 11, offline: the whole journey, then `scoreboard.py --record`.

## Known problems

1. Conservative envelopes can close recesses/cavities and reduce nesting; they neither
   repair the source nor establish printability. No shrinking to fit.
2. Reconstruction is approximate and can erase features; prefer the envelope.
3. Exact fits with no slack stay undecided and are rejected: the L pair needs 0.05 mm
   of box slack, which the desktop size tolerance supplies. The bracket's known 50-copy
   layout is a target: 42 found in an exterior-size box, interior dimensions unknown.
4. `10_kg_np.stl` is 400 x 350 x 285 mm, not the real 400 x 340 x 285 mm Nova Post box.
5. Checked STL export of large envelope layouts is large and CPU-bound: 429 Pryanik
   copies write a 2.9 GB STL in about 95 s on eight threads (13 minutes before).
   Every copy is still reimported and enclosure-certified; fewer cores take longer.
6. Projects saved before the diagonal/2000 envelope offset need re-import.
7. Timed searches still depend on machine load: the same settings can return a
   different count on a busy machine (the 40.6 mm sphere cube gave 66 once, 64 since).
8. The desktop cannot show a packing of about 3,900 copies or more: a live packing is
   one record capped at 1 MiB, and a larger one ends the search with a publication error.

See [ALGORITHM.md](../docs/ALGORITHM.md) and [BUILDING.md](../docs/BUILDING.md).
