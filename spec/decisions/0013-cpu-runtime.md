# ADR 0013: CPU runtime, retained preparation and Start deadlines

Date: 2026-10-01. Status: core design accepted by the coordinating primary;
parallel-kernel selection and numerical targets remain explicit measurement gates.
This document specifies work, not implemented or qualified behavior.

Delivery ordering is superseded by [ADR 0014](0014-runtime-delivery-split.md):
T-011 serial foundations first, then T-010 threading after its user merge.
Existing mechanisms are under complexity review; this record is not a requirement
to retain an unnecessarily complicated implementation.

Requirements: SOL-02, SOL-05–SOL-08, GEO-04/GEO-06, UI-03, DATA-01–DATA-03;
T010-A1–A7, T011-A1–A7 and the retained T009-A2/A3/A6 obligations.

## Evidence and implementation order

The inspected baseline is merged T-009 at `b82117a`. `spectral_pipeline.cpp`
currently recreates voxel geometry, container masks, every retained-copy blocker,
occupancy and proximity for each orientation/page. `solve.cpp` loads/replays assets
before constructing its clock and Stop watcher. Rust `Core::start` invokes
`desktop-prepare` and then `solve`; both reconstruct the accepted solid.
`validate_quantized_export` imports the current copy and every preceding copy
inside the pair loop: 36 copies can cause 666 imports. This is a code-path count,
not a measured account of where the full36 failure occurs.

T-009's measured medium synthetic proximity time is 0.3695 ms of 13.6970 ms for
all fields; the large case is 1.4796 ms of 37.1546 ms. Those measurements cannot
justify choosing proximity as the useful first parallel lane. They do not split
rasterization, material classification, correlations, validation or preparation.
The original Ulamok/full-Pryanik spectral failures are not completed speed samples.

Implement in these distinct stages, retaining separate evidence:

1. Preserve the old Release executable, inputs, failure cases and cold/warm
   preparation samples. Add phase measurements and practical failing cases using
   production paths, without optimizing them. Freeze preparation targets first.
2. Implement serial workspace reuse and the bounded raster reduction below to
   make the original practical spectral workloads complete under existing caps.
   This is the prerequisite correctness/scalability change, not a threading win.
3. Freeze a clearly identified, successful serial revision and its phase/work,
   memory, ordered-pose and one-warmup/five-sample records. Freeze separate
   threading phase/end-to-end targets and a maximum memory increase before
   parallel implementation. A pre-existing supported practical profile may also
   supply a comparable baseline; disclose every difference.
4. Select and implement exactly one useful parallel lane. Retain the original
   h16 Ulamok full-catalog success gate (at least 36 copies and the existing
   first-pass 24-orientation/48-correlation assertions), supported-pitch advice,
   both full Pryanik workflows and full36 checked STL as independent gates.

## Boundaries and exact shared control

Dependency direction remains CLI → I/O + solver; solver → geometry + compute;
geometry and compute never depend on I/O, solver or one another. A dependency-free
header target is allowed for control values; it is not a job service or scheduler.

The native owner adds
`engine/common/include/spectrapack/runtime/operation_control.hpp` and the root
CMake interface target `spectrapack_runtime_values`, exporting that include root.
The integration owner consumes it. The agreed source interface is:

```cpp
namespace spectrapack::runtime {
using Clock = std::chrono::steady_clock;
enum class StopCause { none, user_stopped, deadline };
enum class Phase {
  loading, preparing, voxelizing, planning_fft, placing, improving,
  validating, saving, cleanup
};
using NowFunction = Clock::time_point (*)(void*) noexcept;
using PhaseSink = void (*)(void*, Phase) noexcept;
struct OperationControl {
  std::stop_token stop;
  std::optional<Clock::time_point> deadline;
  NowFunction now_fn{};       // nullptr selects Clock::now()
  void* now_context{};
  PhaseSink phase_sink{};     // called only by the serial coordinator
  void* phase_context{};
  [[nodiscard]] Clock::time_point now() const noexcept;
  [[nodiscard]] StopCause poll() const noexcept;
  void phase(Phase) const noexcept;
};
} // namespace spectrapack::runtime
namespace spectrapack::solver {
using RunControl = runtime::OperationControl;
}
```

`poll()` checks the stop token first, then `now() >= deadline`. Copies borrow
callback contexts; the operation owner keeps them alive until every worker joins.
The clock callback must support concurrent calls. Production phase callbacks must
be nonblocking and nonthrowing. Existing `{token, deadline}` initialization works.
Keep the current public solver overloads and `SnapshotSink` unchanged.

Append `const runtime::OperationControl& control = {}` to the existing full
factory/query signatures used in preparation and search: `inspect_stl`,
`propose_weld`, `prepare_voxel_geometry`, `voxelize_object`, `voxelize_placed`,
`voxelize_container`, `validate`, `revalidate`, and both CPU correlation methods.
Add controlled physical-query/display-LOD overloads wherever those calls perform
long work. Keep old call forms through defaults or forwarding wrappers; fix friend
declarations together. Do not add a solver dependency to geometry.

Apply the same final default control to the attempt-taking `make_blocked_field`,
`BlockedField::add` and `BlockedField::remove` forms. Poll bounded preparation
scans and immediately before committing a mutation. Interrupted mutation calls
preserve their input field, independently of the solver's outer transaction;
polling must not expose partially updated reference counts. Retain existing call
forms through defaults/wrappers. These native signatures require rebuilding
consumers but change no persisted or wire schema.

The integration-owned I/O signature is
`AssetLoadOutcome load_accepted_asset(const filesystem::path&, const
runtime::OperationControl& control = {}, const AssetLoadLimits& limits = {},
shared_ptr<const VerifiedAsset> reuse_candidate = {})`,
where `AssetLoadLimits::max_working_bytes` defaults to 512 MiB. Existing call
forms remain source compatible; the loader now rejects excess aggregate import
memory before allocation with an owned `MEMORY_LIMIT` cause. This bounded-loader
policy is a deliberate behavior change, not a promise that formerly unbounded
loads remain accepted. Apply the final optional control argument to `build_result`
and `export_result`. Propagate control into reads,
hashing, native import/repair replay and independent validation. Poll during bounded
read/hash chunks as well as before and after each heavy native call.

