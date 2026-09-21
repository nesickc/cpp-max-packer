# ADR 0009: CPU spectral placement and checked export

Date: 2026-09-21. Status: architecture contract agreed with the primary for
T-007 implementation; no implementation or qualification is implied.

Requirements: SOL-02, the fixed/cube/custom subset of SOL-03, DATA-03;
spec §§5.2, 6.3, 8.3, 8.5, 11.1; AT-10, CPU AT-12, transform/export portions
of AT-14. [T-007](../../doc/T-007.md) records scope and acceptance.

## Ownership and compatibility

Dependencies remain engine → solver + I/O, solver → geometry + compute,
I/O → geometry. Compute has no geometry, solver, JSON or file dependency.
Keep the existing pinned pocketfft dependency adapter; add no dependency pins.

- `pack_compute` owns synchronous CPU double correlation and numerical checks.
  Borrowed input spans cannot escape calls; returned arrays own their storage.
- `pack_solver` owns orientation order, pages, ranking, trial poses, field
  lifetimes, budgets and the retained `Incumbent`. Only existing opaque
  `ValidatedSolution` handles authorize admission.
- `pack_geometry` retains immutable accepted solids and physical validation.
  A narrow export adapter validates distinct quantized copies using the existing
  solid kernel; it cannot mint a replacement search solution.
- `pack_io` owns verified asset reconstruction, result metadata, canonical
  serialization, streaming STL, temporary files and publication. It consumes
  `ValidatedSolution`, not solver headers. The CLI combines these boundaries.

Existing native baseline calls, import/validation contracts and schema version 1
remain compatible. Native `OrientationMode::catalog` maps to wire `custom`.
The CLI rejects unsupported upright/free, auto resolution and non-CPU resolved
requests explicitly for this tranche. Baseline's existing upright/free seeds
remain available through its existing native API; they do not establish M4.
An Auto backend request may resolve explicitly to CPU without initializing
Vulkan. Full service jobs, persistence/restore and desktop are later work.

The STL companion is a new separately versioned document, not an extension
smuggled into `results.schema.json`. No source/accepted asset changes or changes
to clearance, tolerance, rigid dimensions or result validity are authorized.

## Compute contract

New `engine/pack_compute/include/spectrapack/compute/correlation.hpp`, namespace
`spectrapack::compute`; these are proposed new declarations, not existing types:

```cpp
using Shape3 = std::array<std::uint32_t, 3>;
using Index3 = std::array<std::int64_t, 3>;
struct CorrelationSpec {
  Shape3 environment_shape, kernel_shape;
  Index3 environment_first, kernel_first;
};
struct CorrelationLimits {
  std::uint64_t max_working_bytes{512ULL << 20}, reserved_bytes{};
  std::uint64_t max_padded_cells{16'777'216}, max_direct_terms{200'000'000};
};
struct CorrelationStats {
  std::uint64_t padded_cells{}, working_bytes_peak{}, direct_terms{};
};
struct NumericReport {
  double max_integer_residual{}, mass_residual{}, max_probe_error{};
};
struct CorrelationFailure {
  std::string_view code;
  std::string message;
  CorrelationStats stats;
};
struct CorrelationResult {
  Index3 translation_first;
  Shape3 shape;
  std::vector<double> values;
  NumericReport numeric;
  CorrelationStats stats;
};
using CorrelationOutcome = std::variant<CorrelationResult, CorrelationFailure>;
CorrelationOutcome correlate_binary_cpu(const CorrelationSpec&,
    std::span<const std::uint8_t> environment,
    std::span<const std::uint8_t> kernel, const CorrelationLimits&);
CorrelationOutcome correlate_proximity_cpu(const CorrelationSpec&,
    std::span<const double> environment,
    std::span<const std::uint8_t> kernel, const CorrelationLimits&);
```

Require positive shapes, checked products/ranges, exact matching span lengths,
binary bytes in `{0,1}`, and proximity values finite in `[0,1]`. Reject unsupported
numeric/index ranges before allocation. Zero-filled fields are legal.

Arrays are X-fast: `x + nx*(y + ny*z)`. Let `E,K` be environment/kernel
shapes and `b,a` their signed first indices. Return every linear translation:

