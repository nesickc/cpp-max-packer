# Project status

Updated 2026-11-11. Details belong in PRs and `.local/` logs.

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

## Current goal: Core v1.0

The spec defines the MVP (§2.3, §3 Core tier, §11.4). These are the known gaps, in the
order a user meets them. Items 1–7 are one branch and PR each; item 8 is a run. Not in
v1.0: Vulkan, STL containers, upright/free-3D search, crash recovery, Windows 10 and
further packing-count work.

1. [ ] **Installer** (SYS-01/02, §4.3). The supported build and CI stop at
   `cargo build`: the NSIS bundle is never built, skips WebView2 and is version 0.0.0.
   *Done when* one command produces a versioned setup containing the offline WebView2
   installer, plus the staged engine as a zip.
2. [ ] **Search panel** (UI-01/03, SOL-05/08, §7.1–7.2). A new project starts at a
   fixed 2 mm pitch, 60 s and one thread; no presets, RAM estimate, remaining budget
   or time since improvement. *Done when* Fast/Balanced/Thorough set the budget and
   an object-sized pitch (coarsened to fit RAM, with a notice), threads default to
   the shell's value, and resolved pitch, threads, RAM and both times are shown.
3. [ ] **Continue** (SOL-06, §8.2, §11.4, AT-13). Engine and desktop have none; every
   Start is a fresh job. *Done when* Continue adds budget to a stopped, finished or
   reopened result, starts from its revalidated poses with geometry, box, clearances
   and rotations locked, never lowers the count and records a second run segment.
4. [ ] **Project and export actions** (UI-01, §7.1, §8.5). Save always asks for a
   path; no viewport PNG; no open-output-folder. *Done when* Save reuses the current
   path, Save As asks, Export PNG writes the view with its count/settings companion,
   and a finished save or export can reveal its folder.
5. [ ] **Long operations** (UI-03, §10.1, AT-16). Prompt Stop, and results kept through
   write and allocation failures, are already tested. Only a solve can be stopped:
   envelope builds (~24 s) and STL export (minutes) show no progress. *Done when*
   export reports copies done, both cancel cleanly, and a test fails an export on an
   unwritable target and finds the result and project intact.
6. [ ] **Guide and messages** (§2.2, §8.5, M6). `docs/DESKTOP.md` is a stale developer
   note; the results panel lacks the geometric-only scope note; a quantization
   failure gives no advice. *Done when* DESKTOP.md is a short user guide opened from
   the app (best found, envelopes, failed exports), the scope note is shown, and a
   failed STL export says to raise clearance and that the JSON stays valid.
7. [ ] **CLI surface** (DATA-01, §8.1, AT-01/13). `pack`, `validate` and `benchmark`
   return `METHOD_UNSUPPORTED`; the stdio service answers only `capabilities.get`.
   *Done when* question B is settled and the shipped commands repeat a desktop job
   headless, including a standalone `validate` that exits 5 on a failed result.
8. [ ] **Release run** (AT-01, AT-14, AT-15 box journey, §11.4). A checklist in the
   guide, run from the installer on clean Windows 11, offline: the whole journey,
   keyboard, screenshot, Save As, error recovery, CLI job, `scoreboard.py --record`.

Spec questions to settle first (this page does not change the spec):

- A. §7.2 gives Balanced and Thorough Free 3D orientations, which SOL-03 makes Later.
- B. §8.1 requires `pack`/`validate`/`benchmark` and eleven stdio methods; the app
  ships on `inspect`, `solve` and its own session protocol. Build them or narrow §8.1.
- C. AT-11 is Core but needs the exact-contact L-prism pair (`lprism2` finds 1 of 2).
- D. §7.3 full-resolution copy inspection, AT-15 LOD switching and §10.1 parser
  fuzzing are unbuilt and carry no Later mark.

## Known problems

1. Conservative envelopes can close recesses/cavities and reduce nesting; they neither
   repair the source nor establish printability. No shrinking to fit.
2. Reconstruction is approximate and can erase features; prefer the envelope.
3. The exact-contact L-prism pair remains unfound. The bracket's known 50-copy layout
   is a target: 42 found in an exterior-size box, real interior dimensions unknown.
4. `10_kg_np.stl` is 400 x 350 x 285 mm, not the real 400 x 340 x 285 mm Nova Post box.
5. Checked STL export of large envelope layouts is slow: 420 Pryanik copies take
   about 12 minutes (2.9 GB), roughly 1 s per copy for source-enclosure certification.
6. Projects saved before the diagonal/2000 envelope offset need re-import.

See [ALGORITHM.md](../docs/ALGORITHM.md) and [BUILDING.md](../docs/BUILDING.md).