Operational interruption is distinct from an invalid solid: use owned error codes
`OPERATION_CANCELLED` and `DEADLINE_EXCEEDED`; geometry reports `indeterminate`
and no accepted/validated output when interrupted. Adapters map those causes to
`user_stopped`/`budget_exhausted`, never `INVALID_SOLID` or an impossibility result.
Preserve nested phase/resource codes and counters on genuine failures. Existing
resource, arithmetic and allocation checks still apply.

## Retained preparation and desktop adapter

The 2026-10-02 simplification shares a private typed solve entrypoint between the
auxiliary CLI and retained session. A request consumes validated settings, native
verified authority or scoped report paths, output paths and engine identity;
the outcome returns structured success metadata or an I/O error plus the CLI exit
code. CLI parsing/file admission remains in its adapter. The session passes its
validated in-memory request directly: no `session.settings.json`, synthetic argv,
captured terminal stream or JSON-text reparse. This private C++ change does not
change wire schemas, authoritative validation, result paths or budget semantics.
Runtime borrows the existing operation control and a monitor-status callback/context
so transport failure remains distinguishable from user Stop before publication.
The adapter owns monitor lifetime: standalone CLI may finish its search monitor,
while the session samples status and keeps its sole monitor alive through final
publication, then joins it at the existing operation boundary. Charge live
adapter/request owners; remove only the
obsolete serialization/file-roundtrip ownership allowances. No new lifecycle
framework or child owner is introduced by this refactor.

Add one auxiliary `desktop-session` child owned by Rust for the lifetime of the
open desktop session. It handles commands sequentially and retains native state
between Starts. Keep the existing public prepare/solve/restore commands working.
The public `serve` job methods, durable jobs, Continue and recovery stay deferred.
Changing capabilities to advertise the auxiliary command does not advertise those
unsupported service methods.

Use bounded UTF-8 NDJSON records (maximum 1 MiB) with
`runtime_version: 1`, `request_id`, `operation_id` for operation commands, and
`method`. Methods are `prepare`, `run`, `release`, `shutdown`. `prepare` accepts a
scoped report path and output directory, performs real native verification and
preview creation, and returns a process-local `asset_token`. `run` accepts that
token, the captured desktop request/settings, scoped output paths, its unique
Stop-marker path and the clock envelope below. Responses echo request/operation
identity; phase records carry operation identity and increasing sequence numbers.
Only small metadata/path records cross stdout; diagnostics use stderr. Put this
auxiliary schema in `spec/schemas/desktop-runtime.schema.json` and validate it on
both sides. There is at most one in-flight command and no unbounded input queue.

Rust serializes requests; a native per-operation marker watcher remains responsive
while the command thread works. Create the fresh stop source/watcher before any
loading/preparation. A process-global stop source cannot be reused: the current
`console_stop_source` would remain stopped after the first Stop. Repeated request
IDs replay the last bounded response only when the canonical request matches;
conflicting or expired IDs fail explicitly, never rerun a completed operation.
EOF/shutdown requests cancellation and joins work before releasing the session.

The native session owns at most one active object bundle (and, when used, one
container bundle). The internal CLI bundle is:

```cpp
struct PreparedAsset {
  std::string asset_token;  // native-generated, valid only in this process epoch
  std::shared_ptr<const io::VerifiedAsset> verified;
  std::shared_ptr<const geometry::DisplayLod> display; // existing display handle
  // Preview artifact paths/metadata are adapter-owned derivatives.
};
```

This bundle is internal to the CLI, not a cross-lane ABI. `VerifiedAsset` already pins source/report/PLY/repair
bytes and the immutable `AcceptedSolid`. Reuse that owner; do not construct an
accepted handle from cached JSON assertions. The key includes source and accepted
hashes, role, units/scale/frame, repair recipe hashes/version, native build and
preparation version. Display policy is an additional preview key only.

An optional native `reuse_candidate` is a performance hint for repeated prepare.
Its fast path requires the same normalized lexical/canonical report and artifact
paths, current file identities matching the candidate's pins, and exact actual
bytes for the report, source, accepted PLY and every repair artifact. Compare in
bounded controlled chunks; never accept a report's asserted hash as proof of the
current file contents. Require the same native build/preparation and display
policy. On an exact match return the same immutable `VerifiedAsset` owner and
share its existing display handle under a fresh session token. A changed identity,
path or byte is a cache miss into full verification; owned I/O, interruption and
memory errors propagate. No reuse hint is supplied for untrusted archive Open,
which still replays geometry and independently revalidates placements.

The session charges shared verified/solid/display owners once by native identity,
including both current and transactional previous bundles. Token metadata,
preview publication and comparison scratch remain separately charged. The loader's
remaining allowance excludes candidate residency already charged by its caller;
it covers any new reconstruction on a miss. No additional asset LRU is introduced.

Repeated Start references the pinned native token and bytes. It must not silently
reload a mutable path under the same token. New/replacement import, changed units,
repair or engine epoch releases/replaces the bundle after active work joins.
Open always verifies/replays the untrusted archive and revalidates its placements
before transactionally replacing state; archive metadata cannot mint a token.
Changed constraints/pitch/catalog preserve accepted geometry but create fresh job
derivatives. Eviction may discard preview/derivatives, never alter authority.

Bound residency by the unchanged effective host cap, including retained bundles,
active and previous result owners, preview/viewer reserve and operation workspace.
Allow one replacement bundle during transactional Open/import only if both fit;
otherwise fail while preserving old state. Do not retain an unbounded asset LRU.
Serialization/export keeps its existing independent native checks and pinned bytes.

