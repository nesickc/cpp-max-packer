# ADR 0017: Bounded count improvement and immutable public packing

Date: 2026-10-11. Status: accepted coordination contract. The coordinating primary
accepted the interfaces and ownership decisions below; reservation proofs and
implementation acceptance remain pending. No passing product result is claimed.

Scope: [T-015 bounded work package](../../doc/T-015.md), extracting only the
necessary count-publication part of T-021. Requirements: SOL-01/SOL-04/SOL-08,
GEO-02/GEO-04/GEO-06, UI-02/UI-03, DATA-01/DATA-02/DATA-03; relevant
AT-10/AT-11/AT-14/AT-15 subsets. Full T-014/T-015/T-021 and service/checkpoint
milestones remain open. The primary owns canonical specification/ticket updates.

## Observed integration seams

At merged base `858940a`, `Incumbent::offer` deliberately accepts secondary-score
improvements at equal count. `NativeSnapshot` and `ValidatedSolution` are immutable
shared owners; `SnapshotSink` runs on the solver coordinator. The spectral wrapper
currently suppresses the baseline sink, and internal revisions can restart across
its baseline/spectral incumbents. `solve.cpp` remembers the last callback but
exports the final internal retained solution. These are insufficient public
count/identity gates.

The auxiliary `desktop-session` already has a bounded writer, single pending phase,
cooperative Stop and transport watchdog. Rust's `NativeSession::request` reads it
asynchronously; `Core::start` installs `result.json` only after terminal response.
React polls complete `Desktop.State` at 250 ms and the viewer already separates
mesh loading, instance updates and demand rendering. Save/export currently require
the solve operation to finish. Extend these paths; introduce no second solver,
service-job framework, checkpoint protocol or renderer.

## Decisions and dependency direction

1. Geometry alone mints a `ValidatedSolution` after a complete authoritative valid
   pass. An invalid/indeterminate trial, scalar count, voxel result or wire
   `validation_status` cannot mint authority. Solver owns mutable working layouts
   and its internal best, including equal-count exploration and bounded removal
   where a separately selected strategy requires it. Existing 24 proper cube
   rotations, units, dimensions and separate clearances remain unchanged.
2. The operation coordinator owns a **count-only public gate**. Its watermark
   starts at zero for every accepted Start. The first complete valid nonempty
   handle wins; thereafter only a strictly greater count replaces it. Ordered
   copy IDs and canonical translations/quaternions are frozen together. Empty,
   lower, equal and invalid offers never replace it. Internal best is not the
   public owner. Do not change `Incumbent::offer` to implement a viewer rule.
3. The gate creates its own strictly increasing publication revision. Internal
   incumbent revision is not a transport identity. Forward baseline callbacks
   through this same gate before spectral preparation, including early Stop,
   deadline and resource-return paths. A sink cannot authorize geometry.
4. Native solve/session owns the immutable public handle; Rust owns its accepted
   public DTO and scoped final files; React only presents Rust's public packing.
   Geometry/compute gain no transport or UI dependencies. Solver never writes
   files. Serialization belongs to the existing CLI/session adapter and final
   result construction/export remains in `pack_io`.

## Narrow native interface

Keep `SnapshotSink` and existing solver call sites source-compatible. Add this
source-private adapter value alongside `SolveRuntime` in `engine/cli/src/solve.hpp`:

```cpp
struct PublishedSnapshot {
    std::uint64_t revision;          // operation-local, count increases only
    double time_to_count_seconds;    // same Start clock basis as run metadata
    solver::SnapshotHandle native;   // opaque complete valid solution authority
};
using PublishedSnapshotHandle = std::shared_ptr<const PublishedSnapshot>;

// Optional SolveRuntime members; absent means existing auxiliary CLI behavior.
std::uint64_t publication_reserved_bytes{};
void (*publication_sink)(void*, PublishedSnapshotHandle) noexcept{};
void* publication_context{};
```

