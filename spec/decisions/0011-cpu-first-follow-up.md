# ADR 0011: CPU-first follow-up and explicit reconstruction

Status: accepted for planning, 2026-09-30. No implementation, library selection,
schema migration or new qualification is implied.

Requirements: SOL-01/SOL-02/SOL-05–SOL-08, GEO-03/GEO-04/GEO-06,
UI-01/UI-03, DATA-01–DATA-03 and QA-01/QA-02.

## Context

T-008 is merged at `a05fa2c`. The user prioritizes practical CPU packing speed,
multithreading and repair over live result rendering and GPU compute. The saved
Ulamok run retained 17 valid copies before an unspecified internal failure; an
independent rotated-box construction establishes room for at least 36. Current
grid limits, quadratic proximity work and repeated asset preparation are known
constraints, not proof of the first error in that saved run. The two full
Pryaniks passed bounded native import/preparation probes; the simplified source
has independently confirmed self-intersections. See
[evidence](../../doc/NEXT_WORK.md) for provenance and limits.

## Decision

1. Deliver CPU scalability/correctness, bounded CPU multithreading and preparation
   reuse/deadlines before Vulkan. Add SOL-08 to make useful CPU parallelism a v1
   obligation. Preserve SOL-07's deterministic same-configuration contract.
   Thread count is part of resolved configuration; identical poses across
   different thread counts are not required. All outcomes remain independently
   validated and all worker memory is included in preflight.
2. Add the Ulamok 36-copy lower bound to the practical acceptance matrix. Keep
   the physical baseline independent of voxel pitch. Unsupported manual grids
   must fail early with specific resource diagnostics and a supported alternative;
   early rejection alone does not satisfy the successful packing gate.
3. Require a bounded library-backed repair route for the pinned simplified
   Pryanik, with explicit preview/acceptance, a separately retained repaired mesh
   and independently validated packing/export. Broader malformed inputs may
   still be rejected with actionable diagnostics. Reconstruction is an explicit
   extension of GEO-03, not permission for arbitrary automatic shape changes.
4. Repair resolution is independent of packing pitch. After acceptance, the
   reconstructed solid is the authoritative geometry for that asset. Preserve
   source bytes and transformation history; search/display changes cannot alter
   the accepted repair. Library success, voxel occupancy and preview appearance
   do not replace the solid validator. A library such as OpenVDB is a candidate,
   not a chosen or qualified dependency in this decision.
5. Defer live incumbent rendering until after CPU, repair and core packing work.
   Basic phase/error feedback and usable Stop remain earlier requirements.
   Keep job lifecycle, durable recovery and live viewer delivery separately
   reviewable. The full-v1 requirement for live validated state is not removed.
6. Revised by user-approved [ADR 0014](0014-runtime-delivery-split.md) on 2026-10-02:
   deliver T-009, T-011 serial runtime foundations, T-010 measured threading, then
   T-012. The former combined T-010/T-011 delivery is split; every acceptance ID
   and performance target remains. T-011 carries the remaining T-009 practical
   obligations and supplies the successful serial baseline. Dependent branches
   wait for user merges. One primary, at most two leaf workers and one branch/PR
   per delivery remain the limits; no automatic merge or stacked branch.

The [delivery milestones](../../doc/MILESTONES.md) and individual T-009–T-023
tickets define order, technical prerequisites and observable acceptance. Existing
M0–M6 and AT-01–AT-17 identities remain unchanged.

## Compatibility and design gates

- This change is to the product plan/specification only. Existing binaries,
  settings, project files, result semantics and capability claims remain as-is.
- T-009 preserves nested failure codes and retained valid results. Freeze any
  public diagnostic extension before changing schema/adapter behavior.
- T-011 and subsequent T-010 preserve a shared design for thread ownership, scratch
  accounting, deterministic admission, immutable prepared assets and Start-origin
  deadlines before parallel implementation. Cached unverified files cannot
  authorize geometry. Shared CLI/I/O/desktop adapters have one integration owner.
- T-012 needs a separate accepted-repair contract decision before implementation:
  recipe/library version, input/output hashes, acceptance evidence, replay or
  verification policy, legacy weld/project loading and unsupported-version errors.
  Do not force every old project to reconstruct its original source through a
  newly selected library. Do not silently rewrite source, geometry or units.
- Later service/checkpoint/backend tickets freeze their interfaces before parallel
  edits. Protocol/settings/project version changes require explicit migration
  assessment and generated types; this ADR does not preselect wire fields or ABI.

## Consequences and verification

More cores do not solve quadratic work or fixed memory limits. Performance tickets
must retain source-bound fixed-work count/pose checks, phase timings and actual
end-to-end measurements. Set numerical improvement targets from a retained
baseline before optimizing; do not choose them retrospectively from a result.
Unsupported cases, failed seeds and overruns remain visible.
For the combined runtime ticket, compare threading with preparation state fixed
and preparation reuse at a fixed thread count, then report combined performance.
Serialize measurement workloads. One integrated ledger/review may cover both
acceptance sheets; no gate is removed by grouping and no agent speedup is assumed.

The repaired simplified Pryanik must be demonstrated rather than assumed; failure
to establish a valid derivative leaves that acceptance gate open. Geometry,
cavity and clearance rules are unchanged. Documentation validation does not
establish any of these product gates.