Preparation admission must precede reconstruction and preview allocation. Append
`ImportLimits::max_working_bytes` with the compatibility default `UINT64_MAX`.
Expose `ImportAdmission { working_bytes_upper_bound, triangle_upper_bound }` and
`ImportAdmissionOutcome = variant<ImportAdmission, ImportFailure>` through
`estimate_import_admission(span<const byte>, const ImportLimits& = {}, const
runtime::OperationControl& = {})`. The query scans actual pinned bytes, not report
counts. Its bound excludes the caller-owned source byte buffer and includes the
decoded/retained draft, parsing and deduplication storage, topology/BVH scratch,
growth overlaps and report storage. `inspect_stl` enforces the same checked bound
before parsing mesh allocations; rejection uses `MEMORY_LIMIT` with reason
`IMPORT_WORKING_BYTES`. Overflow rejects. Existing callers retain their old
default allowance; the retained adapter explicitly supplies the remaining cap.

The adapter subtracts existing bundle/result owners and its declared reserve before
calling the loader. The loader charges incoming pinned artifacts, JSON parsing and
its own scratch before passing the remaining allowance to native import. Callers
that load multiple bundles, including standalone solve/restore, subtract already
live owners instead of granting each load an independent full allowance. Preserve
the nested native memory reason rather than relabeling it as an asset mismatch.
Repair replay receives the remaining allowance
and includes distinct retained original/candidate owners. Display generation uses
its existing `RepresentationLimits::reserved_bytes`; preview serialization checks
its computed byte requirement and growth before allocation. A failed replacement
preserves the old token and complete state. The native bound requires a documented
capacity/lifetime audit and below-bound tests. The locked parser releases its
decoded/deduplicated temporary payload before solid analysis. Admission takes the
maximum of the parser and analyzer phases: 1,664 bytes per triangle plus five
times ASCII source bytes for parsing, or 1,792 bytes per triangle for analysis,
including Debug nested-vector proxies. Both add 64 KiB and 1,024 bytes per allowed
diagnostic example. The table and width guards live in `import_admission.hpp`.
Full Pryanik 2 requires 249,598,976 native payload bytes. Caller pins, allocator
overhead, library stacks and measured process RSS remain separate accounting;
raising the 512 MiB host cap is not permitted. Changes to allocation policy or
these lifetime boundaries require updating the audit and boundary tests.

Append `WeldOptions::max_working_bytes` with compatibility default `UINT64_MAX`.
Expose `estimate_weld_admission(const AssetDraft&, const WeldOptions& = {},
const runtime::OperationControl& = {})` returning `ImportAdmissionOutcome`.
Weld admission derives its bound from the actual retained vertex/triangle spans,
includes the original draft payload once plus proposal scratch/candidate storage,
and excludes caller-owned pinned artifacts. Enforce the minimum of the original
draft's import allowance and the explicit weld allowance. The I/O caller subtracts
other owners and pinned artifacts; it does not subtract the original again when
passing an allowance whose native bound already includes that owner. This retained
original allowance is conservative and may reject a later larger repair request.

Before constructing the runtime request DOM, a bounded SAX pass admits at most
2,048 value nodes, 64 KiB of decoded key/string bytes and depth 32, in addition to
the 1 MiB wire limit. Auxiliary contract parsing may use source-compatible
per-call diagnostic limits (at most 16 retained issues and 256 UTF-8-safe bytes
per path/message); default public parsing preserves existing diagnostics.
Truncation limits diagnostic storage only and must never turn a validation error
into acceptance, including a zero diagnostic allowance. Keep the 1 MiB terminal
record capability for admitted path metadata. Audit simultaneous raw/canonical
records, DOM copies, diagnostics, writer queue and schema-validator ownership
against the adapter reserve before accepting the integrated memory gate.

Propagate the diagnostic policy explicitly through I/O: append
`ContractDiagnosticLimits diagnostic_limits{}` to `AssetLoadLimits`,
`ResultRequest`, `ExportRequest` and `InspectRequest`, preserving existing aggregate
call forms and public defaults (32 issues, unlimited diagnostic string bytes).
The retained session, its private `SolveRuntime` and bounded auxiliary CLI paths
pass `{16,256}` through object/container loading, result building and export.
This is an operation policy, not a saved setting or provenance change. Apply it
when rejection diagnostics are retained; do not truncate authoritative inspection
reports or alter repair recipe bytes, validity or counters. Recipe parsing still
requires independent lexer scratch admission. Bound both lexer token buffers,
capacity growth and error formatting before allocation; a SAX callback after a
large token has been built does not provide that bound.

The auxiliary `desktop-session` admission format additionally limits each numeric
lexeme to 128 ASCII characters. Shipped desktop number encodings fit this bound;
longer spellings receive `INVALID_RUNTIME_RECORD`. Ordinary public settings,
project, report and recipe JSON retains its existing numeric syntax. This is a
new auxiliary resource bound within the unreleased runtime protocol, not a change
to persisted number values or authoritative geometry.

Before invoking the auxiliary lexer, a nonallocating admission pass may collapse
runs of JSON whitespace outside strings to one space, preserving token separation,
and enforce the existing 64 KiB aggregate decoded-string allowance and the numeric
lexeme bound. Preserve every byte inside strings. Reserve admitted normalized
storage before writing it; growth overlap belongs in the memory ledger. The pinned
JSON parser remains responsible for full syntax validation. Invalid input must
not become valid through normalization. Check escaped/surrogate string boundaries,
numeric-length boundaries, token separation and the large-whitespace malformed
input that otherwise expands the lexer's error text.