These interface spellings are accepted for implementation.
The solve adapter performs count gating and retains the latest public handle even
if delivery is coalesced. Its sink transfers ownership only; it does no JSON,
filesystem work, full revalidation or pipe write. The retained handle, its time and
public revision select the existing final-export input. A sink exception must not
escape; admission/transport failure has a structured operational error, not a
fabricated validation verdict. Cleanup detaches the callback before destroying
its context and joins the writer before destroying any referenced run state.

## Auxiliary wire and desktop contract

Add optional `run.params.publication` with exactly:

```text
{ version: 1, session_id, native_epoch, settings_id }
```

All IDs use the existing nonempty bounded identity grammar. Rust creates a fresh
`settings_id` for its immutable Start settings capture, even when values equal a
previous run. The existing request/operation IDs remain mandatory. The captured
tuple `(session_id, native_epoch, operation_id, settings_id, asset_token)` binds
that run to the fixed prepared asset and settings; `job_id: "cli-solve"` is never
used as its live identity. Native echoes this binding. No new public service
method is advertised.

Add an auxiliary `kind: "packing"` record, separate from a terminal response:

```text
{ runtime_version: 1, request_id, operation_id, kind: "packing",
  publication: { version: 1, session_id, native_epoch, settings_id },
  sequence, native_elapsed_seconds,
  packing: { solution_revision, count, validation_status: "valid",
             placements: [Results.Placement, ...] } }
```

`sequence` is a monotonic packing-event sequence; phase events retain their
existing independent sequence. `solution_revision` is the count-only publication
revision above. Gaps are permitted. Count equals placement length, IDs are unique,
values finite and rigid, and matrices derive from canonical poses through the
existing native transform convention. Native serialization reads the validated
handle, never a mutable trial. There is no mesh payload and no invented terminal
reason in a live record. The unchanged 1 MiB record limit applies: count/admit
serialization before allocation, retain the last complete public DTO on overflow,
and report an explicit publication resource error. Never truncate poses, split
one packing into partially applicable messages or raise the cap. Qualify the
known-case counts within this bound; broader count coverage is not claimed here.

Extend `NativeSession::request` with an optional packing callback in addition to
the existing phase callback. It checks record structure and current request/run
binding before dispatch. Matching but duplicate/older packing sequence or public
revision is ignored without advancing any watermark. Unexpected identity cannot
change state; malformed active records retain state and surface a transport error.
Phase sequence and packing sequence must not suppress each other.

Add optional `Desktop.State.live_result` (absent/null in old/project states):

```text
{ publication_version: 1, session_id, native_epoch, operation_id, settings_id,
  solution_revision, sequence, count, placements, validation_status: "valid",
  settings: Desktop.Settings, preview: Desktop.Preview | null }
```

Rust supplies settings and preview from its captured operation/object, not a new
webview grant. Check the current session, native epoch, active operation, settings
capture and strictly increasing positive count again under the state mutex before
one atomic replacement. Parsing, hashing and file reads occur outside that mutex.
Keep the existing overall State revision and frontend session/generation checks.
Generate DTOs from `desktop-runtime.schema.json` and `desktop.schema.json` before
consumers; do not hand-maintain independent TypeScript shapes.

## Bounded ownership, scheduling and memory

The native handoff retains one latest immutable packing slot and at most one
serializer/writer in-flight packing. Replacement drops the obsolete pending
packing; do not enqueue every intermediate revision or serialize on the solver
thread. A tiny shared-handle transfer may synchronize ownership, but search must
never wait for output readiness, throttling, acknowledgement, encoding or disk.
If a try-transfer implementation can miss a busy slot, it must retain/retry the
dirty latest handle; dropping the only nonempty offer until terminal is incorrect.
Do not claim that standard-library atomic shared ownership is hardware lock-free.

