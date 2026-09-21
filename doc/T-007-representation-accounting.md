# T-007 representation accounting contract

2026-09-21. Status: **accepted design under ADR 0009**, pending implementation; not an
implemented API. Scope: conservative fields and their solver integration only.
No geometry, occupancy, clearance or acceptance semantics change.

## Finding and decision

Expose an allocation-free snapshot of the **current retained ownership graph**,
and add overloads with a mandatory **per-call attempt-statistics output**. These
are different quantities. Neither `RepresentationStats::working_bytes_peak`
nor the present `FieldBuilder::resident()` value is an ownership graph.

Observed in `conservative_fields.cpp`:

- `VoxelGeometry::Storage` retains accepted source, prepared kernel, private
  preparation stats and an aggregate resident total.
- `CellField::Storage` retains geometry, a placed kernel, cells, pose and possibly
  a container. Several fields can share the accepted/prepared payload.
- `BlockedField` retains its container mask and PMR counts/footprints. It does
  **not** retain the placed blocker passed to `add`; it copies its occupied
  indices. `CountingResource::current()` currently also includes temporary
  external charges; `base_bytes` includes creation-time caller reservation.
- Failed raster/fill/dilate/preparation attempts lose work and cell-visit counts.
  `add` scans cells twice then updates occupied counts; its present preliminary
  size check does not expose all that work. `remove` has no per-call limits.

Use the existing owner-known capacity calculations and PMR requested byte counts.
Do not derive payload from a peak, subtract guessed coefficients, or estimate a
placed kernel from a vertex-count multiplier in the solver.

## Exact public additions

In `representation_types.hpp` (existing declarations stay compatible):

```cpp
enum class RepresentationResidentKind : std::uint8_t {
  accepted_draft_payload,
  voxel_geometry_owned,
  cell_field_owned,
  blocked_field_owned
};

struct RepresentationResidentBlock {
  RepresentationResidentKind kind{};
  const void* identity{};  // Opaque equality token; never dereference/serialize.
  std::uint64_t bytes{};
};

struct RepresentationResidency {
  std::array<RepresentationResidentBlock, 4> blocks{};
  std::uint8_t count{};
};

struct RepresentationAttemptStats {
  std::uint64_t kernel_work{};
  std::uint64_t cell_visits{};
  std::uint64_t input_resident_bytes{};
  std::uint64_t working_bytes_peak{};
  std::uint64_t additional_bytes_peak{};
  bool input_accounting_complete{};
};
```

Add allocation-free, nonthrowing getters:

```cpp
// AcceptedSolid, VoxelGeometry, CellField and BlockedField each expose:
std::optional<RepresentationResidency> representation_residency() const noexcept;

// VoxelGeometry additionally exposes its already retained preparation stats:
const RepresentationStats& stats() const noexcept;
```

`AcceptedSolid` needs the representation value declaration available in its
header; including the dependency-free `representation_types.hpp` is sufficient.
There is no dependency from those value types back into import/validation.
Do not change `AcceptedSolid::resident_buffer_bytes()` or the legacy meaning of
`CellField::stats()`/`RepresentationStats::working_bytes_peak`.

Add these overloads in `conservative_fields.hpp`; the final attempt reference is
mandatory, so existing calls remain unambiguous:

