# ADR 0010: first native desktop journey

Status: accepted for the bounded T-008 implementation, 2026-09-30. Interfaces were
agreed with the primary before parallel implementation. No implementation or
qualification is implied. Based on main `21a7033`, after T-007/PR #8 merged
at `1014b39` and the PR #9 agent setup update.

Governing requirements: UI-01–UI-03, DATA-02, DATA-03; supporting GEO-02–GEO-06,
DATA-01, SOL-06 and SYS-01/SYS-04. Read specification §§5.2, 7, 8 and AT-13–AT-15.
[T-008](../../doc/T-008.md) bounds acceptance. Existing ADRs 0004–0009 continue
to govern engine contracts, accepted geometry, validation and export.

## Decision and scope

Deliver a real Tauri 2/Rust application with React/TypeScript and Three.js:
explicit-unit STL object import, diagnostics, box settings, CPU fixed/cube
packing, Stop, one shared object mesh with instance poses, result inspection,
portable Save/Open and authoritative JSON/STL export. Default clearances are
1 mm pair and 1 mm wall, separately editable to zero. Fixed identity is the
initial orientation; label fixed/cube and CPU explicitly. Do not label the
restricted slice Balanced/Free 3D. Pitch is an explicit millimeter setting;
an initial suggested value is a visible, editable default, not automatic
memory preflight or a change to physical object size.

Use the existing auxiliary `inspect`/`solve` CLI paths asynchronously from Rust.
Add narrow native prepare/restore commands and a cooperative solve cancellation
hook. Expanding `serve` now would also require registry, job scheduling, replay,
events, checkpoints and continuation; that is a separate integration tranche.
Do not report these auxiliary operations as implemented service methods.

Box containers are the committed acceptance boundary. STL containers, repair
proposal acceptance UI, full preset resolution, automatic pitch, Vulkan,
continued search, periodic checkpoints, exact continuation, live incumbent
streaming, screenshots and the qualified 1,000-copy performance gate remain
later work. Explicitly accepted repaired reports must load via the native
loader. Baseline inspection found that the T-007 loader rejected every repair
record; T-008 therefore adds bounded replay of the recorded weld proposal and
explicit acceptance evidence through the existing geometry APIs. Stored PLY
bytes alone never authorize the repaired solid.

Minimal projects cover portable retained source/accepted geometry, decisions,
settings and complete results. They do not establish the interruption/checkpoint
part of DATA-02 or full AT-13–AT-15/M3. The current solve command's search budget
does not yet cover all desktop preparation; show that distinction and total
elapsed time. Full Start-to-terminal budget accounting remains an explicit
§7.2 integration obligation, not a claim of this slice.

## Ownership and dependency direction

`desktop React → narrow Rust commands → fixed C++ executable → existing native
libraries`. Rust owns dialog grants, processes, sessions, scoped file access,
ZIP transport, hash verification, atomic archive replacement and operation
state. React owns presentation and view state. Neither implements packing,
orientation catalog identity, accepted-solid reconstruction or feasibility.

C++ owns settings resolution, catalog normalization/hash, immutable accepted
handles, display LOD generation, pose validation, result rebuilding and checked
export. Put reusable restore/serialization logic in `pack_io`, with the CLI
coordinating catalogs through `pack_solver`; I/O must not depend on the solver.
Reuse `load_accepted_asset`, `make_orientation_catalog`, `result_catalog_sha256`,
`make_validation_context`, `make_candidate`, `validate`, `build_result`,
`export_result` and `make_display_lod`. Do not duplicate their scientific logic
in Rust/TypeScript or create a second solver in a desktop-only binary.

Accepted assets and completed results are immutable. Every new solve/open uses
a fresh private staging directory and a new opaque shell ID. Stored JSON's
`job_id` is currently `cli-solve`; it is not unique enough for UI routing. The
shell's session/result/operation IDs supply that isolation without rewriting
native provenance. Settings submitted to Start are copied. Edits afterward are
pending settings and never relabel the old result.

## Frozen frontend/Rust interface

