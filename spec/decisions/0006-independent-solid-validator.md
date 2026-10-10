# ADR 0006: independent native solid validation

Date: 2026-09-12. Status: accepted interface and numerical contract; implementation
and qualification are tracked in [T-004](../../doc/T-004.md).

Requirements: GEO-05/GEO-06; spec §§5.2–5.3; AT-06/AT-07/AT-09 and the native
orientation-permission portion of AT-08. This extends the accepted-solid boundary
from ADR 0005 without changing clearance, container or source-frame semantics.

## Ownership and publication

`pack_geometry` exposes `make_validation_context`, `make_candidate` and `validate`.
An immutable context owns one accepted object solid, an analytic box or accepted
STL interior-volume container, separate pair/wall clearances and resolved
orientation permissions. All geometry is in millimeters. A candidate owns its
copy IDs and active XYZW rigid poses and retains that context. Validation requires
the expected context to be the identical immutable context; substituting an asset,
container or constraint set requires a new context and complete validation.

Only accepted-solid handles enter this boundary. No caller-supplied mesh, proxy,
LOD, voxel field or claimed hash can substitute for them. File/hash resolution
belongs to the I/O adapter: this native API does not accept external asset hashes
or load a serialized solution. Its identity guarantee is the retained immutable
accepted object and context, not an unverified caller-provided digest.

Context factories reject unusable roles, invalid dimensions/clearances and
orientation policies. Candidate validation checks finite rigid poses, normalized
canonical quaternions, orientation permissions and unique nonempty copy IDs.
Preserve the existing quaternion admission and 1e-7 rad permission tolerances.
No matrix or scale input can introduce shear or dimension changes.

Only completed `valid` validation constructs an opaque `ValidatedSolution` with
its own immutable context, copied poses and report. Public report values are
diagnostics; changing them cannot authorize a layout or mutate a validated
snapshot. Invalid and indeterminate candidates produce no validated handle.
Future incumbent/export code must consume that handle. T-004 does not implement
solver lifecycle or a serialized-result validation command.

Opaque assets, drafts, repair proposals, contexts, candidates and validated
solutions are shared through handles, with value copy/move operations disabled.
Otherwise a caller could copy one into a mutable object, retain it through a
const shared pointer, and later replace or move out its storage. Private fields
alone would not preserve the identity guarantee. This tightens the T-003 native
types' implicit value-copy surface; existing repository clients use shared
handles and need no migration. It does not change serialized contracts.
Factories also copy caller-owned pose and orientation-catalog values into private
storage. Merely moving a vector would preserve external references to its elements
and allow those references to mutate a supposedly immutable snapshot.

## Physical predicates

The classification tolerance remains `max(1e-6, 1e-9 * D)` mm, using the larger
accepted object/container diagonal. Report it with kernel revision, actual test
methods, affected IDs and work counters. It is not a clearance discount or an
overlap-volume threshold. Unresolved predicates, arithmetic overflow or exhausted
limits yield `indeterminate`, never a geometrically valid certificate.

Preserve local authoritative coordinates and poses while evaluating geometry.
For an admitted near-unit quaternion, the physical rotation is the exact rational
matrix `H(q) / dot(q,q)`, using the homogeneous quadratic quaternion matrix:
its first row is `(w*w+x*x-y*y-z*z, 2*(x*y-z*w), 2*(x*z+y*w))`, with the usual
cyclic remaining rows. The diagonal constants must not be replaced by literal
ones before division for a near-unit quaternion. This
is the rotation of the mathematically normalized quaternion and is rigid; it does
not impose a hidden anisotropic scale. Translation remains separate until the
predicate is evaluated. Rounded world vertices alone cannot authorize a result:
at a translation of 1e16 mm, binary64 rounding can erase a quarter-millimeter
overlap. Tests must retain such cancellation cases.

Outward-rounded finite intervals may certify a predicate when the entire interval
has the required sign or threshold relation. Otherwise use bounded exact integer
or rational evaluation of the original coordinates and pose. Unsupported
floating modes or interval overflow require the robust path or uncertainty.
Document and check the arithmetic capacity; the import kernel's degree-three
capacity proof does not automatically cover rotated distance expressions.

Conservative world AABBs and separating-plane certificates may eliminate pairs.
At zero clearance an exact separating plane permits boundary contact while
excluding positive material overlap. Positive clearance requires a sound
Euclidean distance lower bound, not a per-axis gap approximation. A recognized
exact cuboid path may resolve contact, overlap, coincidence, enclosure and exact
gap equality. Recognition must prove the accepted solid is the complete box
boundary; an arbitrary mesh's AABB is not a cuboid certificate. Cardinal rotations
are recognized algebraically, never by epsilon snapping.