The initial cold prepare reserves 16 MiB for bounded adapter records/DOM/writer
metadata. Transactional replacement additionally reserves 64 MiB for the still-live
viewer, plus all distinct native old owners. Actual preview payload/staging receives
separate checked admission; these reserves are not permission to allocate a full
64 MiB preview without accounting. Start retains its 64 MiB viewer reserve.
Use the same 16 MiB adapter allowance for each overlapping session/helper adapter;
the host cap remains 512 MiB. The earlier 8 MiB proposal was increased after the
simultaneous record/DOM/diagnostic audit. Reserved headroom for fixed embedded
schema catalogs and compiled validators still needs a supported ownership bound;
source JSON byte count alone is not that bound. Process peak measurements remain
separate from portable payload admission.

Auxiliary inspect/prepare/restore/export work may overlap retained session owners.
Those processes cannot each claim a fresh full host allowance. Add an optional
reducing-only `--available-host-bytes` argument to `inspect`, `desktop-prepare`
and `desktop-restore` (including its checked-export path): integer 1 through
512 MiB, default 512 MiB. Propagate it through loading, LOD, validation, result
building and export. This operational allowance does not rewrite saved settings,
geometry, thread policy or host-cap provenance.

Completed session `prepare`, `run` and `release` responses, including owned
failures, carry optional envelope `retained_native_bytes`: a checked conservative
portable-payload bound for persistent verified buffers, accepted solids, display
handles and retained solution/context/catalog owners after ownership settles.
Deduplicate public owner identities. Adapter/viewer reserves are excluded and
charged separately. Rust
tracks this value per child epoch and subtracts it before launching an auxiliary
native process. Missing or unrepresentable residency is never zero; reject the
auxiliary operation if a safe remaining allowance cannot be established. Busy,
phase and malformed-input records may omit the field; do not inspect concurrently
changing ownership to decorate such records. The fixed engine hash lets Rust
require the field on completed mutating operations. Replacement failure preserves
the prior complete state and its ownership; test this accounting transaction.

Native `ValidationContext::resident_buffer_bytes()` and
`ValidatedSolution::resident_buffer_bytes()` return `optional<uint64_t>` without
allocation or exceptions. The context query includes its own wrapper/storage and
orientation capacity, excluding accepted-solid owners. The solution query includes
its own solution/candidate storage, copy poses/ID capacities and owned validation
report data, excluding the context and accepted solids. Overflow is unavailable
residency, not zero. The adapter deduplicates these owner identities separately.
Revalidation can create a distinct solution handle sharing candidate storage;
summing both query values may conservatively count that storage twice. The
session retains one solution, and no extra Candidate-identity ABI is introduced.
These are portable payload bounds, not exact unique-allocation measurements;
allocator headers/control blocks and process RSS retain their separately declared
accounting convention. A conservative overlap must never become an undercount.

## Start clock, Stop and terminal ownership

Rust captures `QueryPerformanceCounter` and its frequency during successful Start
registration, before spawning/sending work. Transfer decimal uint64 strings
`start_qpc_ticks`, `qpc_frequency_hz`; the captured settings supply the budget.
Native verifies the frequency against its own Windows counter and rejects malformed,
future or overflowing anchors. It does not restart the allowance on receipt.

Map to native steady time by sampling `steady_before`, then QPC. Compute elapsed
ticks from the transferred anchor using checked arithmetic; set native deadline
to `steady_before + (budget - elapsed)` with rounding toward earlier expiry.
Negative remainder means already expired. This charges queue, process startup and
transit time; the conservative sampling interval cannot extend the budget.
For the standalone CLI capture its monotonic Start before settings/file loading.
Inject the common clock for deterministic tests; never use UTC timestamps as budgets.

The deadline covers job-specific loading, preparation, FFT work and validation.
Prior completed import/preview diagnostics are separate. At deadline or Stop,
launch no new search work; discard an unfinished candidate. Already validated
snapshots remain immutable. Perform bounded joining and final publication cleanup;
measure it separately. Independent final revalidation remains required. Give that
cleanup an explicit fresh control limited to the remaining qualified cleanup bound,
without reopening search or resetting the run budget. If it cannot finish, retain
the native last validated handle and previous complete disk/UI result, and report
publication failure explicitly; do not fabricate a new valid result.

Stop acknowledgement is Rust's successful operation-scoped marker write within
250 ms. Transport/marker failure is an error, not acknowledged cancellation.
`stopping` persists until safe native completion. Qualification requires ordinary
phases and cleanup to finish within 5 s; forced termination is a separate failure
path, never cooperative-Stop evidence. Noninterruptible library calls require
measured bounded duration on every qualified profile or an implementation change.

Before any new valid nonempty incumbent exists, Stop/deadline preserves the previous
complete result and reports `no_nonempty_incumbent`. On a first run it leaves no
result unless an explicit empty candidate has actually passed native validation.
An active job's validated empty snapshot does not erase an older complete displayed
result merely because preparation/search was interrupted. Once a nonempty result
is validated, retain it on failure. A result always retains its original settings.

Only the operation coordinator invokes the snapshot sink, assigns revisions,
commits reference counts and publishes terminal artifacts. Rust additionally checks
session epoch, operation ID and sequence before replacing state. Late events from
old operations are ignored. Workers join before terminal success/failure or input
release; no worker performs I/O or mutates the incumbent.

## Field workspace and reduced raster work

Make `solver::detail::SpectralWorkspace` a move-only internal owner created once
by `run_cpu_spectral`; pass it to `build_spectral_pipeline`. It retains:

- immutable voxel geometry and container mask keyed by context/lattice/clearances;
- one coordinator-owned `BlockedField`, per-copy footprints and exact pose identities;
- immutable occupancy/proximity buffers for one committed layout revision;
- bounded orientation kernel entries keyed by accepted geometry, lattice, quaternion
  and representation version; a deterministic LRU with byte admission;
- correlation/ranking arrays for at most the currently processed orientation,
  keyed additionally by layout revision. Cursor/page mode does not alter fields.

