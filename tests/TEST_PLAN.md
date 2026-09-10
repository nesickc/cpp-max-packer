# Spec-linked test plan

The product acceptance cases below are **planned**, not passing tests. Executable foundation checks are listed in [README.md](README.md); they do not establish engine correctness. Read the canonical [spec §10](../doc/spectrapack-spec.md#10-acceptance-tests) for the full gates.

## Test-first work order

0. QA-01: native/Python test runners, small analytic fixture builders, an independent direct integer correlation oracle, and bounded reference timing reports. Merged through PR #1; see [project status](../doc/PROJECT_STATUS.md).
1. M0: merged [T-001](../doc/T-001.md) pins the CPU build and checks actual dependency calls, toolchain/source drift, static runtime flags and staged DLL dependencies. [T-002](../doc/T-002.md) adds schema examples and CLI/stdio envelopes, error codes and request identity. Settle the contracts before backend/frontend implementation diverges.
2. M1: STL content detection, units/source frames, diagnostics/accepted repairs, independent solid overlap/containment/clearance, and display isolation. Start with analytic small solids; then run every applicable `rc/` asset through the same public boundary.
3. M2: physical AABB baseline, conservative fields, direct integer correlation oracle, CPU FFT, and validated result/export round trips.
4. M3–M6: lifecycle and UI, count/orientation search, actual Radeon compute/failure recovery, and installer/release evidence.

For each slice, name its requirement and AT case, add a failing observable test, record the intended failure, implement, and record the passing command. Refactor only while preserving those gates. Use fixed work and seeds for deterministic regressions. Do not create an all-green collection of skipped acceptance placeholders.

## Practical cases and independent checks

T-002's bounded contract coverage is separate from full AT-13/AT-14. Its checks
cover native/Ajv agreement for structural fixtures, semantic mutations and source
frame/pose goldens, generated TypeScript freshness and negative compile fixtures,
immutable asset registry handles, malformed/oversized/truncated NDJSON, replay and
conflicting request IDs, cache exhaustion, unknown/unsupported methods, and real
CLI/pipe behavior. Fixed-work validation/dispatch timings are informational; there
is no solver or stop-latency performance claim. Job lifecycle, physical validation
and moved-project/export round trips remain required by the full gates below.

| Gate | Concrete fixture/action and observable result | Tier / first milestone |
| --- | --- | --- |
| AT-01 | Offline clean Windows 10/11 install; CPU import → solve → save/reopen → JSON/STL through UI and CLI; inspect loaded dependencies | Installation / M6 |
| AT-02 | Pinned `benchy_small` on actual Radeon and forced CPU; absent loader/device gives Auto→CPU and explicit Vulkan→`GPU_UNAVAILABLE` | Hardware integration / M5 |
| AT-03 | Equivalent generated ASCII/binary cubes, `solid` binary header, Unicode path, negative origin, 1-inch cube→25.4 mm; malformed/nonfinite files reject. Add all ten `rc/` import regressions | Geometry unit + import integration / M1 |
| AT-04 | Generated open, duplicate, reversed, seam, non-manifold, self-intersecting, and ambiguous-shell cases; explicit repair acceptance and unchanged source hashes. User assets must diagnose actual defects | Geometry / M1 |
| AT-05 | Generated million-triangle solid and thin fin; vary display LOD while hashes, fields, validation, and deterministic poses remain unchanged; resource preflight rejects oversize jobs gracefully | Geometry/resource integration / M1–M5 |
| AT-06 | Two cubes: separation, contact, tiny overlap, coincidence, full enclosure; hollow solid with legitimate cavity; undersized proxy false pass must reject; `indeterminate` cannot publish | Validator unit/integration / M1 |
| AT-07 | Every box wall with fractional pitch; U-volume bridge crossing empty space; object enclosing excluded cavity; prescribed concave valid pose. Add user-container solid containment once import is accepted | Validator / M1 |
| AT-08 | `tilted_bar`: cube catalog zero, Z45/free at least one; upright preserves +Z; reject zero/NaN quaternions and deduplicate q/−q; no discrete-mode drift | Geometry/solver / M1, M4 |
| AT-09 | `cube_clearance`: 10 mm cubes, 45³ mm box, 1 mm gaps; test `c−10ε`, `c`, `c+10ε`, large translated coordinates and tiny features with independent distances | Validator / M1 |
| AT-10 | `cube_exact`: 10 mm cube, 40³ mm box, zero gaps→64 validated/exported placements; too-large item→valid empty `best_found`; monotone best count and fixed-volume utilization | Solver + export / M2 |
| AT-11 | Two unit-cube L prisms in 3×2×1 mm; AABB baseline one, improved layout two. Start one L offset +0.5 X, disable fresh restarts, 10,000 candidates/100 passes, 0.25 mm pitch; overlapping blocked cells survive removal | Solver / M4 |
| AT-12 | Direct integer cross-correlation at every small asymmetric translation; non-power-of-two padding/origins/boundaries; FFT error <0.25 and equal rounding; bad normalization/fault triggers rejection/fallback | Compute unit / M2 CPU, M5 GPU |
| AT-13 | Duplicate start requests, stop/continue/query; kill during a trial/checkpoint replacement; drop progress events/restart UI; retain last complete validated state | Service/process integration / M3 |
| AT-14 | Nontrivial source frames; save/move/reopen; JSON/viewer/STL coordinates agree and count matches poses; tamper hashes/matrices/archives, traversal and STL quantization faults reject | Data/export integration / M2–M3 |
| AT-15 | Both box and user STL container workflows; stale events, selection, clipping, hidden-copy count, keyboard and recovery; recorded 1,000-copy viewport frame-time gate on qualified hardware | Desktop end-to-end / M3, M6 performance |
| AT-16 | Fault-injected host/device allocation, device loss, blocked native operation and full disk; preserve valid incumbent; Auto CPU recovery; Stop acknowledgement ≤250 ms | Resource/process integration / M5 |
| AT-17 | Pinned official Benchy plus analytic suites; validate every saved pose/export independently; retain failed seeds and record counts, poses, settings, hardware and timing | Benchmark/release / M6 |

## User-provided assets

`rc/` contains six container STLs and four item STLs. Their coordinates are millimeters and their intended container semantics are usable interior volumes, confirmed by the user. [rc-manifest.json](fixtures/rc-manifest.json) pins the actual files and inspection evidence. It does not mark authoritative-solid validation as passed.

- Unit tests use small analytic/generated geometry for exact contact, overlap, transforms, padding, and known counts. Heavy user assets belong in tagged integration/regression tests; do not reread all meshes on every tiny unit check.
- Run all assets through import/diagnostics. Once accepted as valid, exercise each container with each item in a bounded fixed-work smoke matrix, independently validating every published pose and export. Do not invent expected absolute counts for irregular shapes or assert a nonempty result unless a known valid placement establishes it.
- For each accepted pair, record asset hashes, units, allowed rotations, separate clearances, pitch, work budget/seed, backend, and actual accepted geometry. A rejected input is a diagnostic case, not a packing success.
- `pryanik_simplified.stl` and `ulamok_2kg_simplified.stl` are distinct supplied source files; their names do not establish display-LOD relationships. Keep originals unchanged and derivative fixtures separate.
- Local test use is user-authorized. Public redistribution provenance remains unspecified. These files complement the pinned official Benchy requirement and do not replace it.

Run targeted unit tests first, affected integration tests next, and hardware/installer/long benchmarks at their explicit gates. Preserve failing cases and meaningful logs; route routine log summaries to Luna. Change a baseline only with evidence and an explicit record, never to hide a regression.