```cpp
RepresentationOutcome<VoxelGeometry> prepare_voxel_geometry(
    std::shared_ptr<const AcceptedSolid>, const RepresentationLimits&,
    RepresentationAttemptStats& attempt);

RepresentationOutcome<CellField> voxelize_object(
    std::shared_ptr<const VoxelGeometry>, GridLattice, Quaternion,
    const RepresentationLimits&, RepresentationAttemptStats& attempt);

RepresentationOutcome<CellField> voxelize_placed(
    std::shared_ptr<const VoxelGeometry>, GridWindow, const CopyPose&,
    double pair_clearance_mm, const RepresentationLimits&,
    RepresentationAttemptStats& attempt);

RepresentationOutcome<CellField> voxelize_container(
    Container, GridWindow, double wall_clearance_mm,
    const RepresentationLimits&, RepresentationAttemptStats& attempt);

std::variant<std::unique_ptr<BlockedField>, RepresentationFailure>
make_blocked_field(std::shared_ptr<const CellField>, const RepresentationLimits&,
                   RepresentationAttemptStats& attempt);

// BlockedField member overloads:
std::optional<RepresentationFailure> add(
    std::string_view copy_id, std::shared_ptr<const CellField> placed_blocker,
    const RepresentationLimits&, RepresentationAttemptStats& attempt);
std::optional<RepresentationFailure> remove(
    std::string_view copy_id, const RepresentationLimits&,
    RepresentationAttemptStats& attempt);
```

No new result wrapper, global accounting registry or field-mutability API is
needed. `RepresentationFailure` itself stays unchanged: the mandatory output is
the failure-statistics channel, including failure to allocate an error string.
Old overloads delegate with a local ignored attempt. For old `add/remove`, use
the saved creation limits and creation reservation as their compatibility policy.
The solver must use the explicit overloads exclusively.

The new `voxelize_placed` borrows `CopyPose` so it can admit retained ID storage
**before** copying it. The new `add` borrows `string_view` for the same reason.
Legacy by-value entry points cannot account for a caller's pre-entry argument
copy and retain that existing caller responsibility. Do not make a temporary
owning string or pose in the wrapper used by the explicit overload.

## Retained graph and deduplication

Each block owns disjoint tracked payload. Equal `(kind, identity)` means the
same underlying owner, not equal content or geometry hashes. Sum one instance
of each token, with checked arithmetic. Two blocks with one identity and unequal
bytes in one snapshot indicate inconsistent sampling and must reject accounting;
never select the smaller value. Immutable blocks never change; the blocked-field
owned block can change after a mutation attempt.

| Owner's block | Exact inclusion and exclusions |
| --- | --- |
| Accepted draft payload | Refactor the existing `resident_buffer_bytes()` draft helper: actual vertex/triangle/report vector capacities and external issue-string capacity. One token is the private `AssetDraft::Storage` address. A repaired solid exposes accepted and distinct original draft blocks separately. Do not use the outer `AcceptedSolid*`: separate accepted handles can share the same draft. |
| Voxel geometry owned | `sizeof(VoxelGeometry::Storage)` plus the private prepared kernel's own `sizeof(PreparedSolid)` and actual shell-witness capacity. Excludes accepted draft payload, which is listed separately. Identity is the voxel storage address. Prepared kernel ownership is bundled here because it is private and shared through this `VoxelGeometry`. |
| Cell field owned | `sizeof(CellField::Storage)`, actual cells capacity, retained external pose-ID capacity, placed kernel's own `sizeof(PlacedSolid)` and actual vertex/interval/exact-coordinate capacities. For STL-container fields also include their privately owned prepared kernel payload. Excludes the separately listed source and shared voxel geometry. Identity is field storage address. |
| Blocked field owned | `sizeof(BlockedField::Storage)` plus **owned PMR outstanding bytes** for counts, map buckets/nodes, keys and footprints. Excludes mask dependency, caller reserve and temporary blocker graph. Identity is blocked storage address. The counts vector's actual allocation appears through PMR, not again via a capacity formula. |

The graph returned by a handle includes these dependency blocks as applicable:

```text
accepted:        accepted draft [+ original draft]                   <= 2
voxel geometry: accepted graph + voxel-owned                        <= 3
object field:   voxel graph + field-owned                           <= 4
STL mask:       accepted container graph + field-owned               <= 3
box mask:      field-owned                                          <= 1
blocked field: mask graph + blocked-owned                           <= 4
```

Thus four blocks are enough for the current documented ownership graph, not an
empirical allocation allowance. If a future dependency changes that bound,
update the contract; never truncate the block list. Overflow or inconsistent
ownership returns `nullopt` and requires operational rejection.