[desktop.schema.json](../schemas/desktop.schema.json) is the source for Desktop
types, alongside the existing generated namespaces. Existing assets/results
are referenced, not hand-maintained duplicate interfaces. Its named definitions
are normative; root union membership is not an operation dispatch mechanism.
Validate the specific definition appropriate to each command. Schema success
does not replace semantic, scope, hash or physical checks.

| Command | Arguments | Return |
| --- | --- | --- |
| `desktop_state` | none | `Desktop.State` |
| `desktop_import_object` | `Desktop.ImportRequest` | `OperationReceipt` or null if dialog cancelled |
| `desktop_start` | `Desktop.StartRequest` | `OperationReceipt` |
| `desktop_stop` | `Desktop.StopRequest` | `StopResponse` |
| `desktop_save_project` | `Desktop.SaveRequest` (`{settings: Settings}`) | `OperationReceipt` or null if dialog cancelled |
| `desktop_open_project` | none | `OperationReceipt` or null if dialog cancelled |
| `desktop_export` | `Desktop.ExportRequest` | `OperationReceipt` or null if dialog cancelled |
| `desktop_new` | none | `State` |
| `desktop_read_preview` | `Desktop.PreviewRequest` | bounded binary response/ArrayBuffer, never a JSON vertex array |

All dialogs execute in Rust. No command accepts an executable, argv, arbitrary
read/write path, archive entry path or process ID from JavaScript. Export selects
a destination directory; native artifacts retain consistent fixed names and
portable relative references. New rejects an active operation. Save requires
an accepted object; export requires a complete native-validated result. Start
requires the current object to be accepted and diagnostics valid. Invalid
import reports remain visible for diagnosis, with Start unavailable.

`Settings` has exactly `desktop_version:1`, positive `box_dimensions_mm[3]`,
`clearance_mm:{pair,wall}`, fixed quaternion or cube `orientation`, positive
`pitch_mm`, positive `budget_seconds`, and decimal uint64 string `seed`.
Custom scale is required only for `ImportRequest.units == custom`. Normalize
quaternions and check seed bounds natively; JavaScript numbers cannot safely
represent every uint64 seed. No unspecified preset or backend is implied.

`State` always contains `desktop_version`, `session_id`, `revision`, `object`,
`draft_settings`, `result`, `operation`, `last_error`; the last five are nullable.
`object` is `{id,display_name,report,preview}`; `report` is the existing complete
Assets document, including portable paths, original units/frame and diagnostics.
It may describe an inspected invalid object. `preview` is nullable.

`result` is `{id,document,preview,origin}`; document is the existing complete
Results document, preview is nullable and origin is `solve` or `project`.
Result preview belongs to that result's object, even after a different import.
A preview descriptor is exactly `{preview_id,format:'ply',sha256,byte_length,
triangle_count,coordinate_frame:'object_local_mm'}`. No native absolute path is
exposed. Preview IDs resolve only in a private Rust registry.

`operation` contains `{id,kind,phase,started_at,finished_at,result_id,detail}`.
Kinds: import/solve/open/save/export. Phases: preparing/running/stopping/
validating/saving/finished/failed. `finished_at`, `result_id`, `detail` are
nullable. Timestamps are UTC RFC3339. A terminal operation remains observable
until the next operation; `finished_at` is non-null iff terminal. `result_id`
identifies the result operated on or published, never an unfinished trial.
`last_error` is null or the existing structured `{code,message,details,recoverable}`.

Poll state at most four times per second. Revision increments on state changes;
ignore older revisions or other sessions. Elapsed animation can use the local
clock and recorded timestamps. Operations run off the UI thread, stdout/stderr
are drained asynchronously, and the lock protecting state is never held while
waiting for native work. Only one mutating native operation runs per session.
Concurrent starts return `JOB_BUSY`; this is not the service request-replay gate.

Long commands return `{operation_id}` after registering the task. Stop returns
`{operation_id,accepted:true}` within 250 ms on the qualified local test path,
without waiting for native validation/export. It is idempotent for the same
active stopping operation. Wrong/stale IDs or non-solve operations are errors.
The phase remains stopping until terminal publication. A Stop during preparation
cancels the pending solve; it cannot claim a new result existed. Completed old
results remain available and clearly labeled as previous while new work runs.
No live unvalidated poses or invented completion percentage are displayed.