Build retained-copy blockers once, not once per orientation. Transactionally update
only changed copy footprints after a layout change; preserve overlapping reference
counts. A removed/replaced copy invalidates occupancy, proximity, correlation and
candidate pages, not source geometry/container preparation. Rejected candidates
leave all committed fields untouched. Cache hit counters cannot replace real work
counts: charge actual construction, updates, lookup/iteration and recomputation.
No arrays survive across Starts in this first bounded workspace design.

The concrete transaction seam is an additive const native
`BlockedField::clone(limits, attempt, control = {})`, returning
`variant<unique_ptr<BlockedField>, RepresentationFailure>`. It shares only the
immutable container mask and copies the reference counts, compressed per-copy
index footprints and IDs into independently admitted PMR storage. Admit source
and staging ownership simultaneously, including growth overlap, before allocation;
charge actual count/index/ID copy and lookup work. Poll long copies in bounded
chunks. Allocation failure, work exhaustion or cancellation leaves the source
unchanged. The initial empty workspace needs no redundant clone, and unchanged
layouts must not clone on each orientation or page. After changed-copy operations
succeed and a final control check passes, commit the blocked field, exact pose
identities and layout revision together, invalidating dependent arrays. Geometry
and placement authority remain unchanged; normal native consumers rebuild.

Keep at most two orientation entries in a deterministic byte-admitted LRU and one
current correlation pair. Eviction cannot leave unaccounted aliases. Expanding
the fixed residency ledger from 24 to 32 tokens requires an explicit simultaneous
owner count, with overflow still rejected; aggregate PMR footprints do not become
one token per copy. Test overlapping copies, removal/replacement, actual work,
late allocation/Stop rollback and below/exact memory admission through this seam.

Full Pryanik can remain over the work cap even after orientation reuse; diagnose
actual raster/classification counters before declaring the serial gate fixed.
The approved bounded raster candidate is an early existing-boundary test: once a
cell has the conservative boundary tag, further triangles cannot make it less
occupied, so omit their triangle/cell SAT work for that cell. Preserve triangle
order, closed-cell contact and unknown-as-occupied semantics. Count actual cell
visits and the new lookup work; keep full SAT charges for SAT calls performed.
Audit/update the work revision and lower-bound/advice code together. The old
`27 * 180` per-triangle floor is invalid if SAT work is skipped; deleting only that
diagnostic would not fix execution. Do not simply lower costs for unchanged work.
If this does not suffice, report the bounded remaining kernel; broader geometry
algorithm changes need a focused amendment/review, not a cap increase.

Object-grid admission must enclose the actual translated interval window. Exact
cardinal rotations alone do not justify a fixed `ceil(width / pitch) + padding`
bound at arbitrarily large lattice origins. Add the read-only native helper
`estimate_object_window(const AcceptedSolid&, GridLattice, Quaternion) noexcept`
returning `optional<GridWindow>` in the conservative-field API. It uses the
existing homogeneous rotation and transformed-interval arithmetic on the eight
source-AABB corners at the actual lattice origin, with the production trim and
overflow rules. It allocates no mesh and returns no bound for unsupported floating
environments or unrepresentable input. Its enclosure must cover the cardinal
cuboid fast path as well as the ordinary placed-vertex path. A failed estimate
cannot fall back to an unproved narrower bound. Cover every admitted catalog
orientation, including axis-dependent translation widening in cube mode.

This is an additive native admission API requiring normal consumer rebuilds;
persisted projects, placement/solid authority and legacy validation work revisions
are unchanged. Preserve the original ranked-orientation cell cap and add a red
large-origin enclosure case, then check positive/negative origins, adjacent pitch
boundaries and cardinal permutations against actual produced field windows.

### Certified placed-field support reduction

The serial workspace still exhausts the unchanged 1.3B work cap at the actual
4 mm Ulamok advice after 46 of 48 correlations. The 16 mm profile and both full
Pryanik retained-two profiles pass their current Release trials; these are
distinct observations, not a successful advised-pitch or timing baseline.
Following selective geometry review, approve this bounded proposal-field change
under GEO-04/GEO-06, SOL-02 and AT-09/AT-12/AT-16 before implementation.

Preserve the requested output window R, existing clearance stencil and its halo
H, and all current input/expanded-window W = expand(R,H) checks and cell caps.
Admit the existing owners and place the solid once. Derive B from that immutable
placed solid's actual conservative bounds, including its vertex intervals or
certified cardinal-cuboid path. Do not substitute physical translation for the
lattice origin. Form a candidate support S with the existing checked padded trim
and intersect it with W, but do not treat padding as proof of exterior material.

First require finite outward cells at W's first and last diagonal indices; checked
index conversion, positive pitch and monotone outward arithmetic then enclose all
cells between them. For each actually omitted lower/upper slab, certify its nearest
omitted cell strictly below/above B on that axis using `outward_grid_cell`. Strict
separation extends to every farther cell in the slab; the six slabs cover W minus
the cropped window. Equality, unsupported arithmetic or unavailable certificates
select the original W. An empty intersection permits a full-R zero blocker only
after a separate strict separation certificate for the whole outward W envelope.
Never represent uncertified emptiness by a zero-shape window. Cancellation or work
exhaustion remains failure and cannot be erased by numerical fallback.

Rasterize the selected domain in the original triangle order and classify every
surface-free component against the full placed solid. An artificial crop edge is
never an exterior seed. Conservative boundary tags separate material transitions;
narrowing the domain may split components but does not invalidate a full-solid
witness for each resulting component. Current boundary, occupied and indeterminate
cells remain dilation seeds. Every stencil offset is bounded by H, so seeds outside
W cannot reach R; independently certified omitted cells inside W contain no material.
Retain the full requested output, physical pose, source and clearance ownership.

