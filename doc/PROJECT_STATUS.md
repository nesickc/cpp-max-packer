# Project status

Updated 2026-11-28. Details belong in PRs and `.local/` logs.

## What works

- **Engine (C++20, CPU):** STL import, independent overlap/containment and pair/wall
  clearance validation (validator v8), mixed-orientation packing, CPU FFT search,
  interlocking twin layers, whole-arrangement compaction and checked export.
- **Desktop:** units and diagnostics, box with size tolerance (default 0.1 mm, ADR
  0022), clearances, 24 cube rotations or 96 with 45° turns, Start/Stop, Continue from
  a result's revalidated poses, live validated packing with a search panel, portable
  Save/Save As/Open, JSON, checked STL and view PNG export, Show in folder. Results keep
  their settings and run segments.
- **Packing envelopes** ([ADR 0021](../spec/decisions/0021-packing-envelopes.md)): every
  desktop import builds a cavity-free CGAL envelope that native validation checks
  against the retained source. Original, Envelope and Overlay views share poses,
  clipping and selection; STL export writes the source and re-verifies enclosure
  after float32 quantization. Exact-valid solids stay available through the CLI.
- **Practical counts** (0/0 mm clearance, `best_found`): simplified Pryanik 336 in 120 s
  (400 x 340 x 285 mm) and Bulba 27 in 90 s (410 x 350 x 285 mm), both at 2 mm pitch;
  bracket 60 (400 x 340 x 285 mm). Logs: `.local/`.

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
| sphere14 (10 mm, 24.6 mm cube) | **14** | 14 | 14 |
| sphere66 (10 mm, 40.6 mm cube) | 64 (66 once) | 64 | 66 |
| sphere40-np (40 mm, 400 x 340 x 285 mm) | **660** | 660 | 690 |
| tile5, tile10 (45° turns, 0.1/0.1 mm) | **5**, 9 | 5, 9 | 5, 10 |
| tile5-0, tile10-0 (45° turns, 0/0.1 mm) | **5**, 9 | 5, 9 | 5, 10 |
| yasya-0, -1, -wall0 (envelope, 400 x 240 x 200 mm) | **74**, 74, 74 | 74 | 74 |
| bunny (envelope, 400 x 340 x 285 mm, local model) | **47** (48 once, at 59 s) | 46 | 47 |
| benchy (envelope, 240 x 170 x 90 mm, local model) | **47** | 46 | 47 |

## Current goal: Core v1.0

In working order; each item is one branch and PR. Spec v0.4 records the decisions behind
it. Not in v1.0: Vulkan, STL containers, free-3D search, search presets, crash recovery,
Windows 10, and the `pack`/`validate`/`benchmark` and stdio surface.

**Daily use**

1. [x] **Drag and drop** (UI-01). Dropping one STL on the window imports it as the
   Import button does; anything else, or a drop during an operation, is refused with a message.
2. [x] **Box presets** (UI-01). A list fills W/D/H, which stay editable.
   Nova Post, mm: 0.5 kg 170x120x90, 1 kg 240x170x90, 2 kg 340x240x90, 3 kg 240x240x200,
   5 kg 400x240x200, 10 kg 400x340x285, 15 kg 600x350x280, 20 kg 470x400x420,
   30 kg 700x400x420. Only the 10 kg interior is measured; the rest are published sizes.
3. [x] **Build output and installer** (SYS-01/02, §4.3). `Invoke-Desktop.ps1 -Mode Build`
   leaves `bin/release/spectrapack-desktop.exe` with its engine and notices (`-DebugBuild`:
   `bin/debug/`); `-Mode Installer` adds an unsigned `bin/installer/SpectraPack_0.1.0_x64-setup.exe`
   (212 MiB) with the offline WebView2 installer, which CI uploads. The CMake project
   version (0.1.0) is the product version.

**Denser packing**

4. [x] **Density cases** (QA-01). Spheres, tilted tiles and the L pair at 0.1 mm are on
   the scoreboard above. The 17-tile packing needs free rotation: a later target.
5. [x] **Solver cleanup** (first slice of item 8). Simplify seeding and compaction in
   `pack_solver` (11,300 lines) before extending them; scoreboard unchanged.
   Each phase was knocked out in turn: only compaction's greedy empty-box layouts
   never helped and are gone, and so is the stagger seed with `diagnostics.stagger`,
   which never decided a timed result. The other older phases still win a case or a test fixture.
6. [x] **Denser search**, tried in this order, each kept only if the scoreboard moves.
   (a) Repeating patterns: search the three repeat vectors of one copy, or of a copy
   and its twin, on the existing pair no-fit maps, then the placement in the box that
   holds most. It replaces the grid, stagger and twin-layer seeds if it beats them.
   (b) One more copy: insert an overlapping copy and move its neighbours downhill on
   total overlap until none is left, for the last copies and the wall regions. Poses
   are continuous so free rotation can join later. (c) Settling with shaking, only if
   measured cheap enough. *Done when* the small cube reaches 14, the box case 620,
   and no other case loses count or validity.
   Reached with (a) alone: layers shift and sit closer than a part's height (box
   case 540 to 600), and each lattice is tightened from conservative cells until the
   validator reports contact (small cube 10 to 14, box case 660). Not reached: the
   66-sphere cube needs a half-cell layer shift and stays at 64, and tightening adds
   nothing at zero pair clearance.
   (b) grows one more copy in from a seed while its neighbours move aside: bunny 45
   to 47, five tiles at zero pair clearance 4 to 5; lattices and the other STL cases
   have no slack for it. The five-tile case at 0.1 mm needed validator v8, which
   proves a pair clearance for turned copies (4 to 5); ten tiles stay at 9.
   (c) is untried.