## Frozen auxiliary native command interface

```text
spectrapack-engine desktop-prepare --object-report FILE --request FILE --output DIRECTORY
spectrapack-engine solve --settings FILE --object-report FILE --result FILE [--stl FILE] [--stop-file FILE]
spectrapack-engine desktop-restore --object-report FILE --result FILE --output DIRECTORY [--stl FILE]
```

Existing solve/container options remain supported. New command success is one
UTF-8 JSON line `{ok:true,result:PAYLOAD}`. It is an auxiliary CLI response,
not a service response: do not emit a successful protocol record with null
request ID. Errors use the existing structured CLI envelope with
`protocol_version:1, request_id:null, ok:false, error`. Maximum terminal record
is 1 MiB; diagnostics use stderr. Rust validates complete terminal records and
exit code together. EOF, invalid JSON and nonzero exit never become success.

Prepare consumes `Desktop.Settings`, loads the accepted object report through
the native loader, resolves CPU/one thread/manual pitch and fixed/cube policy
using existing version-1 catalog construction, then persists normal resolved
Settings. Use a distinct preset name `desktop-cpu-v1`, deterministic false and
the supplied time budget/seed. Never hardcode a catalog hash in the shell.
The output directory is new and private; fixed output names are `settings.json`
and `preview.ply`. LOD uses the existing representation limits and default
20,000-triangle target/error setting; target attainment is not guaranteed.
Accepted full-resolution source geometry is unchanged.

Prepare success payload is exactly `{settings_path,preview_path,preview,warnings}`.
`preview_path` and `preview` may be null together if display generation fails;
warnings is an array of existing structured errors. Native preview metadata is
the Preview shape without `preview_id`; Rust registers that ID after size/hash
checks. Paths in this native payload are shell-internal, never a grant from
the webview. Display failure must not declare geometry invalid or prevent CPU
operation/export where those remain supported.

`--stop-file` is optional and preserves existing solve behavior when absent.
The shell allocates a previously nonexistent marker path inside that run's
directory. C++ observes existence through a bounded cooperative watcher and
requests the same stop source used by existing controls. A preexisting marker
requests stop immediately. Join the watcher at command exit; filesystem errors
cannot silently disable cancellation. Do not kill the process to implement
normal Stop. Safe-boundary native publication retains the current validated
solution, including a valid empty solution, and its actual termination reason.
This hook implements no periodic checkpoint or process-crash recovery claim.

Restore takes a complete result and an independently verified inspection report,
using immutable original resolved settings and canonical poses. Verify schema,
semantic constraints, identities, hashes, source frame, count, matrix/quaternion
agreement, catalog version/hash and all physical settings. Recreate native
accepted/context/candidate handles, then run the full independent validator.
Invalid or indeterminate refuses activation/export. Merely reading a stored
`validation.status:'valid'` or calling the general schema validator is insufficient.

Build a fresh bound result through the existing builder. Require agreement of
all authoritative derived fields: asset identities/frame/dimensions, container,
constraints, placements/count, label and physical volume metrics. Do not repair
a mismatch by silently using rebuilt values. Preserve identity (`job_id`,
revision, creation time), original search-engine provenance, resolved settings,
search history, measured metrics and pose order/IDs. Fresh validation evidence
may reflect the current validator; it does not claim the old search was rerun.
Drop optional old artifact claims unless their bytes were separately verified;
new STL references are added only through existing checked export. Unsupported
catalog/schema versions fail clearly. Accepted repair records are replayed
by the extended native loader with their explicit acceptance evidence, original
source and recorded options. Recomputed proposal/mesh hashes, diagnostics and
physical frame must match the retained artifacts; mismatches fail closed.

Restore publishes `result.json` and referenced assets in the new output
directory via existing `export_result`; optional STL is generated from the
fresh validated handle without running search. Payload is exactly
`{result_path,count,validation_status:'valid',stl_path,companion_path}`, last two
null when not requested. STL failure can retain valid JSON with its path in the
error details, as ADR 0009 requires. Rust may expose that checked JSON while
showing the operational failure. Unknown/unscoped error paths are never opened.