```text
translation_first = b - a - (K - 1)
output_shape = E + K - 1
C(t) = sum_e B[e] * O[b + e - t - a]
```

The existing independent integer oracle has environment first zero. To compare,
pass kernel origin `a-b` and compare the same `t`; do not rewrite that oracle
using production indexing or helpers.

Initially use full complex double FFT buffers, single thread, with each padded
axis exactly `E+K-1` (pocketfft supports these lengths). Put both arrays at padded
index zero, compute `FFT(B)*conj(FFT(O))`, inverse with factor `1/product(P)`.
Fetch a translation from circular slot `(t+a-b) mod P` independently per axis.
Only these full-linear output slots are returned. Future efficient padding or
real transforms must preserve this public contract and oracle suite.

Preflight the checked live byte sum before allocating: caller reserve, borrowed
input residency as declared by caller, both complex arrays, inverse/output
storage, pocketfft plans/scratch and numerical probes. Audit the pinned header's
actual allocation path and disable hidden persistent caches. Do not assume a
generic library overhead is zero. An implementation unable to bound workspace
must fail resource admission rather than advertise a false bound. Record tracked
bytes separately from process RSS. No silent pitch coarsening or cache eviction
that discards a validated incumbent.

Binary output must be finite, within its mathematical nonnegative/popcount
bounds up to error strictly below 0.25, and have distance to nearest integer
strictly below 0.25 everywhere. Check total mass against
`sum(B)*sum(O)` and deterministic direct probes. Use checked integer arithmetic
for binary sums; reject sums outside the exactly representable integer domain.
The full-linear mass test must catch systematic normalization errors even when
every wrong value happens to be integral. Conservative initial absolute mass
error limit is `<0.25`; a legitimate field exceeding it is unreliable, not free.
Probe first/last/middle translations, maximum-magnitude output and fixed
deterministic interior samples, deduplicated; check exact binary sums with the
same `<0.25` criterion. Charge actual direct terms and reject exhausted checks.

Proximity uses the same indexing and normalization, but no integer residual
test. Check finite/range/mass and direct probes with relative tolerance
`1e-10 * max(1, expected magnitude)`; record these as ranking checks only.
This threshold is policy, not a measured GPU/numerical envelope. The test suite
compares every tiny proximity translation to an independent direct sum.

Any failed check returns `CorrelationFailure` with no usable result values.
Because this is already the double CPU reference, T-007 reports an unreliable
pass and preserves best; it need not retry the same computation. Never translate
unreliability into an infeasibility claim. Private test hooks before the real
numeric checker inject wrong inverse scaling, integral offsets and NaN; user
settings expose no fault-injection controls.

## Solver contract and search

Extract existing orientation construction from `baseline.cpp` into a shared
solver implementation without changing its order or cardinal representatives.
New `solver/orientations.hpp` exposes:

```cpp
struct OrientationCatalog {
  std::uint64_t version{1};
  std::vector<geometry::Quaternion> quaternions;
};
struct CatalogFailure { std::string_view code; std::string message; };
using CatalogOutcome = std::variant<OrientationCatalog, CatalogFailure>;
CatalogOutcome make_orientation_catalog(
    const geometry::OrientationPolicy&, std::uint64_t max_orientations);
```

Version 1 retains fixed identity/specified quaternion, 24 lexicographically
sorted proper cube matrices, and canonical custom input order with exact
duplicate removal only. The context remains the authority for permissions.
For wire catalog identity, hash UTF-8 compact JSON
`{"version":1,"quaternions_xyzw":[...]}` with keys in that displayed order,
round-trip double values and positive zero. No geometry hash is invented.

New `solver/spectral.hpp` consumes the existing context, `GridLattice`,
`RunControl`, `SnapshotSink`, and optional initial valid handle:

