# Work after T-008

Scoped 2026-09-30 from merged T-008 (`a05fa2c`, PR #10). This is a proposed
delivery order, not an implementation or performance claim. The user's first
priority is CPU packing speed, including multithreading, and support for the
supplied geometries. The reported Ulamok failure is a primary regression case.
Library-backed explicit STL repair follows the CPU work. Live result rendering
is lower priority; Vulkan follows useful and measured CPU parallelism.

## Evidence and problem boundaries

- T-008 delivers the first desktop journey, not full M3. Its remaining scope is
  recorded in [T-008](T-008.md) and [ADR 0010](../spec/decisions/0010-desktop-workflow.md).
- Both full Pryanik sources have completed native import/solid acceptance:
  `pryanik_1.STL` has 84,820 triangles; `pryanik_2.STL` has 139,212 triangles,
  including a cavity. Their reported desktop failures still need reproduction
  at the exact failing stage; native import success does not prove desktop
  prepare, search, project reload or export success.
- `pryanik_simplified.stl` has 3,796 triangles and 13 confirmed self-intersection
  pairs. [T-003](T-003.md) records an independent exact overlap check. Its rejection
  is correct for the current solid contract. It needs an explicit repair workflow;
  increasing a resource limit or ignoring intersections does not make it valid.
- The existing CPU budget covers search, while desktop preparation and terminal
  work add time. The viewer receives a terminal packing, not live incumbents.
  End-to-end timing, earlier useful results and actual compute cost are separate
  acceptance concerns.
- Source tracing finds repeated work: Rust Start runs `desktop-prepare` and then
  a separate `solve`; both reload/re-inspect the accepted source, and preparation
  regenerates the display LOD. See
  [Rust orchestration](../desktop/src-tauri/src/core.rs),
  [native preparation](../engine/cli/src/desktop.cpp),
  [solve](../engine/cli/src/solve.cpp) and
  [accepted-asset loading](../engine/pack_io/src/result_export.cpp).
  This is a concrete optimization candidate, not yet a measured phase breakdown.
- The retained T-008 manual practical run used a 10-second search setting and
  recorded 31.1 seconds total / 11.13 seconds search
  (`.local/t008/manual/journey.md`). This historical build observation motivates
  fresh Release measurement; it is not the new performance baseline.
- CPU proximity generation scans all environment cells for every cell: quadratic
  work in grid cell count. Its default 200-million-term cap excludes grids above
  14,142 cells (even 25 cubed is 15,625). Each axis also includes two halo cells.
  Candidate paging rebuilds fields/proximity. See
  [spectral pipeline](../engine/pack_solver/src/spectral_pipeline.cpp) and
  [solver limits](../engine/pack_solver/include/spectrapack/solver/spectral.hpp).
  This structural limit can reject a search workload for valid geometry; it has
  not been established as the cause of the user's particular failure.
- The user supplied a Ulamok screenshot with a 400×350×285 mm box, 0.1 mm pair
  clearance, 1 mm wall clearance, 24 cube rotations, manual 1 mm pitch and a
  600-second search budget. It shows `SOLVE_FAILED`, a retained native-valid
  result with termination `error`, 122.79 seconds search and 142.8 seconds total.
  It demonstrates a failed run, not exhausted time or proof that more copies
  cannot fit. The generic UI message omits the native cause; preserve and surface
  that cause in the next regression. The count is clipped in the screenshot.
- The matching local desktop artifact confirms **17 valid copies**, 28.8447%
  utilization, 122.792046 seconds search, 51 candidate evaluations and one
  completed pass; all retained poses carry baseline IDs. It was produced by
  `8f24835c...-dirty`. Evidence is the `solution/result.json` under local desktop
  session `session-fdeb7179607f41888b75cabb291b3639`, operation
  `operation-0253b520e025418eacfe4160c483d7c9`. That operation does not persist the
  native failure diagnostic. [App.tsx](../desktop/src/App.tsx) displays the error
  message but omits its structured details, so the first failing stage is still
  unresolved. Preserve diagnostic code/details in both UI and run evidence.
- The 36-copy target is supported independently by the accepted Ulamok bounds.
  A +90-degree X cube rotation gives approximately 90.46015×105.50744×90.46014 mm.
  A 4×3×3 arrangement occupies 362.14059×316.72232×271.58043 mm with 0.1 mm
  between bounding boxes. Adding 1 mm on each wall gives
  364.14059×318.72232×273.58043 mm, below 400×350×285 mm on every axis.
  Disjoint enclosing boxes give a geometric lower-bound witness; this arithmetic
  is not an executed native solver/validator result and does not claim optimality.
- At 1 mm pitch this box requires 402×352×287 = 40,611,648 environment cells,
  exceeding the current 16,777,216-cell representation cap before container-field
  allocation. The pipeline wraps that nested field failure as
  `SPECTRAL_PIPELINE_MASK`, which becomes a generic `error`. This source-grounded
  route is compatible with the observed failure, but does not prove it happened
  before any baseline/validation failure. Even raising the cell cap is insufficient:
  mask, reference counts, occupancy and double proximity alone require
  568,563,072 bytes, above the current 512 MiB limit before geometry/kernel/FFT;
  quadratic proximity would require approximately 1.649×10^15 terms.
- CPU FFT threading and vector paths are explicitly disabled in
  [correlation.cpp](../engine/pack_compute/src/correlation.cpp); the current
  audited workspace bounds depend on those modes. CPU multithreading needs an
  implemented thread/resource policy and measured scaling, not just a UI toggle.

Fresh bounded probes of the local staged Release engine completed as follows:

| Source | Native inspection | Native `desktop-prepare` |
| --- | --- | --- |
| `pryanik_1.STL` | Valid, 14.976 s | Success, 14.849 s |
| `pryanik_2.STL` | Valid, 28.599 s | Success, 30.338 s |
| `pryanik_simplified.stl` | Invalid, 1.410 s; 13 intersections | Ineligible; not run |

Both full sources produced 20,000-triangle previews without warnings; Pryanik 2
retained its cavity. These are single informational samples, not a performance
baseline or a full desktop/solve qualification. The staged executable identifies
`8f24835...-dirty`, so they are not new builds of merged `main`. Commands, exact
request/settings, executable hash and reports are retained in
`.local/qa01/pryanik-diagnosis/summary.json`. The reported full-Pryanik rejection
was not reproduced at these two boundaries; Start/solve and the actual GUI path
remain to be exercised.

The source hashes and accepted outcomes in
[import-expectations.json](../tests/fixtures/import-expectations.json) remain the
baseline. Preserve source bytes, physical dimensions, cavity semantics, separate
clearances and full-resolution validation/export throughout this work.

## Implementation specifications

The formal [delivery milestones](MILESTONES.md) replace the earlier provisional
scope list. All tickets below are planned; source changes, red/green tests and
performance/repair qualification remain future work.
The user-approved delivery split is T-009, T-011 foundations, T-010 threading,
then T-012. See [ADR 0014](../spec/decisions/0014-runtime-delivery-split.md) and
the milestone execution rules for separate PRs and prerequisite user merges.

| Priority | Specifications |
| --- | --- |
| CPU correctness and speed | [T-009 practical CPU scalability](T-009.md), then [T-011 runtime foundations](T-011.md), then [T-010 threading](T-010.md) |
| Geometry workflows | [T-012 explicit library repair](T-012.md), [T-013 STL containers](T-013.md) |
| Packing quality and controls | [T-014 orientations/refinement](T-014.md), [T-015 count search](T-015.md), [T-016 presets/resources](T-016.md) |
| Durable jobs | [T-017 CLI/service/Continue](T-017.md), [T-018 checkpoints/recovery](T-018.md) |
| Radeon acceleration | [T-019 Vulkan compute](T-019.md), [T-020 Auto fallback](T-020.md) |
| Deferred live viewer | [T-021 live revisions/inspection](T-021.md) |
| Qualification and release | [T-022 benchmark evidence](T-022.md), [T-023 offline distribution](T-023.md) |

[ADR 0011](../spec/decisions/0011-cpu-first-follow-up.md) records the CPU-first
priority, Ulamok lower-bound gate and explicit repaired-solid boundary.
[DELIVERY.md](DELIVERY.md) remains the branch/review/merge policy. Preserve the
historical limits above: the saved run does not identify its first failure,
the 36-copy witness is analytical, and the staged Pryanik probes are not a full
native desktop qualification of the merged source.
