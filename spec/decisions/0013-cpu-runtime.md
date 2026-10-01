# ADR 0013: CPU runtime, retained preparation and Start deadlines

Date: 2026-10-01. Status: core design accepted by the coordinating primary;
parallel-kernel selection and numerical targets remain explicit measurement gates.
This document specifies work, not implemented or qualified behavior.

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

The integration-owned I/O signature is
`AssetLoadOutcome load_accepted_asset(const filesystem::path&, const
runtime::OperationControl& control = {})`. Apply the same final optional control
argument to `build_result` and `export_result`. Propagate control into reads,
hashing, native import/repair replay and independent validation. Poll during bounded
read/hash chunks as well as before and after each heavy native call.

Operational interruption is distinct from an invalid solid: use owned error codes
`OPERATION_CANCELLED` and `DEADLINE_EXCEEDED`; geometry reports `indeterminate`
and no accepted/validated output when interrupted. Adapters map those causes to
`user_stopped`/`budget_exhausted`, never `INVALID_SOLID` or an impossibility result.
Preserve nested phase/resource codes and counters on genuine failures. Existing
resource, arithmetic and allocation checks still apply.

## Retained preparation and desktop adapter

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

The adapter subtracts existing bundle/result owners, pinned incoming artifacts and
its declared reserve before import. Repair replay receives the remaining allowance
and includes distinct retained original/candidate owners. Display generation uses
its existing `RepresentationLimits::reserved_bytes`; preview serialization checks
its computed byte requirement and growth before allocation. A failed replacement
preserves the old token and complete state. The native bound requires a documented
capacity/lifetime audit and below-bound tests. Its initial coefficient is not yet
accepted: 4,096 bytes per triangle alone exceeds 512 MiB for the required 139,212
triangle Pryanik 2. A tighter justified bound must preserve that practical profile
and admitted transactional replacement; raising the host cap is not permitted.

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
case, invent a result, raise limits or claim the combined delivery complete.