Use one 250 ms scheduling gate for ordinary progress/packing delivery, with
packing taking precedence over obsolete progress. The writer takes the newest
pending higher count at each opportunity. Existing React polling remains at most
4 Hz and cannot install equal-count terminal transforms. On normal termination,
stop production, flush the latest unsent higher count at the next allowed slot,
then deliver terminal metadata. This may wait at most the remainder of the 250 ms
interval during bounded cleanup, never during search; it stays inside the existing
five-second cleanup allowance. No final-flush exception may burst pose updates
above 4 Hz. Existing blocked-pipe watchdog remains an error path.

Before starting search, charge `publication_reserved_bytes` to the unchanged
512 MiB host cap. It must cover **two additional retained solution payload slots**
(latest and in-flight) plus snapshot wrappers/control blocks and the bounded
serializer/DOM/record buffers. A new candidate still belongs to normal solver
accounting while being admitted. Query actual `ValidatedSolution::resident_buffer_bytes()`;
a shared pointer does not make pinned pose/report capacity free. Enforce each
slot's declared payload ceiling before retaining it; overflow/missing accounting
fails closed and preserves the previous public snapshot. Do not derive an
unverified reserve by measuring only today's 306 poses or treating the existing
16 MiB adapter allowance as unused space.

Implementation must supply a checked capacity-based upper bound or a separately
bounded reserved pool for these two slots, and demonstrate allocation/failure
tests before acceptance. Conservative double charging against a live solver owner
is allowed; hidden undercounting is not. Count accepted-solid/context/catalog
owners once where identity can be proved, as ADR 0013 specifies. Once same-count
internal search diverges, public poses really are an additional owner. On terminal
success retain only the chosen public native solution in session ownership and
include it in `retained_native_bytes`; release obsolete working/in-flight owners.
Rust/React likewise keep one current public payload, bounded reader/in-flight
storage and existing single-flight polling, without per-revision history.

## Terminal, Save/export and viewer behavior

For this publication-enabled desktop run, feed the frozen public native solution,
publication revision and time-to-count to the existing `build_and_export_result`
path. It still performs required full final validation and checked STL export.
If internal best ends at the same count with different poses, those poses never
replace the public input. If a strictly higher terminal native solution was not
offered yet, submit it through the same gate and final flush first. Preserve actual
run diagnostics, timings and termination reason without changing canonical poses.

Rust compares final artifact identity, physical settings, ordered IDs and exact
canonical pose values with the selected public packing before installing
`ResultFiles`. Matching terminal metadata may change without invalidating instance
transforms; keep the same frontend pose key. A lower or differing equal-count
terminal artifact is a publication error. Save/export remain unavailable during
the active run; after successful finalization they use precisely these files.
Open retains existing full native revalidation and transform checks.

Stop, deadline and ordinary search failure still use the existing exporter and
fresh bounded cleanup control, retaining the latest public packing even when the
operation reports a nonzero failure. Actual file/transport/final-validation
failure cannot guarantee a new artifact: retain the visible immutable DTO, report
the concrete error, and refuse Save/export against an older or mismatching file.
This limitation is explicit; no retry framework or new recovery method is added.
An old complete artifact may remain privately retained as previous, not silently
become the visible packing's Save/export authority.

A fresh Start resets that run's watermark independently of a previous count.
Until the first new nonempty public snapshot, retain the previous displayed result
and its `ResultFiles` as one bound pair with their original settings and identity.
If the current operation ends with only an empty result, keep that pair together;
report the current operation's termination/error and `no_nonempty_incumbent`
separately. Never install a new empty artifact as Save/export authority while old
poses remain visible. This extends the existing Stop/deadline preservation choice
to every terminal reason for publication-enabled runs with a previous result.
An empty current-run artifact, if produced, is not activated in that case.

On a first run with no previous result, a complete native-valid empty terminal
result may be installed only atomically with its empty viewport and matching
files; otherwise retain no result. No empty-packing claim follows merely from a
failed preparation. New/Open/import use existing explicit identity transitions
and must clear obsolete live bindings. Pending UI edits never mutate a running
binding. These cross-run retention rules do not compare the new run's first
nonempty count with the previous run's count.

