# ADR 0001: Initial architecture

Date: 2026-09-08. Status: accepted design baseline; implementation pending.

## Decisions

Retain the [specification](../../doc/spectrapack-spec.md), requirement IDs and [architecture boundaries](../../doc/ARCHITECTURE.md): C++20 engine; Rust sidecar integration; React presentation; pocketfft CPU and VkFFT/Vulkan compute, without CUDA/HIP/ROCm. CPU operation must not initialize or require Vulkan.

Use spectral proposals beneath a maximum-count controller. Immutable accepted solids authorize results through independent three-state validation. Physical dimensions, orientation policy, pair/wall clearances and STL interior-volume meaning remain fixed. JSON Schemas own external shapes; canonical poses and source mappings own transforms. Search, display and backend caches cannot redefine geometry. Preserve atomic validated incumbents, including zero copies; report `best_found`.

For supplied `rc/` files, the user confirmed millimeters and containers representing closed usable volumes; acceptance still requires geometry validation. TDD begins with contract and analytic failures before implementation, followed by independent correlation oracles.

## Open choices and consequences

M0 must verify/pin actual dependency revisions, select the C++ test framework and schema/type-generation tooling, and finalize grid indexing examples. M1 must establish Manifold/FCL ambiguity handling against adversarial gates. Fixture provenance and which supplied item variants to benchmark remain to be recorded. No GPU qualification or performance claim exists.

Compatibility: this records the existing pre-build semantics and ownership, without changing schemas or migrating files. Future semantic changes require their own decision and compatibility assessment. Keep the canonical spec in `doc/`; `spec/` initially contains decisions and, when implemented, schemas.
