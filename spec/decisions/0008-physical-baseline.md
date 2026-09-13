# ADR 0008: physical AABB baseline and immutable incumbent

Date: 2026-09-13. Status: accepted for T-006 implementation.
Requirements: SOL-01, SOL-02, QA-01; spec §§6.1–6.2 and the native portions of
AT-10. Prerequisite: T-005 merged at `02f62d0`.

## Boundary and compatibility

Add `pack_solver` depending on the public `pack_geometry` API. Geometry owns
bounded numerical bounds/grid queries; solver owns orientation seeds, candidate
order, budgets, score and incumbent policy. Neither owns files or service jobs.
Use the existing immutable `ValidationContext`, `Candidate` and unforgeable
`ValidatedSolution` handles. No private validator headers enter solver code.

The native result and qualification payload are diagnostic interfaces, not new
versions of the product JSON schemas. T-007 retains product transform/export
round trips and CPU FFT; service lifecycle and desktop remain later work. Existing
direct integer correlation test helpers remain independent and are reused.
This adds behavior without changing accepted-solid, quaternion or clearance
semantics. It does not complete AT-10, M2 or the full orientation search contract.

## Numerical geometry queries

`geometry/physical_bounds.hpp` exposes value-or-`PhysicalQueryFailure` outcomes:

- `oriented_bounds(accepted_solid, quaternion, limits)` returns enclosing bounds
  at zero translation, an exact-cardinal flag and work/peak-storage counters.
- `plan_regular_axis(object_min, object_max, container_min, container_max,
  pair_gap, wall_gap, count_cap, limits)` returns retained inputs, bounded count,
  whether the count was capped, and counters.
- `axis_translation(plan, index, limits)` returns a finite binary64 translation
  and counters. Recheck public value inputs and index bounds.

These are proposals, never validity certificates. Exact signed-permutation
rotations preserve accepted extrema without gratuitous `nextafter` expansion.
Generic orientations scan authoritative vertices using the validator's
homogeneous quaternion semantics `H(q)/dot(q,q)` and outward interval bounds.
Do not rotate only the corners of an existing AABB, normalize to another pose,
epsilon-snap, or use display/voxel data.

Find the largest bounded integer `n` satisfying
`2*cw + n*(object_max-object_min) + (n-1)*cp <= container_max-container_min`.
Use checked filtering and bounded exact dyadic comparison when needed. Integer
binary search and an explicit axis cap avoid overflow or unbounded enumeration.
No floating epsilon may alter the gap or floor. Preserve exact 10-in-40 tiling.

Derive each translation independently from original inputs:
`container_min + cw - object_min + i*((object_max-object_min)+cp)`.
Control rounding and preserve exact representable cases. Authoritative validation
still decides every candidate; an unresolved boundary cannot relax clearance.
STL container bounds stay in their already conditioned world frame.

### Exact wall thresholds during baseline validation

The regular physical grid deliberately reaches requested wall thresholds. The
existing validator's interval-only branch can leave a valid non-cuboid or
generic rotated pose unresolved there. Resolve that proof gap at the authoritative
axis-container boundary, preserving the original candidate coordinates:

- For any cardinally rotated accepted object, derive exact `AxisCoordinate`
  extrema from its accepted bounds and signed permutation. Use bounded exact
  containment/gap comparisons against the six analytic-box or certified STL
  cuboid planes. The argument applies because the container is convex; it must
  not replace true containment for a concave or cavity-bearing STL container.
- For generic rotations, keep the interval filter. Resolve an ambiguous vertex/
  plane comparison by the sign of the exact dyadic polynomial
  `sum_j H_ij(q)*v_j + (t_i - boundary_i - clearance)*dot(q,q)` (with the appropriate
  upper-plane sign). The denominator is positive. Use the actual supplied
  quaternion, the existing checked integer capacity and shared validation budget.
  Unresolved resource/arithmetic cases remain indeterminate and cannot publish.

This refinement can turn a previously indeterminate result into proved valid or
invalid; it changes no physical pose, allowed rotation, clearance or accepted
solid. The independent validator remains the admission boundary. Add direct
public-validator red/green cases for non-cuboid cardinal wall contact, generic
rotated clearance, below-threshold rejection and exhausted exact work. Then
verify the unchanged baseline grid against those cases and practical sources.
Do not add centered-placement retries or hidden clearance padding to conceal
uncertain validation.
Report the refined validation kernel as `homogeneous-rational-interval-v2` so
new decisive evidence is distinguishable from the earlier interval boundary.

### Positive pair-clearance thresholds

The first practical pilot exposed another exact-threshold cost: two supplied
irregular items retained one copy before exhausting the unchanged cumulative
validation budget on the next proposal. Outward floating bounds could not
certify a pair whose exact cardinal bounds were separated by precisely 1 mm;
the validator consequently entered its expensive surface comparison.

