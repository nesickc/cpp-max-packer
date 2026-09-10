# Initial architecture

Design baseline, 2026-09-08; no product implementation or qualification implied. [The specification](spectrapack-spec.md) is authoritative; [ADR 0001](../spec/decisions/0001-initial-architecture.md) records decisions and open choices.

## Ownership and dependencies

Dependencies point inward: desktop → engine protocol; engine → solver + I/O; solver → geometry + compute; I/O → geometry and shared contract values. Geometry and compute are independent of each other, search policy, I/O, and UI. Small dependency-free value headers hold IDs, physical poses, constraints, and reports; they are not another service/framework.

| Owner | Responsibility and owned data |
| --- | --- |
| `pack_geometry` | Import/repair diagnostics, immutable accepted solids, frame conversion, BVHs, conservative voxelization, independent validation, display LOD generation. Hide Manifold/FCL/meshoptimizer behind adapters. |
| `pack_compute` | Array correlation, FFT plans, reductions, capability/error checks, bounded backend buffers; pocketfft CPU and VkFFT/Vulkan implementations. No placement validity decision. |
| `pack_solver` | Catalogs, candidate ranking, trial layouts, count controller, RNG/work budgets and caches. Emit snapshots through callbacks; never write project files. |
| `pack_io` | Schema decoding/encoding, asset hashes, portable projects, atomic checkpoints and streamed export. No search policy. |
| `spectrapack-engine` | Asset registry, job lifecycle, input/preflight checks, one active solver, request deduplication, immutable revision publication and I/O coordination. |
| Rust / React | Rust owns scoped files and fixed sidecar lifecycle; React/Three.js renders engine state using one shared display mesh and per-copy transforms. |

The registry retains source bytes, accepted meshes, repair records and their separate hashes. Jobs share immutable accepted assets and resolved constraints; trials own poses and temporary search data. LODs and voxel/FFT caches are disposable derivatives keyed by relevant geometry, constraints, grid and backend identity. Changing display LOD cannot affect search, validation or export (`GEO-03`, `GEO-04`).

## Public boundaries

These are contract sketches, not committed ABI signatures:

```text
inspect_stl(bytes, explicit_units) -> AssetDraft + ImportReport
accept_asset(draft, repair_acceptance_if_required) -> AcceptedAsset
validate(poses, accepted_assets, constraints) -> ValidationReport
correlate(blocked_field, object_kernel, grid_spec) -> Field + NumericReport
run(job_inputs, budget, stop_token, snapshot_sink) -> RunOutcome
save/export(immutable_result, accepted_assets, destination) -> ArtifactReport
```

`grid_spec` explicitly carries physical pitch, axis/storage order, world origin, trimmed kernel origin, padding and legal translation bounds. Fix indexing examples before adapting FFT APIs. Geometry owns physical/grid conversion; compute consumes explicit array layouts. Solver shortlists FFT proposals, performs discrete checks, then invokes authoritative validation; the bounded physical refinement path still permits contact missed by voxels (`SOL-02`).

## Contracts, validity and state

[ADR 0004](../spec/decisions/0004-versioned-contracts.md) freezes the initial
schema/stdio implementation. `pack_io` consumes embedded Draft-07 schemas and
checks serialization semantics without geometry dependencies. `pack_service`
owns framing, dispatch and replay; `spectrapack-engine` is its CLI adapter.
Contract-valid metadata does not establish accepted physical geometry.

`spec/schemas/` is the single source for versioned wire/persistence shapes and generated TypeScript types. C++ CLI and service use one decoder and semantic-validation path. The planned `pack_geometry` module will supply the canonical physical transform implementation and golden vectors shared with viewer/export tests: millimeters, right-handed Z-up, active normalized XYZW quaternion plus translation; matrices are derived and checked. Preserve source mappings; never scale to fit (`GEO-02`, `DATA-01`, `DATA-03`).

Validation returns `valid`, `invalid` or `indeterminate`, with copy IDs and diagnostics. Only `valid` can enter incumbent publication. Validate accepted solids, including enclosure/coincidence, STL-volume difference and separate pair/wall distances; tolerance never reduces clearance. Restore/final/export validation bypasses search certificates. Re-read quantized STL coordinates; retain valid JSON/project if STL export fails (`GEO-05`, `GEO-06`).

Keep structured operational errors separate from validation verdicts. Preserve §8.1 error codes/exit codes and protocol-only stdout. Lifecycle follows `created → preparing → running → stopping → finished`, with natural completion and failure transitions. Stop acknowledges promptly; safe-boundary completion retains the last validated checkpoint. Revisioned events refer to immutable results. Continue records a new segment; incompatible search state resets explicitly.

Count dominates all secondary scores; trials may shrink, published best count may not. A valid zero-copy result is explicit. Results say `best_found`; exhausted search never proves impossibility or optimality (`SOL-01`).

## TDD sequence and seams

1. **M0:** pin the real Windows CPU toolchain/dependencies; start with failing schema examples, malformed envelopes, duplicate-start, version and transform-consistency cases. Implement the smallest CLI/service handshake and shared semantic decoder. Use fake compute/job clocks and a publication sink for deterministic lifecycle tests. Adapter smoke tests establish linkage only.
2. **M1 import:** write analytic ASCII/binary and unit/frame goldens before parser/acceptance code (`AT-03`, `AT-04`); retain invalid inputs and explicit repair-acceptance cases.
3. **M1 validation:** use independent analytic expectations for overlap/enclosure, all box walls, concavity, cavities, clearances and uncertainty (`AT-06`, `AT-07`, `AT-09`). Add allowed-orientation validation from `AT-08`; solver search success follows later. Gate the geometry adapter here, then test LOD isolation (`AT-05`).
4. **M2 preparation:** retain a direct integer correlation oracle independent of FFT adapters; test every translation of asymmetric tiny arrays before production spectral placement (`AT-12`).

User-supplied `rc/` assets use millimeters; container files intend whole closed usable volumes. These facts do not establish valid topology. Pin hashes/provenance and import diagnostics; treat simplified item files as separate assets, not automatic authoritative replacements. Real fixtures supplement analytic oracles.