```cpp
struct SpectralLimits {
  BaselineLimits baseline{}, spectral{};
  geometry::RepresentationLimits per_representation{};
  compute::CorrelationLimits per_correlation{};
  std::uint64_t max_working_bytes{512ULL << 20}, reserved_bytes{};
  std::uint64_t max_representation_kernel_work{1'300'000'000};
  std::uint64_t max_representation_cell_visits{200'000'000};
  std::uint64_t max_direct_terms{200'000'000};
  std::uint64_t max_proximity_terms{200'000'000};
  std::uint64_t max_refinement_evaluations{128};
};
struct SpectralStats {
  std::uint64_t correlations{}, unreliable_passes{}, pages_examined{};
  std::uint64_t discrete_rechecks{}, refinement_evaluations{};
  std::uint64_t representation_kernel_work{}, representation_cell_visits{};
  std::uint64_t direct_terms{}, proximity_terms{};
};
struct SpectralOutcome {
  BaselineOutcome run;
  RunStats baseline_stats;
  SpectralStats spectral_stats;
};
SpectralOutcome run_cpu_spectral(
    std::shared_ptr<const geometry::ValidationContext>, geometry::GridLattice,
    const SpectralLimits&, const RunControl&, SnapshotSink = {},
    std::shared_ptr<const geometry::ValidatedSolution> initial = {});
```

Run the baseline with its own explicit work slice and no external sink. Retain
its best, then offer that handle to the central incumbent before spectral trials.
Only central revisions reach the caller. Preserve baseline/initial even when
preparing the central wrapper fails; reuse `retained_solution` semantics. Never
replace an already published snapshot with a newly numbered inferior one.
`run.stats` aggregates common counters across both phases; `baseline_stats`
provides attribution. Work slices are additional bounded search effort, not
claims that the complete baseline found every grid placement. `run_cpu_spectral`
explicitly rejects upright/free before search; the existing baseline is unchanged.

First try extending the baseline best; then permit one empty initial spectral
trial for mixed-orientation greedy placement. There is no count controller,
random restart, removal/reinsertion, local rotation or multilevel pitch search.
All trials feed the same incumbent. No trial can reduce its published count.

Use T-005 fields on one lattice `(g,h)`. Kernel first `a` is never recentered:
integer shift `t` means physical anchor `g+h*t`, and kernel cell `j` occupies
global cell `a+j+t`. Derive a checked finite environment window from the
container's unchanged AABB, including one closed-cell exterior halo; existing
field factories own further clearance halos. Restrict fast-path translations
componentwise to `b-a <= t <= b+E-K-a`. Cells outside the window are blocked.
An empty legal domain still permits bounded physical refinement.

Build container blockers with wall clearance and placed blockers from actual
poses with full pair clearance. Use `BlockedField` reference counts, including
for baseline poses that are off-grid. Retain private immutable field handles;
never round an accepted pose to update a mask. Rebuild affected environment data
after insertion and discard stale pages/spectra. Request-local caches are bounded
by the same total live-memory budget; one orientation at a time is acceptable.

Ranking proximity is a grid approximation: for cell center `i`, let `d(i)` be
the Euclidean distance to the nearest blocked cell center, including virtual
blocked cells just outside the environment. Set `P(i)=exp(-d(i)/(2*h))`.
Use a bounded separable squared Euclidean distance transform or bounded direct
implementation, never an uncharged all-pairs loop. Correlate `P` with `O` and
divide by occupied kernel cell count. This is a ranking heuristic only.

Normalize candidate top height as
`(translation_z + oriented_bounds.max_z - container.min_z)/container.height`.
Rank ascending `0.65*normalized_top_height - 0.35*mean_proximity`; ties use
orientation index, then physical Z/Y/X without epsilon comparators. Return
32 candidates per orientation and merge page heads by this ordering. When a
page is rejected, advance its deterministic cursor and fetch the next page while
budget remains. A bounded top-32 scan/recomputation is acceptable; charge rescans.
Do not truncate a field permanently to its first 32 candidates.

For ordinary insertion, recompute exact integer occupancy at each shortlisted
translation; only zero-overlap proposals reach full-prefix `validate`. FFT zero
is never permission. When this path stalls, use a bounded refinement pool of
low-overlap grid proposals and physical container-wall/placed-AABB-face alignment
candidates (separate pair/wall offsets). Order low-overlap proposals by overlap,
then the ranking key; generate physical combinations lazily in stable Z/Y/X
order. These proposals bypass voxel veto but still require authoritative full
validation. Refinement never changes orientation outside the catalog. Count all
submitted proposals, rejected and indeterminate included, against work slices.

