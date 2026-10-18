# Project status

Updated 2026-10-18. Details belong in PRs and `.local/` logs.

## What works

- **Engine (C++20, CPU):** STL import and cleanup; authoritative solids;
  independent overlap, containment and pair/wall-clearance validation; grid,
  stagger, residual-block and mesh-envelope interlocking proposals with mixed
  rotations and relative heights; CPU FFT placement; JSON results and validated STL export.
- **Desktop (Tauri/React/Three.js):** import, box settings, Start/Stop, live count,
  shared-mesh viewer, Save/Open project and export.
- **Timed search:** CLI inputs without a resolved resource policy now use the
  desktop RAM-share policy and bounded work recovery. Manual pitch, explicit
  memory caps, Stop and deadlines remain enforced; fixed-work behavior is unchanged.
- **Interlocking rows:** timed search derives lateral and relative-height offsets
  from the accepted mesh, then tries alternating permitted half-turn rotations
  across copies, rows and layers. Native v5 slab-support certificates validate
  deep wing overlaps; inconclusive pairs retain the full solid classifier.
- **Verification:** mixed-orientation behavior, independent transform review,
  scoreboard and fresh loading checks pass. Desktop rebuilt; all 629 Release,
  15 Debug solver, 44 Rust and 20 frontend tests pass, as do contract checks.

Not yet: Vulkan, STL containers, upright/free-3D rotations, library repair, crash
recovery or installer. These are mostly **Later** in spec v0.3.

## Scoreboard

`python tools/scoreboard.py --record` (Release, 8 threads), 2026-10-18 at `a42c584`.
Budgets are unchanged: 10 seconds for the analytic fixtures, 60 seconds for the STL cases.

| Case | Count | Floor | Target | Termination |
| --- | --- | --- | --- | --- |
| cube64 | 64 | 64 | 64 | budget_exhausted |
| lprism2 | 1 | 1 | 2 | search_stalled |
| pryanik2 (1/1 mm) | **435** | 425 | 426 | budget_exhausted |
| pryanik2-0 (0/1 mm) | **416** | 416 | 416 | budget_exhausted |
| ulamok | 36 | 36 | 36 | budget_exhausted |

Pryanik uses the unchanged 400 x 340 x 285 mm box and 4 mm manual pitch.
The positive-clearance layout improves from 425 to 435 in 33.2 seconds, using
240 copies of one orientation and 195 copies flipped relative to them.
The 2 mm run also reaches 435; both pitches and all saved scoreboard layouts
pass fresh native result-loading validation. Zero pair clearance retains 416.
History: [benchmarks/SCOREBOARD.md](../benchmarks/SCOREBOARD.md).

## Known problems and current goal

1. Mixed-orientation interlocking beats the 425-copy Pryanik baseline. The
   bounded search can still miss layouts; it samples two-state half-turn patterns.
   There is no neighborhood removal/reinsertion or equal-count pose exploration yet.
2. Failed raster operations can restart with larger work allowances. Completed
   preparation is cached; partially completed raster work is not resumable.
3. The L-prism pair remains unfound in the fixed benchmark. Heuristic stalls and
   budget exhaustion do not establish optimality; results remain `best_found`.
4. `rc/containers/10_kg_np.stl` is 400 x 350 x 285 mm; the real Nova Post 10 kg box
   is 400 x 340 x 285 mm. Use box dimensions, not that STL.

PRs #16 and #18 are merged. This change is stacked on open PR #22 (and its parents #19/#20/#21).
See [docs/ALGORITHM.md](../docs/ALGORITHM.md) for implemented behavior and limits,
[docs/BUILDING.md](../docs/BUILDING.md) for builds, and [AGENTS.md](../AGENTS.md)
for the workflow. Historical ticket/evidence documents remain frozen.
