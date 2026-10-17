# Project status

Updated 2026-10-17. Keep this to one page: what works, the current goal, known
problems and the latest scoreboard. Details belong in PRs and `.local/` logs.

## What works

- **Engine (C++20, CPU):** STL import with diagnostics and safe cleanup; accepted
  full-resolution solids; independent validator (overlap, containment, pair/wall
  clearance); AABB grid baseline, staggered-row seed and CPU spectral (FFT)
  placement with cube/custom rotations; multithreaded CPU search; JSON result and
  validated binary STL export. CLI: `inspect`, `solve`, `capabilities`, `serve --stdio`.
- **Desktop (Tauri/React/Three.js):** import, box settings, Start/Stop, live count,
  shared-mesh viewer, Save/Open project, export.
- **Build/tests:** pinned Windows CPU build ([docs/BUILDING.md](../docs/BUILDING.md)),
  native Catch2/CTest suites, Python tool tests, hosted Windows CI.

Not yet: Vulkan, STL containers, upright/free-3D rotations, library repair, crash
recovery, installer. These are mostly **Later** in spec v0.3.

## Scoreboard

`python tools/scoreboard.py` (Release build, 8 threads). First run 2026-10-17 with
the engine built from PR #16 (`feature/SOL-05-resource-recovery`):

| Case | Count | Floor | Target | Block layout | Termination |
| --- | --- | --- | --- | --- | --- |
| cube64 | 64 | 64 | 64 | 64 | budget_exhausted |
| lprism2 | 1 | 1 | 2 | 1 | search_stalled |
| pryanik2 (1/1 mm) | 340 | 340 | 350 | 343 | resource_limit |
| pryanik2-0 (0/1 mm) | 1 | 1 | 350 | 388 | resource_limit |
| ulamok | 36 | 36 | 36 | 36 | resource_limit |

"Block layout" is a simple bounding-box reference computed by the script, not a
bound. History: [benchmarks/SCOREBOARD.md](../benchmarks/SCOREBOARD.md).

## Known problems (priority order)

1. **Zero pair clearance is effectively broken for real models.** Validating touching
   Pryanik copies exhausts the validator's kernel-work cap after ~33 candidates; the
   run returns 1 copy (`PHYSICAL_VALIDATION_RESOURCE`). Hand packing reaches 350.
2. **Most of the time budget goes to preparation.** Pryanik-2 at 4 mm: ~27 s
   preparing, ~12 s voxelizing, ~7 s validating of a 60 s Start; search itself gets
   seconds before stopping at `FIELD_KERNEL_WORK_LIMIT`.
3. **Search ends instead of degrading** at resource limits (spec v0.3 §6.6 now asks
   for degrade-and-continue). Pryanik 2 mm fails memory admission outright.
4. **The baseline is weaker than simple block layouts** (315 grid vs 343 blocks for
   pryanik2). The L-prism pair (`lprism2`) is not found in a 10 s timed run.
5. `rc/containers/10_kg_np.stl` is 400×350×285 mm; the real Nova Post 10 kg box is
   400×340×285 mm. Use box dimensions, not that STL.

## Current goal

Raise the Pryanik scoreboard counts, starting with problem 1, then 2–4. Each step is
one short PR with a scoreboard result.

## Open PRs and branches

- [PR #16](https://github.com/nesickc/cpp-max-packer/pull/16) `feature/SOL-05-resource-recovery`:
  resource recovery (T015-P2). Retains 340/36; no count improvement. Local branch also
  has an unpushed WIP commit `3b76463` with agent-configuration edits superseded by
  this reset.
- `feature/process-reset`: this workflow reset (slim AGENTS.md, spec v0.3, scoreboard).

## Process

[AGENTS.md](../AGENTS.md) is the whole workflow. Spec v0.3 ([spec](spectrapack-spec.md))
splits Core v1.0 from Later work; algorithm details are in
[docs/ALGORITHM.md](../docs/ALGORITHM.md). Tickets `doc/T-*.md`, milestone and
evidence documents and decision records 0001–0019 are frozen history.
