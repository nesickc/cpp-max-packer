# Project status

Updated 2026-10-18. Details belong in PRs and `.local/` logs.

## What works

- **Engine (C++20, CPU):** STL import and cleanup; authoritative solids;
  independent overlap, containment and pair/wall-clearance validation; grid,
  stagger, residual-block and mesh-envelope interlocking proposals; CPU FFT
  placement with permitted cube/custom rotations; JSON results and validated STL export.
- **Desktop (Tauri/React/Three.js):** import, box settings, Start/Stop, live count,
  shared-mesh viewer, Save/Open project and export.
- **Timed search:** CLI inputs without a resolved resource policy now use the
  desktop RAM-share policy and bounded work recovery. Manual pitch, explicit
  memory caps, Stop and deadlines remain enforced; fixed-work behavior is unchanged.
- **Interlocking rows:** timed search derives row/column offsets from the accepted
  mesh; complete layouts still require independent full-resolution validation.
- **Verification:** 623 Release tests, focused Debug stagger tests, desktop
  tests/build, contract checks and independent review passed.

Not yet: Vulkan, STL containers, upright/free-3D rotations, library repair, crash
recovery or installer. These are mostly **Later** in spec v0.3.

## Scoreboard

`python tools/scoreboard.py --record` (Release, 8 threads), 2026-10-18 at `bee20f2`.
Budgets are unchanged: 10 seconds for the analytic fixtures, 60 seconds for the STL cases.

| Case | Count | Floor | Target | Termination |
| --- | --- | --- | --- | --- |
| cube64 | 64 | 64 | 64 | budget_exhausted |
| lprism2 | 1 | 1 | 2 | search_stalled |
| pryanik2 (1/1 mm) | **350** | 340 | 350 | budget_exhausted |
| pryanik2-0 (0/1 mm) | 360 | 360 | 360 | budget_exhausted |
| ulamok | 36 | 36 | 36 | budget_exhausted |

Pryanik uses the unchanged 400 x 340 x 285 mm box and 4 mm manual pitch.
The positive-clearance layout improves from 343 to 350 in 6.6 seconds. Separate
2 mm runs reach 350/360. All five scoreboard layouts and both 2 mm results passed
fresh native result-loading validation.
The zero-clearance 388-copy block reference remains unvalidated, not a bound.
History: [benchmarks/SCOREBOARD.md](../benchmarks/SCOREBOARD.md).

## Known problems and current goal

1. Improve beyond 350 valid Pryanik copies. Interlocking rows now use clipped mesh
   slices, but the envelope/search is bounded and larger proposals can consume the
   remaining time in full validation. The controller still has no neighborhood
   removal/reinsertion or equal-count pose exploration.
2. Failed raster operations can restart with larger work allowances. Completed
   preparation is cached; partially completed raster work is not resumable.
3. The L-prism pair remains unfound in the fixed benchmark. Heuristic stalls and
   budget exhaustion do not establish optimality; results remain `best_found`.
4. `rc/containers/10_kg_np.stl` is 400 x 350 x 285 mm; the real Nova Post 10 kg box
   is 400 x 340 x 285 mm. Use box dimensions, not that STL.

PRs #16 and #18 are merged. This change is stacked on open PRs #19 and #20.
See [docs/ALGORITHM.md](../docs/ALGORITHM.md) for implemented behavior and limits,
[docs/BUILDING.md](../docs/BUILDING.md) for builds, and [AGENTS.md](../AGENTS.md)
for the workflow. Historical ticket/evidence documents remain frozen.