Explicitly allow conservative reclassification of proposal fields: a different
witness or a certified exterior support region can change historical false-positive
bits. Neither universal legacy bit equality nor a subset relation is promised.
Every newly free region still requires a current support/component proof; a crop
edge alone never authorizes it. Work/uncertainty counts, candidate ranking and
heuristic poses may differ across builds. Same-build/configuration determinism is
unchanged. Raw uncertainty counts describe the domain actually classified. Record
private policy `certified-placed-support-v1` separately from the existing raster
work revision in native benchmark provenance. Per-Start derivative caches are
disposable; source/repair hashes, validator revisions, schemas, saved projects,
physical dimensions, container masks and validity/export authority are unchanged.

Implement private geometry helpers, with no new public signature. Pass the already
placed immutable owner into raw-field construction. Keep an explicit source-local
full-window reference seam that bypasses the planner and executes the old behavior;
do not use a mutable global switch. Admit simultaneous raw cells/labels/queue and
raw/stencil/full output, placed geometry, IDs and publication metadata. Freed scratch
still contributes to the successful admitted maximum. Charge actual new certificate
arithmetic, scans including skipped-zero cells, full output initialization and all
existing executed work. Keep bounded polls and transactional publication; do not
claim interruption inside an uninterruptible allocation/initialization operation.

First observe the old resource failure on a sparse analytic placed-field case,
then compare every output cell against an independent closed-cell/Euclidean-dilation
oracle and the old full-window path. Require equality on ordinary determinate
cases; explain any uncertainty-driven difference with an independent exterior or
occupancy proof. Cover rotated/asymmetric and hollow/thin/disconnected solids,
windows wholly inside material, off-grid poses, large origins/near-limit indices,
strict-separation ambiguity, empty/fallback paths and support entering through a
clearance halo. Verify cap/overflow/allocation/Stop/deadline rollback in Debug and
Release. Execute the actual returned Ulamok pitch through the full catalog while
retaining at least 36 native-valid copies, and preserve both full Pryanik cases.
Only then freeze a successful serial repeated benchmark. This decision provides
no sufficient pitch-advice bound, hidden trial/retry, cap increase or threading
authorization; audit changed necessary-floor terms without reversing their meaning.

## CPU policy and aggregate admission

Add `std::uint32_t cpu_thread_count{1}` to `SpectralLimits`. Native integration
accepts explicit integers from 1 through `min(8, max(1, hardware_concurrency()))`;
new desktop requests default to `min(4, supported_max)`. Resolve once and report
the actual count; never silently reduce an explicit request after memory failure.
Legacy resolved settings continue to mean exactly one thread. The cap of eight is
a bounded product policy, not a measured optimal setting.

Kernel choice is still pending serial measurements. Prefer a measured dominant
lane that can own disjoint output regions with bounded scratch. Candidate order:
field tiles if raster/fill dominates; batched separable FFT lines if transforms
dominate; orientation preparation only if the fixed practical catalog supplies
enough work. Validation parallelism requires a separate authority/concurrency
review and is not the default. Do not select by thread utilization alone.

Whichever lane is selected, its native owner creates one operation-lifetime team:
`N-1` workers plus the calling coordinator participating in kernel work. Other
stages remain serial. At most `N` active tiles and `2*N` queued descriptors;
descriptors borrow pinned input and contain no full-grid buffers. Use fixed
indexed tiles/lines, deterministic assignment and an axis/stage barrier. Record
the concrete lane, tile size and scheduling version before implementation.
Merge reports in index order; a failed tile discards the entire output generation.
Same-build/same-thread fixed-work reproducibility is mandatory. Preserve ranking
tie order; do not weaken exact/discrete/solid rechecks.

Keep pocketfft cache/vector/multithreading macros unchanged unless FFT is selected.
If selected, prefer externally batched independent lines with bounded cancellation
points; audit all per-worker plan/line scratch and revise the estimator first.
Never enable a hidden nested pocketfft team or multiple full correlations without
aggregate admission. Native field work and FFT cannot create concurrent teams.

Memory admission is a checked union of unique retained owners plus caller/viewer
reserve, job fields, retained result, all simultaneous worker scratch, queue/result
descriptors, FFT buffers/plans, validation and staging. Include thread stack reserve
as a separately reported conservative host reservation; portable payload tracking
is not RSS. Extend the fixed 24-block residency ledger deliberately or use a bounded
dynamic ledger charged before allocation; silently dropping excess entries is invalid.
Check cap-before-allocation and current residency during growth; measured RSS and
the estimate remain separate evidence. Workers receive pre-admitted finite work
allowances; unused work is reconciled at ordered barriers. Their combined allowance
cannot exceed the existing aggregate cap. Allocation failure cancels/joins siblings,
discards the partial generation and retains the last valid solution.

Failure reporting must remain safe when allocation also fails during diagnostic
construction or transfer. [ADR 0015](0015-computational-failures.md) supersedes the
earlier owning-diagnostic copies/prvalue-finalizer mechanism and native DTO layout
constraint with static computational descriptors, explicit native compatibility
migration and narrow vector-transfer safety. Preserve actual causes, completed
counters and the latest validated handles; admit simultaneous metadata before
dynamic work and verify transient/persistent failure and non-elided transfers.
Wire schemas, accepted geometry and placement authority remain unchanged.

Expose an optional `SpectralOutcome::field_admission` with
`CpuFieldAdmissionEstimate { working_bytes_upper_bound, footprint_copy_count,
effective_host_cap_bytes, cpu_thread_count, scheduling_policy }`. Populate it
only after successful admission for an actual retained layout; ignore speculative
empty-layout preflight. Across field generations retain the maximum admitted byte
estimate with its associated copy/thread/policy basis. Result/terminal adapters
copy the policy string and preserve that basis. Label this as the field-phase
estimate for that copy and thread count; show prepared residency and the host cap
separately. It is not an estimate of an unknown final Start copy count. Every later
growth still requires admission including all simultaneously live owners.