All allocation/work/index failures are operational outcomes. Unreliable
correlation terminates with `error` plus a specific diagnostic, retaining best;
resource limits use `resource_limit`, ordinary bounded exhaustion uses
`budget_exhausted`, and complete proposal exhaustion uses `search_stalled`.
None implies impossibility. Preserve T-006 safe-boundary stop/deadline precedence
and sink-failure behavior. Fixed-work replay has one thread and no deadline.

Count simultaneous accepted residency once, baseline/central/working snapshots,
trial vectors, catalog, fields/reference counts, correlation and proximity
scratch, pages and validation/score scratch. Clamp every nested call to remaining
work and bytes; cumulative geometry/representation/probe work cannot reset on
each orientation. The portable byte accounting is not a measurement of RSS.

## Result and export boundary

New I/O functionality builds schema version 1 output from an immutable
`ValidatedSolution` and explicit provenance, using existing `Json`,
`ContractValidator`, `Error` and `ValidatedDocument` types. It does not depend
on `NativeSnapshot`. The integration layer supplies solution revision, search
metadata and observed resource/time data. Proposed `io/result_export.hpp`:

```cpp
class VerifiedAsset;  // Private construction; immutable solid + provenance bytes.
using AssetLoadOutcome =
    std::variant<std::shared_ptr<const VerifiedAsset>, Error>;
AssetLoadOutcome load_accepted_asset(const std::filesystem::path& report_path);

struct ResultCatalog {
  std::uint64_t version{1};
  std::vector<geometry::Quaternion> quaternions;
};
struct ResultRequest {
  std::shared_ptr<const geometry::ValidatedSolution> solution;
  std::shared_ptr<const VerifiedAsset> object_asset, container_asset;
  ResultCatalog catalog;
  Json metadata;
  geometry::ValidationLimits validation_limits{};
};
using ResultOutcome = std::variant<ValidatedDocument, Error>;
ResultOutcome build_result(const ResultRequest&);

struct ExportRequest {
  std::shared_ptr<const geometry::ValidatedSolution> solution;
  std::shared_ptr<const VerifiedAsset> object_asset, container_asset;
  ResultCatalog catalog;
  ValidatedDocument result;
  std::filesystem::path result_path;
  std::optional<std::filesystem::path> stl_path;
  geometry::ValidationLimits validation_limits{};
  geometry::ImportLimits per_copy_import_limits{};
  std::uint64_t max_working_bytes{512ULL << 20};
  std::uint64_t max_output_bytes{8ULL << 30};
};
struct ExportSuccess {
  std::filesystem::path result_path;
  std::optional<std::filesystem::path> stl_path, companion_path;
};
using ExportOutcome = std::variant<ExportSuccess, Error>;
ExportOutcome export_result(const ExportRequest&);
```

`VerifiedAsset` exposes const getters `solid()` returning
`const shared_ptr<const geometry::AcceptedSolid>&`, `record()` returning
`const Json&`, and `resident_buffer_bytes()` returning a checked optional uint64.
It privately retains verified immutable source/PLY/repair bytes (or privately
owned snapshots with equivalent checked publication semantics), not untrusted
paths that can change unnoticed. Only `load_accepted_asset` constructs it; it is
not an independently caller-asserted `{solid,hash}` pair. Its accepted handle
must be pointer-identical to the solution context's corresponding asset.
Container handle is absent for analytic boxes. Charge provenance residency
through the solver's caller reserve and export's total live-memory allowance.
`VerifiedAsset::resident_buffer_bytes()` counts its retained provenance buffers
and record payload; it excludes the accepted solid's separately reported
`AcceptedSolid::resident_buffer_bytes()` so the integration layer counts that
shared solid once. Neither measure claims allocator overhead or process RSS.
Count retained JSON container and string payload, rather than using serialized
JSON length as a proxy. The loader caps a report at 16 MiB and each source/PLY
file at 256 MiB, also checking native allocation and stream-size ranges before
reading. These are operational admission limits, not changes to source geometry;
oversize, allocation and I/O failures return `Error`. Resolve artifact containment
across Windows reparse points, including junctions. Replay binding compares exact
source bounds and recorded source byte size as well as frames, hashes and counts.