For positive requested pair clearance, an exact separation of cardinal
authoritative extrema by at least that clearance proves disjoint material and
a sufficient surface-distance lower bound. Permit that bounded certificate
before surface comparison, using original signed local coordinates and
translations with the existing exact comparison and shared budgets. A bound
equal to the requested clearance does not establish that the actual surface
distance is equal; preserve that distinction in the internal result. Unproved
bounds retain the existing narrow-phase path. Zero-clearance contact and
overlapping AABBs gain no new certificate from this positive-gap rule.

Keep this optional certificate separate from the full pair classifier. The
validation orchestrator may use the certificate before requesting full pair
classification. Direct classifier tests must continue exercising exact
vertex-face and edge-edge threshold predicates; a sufficient bounds proof must
not silently replace that independent narrow-phase coverage.

Add public-validator runtime red/green coverage for a non-cuboid exact-threshold
pair under bounded work, an actual below-threshold violation, and exhausted
proof work. Re-run the retained three-item pilot and unchanged registered
eight-copy workloads. This is part of the unreleased v2 refinement in T-006;
accepted solids, poses, clearance semantics and budgets remain as specified.

## Baseline and orientation order

Each orientation begins a separate empty working layout. Enumerate Z, then Y,
then X (X fastest), streaming cells without allocating the grid product. Use
stable nonempty copy IDs; geometric score ties ignore ID permutations.

| Native policy | Baseline seeds |
| --- | --- |
| Fixed | The supplied resolved quaternion, or identity for an empty fixed catalog |
| Cube | All 24 proper cube matrices, sorted lexicographically, with exact cardinal representatives |
| Catalog | Resolved canonical quaternions in supplied order, removing only provably equivalent duplicates |
| Upright | Yaw 0, 15, ..., 345 degrees; exact cardinal representatives at multiples of 90 degrees |
| Free | Sorted 24 cube rotations followed by their `R_cube * R_z45` compositions, deduplicated |

Record seed/score order as version 1 in diagnostics. The Free prefix has at most
48 seeds; it is not the full Halton/preset catalog or continuous refinement.
Do not collapse orientations merely because their AABB dimensions match. Reject
an oversized input catalog explicitly; search pass limits remain observable.

For each cell, submit `working + proposed_pose` through `make_candidate` and
the real validator. Only a returned valid handle advances working or the best.
This includes true containment for STL concavities and excluded cavities.
Full-prefix validation repeats work; T-006 records that cost without introducing
a private incremental certificate or making a scalability claim.

## Incumbent, score and failure behavior

`Incumbent` takes one context and accepts only `ValidatedSolution` handles from
that exact context. Null, foreign, lower-count and non-improving offers preserve
best. Its immutable `NativeSnapshot` contains revision, solution, `LayoutScore`,
optional `VolumeMetrics` and the invariant label `best_found`. Caller-created
snapshot values are never admission inputs.

Compare count first, then conservative occupied Z span, occupied X+Y span sum,
then sorted `(tz,ty,tx,qx,qy,qz,qw)` tuples. There is no epsilon comparator. Complete
ties preserve the existing revision. Unavailable secondary scores sort after
available ones at equal count and cannot block a larger validated count.
Scoring work is bounded across the entire offer, not reset for each copy.

Use accepted material volumes (including hollow-object voids) and the fixed
container volume. For boxes use the unchanged dimensions. Report utilization
only with a usable finite volume bundle and ratio; do not clamp an unexplained
overflow or a ratio above one. Product serialization of unavailable bundles
must follow the existing schema when T-007 integrates this boundary.

Prepare score/snapshot allocations before committing a new handle/revision.
Notify an optional sink after commit. Sink failure returns the latest committed
best with an error and causes no further callbacks; it cannot roll back a valid
improvement. Consumer-retained snapshot history belongs to the caller's reserve.
Revision zero denotes no admitted snapshot. The first admitted snapshot has
revision one, including a run's empty bootstrap or supplied initial handle;
subsequent improvements increment it. Cross-job revision continuation is a
later service concern.

Without an initial same-context valid handle, validate an empty candidate first.
An oversized object normally returns a validated empty `best_found`. Failure of
bootstrap preparation returns an explicit failure without a fabricated handle.
An existing valid initial handle survives later resource/preparation failures.

`BaselineOutcome.retained_solution` supplies an allocation-free fallback while
preparing the first native snapshot. If `best` exists, this field equals
`best->solution`; otherwise it retains a supplied same-context initial handle.
It does not publish a later uncommitted trial. A first-wrapper allocation failure
can therefore return the initial validated geometry without inventing revision
or score metadata. With neither an admitted snapshot nor a supplied initial
handle, failed bootstrap still returns no fabricated solution.