Derive these recorded bounds from enforced admission for the actual layout and
current orientation. Correlation admission may use the existing CPU estimator
plus the deduplicated live owners and bounded query/ranking/worker reserves.
Do not substitute a speculative maximum across every catalog orientation and
full-environment footprint for this actual-generation calculation. Include earlier
geometry, voxelization and transactional staging admissions in the maximum; their
scratch may already be released when correlation begins. Each recorded candidate
bound must be conservative and enforced before its relevant growth, with the same
effective allowance and associated copy/thread/policy basis. A measured allocation
peak alone is not a pre-allocation upper bound. This refines observational metadata
without changing the requirement to admit every allocation or claiming a whole-Start
bound.

Append native `RepresentationAttemptStats::admitted_bytes_upper_bound` as a
`uint64_t`, with zero meaning no complete admitted bound. Record only successful
pre-allocation checks: the kernel budget's planned live-byte maxima and the PMR
resource's checked prospective growth before calling its upstream allocator.
Combine complete input and caller ownership with those values through the attempt
scope; reject overflow and preserve deduplication. A refused request cannot raise
this value. A successfully admitted allocation that subsequently fails may retain
its admitted bound. Existing `working_bytes_peak` remains separate accounting
evidence; legacy import/validation work counters and the public field DTO do not
change. This additive native stats member requires normal consumer rebuilds.

The wire representation is the closed, versioned
`results.schema.json#/definitions/cpu_field_admission` object with required
`version:1` and the five native fields above. Byte/copy values are existing safe
integer counters; `cpu_thread_count` is 1 through 8; `scheduling_policy` is an owned
string of 1 through 128 bytes. The effective host cap is at most 536,870,912 bytes,
and semantic validation requires the admitted byte bound not to exceed it.
An optional `runtime.field_admission` references this definition, so both
`search.runtime` and each segment's runtime retain the complete basis. Native
terminal runtime uses the same DTO, including owned failures only when an actual
admission occurred. The associated copy count need not equal the final count.
Stored estimates are observational metadata, never authority for a subsequent
allocation, placement or restored project.

Desktop `State.cpu_runtime` gains optional `prepared_resident_bytes`,
`field_admission` and `host_cap_bytes:536870912`. This last value is the total host
policy cap; the field DTO retains the operation's actual effective allowance.
Emit prepared bytes under their explicit name. Continue accepting the old optional
`working_set_estimate_bytes` desktop field for compatibility, but stop emitting it
or interpreting it as a whole-job estimate. Show the displayed result's field basis
from its own saved runtime metadata, separately from prepared residency and pending
settings. Absence means unknown. Outer schema versions and legacy documents remain
unchanged; as with the other optional runtime additions, old strict readers may
reject new metadata while new readers continue accepting old documents.

## Checked STL reduction

Keep fresh source-solution revalidation and actual quantized per-copy solid and
containment checks. While reading/importing each copy, retain only its certified
actual-coordinate outward bounds and copy index (O(copy count)). Before importing
an earlier copy for a pair, use these bounds to prove strict separation greater
than the requested pair clearance on at least one axis. Use outward/interval or
existing exact difference comparison; equality, uncertainty, enclosure and overlapping
bounds fall through to the unchanged exact solid/distance kernel. Never use source
pose bounds, voxels or displayed geometry as the quantized-copy bound.

For the separated Ulamok layout this can remove repeated prior-copy imports without
retaining 36 meshes. Every copy still undergoes its initial actual-quantized solid
check, every pair consumes/checks the pair budget, and nonseparated pairs retain the
full fallback. A bounded cache for near pairs is optional only after evidence; no
unbounded N-times-mesh allocation. Qualify the actual full36 case before claiming
that remaining single-copy import work fits.

I/O must pin the same staged file identity/content for bounds, repeated reads,
checksum and publication. Hold a Windows handle excluding external write/delete
during validation, or provide an equivalently verified immutable stage. Reopening
a mutable path and trusting earlier bounds is insufficient. Mutation/fallback tests
must remain; original validation and per-copy aggregate caps are unchanged.

### Bounded quantized self-intersection certificate

The `19736a5` practical checkpoint passes Ulamok36 and full Pryanik1 retained2
exports, but Pryanik2 spends 944,182,452 predicate units on first-copy
self-intersection and exhausts the aggregate cap on its second actual copy.
Introduce a private operation-local `projected_separation_v1` policy only for
baked-world export imports, routed from the existing `baked_world` flag through
solid analysis to the common exact predicate implementation. Ordinary source
import, weld/repair replay and placement validation retain the legacy policy and
counters. Retained repair recipes embed these diagnostics and require exact byte
replay; a global predicate change would break their compatibility. No recipe
migration, global switch or thread-local policy is introduced.

The proposed certificate chooses one coordinate projection using a single
approximate triangle normal (largest absolute component, ties X/Y/Z). This selects
an attempted proof only. Nonfinite selection falls back. An exact nonzero
projected triangle orientation certifies a usable projection; zero or uncertainty
cannot authorize a shortcut. Try at most the three cyclic triangle edges; for one
shared vertex consider its incident edges, and for two consider their shared edge.
Every nonshared second-triangle vertex must be strictly opposite the first
triangle's interior half-plane. This proves disjointness, or intersection confined
to the actual permitted shared feature. Shared indices must be in range/distinct
with exact represented-coordinate correspondence, including signed-zero equality.
Degenerate projections, extra zero signs and unsuccessful separation use the
unchanged classifier. Exhausted work or Stop returns uncertainty/interruption.

Charge the selection, index/equality/sign checks and every attempted exact
orientation, including unsuccessful attempts; preserve candidate-pair accounting.
The certificate uses constant stack scratch and no retained geometry cache.
Its net savings are not yet qualified. Require analytic/adversarial cases,
permutation and exact-reference comparisons, budget/Stop/FP-environment checks,
and the existing two-copy Pryanik2 export green with identical bytes/poses/caps.
Measure hit rates, failed-attempt overhead and total actual work. Keep historical
reports and baseline logs unchanged; this private export policy has its own
method/build provenance and cannot be used to relabel source-import evidence.