Snapshots borrow identities: retain the owning handles while comparing them.
Rebuild/remove ledger entries before releasing owners; do not cache dangling
tokens across releases and allocations (address reuse). No concurrent mutation
or residency sampling of one `BlockedField` is supported. Snapshot enumeration
allocates no memory and exposes no mutable buffer or extra owning reference.

Keep the existing portable accounting boundary: allocator headers/slack,
shared-pointer control blocks and process RSS are outside it. Include known
storage/kernel structures already charged by the field budget; do not broaden
accepted-payload accounting silently. This is exact deduplication of the declared
tracked payload, not a claim to measure all process memory.

## Call accounting semantics

At entry, reset `attempt` before validation or allocations. It must be updated
on every return, including wrong input, arithmetic refusal, work/cell exhaustion,
bad allocation, and metadata-publication failure. An allocation-free scope guard
updates it before any escaping exception; the caller can catch `bad_alloc` and
still retain consumed work. Do not require allocating a failure report to observe
the counters. Do not label an unavailable counter as zero success.

For a call, let `I` be the union of all retained input graphs:

| Call | Input graph union |
| --- | --- |
| prepare | Accepted source |
| voxelize object/placed | Supplied voxel geometry |
| voxelize STL container | Accepted container |
| voxelize box container | Empty |
| make blocked | Container mask |
| add | Current blocked graph **and** incoming placed blocker graph |
| remove | Current blocked graph |

Borrowed strings/poses and other solver buffers stay in caller reserve; newly
retained copies are operation allocations. `input_accounting_complete` becomes
true only after all input graphs and their checked union are known. A complete
empty union legitimately has zero bytes. False means no caller may derive a
memory estimate from `input_resident_bytes`; normally the operation fails.

- `kernel_work` and `cell_visits` are per-invocation consumed counters, never the
  configured limits or lifetime totals. Do not replay preparation stats merely
  because a field retains a prepared object. Add both counters after **each**
  call, successful or failed; clamp the next call to the remaining global work.
- `working_bytes_peak` excludes `limits.reserved_bytes`; it covers the complete
  deduplicated input graph plus this operation's admitted tracked storage and
  workspace at its peak. It starts at `bytes(I)` when input accounting completes.
- `additional_bytes_peak = max(0, working_bytes_peak - bytes(I))` records peak
  growth beyond the call's existing input graph. For a remove without allocation,
  this is zero even though it traverses retained storage.
- These are conservative admitted workspace peaks, not retained payload getters.
  A successful result's current graph, or a freshly sampled blocked graph after
  **either** outcome, is the only retained-memory input to the next call.
- An already-live input larger than a lowered limit may give a peak above that
  limit on immediate refusal, with zero added allocation. Do not clip observed
  input residency to the requested ceiling. A refused allocation is not added to
  the successful-allocation/current ledger; a reservation preceding a failed
  allocator may remain in the conservative attempted workspace peak.

Existing `RepresentationStats::working_bytes_peak` continues to include caller
reserve, for old consumers. Populate it from the same attempt ledger on success;
do not change its interpretation in place. `VoxelGeometry::stats()` reveals its
preparation's actual work. The solver uses the explicit attempt stats for new
calls, rather than adding both the attempt and returned object's stats.

The solver's admission formula is:

```text
G = union of all currently live representation graphs
I = graph union listed for this call
external = all caller-owned tracked bytes outside G, including accounting tables
limits.reserved_bytes = external + bytes(G union I) - bytes(I)
limits.max_working_bytes = global live-byte ceiling
global observed peak = max(previous, limits.reserved_bytes + attempt.working_bytes_peak)
```

All arithmetic is checked, and `I` may contain an incoming blocker not otherwise
in the retained set. For compute/validator calls that do not charge representation
inputs themselves, reserve the **whole** graph union according to their own
existing peak semantics. Never pass whole accepted/field residency as caller
reserve to a factory that also charges those same inputs.