The integration layer copies the actual ordered solver catalog into the value-only
`ResultCatalog`; I/O never depends on solver headers. I/O verifies version,
canonical signs/positive zero, uniqueness, policy completeness and placement
membership before recomputing its canonical hash. Fixed has exactly the selected
orientation; custom retains the complete canonical policy order; cube has all
24 proper symmetries in the versioned matrix order. Export repeats this binding
against the supplied result and verified handles: a `ValidatedDocument` created
by the general schema validator is not proof that `build_result` produced it.

`metadata` contains exactly identity (`schema_version`, `job_id`,
`solution_revision`, `created_at`, `engine`), `search`, and `metrics`.
The builder rejects other top-level keys. Metrics supply only measured
`time_to_best_seconds`, `peak_host_bytes`, `peak_device_bytes`,
`termination_reason`; builder derives the volume bundle, or three nulls.
It derives assets/label/count/placements/container/constraints/validation from
the solution/context and verified asset records, not caller assertions. Match source
frames, accepted canonical PLY hashes, dimensions, catalog hash and settings to
the actual handles; rebuild/validate the candidate independently before marking
the result valid. No placeholder hash, elapsed time or measured-memory claim.
Final JSON must pass the existing schema and semantic validator.

Canonical poses are existing active XYZW quaternions plus millimeter translations.
I/O derives row-major matrices operating on column vectors. Share one checked
rigid point/matrix conversion within geometry/export, matching the validator's
homogeneous normalized quaternion convention and exact cardinal behavior.
`source_to_local` remains `[sI,-anchor]`; compose placement on the left.
Round-trip precision is required; voxel pitch never enters pose recovery.
Native tests must include off-origin, non-mm source coordinates and a nontrivial
allowed quaternion, not only identity cube transforms.

Stream accepted full-resolution triangles in stable copy order and original
accepted face order, writing finite IEEE float32 little-endian STL coordinates.
Check `N*T <= UINT32_MAX` and `84+50*N*T` with checked arithmetic and the output
limit before writing. Attribute words are zero; normals are derived from written
coordinates. Zero copies produce a legal zero-triangle STL and empty ranges.
Source STL bytes, accepted PLY and input reports must never be overwritten.

Publish the verified source/PLY/repair artifacts first, then primary JSON, then
optional STL and companion to unique adjacent
temporary files. Close/flush STL, reopen its actual bytes and verify count,
length, finite coordinates and every copy's recorded triangle range. Every
coordinate must equal the expected float32 transformed accepted vertex; corruption
or silent geometry modification is an export failure. Checksum the completed
artifact. Only validated STL/companion may be published, then update JSON's
`assembled_stl` artifact atomically. An STL failure leaves primary JSON valid
and reports its retained path in `Error.details`; do not claim full success.
No multi-file atomicity claim: interrupted optional publication can leave an
unreferenced artifact, never JSON referencing an unvalidated STL.

Companion content: `schema_version:1`, `units:"mm"`, source and accepted-solid
SHA-256, assembly SHA-256, total triangle count, and ordered
`{copy_id, first_triangle, triangle_count}` ranges. Ranges are zero-based,
contiguous, non-overlapping and exhaust the file. The result stores its normal
`assembled_stl` artifact; companion naming is `<stl filename>.json`.

## Quantized per-copy geometry validation

The existing `validate(context,candidate)` prepares one object for all poses.
It cannot validate independently rounded copies. Do not import an assembly as
one solid, assume a rigid quantization error, or validate original poses twice.

Add `geometry/export_validation.hpp`, owned together with I/O/export:

```cpp
struct ExportReadFailure { std::string code, message; };
using ExportCopyRead = std::variant<std::vector<std::byte>, ExportReadFailure>;
using ExportCopyReader = std::function<ExportCopyRead(std::size_t copy_index)>;
struct ExportValidationLimits {
  ValidationLimits validation{};
  ImportLimits per_copy_import{};
  std::uint64_t max_working_bytes{512ULL << 20};
};
ValidationReport validate_quantized_export(
    std::shared_ptr<const ValidatedSolution>, const ExportCopyReader&,
    const ExportValidationLimits&);
```

