# Project status

Updated 2026-10-18. Keep this to one page: what works, the current goal, known
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

`python tools/scoreboard.py` (Release, 8 threads), 2026-10-18 at `e9904ba`.
All cases freshly revalidated through native result loading:

| Case | Count | Floor | Target | Block layout | Termination |
| --- | --- | --- | --- | --- | --- |
| cube64 | 64 | 64 | 64 | 64 | budget_exhausted |
| lprism2 | 1 | 1 | 2 | 1 | search_stalled |
| pryanik2 (1/1 mm) | 340 | 340 | 350 | 343 | resource_limit |
| pryanik2-0 (0/1 mm) | 360 | 360 | 360 | 388 | resource_limit |
| ulamok | 36 | 36 | 36 | 36 | resource_limit |

"Block layout" is a simple bounding-box reference computed by the script, not a
bound. History: [benchmarks/SCOREBOARD.md](../benchmarks/SCOREBOARD.md).
Pryanik also retains 340/360 at 2 mm pitch with the same 60-second settings.

## Known problems (priority order)

1. **Search still ends early at resource limits.** At 4 mm, Pryanik stops at
   `FIELD_KERNEL_WORK_LIMIT`; at 2 mm, `SPECTRAL_MEMORY_LIMIT` stops spectral
   admission. The validated 340/360-copy incumbents survive, but time remains unused.
2. **Preparation leaves little actual search.** The 4 mm runs reach only four FFT
   correlations before the resource stop.
3. **The baseline is weaker than simple block layouts** (315 grid vs 343 blocks for
   pryanik2). The L-prism pair (`lprism2`) is not found in a 10 s timed run.
4. `rc/containers/10_kg_np.stl` is 400×350×285 mm; the real Nova Post 10 kg box is
   400×340×285 mm. Use box dimensions, not that STL.

## Current goal

Zero-clearance validation is fixed: 1 → 360 copies using existing generated layouts.
Next, use the remaining 60-second budget to improve the retained 340-copy layout
at 1 mm pair clearance, preserving manual pitch and physical constraints.

## Open PRs and branches

- [PR #16](https://github.com/nesickc/cpp-max-packer/pull/16) `feature/SOL-05-resource-recovery`:
  resource recovery (T015-P2), rebased onto main's merged process reset. Retains 340/36;
  both hosted Windows CI runs passed.
- `feature/zero-clearance-validation`, stacked on PR #16: exact contact and oblique
  separation at zero requested clearance; no new packing strategy or saved poses.

## Process

[AGENTS.md](../AGENTS.md) is the whole workflow. Spec v0.3 ([spec](spectrapack-spec.md))
splits Core v1.0 from Later work; algorithm details are in
[docs/ALGORITHM.md](../docs/ALGORITHM.md). Tickets `doc/T-*.md`, milestone and
evidence documents and decision records 0001–0019 are frozen history.