## Blocked-field mutation limits and transactional behavior

For explicit `add/remove`, each call's `reserved_bytes` **replaces**, rather than
adds to, the creation-time reservation. Respect both the creation policy ceiling
and the call's remaining ceiling for each `max_*` field (componentwise minimum).
Creation limits constrain the object's permissible size; operation work counters
reset per call and are clamped by the solver's cumulative remainder.

Separate PMR **owned current bytes** from all external charges. Give its counting
resource a scoped allocation ceiling and per-operation peak capture. Calculate
that ceiling from current call reservation and the deduplicated input graph;
do not keep caller reserve inside `base_bytes` or PMR ownership. The public graph
getter must use actual PMR outstanding allocations, including bucket capacity
that survives a removal or failed insertion. No allocations occur outside the
creation/mutation scopes.

Charge and bound duplicate-ID lookup/comparison, both blocker scans, footprint
construction and count updates; remove charges lookup, invariant scan and count
updates. Cell visits count each examined cell/footprint index in every pass,
not just the unique grid size. Kernel work includes those traversals and bounded
ID processing under a documented deterministic work-unit policy. Reuse existing
kernel counters for geometric preparation/rasterization. This policy measures
charged algorithmic work, not CPU instructions or elapsed time.

Preflight the final nonthrowing count-update phase's remaining work before
changing any count, then record its units as it executes. Allocate ID/footprint/
map storage before committing counts. Work exhaustion, duplicate/missing ID,
overflow/underflow or memory failure preserves counts and the set of copy IDs.
Map rehash capacity may remain after a failed insertion; that is not a placement
mutation, but it **must** appear in the fresh residency snapshot. Do not claim
byte-for-byte storage rollback or omit surviving buckets from the next reserve.
An over-limit remove may fail unchanged; releasing/destroying the whole trial
remains allocation-free cleanup outside placement search.

## Implementation seams and compatibility

1. In import ownership code, extract the existing draft-capacity helper and use
   it for both the scalar resident API and the new two-block snapshot. Keep all
   private accepted/repair state private. Coordinate this small import edit with
   the active export repair owner before editing; do not revert their changes.
2. Add private kernel own-payload queries beside `PreparedSolid`/`PlacedSolid`
   definitions, where vector capacities and actual element types are known.
   Query exact retained capacities; existing budget `bytes_live()` is not a
   substitute for those queries after temporary workspace has been released.
3. Construct immutable voxel/field block summaries at publication from the real
   owners. Obtain field-owned vs shared data there, without subtracting two
   historical peaks. Kernel references retained through `placed` must be counted
   even when not also stored in `CellField::geometry` (STL-container path).
4. Carry one attempt accumulator across `make_raw`, rasterization, component
   fill, stencil, dilation/crop and publication. In particular, a failed
   `make_raw` must transfer its partially consumed cell visits before its local
   `RawField` is destroyed. Capture budget counters before any nested failure
   return. Account retained ID allocation only after admission.
5. Refactor `CountingResource` to separate owned bytes and per-call admission;
   reuse actual PMR allocation/deallocation sizes. Keep add/remove logical
   transaction semantics, with explicit final work admission.

No wire/schema change and no accepted-solid identity/physical change. Existing
source calls remain valid; new fields are additive types/getters/overloads.
Corrected charging can reject old **tight** work/memory limits that previously
omitted scans or duplicated/included stale reserves; this is a resource-accounting
compatibility consequence to document, not a reason to weaken acceptance tests.
Existing broad-budget T-005 geometry/field outcomes must remain unchanged.

## Focused observable red/green tests

Use current analytic fixtures and the real field factories. Add runtime failing
tests at a compiled seam before implementation, then rerun the affected T-005
field tests; do not use missing declarations as behavioral red evidence.