## Portable project boundary

The version-1 `.spectrapack` ZIP manifest is `Desktop.ProjectManifest`:
`{project_version:1,format:'spectrapack-project',saved_at,object_report,
resolved_settings,best_result,draft_settings,files}`. `object_report` is
`object.report.json`; `resolved_settings` is `settings.json` or null;
`best_result` is `result.json` or null. Result/settings are both null before a
complete result exists, otherwise both present. Draft settings can differ from
saved result settings and must remain visibly pending. When a result exists,
its object/report and settings are the archive's authoritative saved snapshot.
If the current imported object's source/accepted identities or frame differ
from the retained result's object, Save rejects with `ASSET_MISMATCH` and asks
the user to finish a new solve or start a new session. Never combine a newly
imported object with an old result or silently discard the displayed result.
Saving an accepted object before any result exists is supported.

`files` contains every non-manifest regular entry exactly once as
`{path,sha256,size_bytes}`. The manifest is not self-hashed. Copy source STL,
accepted float64 indexed PLY, inspection/repair records and all transitively
referenced repair meshes byte-for-byte, content-addressed under `assets/`.
Store each asset once. Persist the actual resolved settings separately and
require exact equality with the result's resolved settings on Open.
Optional checked STL/companion may be omitted together with their result
artifact claims; preserve native identity, poses and provenance. Such omission
is serialization of a dependency-free result, not a new search revision.
Preview meshes/caches are disposable and may be regenerated on Open.

Save accepts the current frontend settings explicitly, validates them and stores
that draft separately from the immutable result's resolved settings. It does not
depend on Start having been pressed after the last edit. Save snapshots immutable
files, creates a unique adjacent archive, closes and
flushes it, verifies its manifest/entries, then atomically replaces the target.
Failure preserves the previous complete archive; cleanup touches only owned
temporary files. No multi-file filesystem transaction or durable power-loss
guarantee beyond the actual platform operations is implied.

Open extracts into a new private directory. Enforce a 1 MiB manifest, at most
1,024 regular entries, 2 GiB compressed archive and 2 GiB total expanded bytes;
check actual streamed bytes as well as declared values. Reject absolute/drive/
UNC paths, traversal, backslashes, empty or dot path segments, colons/ADS,
Windows device names, trailing spaces/dots, links/reparse entries, duplicates
including case-insensitive collisions, unsupported encryption/compression and
unlisted entries. Enforce lexical and final filesystem containment. These
are admission limits, not measured performance or a claim that every smaller
archive will fit memory. Stream assets and never load the entire archive in RAM.
The supported transport is single-disk ZIP32 with stored or deflated regular
entries and ordinary data descriptors. ZIP64, multidisk archives and unsupported
extra fields are rejected explicitly; these formats are not needed within the
bounded writer's limits. Raw central/local headers are checked before the ZIP
library reads entries so duplicate names cannot disappear through deduplication.

Verify every entry size/hash and all graph references before native restore.
Do not swap the active object/result on partial extraction, corrupt hashes,
unsupported versions, failed native validation or exhausted resources. Verify
accepted objects through native preparation even when a project has no result.
Move/rename the archive freely; no absolute source or prior working-directory
reference may be needed. Read-only restore preserves the source archive bytes.

## Process/filesystem security and bounded transport

Bundle the fixed engine using Tauri's external binary mechanism. Select its
name and build location in Rust/build configuration; never resolve through
PATH, a webview argument, a project path or an arbitrary environment override
in release builds. Pass arguments as distinct OS arguments, never shell text.
The shell owns child-process lifetime: closing it must not leave an unmanaged
engine running. Normal Stop remains cooperative; app-exit cleanup may terminate
owned children, preserving previously complete state without claiming a new
checkpoint or completed result from the interrupted operation.
Do not expose generic shell/fs plugins to the webview. Narrow commands still
perform their own authorization: plugin capabilities alone do not validate
custom command arguments. Bundle UI assets; release navigation/CSP permits no
remote application code. Development tooling grants must not enter release.