The reader is synchronous, deterministic and backed by the closed staged STL:
it returns a standalone binary STL header/count plus exactly that copy's actual
triangle bytes. It transfers ownership of bytes, with no retained mutable alias.
Geometry loads at most two copies concurrently plus container and O(N) bounds;
it may reread ranges for candidate pairs. Work/import/pair limits are cumulative,
and all simultaneously retained buffers count. Reader errors/exceptions,
truncation or exhausted budgets yield indeterminate, never permission.

First freshly validate the original solution and its orientation permissions.
Then certify every quantized copy's finite mesh, topology, material/cavities and
containment in the unchanged container. Use the current import/analyze/kernel
implementation, not a second geometry engine. Exact vertex deduplication is
allowed; collapsed/removed faces, rewelding, smoothing, repair or altered shape
are export failure. Quantized copies are distinct solids for pair overlap and
full pair clearance, including enclosure/coincidence and touching-zero-clearance
cases. Their original orientation permission comes from the fresh source-pose
validation, not identity poses of baked world meshes.

An implementation may import each range as an object, then restore its import
anchor with an identity pose only after proving every reconstructed world
coordinate equals the read float32 value exactly. If recentering loses a value,
use a private already-analyzed world-mesh kernel path or return indeterminate;
never validate a further-rounded substitute. The private kernel already supports
`prepare`/`place` per distinct solid and pair/box/STL classifiers. Reuse those
classifiers while keeping this new public API report-only. Existing
`ValidatedSolution` cannot be constructed from these export meshes.

Only report `valid` after all copies, pair checks and container/wall checks
complete. Invalid or indeterminate refuses STL publication, retains JSON, and
gives increased-clearance advice for geometric quantization failures. Do not
nudge a placement or relax a constraint.

## Headless integration and test seams

The initial reachable workflow is synchronous CLI:

```text
spectrapack-engine solve --settings FILE --object-report FILE
  [--container-report FILE] --result FILE [--stl FILE]
```

Settings use the existing resolved shape. Accepted reports come from existing
`inspect`. I/O verifies the report schema, source SHA-256 and canonical accepted
PLY bytes/hash before use. Reconstruct acceptance from the preserved source and
explicit recorded repair operation/acceptance when applicable; compare the
reconstructed accepted mesh's canonical PLY bytes and frame to the report.
Never trust an `accepted` flag or arbitrary PLY without the acceptance checks.
Portable report-relative paths remain contained; content references in settings
must match loaded accepted assets. Relocate the needed source/PLY/repair artifacts
beside result JSON without changing their bytes, updating only portable paths.
Use content-addressed `assets/<sha256>.stl`, `.ply` and repair `.json` references.
Existing destination bytes must match the expected hash/content before reuse;
conflicting bytes refuse publication. Verify every repair record's explicit
operation/tolerance and acceptance token; absence is an error, never permission
to guess acceptance. Preserve internal repair references by copying all referenced
content-addressed original/candidate meshes too. `build_result` emits these fixed
portable paths; `export_result` publishes the corresponding retained bytes before
JSON. Result reload and portable-project APIs remain later work.

The CLI resolves native context/catalog, invokes `run_cpu_spectral`, constructs
provenance, and publishes JSON/STL using the boundary above. Divide the requested
candidate/pass budgets deterministically between baseline and spectral slices,
record the split in diagnostics, and ensure their sum cannot exceed the requested
total. The baseline receives the ceiling half and spectral the remainder; one
unit therefore funds baseline only. Fixed work reports deterministic seed string
and `rng.algorithm:"none"`, `state:"unused"`; do not fabricate random draws.
JSON work counts report actual sums. Wall-clock budgets remain optional and
do not establish fixed-work reproducibility or service stop latency.

Use public APIs for oracle, deterministic solver and headless artifact tests.
Private seams cover corruption at the real numeric checker, allocation failure,
page rejection/cursor progress, export read/write failures and quantization.
No mocked validator may establish packing/export acceptance. Tests can invoke
the page-selection helper directly for deterministic rejection scheduling, with
separate end-to-end tests requiring the real geometry validator.