Native diagnostic codes use static string views so reporting an allocation
failure does not itself require allocation. `OfferOutcome` reports a separate
issue (`none`, `resource_limit`, `allocation_failure`, `numerical_failure`, or
`operational_failure`)
alongside admission status and actual scoring work. A larger validated count
may be admitted with unavailable secondary metrics and a reported issue; the
run notifies the committed improvement before returning that termination.
The offer also reports retained snapshot/key storage separately from scoring
scratch peak; a completed query's scratch must not be counted as permanently
resident in later proposals.

## Bounded work and storage

| Limit | Default |
| --- | ---: |
| Candidate evaluations / copies | 4,096 each |
| Search passes / orientations | 2,048 each |
| Cells per axis | 1,000,000 |
| Tracked live working storage | 512 MiB |
| Cumulative geometry kernel units / vertex visits | 1,300,000,000 / 200,000,000 |
| Cumulative validation kernel units / pair checks | 1,300,000,000 / 50,000,000 |
| Per-query geometry storage / units / vertex visits | 128 MiB / 100,000,000 / 5,000,000 |

These are configurable policy ceilings, not measured capacities. Clamp every
geometry and validation call to the remaining cumulative budgets and live byte
allowance. Include accepted resident buffers once, catalog, working/trial copies,
incumbent, score preparation, simultaneous validator scratch and caller reserve.
Track actual vector capacities/coexisting owners conservatively. Tracked storage
is distinct from process RSS and consumer retention beyond the declared reserve.
Per-validation defaults otherwise remain `ValidationLimits`.

One candidate evaluation is one proposed new pose submitted to authoritative
checking, including rejected and indeterminate proposals. Rechecking its prefix
adds validator work, not candidate count. Empty bootstrap is preparation work.
One completed orientation grid is a search pass, including a zero-cell grid;
interrupted grids are not completed passes. Stop before starting another grid
after the pass limit. Explicit axis truncation must not appear as natural full
enumeration.

Check stop/deadline at safe call boundaries. Fixed-work runs omit deadlines;
timed runs use a monotonic clock. The existing validator is non-preemptible:
this interface does not establish UI acknowledgement or stop-latency gates.

Use `budget_exhausted`, `user_stopped`, `search_stalled`, `resource_limit`, or
`error`. At a safe boundary precedence is observed operational error, stop,
resource exhaustion, search budget, then natural exhaustion. Explicit work,
memory or arithmetic-capacity failures cannot masquerade as ordinary misses.
An isolated geometric indeterminate may be skipped with bounded diagnostics.
No termination implies optimality or infeasibility.

## Acceptance and evidence

Observe actual runtime red tests before implementation, then rerun affected
gates. Cover exact64, clearance64 (centers `6+11*i`), oversized valid empty,
allowed rotations/tilted bar, concave/cavity filtering, numerical boundaries,
deterministic work/revisions, independent utilization, retained initial best,
allocation failures and throwing sinks. Previously delivered snapshots must
remain unchanged. Reuse direct integer correlation characterization evidence.

Start practical pilots with each accepted item in the 5 kg container, Fixed
identity, 1 mm pair/wall gaps, eight candidate evaluations and one pass. Once
tractability is observed, qualify all 18 accepted pairs at the same fixed work,
plus the analytic64/64/empty workloads. Keep all source identities, exact poses,
fresh-context revalidation, limits/counters, termination and raw timing samples.
No irregular baseline count is an optimum. Watchdog failure is incomplete
evidence; retain it rather than calling it a deterministic completed run.

### Hosted qualification watchdog (2026-09-13)

The clean local full baseline takes 427.569 s under the default 600 s native /
900 s harness watchdogs. Hosted run `34766827635` passes 209 Debug tests but
times out the original 27-proposal cavity regression at 30.01 s; the same test
takes 12.68 s locally. This establishes material host variability for that test,
without establishing a Release benchmark speed ratio. The cavity unit regression
is reduced to the prefix that crosses the forbidden cell and then accepts the
next valid cell, retaining its geometry and rejection assertion.

Register an explicit hosted baseline allowance of 1,500 s native / 1,800 s harness,
matching the existing hosted representation allowance. The workflow has a bounded
70-minute envelope for both qualifiers and the preceding build/test gates. Default
local watchdogs stay 600/900 s. Every host still runs all 21 baseline cases with
one warmup and three samples, identical proposal/geometry/validation/storage limits,
and unchanged physical oracles. Reports retain actual timing parameters. A timeout
remains incomplete qualification; no speed or regression threshold is inferred
from this planning allowance. There has been no hosted baseline timeout at the
default allowance; this adjustment precedes that run and is based on the observed
native regression timing difference.