User file selections grant only the selected file or output directory. Use a
private session root, Rust-created names and opaque IDs; reject scope escape,
links/reparse-point redirection and hardlink output aliases to inputs. Do not
give native work write access to the original selected input by naming it as
an output. Dialog cancellation changes no accepted state. Concurrent session
cleanup cannot delete another session's directories.

Native mesh files are scoped local artifacts. Rust serves only registered
preview bytes after hash/length checks, bounded to 64 MiB, via binary IPC.
Result JSON has a 64 MiB desktop admission cap; inspection reports 16 MiB;
settings and command records 1 MiB. Exceeding a transport cap gives a clear
resource error and retains disk artifacts; never truncate or silently simplify
authoritative results. Rendering failure leaves export available.

## Viewer invariants and test seams

Use one indexed BufferGeometry/InstancedMesh from the accepted object's local
preview, one unit-scale transform per copy, and Z-up cameras/world helpers.
The box occupies `[0,W] × [0,D] × [0,H]`; container mesh rendering must not move
the engine frame. Canonical XYZW quaternion/translation compose each instance;
verify stored matrices independently. Three.js matrix storage differs from
JSON row-major arrays: do not pass a flattened row-major matrix to `fromArray`
without an explicit conversion. Source-to-local is already applied to PLY;
applying it again would corrupt dimensions/placement.

Provide orbit/pan/zoom/fit, transparent container/edges, dimensions, selected
copy ID and pose, and a movable clipping plane. Hiding/clipping/selection and
display mesh conversion are view state only; result count, poses and export
input never change. If hiding needs a compact instance mapping, retain a
separate instance-index-to-copy-ID map; array position is not copy identity.

Production Rust core adapters should be callable without a WebView for real
engine integration tests. Inject dialogs/process scheduling only for boundary
failure scheduling, never as the geometry oracle. Test pure UI state reduction
and transform conversion separately; a browser mock cannot establish native
integration or desktop acceptance. See the ticket for exact red/green order.

## Compatibility and dependency record

Engine settings/assets/results/protocol version 1 and existing CLI calls remain
unchanged. New desktop/project schemas and auxiliary commands are additive;
advertise new commands only after their end-to-end paths pass. Service method
capabilities remain truthful and unchanged. The desktop schema should be added
to the closed TypeScript catalog; native embedding/validation can load its
specific definitions without weakening existing schema checks.

Desktop DTO version and project version are independently checked. Unknown
versions reject without replacing current state. Future service integration
may replace the Rust CLI adapter behind these frontend operations while retaining
the scientific contracts and complete immutable state publication. This is no
promise to support unavailable future service semantics via the current adapter.

Use verified registry/toolchain evidence when choosing exact dependencies, then
commit lockfiles, the notice generator and any pinned supplemental license
sources. Generate distribution notices while staging the desktop; do not commit
a concatenated license report. The generated `engine-resources/third-party/index.md`
maps packages to `texts/<sha256>.txt`, storing identical complete license/NOTICE
bytes once while retaining each package's identity and attribution. Frontend
inventory follows the installed locked production dependency closure, excluding
test/build tooling except explicitly recorded generated runtime contributions
(currently Vite's emitted preload helpers, with its core license only). Those
contributions are version-checked against the manifest and lockfile. Rust
inventory conservatively includes the Windows MSVC
normal/build dependency closure because build scripts and macros may contribute
generated code; this is not a claim of exact linked-binary inventory. Missing
required source texts stop staging. Existing C++ runtime notices remain packaged.

This changes packaging and repository maintenance only; runtime behavior,
desktop/project schemas and compatibility are unchanged. This ADR pins no
speculative versions. Follow
official [Tauri sidecar documentation](https://v2.tauri.app/develop/sidecar/) and
[capability documentation](https://v2.tauri.app/security/capabilities/) for the
selected integration. The renderer contract follows the documented
[Matrix4](https://threejs.org/docs/pages/Matrix4.html) and
[InstancedMesh](https://threejs.org/docs/pages/InstancedMesh.html) interfaces.