## Wire compatibility and timing fields

Keep outer existing schema versions and old fields' meanings. Add explicitly
versioned optional runtime members, generated before consumers. Old strict binaries
may reject new members; new readers accept old documents. No silent retroactive
change to old result timing or settings is permitted.

- `settings.search.budget_scope`: optional enum `search_only` / `total_start`.
  Absence means legacy `search_only`; all new desktop Starts write `total_start`.
  A loaded legacy result remains unchanged; creating a new Start constructs fresh
  explicit settings, rather than relabeling the old result.
- `settings.compute.thread_count`: optional positive integer request; resolved
  `thread_count` remains actual execution count. Add
  `resolved.cpu_runtime = {version:1, scheduling_policy:string}`. Absence means
  legacy serial policy and requires resolved count 1 for execution in this build.
  Reject unknown runtime/scheduling versions explicitly.
- Desktop pending settings gain optional `thread_count` and `budget_scope` with
  the same legacy parsing rules. Capability/state reports expose supported min/max,
  requested/resolved count and conservative working-set estimate.
- Add optional `search.runtime` and per-segment `runtime` with
  `{version:1, budget_scope, measurement_boundary:"before_result_commit",
  total_elapsed_seconds, native_elapsed_seconds,
  preparation_seconds, search_seconds, validation_seconds, publication_seconds,
  cleanup_seconds, deadline_overrun_seconds, preparation_reused,
  no_nonempty_incumbent, phases:[{phase, elapsed_seconds}]}`.
  Nonnegative finite seconds only; phases are exclusive wall-time totals (no
  summing concurrent worker CPU times). Existing `search.elapsed_seconds` and
  segment `elapsed_seconds` retain search-only interpretation. Stored `total_elapsed`
  measures Start through preparation of the final result commit. An immutable file
  cannot include the duration of its own eventual atomic commit. Native terminal
  response and Rust operation state therefore carry separate completion timing
  measured after publication and after final UI receipt, respectively. Present the
  operation completion total as the live total; label saved result timing with its
  boundary. Do not rewrite native result bytes to disguise this distinction.
  The I/O adapter may receive a bounded timing finalizer that refreshes only the
  optional runtime metadata after independent validation and asset staging,
  immediately before the primary result commit. It cannot change placements,
  physical constraints, termination cause or validated authority. Its temporary
  metadata copy is included in the unchanged host-memory admission.
  `deadline_overrun=max(0,total-budget)` at the declared boundary;
  fixed-work-only runs use zero and record absence of a time deadline in settings.

Use the existing result termination reason for Stop/deadline/resource/error; runtime
data does not replace nested diagnostics. Persist runtime data through JSON and
Save/move/Open. Preserve the T-009 binary64 round-trip fix and result/pending-settings
association. Import/export outside Start have their own operation timing.

## Exclusive ownership and practical tests

Native owner: common control header and root include target; `pack_compute`,
`pack_solver`, `pack_geometry`, their CMake/tests and native characterization.
Integration owner: CLI/session/solve/desktop adapters, `pack_io` controls/exports,
`engine/service/src/service.cpp` capability advertisement only, desktop Rust/React,
schemas/generated types, contract/protocol/journey tests. Integration owns all
`solve.cpp` and `desktop.cpp` edits. Native provides requested adapter changes as
handoff. Architect owns this ADR. Shared control/header declarations land before
both implementation lanes consume them; kernel choice is a later bounded freeze.

Practical red/green sequence, using existing configured runners:

1. Keep the recorded two-thread CLI rejection as T010-A1 red. Add production
   preparation deadline/Stop reproducers and assert no reset budget, no fabricated
   output, bounded completion and preserved prior result (T011-A1/A4/A5).
2. Compare workspace fields at every cell against existing direct analytic oracles
   and a noncached reference path; add overlap/removal/replacement/cursor and
   nonzero-origin/rotation/halo cases. Compare the raster short-circuit against the
   old full SAT implementation on small fixtures and independent analytic solids.
   Run the retained h16 full24 catalog and both full-Pryanik workflows under caps.
3. At 1/2/4 threads run every affected correlation/proximity/field oracle, repeated
   fixed-work ordered transforms, cap immediately below admission, active-worker
   allocation failure and cancellation at each real heavy phase (T010-A2–A5).
4. Repeated native-session Starts demonstrate no equivalent accepted replay/LOD
   work, with real counters and timing; exercise New, replacement, epoch/units/repair,
   constraints/pitch/catalog changes, eviction and every archive tamper input.
5. Export: analytic separated, touching, enclosed and one-ULP clearance cases;
   stage mutation; failed pair fallback; original full36 `EXPORT_IMPORT_WORK_LIMIT`
   reproducer followed by actual successful independent STL reread. Keep valid JSON
   on STL failure. No skip-count-only test can establish validity.
6. One warmup/five serial Release samples at admitted counts with fixed preparation;
   separately cold/warm preparation at fixed thread count; combined Start and timed
   quality results; measured peak memory and Stop/cleanup tail. Failed repetitions
   remain in evidence. Final integrated legacy/new Save/move/Open/JSON/STL journeys
   map to both acceptance sheets without duplicate runs solely for labels.

The primary's bounded T011 Start comparison may use the frozen practical assets,
dimensions/orientations and identical one-candidate/one-pass fixed work: old
prepare-plus-solve versus repeated retained-session Start at one thread. Require
identical work and independently valid ordered poses; keep cold import, original
full-catalog cases and threading comparisons separate. This isolates real repeated
preparation overhead without depending on intentional search-budget consumption.

Open bounded gates: phase breakdown and lane/tile freeze; successful serial practical
baseline; numerical speed/reuse/memory targets owned by primary; full-Pryanik remaining
raster/material cost; per-copy full36 import cost after separation; worst qualified
noninterruptible phase/cleanup duration. None is permission to omit an acceptance
case, invent a result, raise limits or claim either delivery complete.