**Robustness**

7. [x] **Hostile files** (§10.1). The `windows-ninja-fuzz` preset builds four libFuzzer
   targets under AddressSanitizer: STL import, the packing envelope, the JSON contracts
   and `.spectrapack` open (see [BUILDING.md](../docs/BUILDING.md)).
   CI's `windows-fuzz` job runs each for 60 s with a per-input timeout and a 2 GiB cap.
   They found no memory error. Adversarial meshes found the envelope build unbounded:
   8,000 box-spanning triangles (a 400 KB file) kept it busy for four minutes and took
   1.2 GiB against a 1 GiB allowance. Admission now charges the search pieces of large
   triangles, so that file is refused in under a second, and the wrap's searches have a
   work budget (one billion piece visits, a few minutes; the largest local model needs
   nine million) that ends the build with an envelope-limit error. Envelopes of the
   nine local models are byte-identical.
8. [x] **Review and simplify**. PRs #59 to #64 remove 27,000 lines with
   tests and scoreboard unchanged: the old benchmark harness, profile seams and `serve --stdio`; the
   CLI's and `pack_io`'s byte ledgers and test hooks (the tested library is the shipped one);
   export's file pins; geometry accounting tests; the shell's copy of the arithmetic; the
   contracts' resource layer. Settings and results are version 2
   ([ADR 0023](../spec/decisions/0023-settings-and-results-v2.md)): every run is timed under one
   host memory cap, with no fixed-work mode, resource policy, work counts or recovery record, and
   version 1 projects no longer open. #65 deletes six cumulative work budgets that never end a
   phase and measures that two do: physical-query work ends mixed interlocking on pryanik2, and
   validation work cuts compaction short on pryanik2, yasya, bunny and benchy. #66 removes the
   rest of the work caps and another 9,300 lines: a search ends at its deadline, on Stop or when
   nothing is left to try, phases share out time, and `SpectralLimits` is a memory cap, a thread
   count and a status sink. Those two budgets stay as constants in the two phases they shape
   (without the second, the 46th benchy comes 7 s later). The greedy per-insertion FFT phase
   that the caps mostly served went with them, since timed box runs had not reached it since
   gap filling; it was the only search for STL containers, so `solve` now refuses those (a
   Later tier). FFT stays in the lattice and gap-fill phases.

**Finish the desktop**

9. [x] **Search panel** (UI-01/03, SOL-05/08). Threads start at the shell's default
   (at most four) and Open keeps a project's count. Under the viewer, during a search
   and after it: resolved pitch, threads, the engine's RAM estimate against the host
   ceiling, remaining budget and time since the count last rose.
10. [x] **Continue** (SOL-06, §8.2, AT-13). Continue adds budget to a stopped, finished
    or reopened result: the validator re-checks its poses, the search starts from them
    and the count cannot fall. Object, box, tolerance, clearances and rotations stay
    locked; pitch, budget, threads and seed may change. The result keeps every run
    segment. It is a warm start (`search_state_reset`): the seeded phases run again, so
    extra budget helps where compaction, gap filling or one more copy had time left to
    use. Tampered or mismatched poses are refused and the displayed result stays.
11. [x] **Project and export actions** (§7.1, §8.5). Save reuses the project file set by
    Save As or Open (New forgets it), Save As asks, Export view PNG writes the viewer with a
    `.png.json` of the result's count and settings, and Show in folder selects what a save or export wrote.
12. [x] **Long operations** (UI-03, AT-16). Import, envelope and export show their stage
    under the viewer, with a bar where its size is known (STL writing, per-copy checks and
    hashing: 94% of a 429-copy export). Cancel ends them within about a second (the engine
    is terminated after 2 s), keeping the previous object, result and project; a cancelled
    or failed export removes the folder it created.
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
   different count on a busy machine (the 40.6 mm sphere cube gave 66 once, 64 since;
   on 18 November the 24.6 mm cube gave 10 instead of 14 in one run of ten, and on
   28 November in 5 of 20 runs with the engine from `main` and 7 of 24 with #66).
   The engine asks Windows not to throttle it while its app is in the background, but
   most of a search still runs on one core, so other busy programs slow it.
8. The panel's RAM estimate covers the fields and FFT buffers of the lattice and gap-fill
   phases, each reported before it allocates them (the largest so far is shown). The grid,
   block and row phases and the validator are not in it.

See [ALGORITHM.md](../docs/ALGORITHM.md) and [BUILDING.md](../docs/BUILDING.md).