1. **Shared graph:** two fields from one voxel geometry expose identical shared
   accepted/prepared tokens and distinct field-owned tokens. Their union equals
   shared bytes once plus both exclusive blocks. Adding aliases leaves the sum
   unchanged. Preparing the same accepted solid twice yields distinct prepared
   blocks but the same accepted payload. Accepting one draft twice deduplicates
   its payload; repaired accepted/original blocks remain separately identifiable.
2. **Scratch is not live:** a noncuboid preparation and clearance-dilated field
   use transient shell/flood/stencil buffers. Current graph bytes stay unchanged
   across different external reserves, while the legacy inclusive peak shifts
   by that reserve. New attempt peak excludes it. A tight follow-on allocation
   fits using actual graph residency when using the old peak as residency would
   incorrectly reject it. Assert public cells remain identical.
3. **Partial failure work:** cap work/cell visits after some analytic cell
   classification and after some noncuboid preparation/rasterization. Failed
   calls report positive consumed counters within their caps. Repeating with the
   global remainder cannot spend the original allowance again. Early malformed
   input reports zero work and incomplete or explicitly empty input accounting,
   never a forged completed attempt.
4. **Changing caller reserve:** create blocked storage with a small reserve,
   then attempt add with a much larger live external reserve and a byte ceiling
   that forbids footprint growth. It must fail before exceeding the cap, leave
   counts/IDs unchanged and preserve correct post-failure live bytes. Retrying
   with the smaller current reserve succeeds. A reduced later reserve must not
   remain penalized by an old creation-time reserve in the explicit overload.
5. **Mutation work:** tiny add/remove limits fail after observable bounded work
   without partial count updates. With sufficient budgets, reported cell visits
   include each scan/update pass. Overlapping footprints retain nonzero counts
   when only one copy is removed, matching existing independent occupancy checks.
6. **No blocker retention / PMR truth:** after successful add, releasing the
   supplied blocker and its private dependencies releases their graph ownership;
   the blocked graph contains only mask dependencies plus PMR-owned storage.
   Removal reports remaining bucket capacity honestly. A private allocator fault
   after rehash but before insertion preserves semantic state and exposes any
   surviving allocated bucket capacity in the public snapshot.
7. **Pre-entry clone / allocation failure:** use the explicit borrowed-pose and
   string-view overloads with a large ID and tiny cap. A scoped private allocation
   observer verifies no large ID copy precedes admission. Inject metadata/PMR
   allocation failure after work starts; attempt counters survive failure even
   if an error-string allocation also throws. Existing caller-owned data remains
   unchanged.
8. **Overflow/identity safety:** owner-query arithmetic overflow is an operational
   refusal; no saturated/zero bytes authorize a call. Use a private narrow seam
   to test impossible-size arithmetic, not giant allocations. Snapshot copies
   are non-owning, fixed-size and allocate nothing. Keep lifetime/concurrent-
   mutation rules explicit rather than attempting to dereference expired tokens.

Private capacity/allocator seams may substantiate byte accounting, but physical
tests still use real fields and independent occupancy expectations. No hardcoded
platform-specific PMR bucket size or invented memory coefficient is a test oracle.

## Decision summary

> Conservative representations expose deduplicable current ownership blocks and
> call-specific attempt statistics. Shared accepted/prepared payload is charged
> once; caller reservation and temporary workspace are not retained storage.
> Spectral clamps every factory and blocked-field mutation to current remaining
> work and live-byte allowance, accumulating attempted work on failure as well as
> success. Blocked mutations use the current call's reservation and preserve
> logical occupancy on failure. Owner identities are in-process accounting tokens
> with retained-owner lifetimes, never asset identities or persisted values.

The primary accepted these additive declarations on 2026-09-21. Implement the
import/kernel/field edits after the active export repair. No unresolved algorithm
or dependency choice is required to implement this contract.

Design evidence: inspected the named review/handoff, current public
headers, field ownership/mutation implementations, private kernel owners and
existing accepted capacity calculation. Documentation whitespace/fence checks
only; no code edits, builds, native tests or claimed runtime results.
