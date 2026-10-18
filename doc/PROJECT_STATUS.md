# Project status

Updated 2026-10-18. Details belong in PRs and `.local/` logs.

## What works

- **Engine (C++20, CPU):** STL import and cleanup; authoritative solids;
  independent overlap, containment and pair/wall-clearance validation; grid,
  stagger and residual-block layout proposals; CPU FFT placement with permitted
  cube/custom rotations; JSON results and validated binary STL export.
- **Desktop (Tauri/React/Three.js):** import, box settings, Start/Stop, live count,
  shared-mesh viewer, Save/Open project and export.
- **Timed search:** CLI inputs without a resolved resource policy now use the
  desktop RAM-share policy and bounded work recovery. Manual pitch, explicit
  memory caps, Stop and deadlines remain enforced; fixed-work behavior is unchanged.
- **Verification:** Release and Debug native suites, Python tools, contract
  fixtures, desktop tests/build and independent review passed for this change.

Not yet: Vulkan, STL containers, upright/free-3D rotations, library repair, crash
recovery or installer. These are mostly **Later** in spec v0.3.

## Scoreboard

`python tools/scoreboard.py --record` (Release, 8 threads), 2026-10-18 at `f8e16fd`.
All five saved layouts freshly passed native result-loading validation. Budgets
are unchanged: 10 seconds for the analytic fixtures, 60 seconds for the STL cases.

| Case | Count | Floor | Target | Termination |
| --- | --- | --- | --- | --- |
| cube64 | 64 | 64 | 64 | budget_exhausted |
| lprism2 | 1 | 1 | 2 | search_stalled |
| pryanik2 (1/1 mm) | **343** | 340 | 350 | budget_exhausted |
| pryanik2-0 (0/1 mm) | 360 | 360 | 360 | budget_exhausted |
| ulamok | 36 | 36 | 36 | budget_exhausted |

Pryanik uses the unchanged 400 x 340 x 285 mm box and 4 mm manual pitch.
The positive-clearance layout improved from 340 to 343 in 4.4 seconds. Separate
60-second runs at 2 mm also retain 343/360 and pass fresh native validation.
Explicit 180/300-second runs at 4 mm and 180 seconds at 2 mm retain 343, evaluate
more FFT candidates and pass fresh validation. The zero-clearance 388-copy block
reference remains unvalidated, not a bound.
History: [benchmarks/SCOREBOARD.md](../benchmarks/SCOREBOARD.md).

## Known problems and current goal

1. Find more than 343 valid Pryanik copies. Residual blocks rearrange complete
   layouts, but the spectral controller still starts with greedy empty-layout
   insertion, then tries the retained layout. It has no neighborhood
   removal/reinsertion or equal-count pose exploration.
2. Failed raster operations can restart with larger work allowances. Completed
   preparation is cached; partially completed raster work is not resumable.
3. The L-prism pair remains unfound in the fixed benchmark. Heuristic stalls and
   budget exhaustion do not establish optimality; results remain `best_found`.
4. `rc/containers/10_kg_np.stl` is 400 x 350 x 285 mm; the real Nova Post 10 kg box
   is 400 x 340 x 285 mm. Use box dimensions, not that STL.

PRs #16 and #18 are merged. This continuation is based on updated `main`.
See [docs/ALGORITHM.md](../docs/ALGORITHM.md) for implemented behavior and limits,
[docs/BUILDING.md](../docs/BUILDING.md) for builds, and [AGENTS.md](../AGENTS.md)
for the workflow. Historical ticket/evidence documents remain frozen.