For general solids, complete boundary-disjointness permits exact material
classification with one witness per connected boundary shell, in both directions.
A pair overlaps if either solid's shell witness lies in the other's material.
For STL containment, every object shell witness must be inside permitted material,
and no container shell witness may be inside object material. This detects an
object enclosing an excluded cavity even when its vertices are all permitted.
The witness theorem cannot be applied before boundary disjointness is established.
Parity classification respects nested cavities and rigid disjoint components.

A proper transverse boundary crossing proves a violation. At positive clearance,
any boundary contact also violates the gap. Other contacted or coplanar cases
need a sound contact/separation certificate; unresolved arrangements remain
indeterminate. This documented partial classification must still resolve required
axis-aligned contact, exact analytic gaps and generously separated arbitrary
rotations; it cannot reject all non-axis rotations as a substitute for validation.

Compare required surface distances with the requested clearance squared using
triangle closest features (vertex/face and edge/edge, including endpoints), with
exact rational comparisons at ambiguous thresholds. A third-party BVH may order
or accelerate conservative candidate traversal, but its floating distance or
boolean result supplies no invented certified error bound. Do not authorize an
overlap because a boolean result is small or because two approximate kernels vote
for acceptance.

Analytic box containment checks the authoritative transformed vertices against
all six inward-offset planes. A conservative bound may prove the whole mesh lies
inside; borderline bounds require actual vertex predicates. This convex-box proof
cannot replace STL containment for an arbitrary concave or hollow container.

## Limits, tests and compatibility

Every validation pass is complete and independent of search certificates. Contexts
may cache immutable mesh structure/BVHs, but no prior pose verdict is reused.
Guard copy counts, allocation sizes and geometric work before expensive expansion.
One shared allowance covers a pass; report actual usage and bounded diagnostics.
Default limits are provisional until practical qualification, and explicit small
limits must reproduce uncertainty without publishing a handle.

### Bounds before instance expansion (T-010 follow-up, 2026-10-10)

For many copies of one detailed accepted solid, compute a private lightweight
posed descriptor before allocating transformed per-vertex arrays. Its bounds
come from the accepted solid and physical pose in the current validation pass.
Reuse exact signed-permutation extrema for cardinal positive-clearance pair and
analytic-box proofs; use outward conservative bounds only as sufficient proofs
of separation or containment. An inconclusive enclosing bound cannot reject a
valid sparse shape. Zero-clearance contact and arbitrary STL containment retain
their existing full classifiers and cavity/enclosure checks.

Keep the descriptor type distinct from a fully materialized placed solid. Full
classifiers must never receive an empty or partial vertex representation. Lazily
materialize each required copy at most once in the pass, sharing the existing
immutable prepared object. Charge descriptor/owner storage, proof scratch and
work before use, with the same cumulative limits and cancellation control.
After input/orientation checks, every pass charges one fixed setup work unit,
including empty layouts and fresh revalidation. Deferred mesh preparation must
not turn an empty bootstrap into free completion under a zero-work allowance.
Resource or arithmetic failure yields indeterminate; it is not an inconclusive
geometric proof that permits an uncharged retry.

Final, restore and export validation independently recompute these proofs. This
change reorders existing geometry proofs, without trusting solver bounds or a
previous verdict. It introduces no new public API, wire schema, accepted-solid
identity, physical tolerance or clearance semantics. Persistent spatial indexes
and incremental search validation remain outside this bounded change.
Report `homogeneous-rational-interval-v3` to distinguish the changed validation
strategy and resource evidence. Historical v2 reports/baselines keep their original
revision; the existing string-valued result field needs no schema migration.

Use independent analytic fixtures and rational expectations for contact, tiny
overlap, enclosure, cavities, U-bridge containment, Euclidean clearance, rotated
poses and large translations. The public native API gates immutable snapshot and
context binding behavior. A private pair seam permits differently sized solids in
enclosure tests without changing the product's one-object-per-layout model.
Practical tests retain source hashes and derive poses from accepted local bounds.
Benchmark every retained sample's verdict and work counts; timing alone is not
acceptance.

This adds a native interface and kernel revision, with no schema migration or new
compute dependency authorized by this decision. Existing wire hashes, coordinate
conventions, accepted meshes and import diagnostics remain unchanged. T-004
qualification does not establish full AT-08 search behavior, export round trips,
the desktop workflow or completion of M1.