React derives one displayed packing from live or final public state. Reuse the
same preview ID/shared geometry, renderer and camera; replace all instance poses
atomically only on a new count-publication key. Same-count finalization preserves
that key. Preserve clipping, camera and surviving copy selection; clear selection
only if its copy no longer exists or an explicit object/project transition occurs.
Keep demand rendering and the static busy indicator. Progress text, elapsed time
and Stop state can change independently without touching instance buffers.

## Compatibility, ownership and acceptance sequence

Settings, result and portable-project schemas stay version 1 with unchanged
persistent shapes. Existing nonpublication CLI behavior remains supported. The
new runtime request is opt-in and desktop fields are additive, but strict old
auxiliary readers reject unknown members/kinds: ship native/Rust/frontend as a
matched bundle and fail clearly on an incompatible engine; no mixed-version
fallback silently claims live support. This supersedes ADR 0013's single-solution
assumption during active publication and narrows terminal desktop selection from
internal secondary-score best to frozen count-only public best. It does not
change geometric validity, search ranking or project restore semantics.

Assign one implementation owner for this coupled feature, serially owning solver
callback forwarding/accounting, `engine/cli/src/solve.*`, `desktop_session.cpp`,
runtime/desktop schemas and fixtures/generated types, Rust `native_session.rs` /
`core.rs` / `model.rs`, and React state/App/viewer tests. Reuse existing export and
render helpers. No independent worker edits those shared interfaces concurrently.
The primary owns ticket/spec/delivery/status; the architect owns this ADR only.
Reassess the combined feature near 1,000 added handwritten production lines.

Observe red before implementing these affected seams, then green:

1. Native analytic fixtures mint complete valid layouts A24, B24 with different
   poses, invalid C25 (overlap), valid D25 and E26. Independent geometry validation
   is the oracle; test invalid/indeterminate never yields authority. Feed
   A24 -> B24 -> C25 -> D25 -> duplicate/stale -> E26. Assert public ordered pose
   values A24/D25/E26, while internal same-count exploration can retain B24.
2. Fake clock plus delayed writer: first nonempty delivered before termination,
   burst coalescing, <=4 Hz, final higher-count flush, bounded latest/in-flight
   occupancy, stopped/failed reader, cancellation and callback lifetime. Verify
   continued solver work under a blocked sink, explicit byte admission/allocation
   failures and no uncharged obsolete owners. Use real native subprocess coverage
   after pure scheduling tests; mocks alone do not prove this boundary.
3. Rust/frontend sequence: duplicate/out-of-order messages, old native epoch,
   operation/settings/session mismatch, fresh run below old count, terminal
   equal-count changed poses and higher terminal count. Compare exact ordered
   canonical poses, not count/revision alone. Check Stop/deadline/search-error
   retention, empty terminal preserving previous poses/files together, first-run
   empty activation, and explicit unavailable-artifact behavior.
4. Real desktop: observe a native-valid improvement before completion; interact
   with camera/clip/selection and Stop; verify mesh/renderer/camera lifetime and no
   equal-count instance update. Measure UI overhead and snapshot memory; retain
   raw evidence. Save/move/Open/JSON/checked-STL must agree with displayed public
   poses, using existing independent restore/export checks.
5. The primary freezes practical Pryanik quality/time/memory targets before
   optimization. Separately prove the chosen bounded staggering/refinement
   strategy on an analytic independent oracle and real accepted geometry; retain
   failed 2 mm/resource runs, Ulamok 36-copy and full-Pryanik regressions. Publication
   tests do not establish a packing improvement or complete remaining milestones.

No dependency pins, measured reserve sizes or passing product checks are claimed.
The coordinating primary has accepted these interfaces. Concrete reserve capacity
and known-case live-record size remain required implementation evidence, not
unresolved product semantics.
