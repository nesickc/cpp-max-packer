# Project status

Updated 2026-10-18. Details belong in PRs and `.local/` logs.

## What works

- **Engine (C++20, CPU):** STL import, authoritative solids, independent overlap,
  containment and pair/wall-clearance validation; mixed-orientation interlocking,
  grid/stagger/residual proposals, CPU FFT placement, JSON and checked STL export.
- **Desktop:** import, settings, Start/Stop, live count, shared-mesh viewer,
  portable Save/Open and export. Replacing an STL preserves unsaved settings and
  edits made during import; successful replacement clears the old asset's results.
- **Explicit repair:** bounded welding and approximate closed-surface reconstruction
  with before/after previews, diagnostics and acceptance/rejection. Original bytes,
  frames and versioned recipes survive Save/Open. Only fully validated candidates
  can be accepted; packing and export use that accepted solid.
- **Verified journey:** supplied Benchy has 43 nonmanifold vertices. Its 0.5 mm
  reconstruction is independently valid (129,248 triangles); the real Rust/native
  path accepts it, packs one copy, saves/opens it and exports checked STL.
- **Checks:** 630 active Release and 634 Debug cases, 45 Rust, 28 frontend and
  98 Python tests pass, with environment-sensitive reruns. Contract checks pass.
  Desktop rebuilt with a byte-matching fresh engine. Visual UI check remains
  unverified because computer-use approval and browser preview did not complete.

Not yet: Vulkan, desktop STL containers, upright/free-3D search, crash recovery
or installer. These remain **Later** in spec v0.3.

## Scoreboard

Release, eight threads; unchanged 10-second analytic and 60-second STL budgets.

| Case | Count | Floor | Target |
| --- | --- | --- | --- |
| cube64 | 64 | 64 | 64 |
| lprism2 | 1 | 1 | 2 |
| pryanik2 (1/1 mm) | **435** | 425 | 426 |
| pryanik2-0 (0/1 mm) | **447** | 416 | 416 |
| ulamok | 36 | 36 | 36 |

Pryanik uses the unchanged 400 x 340 x 285 mm box and 4 mm manual pitch.
History: [benchmarks/SCOREBOARD.md](../benchmarks/SCOREBOARD.md).

## Known problems and current goal

1. Supplied matrice has 6 nonmanifold edges and 1,255 nonmanifold vertices.
   Tested welding/reconstruction remains invalid. Acceptance, packing and export
   remain blocked; the intended thin features/topology need external repair.
2. Reconstruction is approximate: resolution can change bounds and erase small
   features, components or cavities. No sampled or certified surface-deviation
   bound is available. Fine grids can exceed memory/work limits; acceptance is
   always explicit and does not establish fidelity to the intended source shape.
3. The L-prism pair remains unfound; bounded interlocking can miss layouts.
   Results remain `best_found`. Improve packing quality without weakening validity.
4. `10_kg_np.stl` is 400 x 350 x 285 mm; the real Nova Post box is
   400 x 340 x 285 mm. Use box dimensions for that comparison.

PR #23 is merged; this work starts from main without a stacking dependency.
See [ALGORITHM.md](../docs/ALGORITHM.md), [BUILDING.md](../docs/BUILDING.md) and
[AGENTS.md](../AGENTS.md). Historical ticket/evidence documents remain frozen.
