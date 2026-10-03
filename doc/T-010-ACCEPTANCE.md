# T-010 / T-011 cross-ticket evidence ledger

Updated: 2026-10-03. Current T-011 branch: `feature/SOL-08-cpu-runtime`.
Prerequisite: merged T-009, `b82117a` (PR #12).

This ledger records demonstrated scope. **No ticket acceptance gate is complete.**
The final integrated source revision and executable hashes remain pending.
[T-010](T-010.md), [T-011](T-011.md) and [ADR 0013](../spec/decisions/0013-cpu-runtime.md)
define the acceptance criteria and compatibility contract. A schema check, a
successful build, or a retained valid result after a resource failure does not
establish the corresponding runtime gate.

[ADR 0014](../spec/decisions/0014-runtime-delivery-split.md) separates delivery:
T-011 first owns T011-A1–A7 and the carried T009-A2/A3/A6 practical obligations;
T-010 then owns T010-A1–A7 after the T-011 user merge. Keep historical evidence
and frozen targets here; changing the delivery order closes no gate. T010-A7
later regresses the successful practical workflows delivered by T-011.

## Gate evidence

| Gate | Evidence retained so far | Evidence still required |
| --- | --- | --- |
| T010-A1 — configuration | Old CLI rejects two threads; native red tests show unsupported counts execute work. The controlled serial checkpoint now rejects unsupported 0/2 requests without work. | Real selected parallel kernel; actual supported/resolved counts; specific rejection outside range; desktop pending/result association. |
| T010-A2 — numerical correctness | Existing oracle suite is available. | Affected direct field/correlation/proximity oracles at 1/2/4 threads, asymmetric/rotated/offset/halo/uncertain cases, independent validation of every compared result. |
| T010-A3 — reproducibility | Old one-candidate Start baseline repeats identical count, work and ordered poses. | New fixed-work repetitions for each supported measurement count, with seed, policy, counters and failed samples retained. |
| T010-A4 — aggregate resources | Unchanged 512 MiB host cap and old measured process peaks are pinned. Native import/repair admission passes below/exact-bound tests and the full Pryanik 2 profile in Debug/Release. | Integrated owner/worker/queue/stack/scratch admission, active allocation failure, measured peak versus conservative estimate. |
| T010-A5 — Stop and races | Shared control and fresh-operation ownership contract are frozen. | Real parallel-phase Stop acknowledgement within 250 ms and safe completion within 5 s; failure/shutdown joins; no late publication. |
| T010-A6 — measured speed | Failed practical phase profiles identify raster work; these are not a successful speed baseline. | Successful serial revision, frozen phase/end-to-end/memory targets, one warmup plus five Release samples at 1/2/4/8 threads, preparation held fixed. |
| T010-A7 — product regression | Original full-catalog Ulamok and full36 checked-STL failures are reproduced. Native checked exports pass Ulamok36 and both full Pryanik retained2 cases in Debug/Release at `9f4a60f`, within unchanged caps and with identical ordered placements. | Supported full-catalog Ulamok count >=36; both full Pryanik spectral runs; pitch advice; invalid-original rejection; complete product journeys and independently reread exports; timed quality. |
| T011-A1 — one budget | Native injected-clock red allows work after expiry. The controlled checkpoint now expires before bootstrap without work or a new handle. Real cold-Pryanik preparation expires without a result and retains runtime/cleanup/overrun metadata. | One-budget coverage through planning and validation; no work begins after deadline; final integrated cleanup/overrun measurements. |
| T011-A2 — real reuse | Exact old preparation and fixed-work Start records are pinned. The first native session process check reuses an immutable cube token and preview after source/report mutation. | Practical replay/LOD work and timing evidence; New/replacement/units/frame/repair/epoch invalidation; fresh job derivatives for changed constraints/pitch/catalog; bounded replacement ownership. |
| T011-A3 — tamper resistance | Native authority/pinned-input contract is frozen. | Independent source/PLY/repair/settings/catalog/pose mutations; moved archive restore; failed Open preserves complete state. |
| T011-A4 — Stop phases | Session availability red and shared cancellation APIs are retained. Real cold-Pryanik and phase-qualified preparation Stop/EOF checks pass at the retained integration checkpoint. | Real loading/fields/FFT/validation/publication Stop; receipt <=250 ms and safe completion <=5 s; UI stopping/result behavior and final integrated repetition. |
| T011-A5 — lifecycle races | Initial native process checks pass shutdown, Stop followed by a fresh run, exact duplicate replay and conflicting request identity. | Repeated/stale Stop, completion race, marker/transport failure, child failure, active EOF/shutdown, backpressure and late completion using built native code. |
| T011-A6 — startup measurements | Four-profile new cold/warm series at 86a42db completes 48 operations with frozen work/count/pose checks. All preparation and measured-memory targets pass; Ulamok and both full Pryanik Start targets pass. | Analytic retained Start misses 87.6 ms target at 115.2 ms; diagnose and correct that specific overhead, then rerun affected evidence. Whole adapter bound and integrated cleanup remain separate. |
| T011-A7 — compatibility | Optional runtime schemas, shared fixtures and generated types pass their checks. | Legacy and new Save/move/Open, original timing semantics, pending/result association, thread policy, independent JSON/STL export and repaired-geometry invariance. |

The original T009-A2/A3/A6 obligations must pass in T-011, and remain T010-A7
regressions with their concrete full-catalog, pitch-advice and checked-export assertions.
A single shared journey may satisfy both rows; duplicate runs are unnecessary.

## Retained evidence and qualification

- Startup series at `86a42db`: all four profiles have one warmup and five cold/
  retained samples. Retained fixed-work Start medians are 0.1151591 / 0.1048717 /
  0.4805324 / 1.1631793 seconds for analytic/Ulamok/Pryanik 1/Pryanik 2. All
  preparation and measured process-memory targets pass; only analytic Start misses
  its 0.0876001-second target, so the series correctly exits 1. Native validity and
  frozen work/count/ordered-pose checks pass for all 48 sample operations. Sources,
  engine and immutable inputs are held; `.local/t011/startup-86a42db/RESULTS.md`
  records cold/retained medians, exact identities, raw evidence and timing-boundary
  limits. This is fixed-work startup, not full packing throughput or adapter proof.
- Benchmark memory-query guard: a real invalid Windows process handle previously
  produced a passing zero-byte peak. The extracted production query now throws
  an explicit `GetProcessMemoryInfo` / Win32 error 6 diagnostic; the actual current
  process query remains successful. The causal probe changes from exit 1 to 0;
  ordinary benchmark Debug/Release builds pass. This is a five-added/two-removed
  line harness correction, not a runtime performance result. Full probe/build
  logs and identities are under `.local/t011/benchmark-memory-guard/`.
- Certified placed-support reduction: current-source Debug/Release practical checks each pass
  2 cases / 54 assertions. Actual advised Ulamok 4 mm completes 48 correlations,
  retains 36 independently revalidated copies, and uses 1,174,267,251 representation
  work units under the unchanged 1.3 billion cap. Separate Ulamok 16 mm uses
  337,270,633 units; full Pryanik 1/2 retain two valid copies with two correlations
  and use 834,836,776 / 1,289,972,641 units.
  `.local/t011/placed-support/PRACTICAL_INDEX.md` indexes both relinked test PEs,
  complete successful-assertion log, command/exit and 128 stable source identities.
  The final field suites pass 38 cases in each build; accounting passes 16 Debug /
  14 Release. Five reached empty-output transfer allocation failures recover,
  and subsequent publication succeeds. `.local/t011/placed-support/RECOVERY_INDEX.md`
  preserves the causal sparse-resource, dangling-descriptor and Debug abort reds,
  fixture setup failures, final sources and binaries. Primary verified 69 hashes
  and rechecked the local transfer correction after the agent thread limit refused
  both reviewer reactivation and a fresh review. The earlier independent geometric
  review stands; the correction recheck is not independent. This step adds 233 /
  removes 49 implementation lines, plus 833 test lines, a 28-line private support
  header and two benchmark provenance lines. Budget assertions are not measured
  heap telemetry. Startup, full adapter bounds and integrated journeys remain open.
- Desktop lifecycle tests now observe sequenced native work instead of waiting
  for the obsolete `running` phase. Against the held `adapter-overlap/r01` Release
  engine, the existing Rust suite passes 25 library and 11 integration tests;
  five subprocess-helper/practical cases remain explicitly ignored. Stop receipt
  is 0.6873 ms and completion 145.1605 ms, retaining 133 native-valid copies.
  Child death preserves a previous 64-copy result and identical preview bytes
  through failed Start; shutdown confirms the real child is active before owner
  termination and exits within 5 s. The change is test-only, with unchanged caps.
  `.local/t011/desktop-lifecycle/README.md` retains exact commands, logs, sources
  and executed test PEs; primary verified 20 current-input/saved-binary hashes.
  The original two failed tests never reached their Stop/shutdown actions and
  remain recorded. This does not qualify every native phase, Debug cleanup, final
  practical journeys or ticket acceptance.
- Compact computational-failure/workspace checkpoint: the indexed affected native
  gates pass 277 cases / 23,638 assertions in Debug and 275 / 23,577 in Release.
  Final ordinary solver specifically passes 130 / 5,854 and 128 / 5,801; it excludes
  the practical `[qualification]` cases. Focused failure/control checks pass
  15 / 557 and 14 / 514. Static diagnostic descriptors, throwing catalog/correlation
  transfers, lazy pages and separate admission/observed peaks preserve exact valid
  handles and failure causes. Causal catalog allocation, entry-control and peak
  failures are retained; the critical recheck has no remaining findings.
  `.local/t011/native-failures/FINAL_INDEX.md` identifies the exact logs, 78 source
  snapshots and 26 saved binaries/libraries. Primary verified all 182 source/
  snapshot/binary checks and audited the complete indexed/final short logs after
  the Luna follow-up hit the tool's thread limit. Retained warnings are D9025 and
  a passing solver numerical diagnostic; Release's unmatched catalog-peak tag is
  a Debug-only allocation path. Native benchmark binaries compile/link but have
  not been measured. Advised 4 mm Ulamok, Debug cleanup and integrated gates remain open.
- Adapter overlap characterization: real prepare A/B reuse, a real custom-catalog
  run and a 2,048-node / 65,536-decoded-byte invalid request overlap at publication.
  Debug observes 6,709,735 peak session C++ payload bytes, with 3,904,487 at the
  publication boundary and 5,361,429 in the held window; Release observes
  5,576,815 / 3,333,615 / 4,527,701 respectively. Each saves eight native-valid
  copies and exits cleanly. Memory checks pass 8 cases / 29 assertions and wire
  checks 3 / 11 in both builds; monitor, exact replay/conflict and cached-error
  cases also pass. Debug's initial long-path fixture failed before the barrier;
  the corrected overlap alone was rerun on the same preserved binaries. Evidence
  is under `.local/t011/adapter-overlap/{d01,d02,r01}/`. No heavy-owner charge was
  subtracted from a non-simultaneous peak. These ordinary-path observations exclude
  allocator bookkeeping, stacks and RSS, and do not prove the full 16 MiB allowance,
  settings-copy overlap, every error producer, or long-path support.
- Current unchanged contract/presentation inputs pass 31 schema, 29 desktop and
  7 retained-runtime fixture checks, generated/header and TypeScript checks, and
  4 frontend files / 10 tests. After pnpm aborted an automatic install before
  testing, the exact underlying scripts ran with all 17 pinned installed package
  versions verified; no dependency reinstall occurred. All 35 input hashes stayed
  stable. `.local/t011/presentation-contract-checkpoint/log-review.md` records the
  independent full-log audit. These checks do not establish native or GUI behavior.
- Typed-solve simplification: the retained session passes a validated in-memory
  request to the shared native solve entrypoint and consumes its structured result.
  The temporary settings file, synthetic argv, terminal text/parse roundtrip, TLS
  runtime state and nested monitor are removed. Real regressions cover an existing
  directory at `session.settings.json` and monitor failure during publication;
  the latter preserves an independently valid saved result. Frozen Release PEs
  pass solve 6/6, controls 4/4 and runtime-session 10 executed cases with two
  practical-report cases explicitly skipped. Selected Debug CTest passes 9/11;
  cube64 and Unicode relocation exceed the unchanged cleanup deadline. A matched
  pre-refactor/final Debug cube64 comparison validates 64 copies then returns
  `DEADLINE_EXCEEDED` in both; it does not establish the Unicode failure's history
  or a speed improvement. The final Debug publication regression passes.
  `.local/t011/runtime-simplification/` retains source/binary manifests, causal
  reds, complete logs and the comparison. Independent Sol review accepts this
  bounded interface; no whole adapter-memory, practical or ticket gate is closed.
- Portable export checkpoint `2292f1a`: captured Windows alias/canonical paths
  and the certified file-name pin remain valid through final JSON publication.
  Full I/O Debug/Release suites pass 65 cases / 12,789 assertions each on the
  preserved pre-format binaries; final equivalent assertion formatting was rebuilt
  and its affected case passes 18 assertions each. Actual path/mutation and
  pre-admission reds are retained. Primary verified all 98 prior snapshot entries
  and preserved final sources/objects/binaries; Luna audited complete indexed logs.
  Manifest and limits: `.local/t011/portable-held-final/manifest.json` and
  `log-review.md`. This closes that component checkpoint, not the adapter ledger,
  integrated runtime, practical journeys or a ticket gate.
- Old engine: `.local/t009/native/final/bin/spectrapack-engine.exe`, SHA-256
  `a593d750f7a4fd09e324bdb70b8feccc33c47799e001ee171614888a65332b8d`.
  Its embedded build metadata predates merged `b82117a`; the binary hash identifies
  the measured implementation without claiming it was built from that exact merge.
- Preparation input identities, settings and numerical targets:
  [preparation-profiles.json](../tests/fixtures/t010/preparation-profiles.json).
  Raw measurements: `.local/t010/inventory/desktop-prepare-old-final/`.
- Old Start timings, work, ordered poses and native validity:
  [start-baseline.json](../tests/fixtures/t010/start-baseline.json).
  Raw measurements and process-memory samples: `.local/t010/start-baseline/`.
- Native test-first source revision: `43da0eb`. Complete commands, build/binary
  identities and logs: `.local/t010/native/stage1-handoff.md` and sibling artifacts.
  Four intended red cases are retained. Existing solver checks are 94/95 cases in
  both Debug and Release; the ranked-orientation admission failure remains open.
  No empirical preexisting-failure claim is made for that case.
- Shared contracts at `4914ca1`: 31 existing, 29 desktop and 7 runtime fixture
  checks pass; generated schemas/types and direct TypeScript checking pass.
  This qualifies contract artifacts only.
- Rust retained transport: `cargo check --locked --no-default-features --tests`
  exits 0 in `.local/t010/integration/cargo-check-3.log`. Earlier infrastructure
  and compilation failures remain retained. This is compilation, not runtime evidence.
- Native controlled checkpoint `25fdbac`: Release passes 23 assertions / 3 cases in
  `.local/t010/native/stage2-controls-green-2.log`, covering expired inspect/weld/
  display/validation, pre-bootstrap expiry, and unsupported counts without work.
  Its intermediate 22/23 run exposed an empty bootstrap publication after expiry;
  that failure and its repair remain retained. Fields/FFT cancellation, whole
  process lifecycle and ordinary practical safe-stop measurements are still open.
- Native memory checkpoint `4550bec`: Debug and Release each pass 184 assertions /
  22 cases in `.local/t010/native/stage2-admission-debug-green.log` and
  `stage2-admission-release-final-green.log`. Zero/below/exact allowances exercise
  real import and weld paths; full Pryanik 2 retains its frozen pre-change vertex,
  triangle, volume, work and nested-cavity report. The audited native bound is
  249,598,976 bytes, with caller-owned buffers and reserves separately charged.
  Parser temporaries are released before analysis rather than counted as live
  throughout both phases. Complete red/green details and the six-file manifest are
  in `.local/t010/native/stage2-admission-handoff.md`. This does not establish
  integrated session/export or worker admission, nor measured process peak memory.
- Combined Release engine builds in `.local/t010/integration/engine-build-1.log`.
  Initial retained-session process checks pass 2/2 in
  `.local/t010/integration/session-green-1.log`. They exercise a real cube token,
  pinned authority after source/report mutation, immutable preview, Stop followed
  by a fresh deadline run, request replay/conflict and idle shutdown. They do not
  qualify practical phase latency, active shutdown or transactional memory admission.
  The source checkpoint is `6e6fbda`; formatting followed that successful build,
  so the formatted source still requires rebuild verification. Its pre-format
  executable is retained in `.local/t010/integration/checkpoints/initial-session-green/`,
  SHA-256 `4e6f8fbffa401e907f698efa027318c90590debfdb773ca2e8122d5288d9408f`.
- Native export/residency checkpoint `19736a5`: Debug and Release each pass
  validation 45 cases / 2,673 assertions and export 26 / 155. Actual-byte import
  admission replaces the old export scratch multiplier. Certified outward bounds
  of actual quantized copies skip earlier-copy import only on strict separation;
  equality/uncertainty retains the exact fallback. Retained context/solution
  payload queries include their owned capacities. The final practical check is
  **4/5 cases, 90/92 assertions, exit 42**: Ulamok36 and full Pryanik1 retained2
  pass, while full Pryanik2 retained2 still returns `EXPORT_IMPORT_WORK_LIMIT`.
  Pryanik2's first copy consumes 1,010,009,258 import units, of which 944,182,452
  occur in self-intersection; the second copy exhausts the unchanged aggregate
  1.3-billion cap. Its tracked native payload peak is 263,242,908 bytes, not RSS.
  Commands, full logs, ordered poses, source/binary hashes and preserved binaries
  are indexed in `.local/t010/native/stage2-export-residency-handoff.md`.
  This qualifies the native checks only; immutable staged-file publication and
  complete desktop export journeys still require integrated verification.
- Focused Rust transport red/green is retained in
  `.local/t010/integration/cargo-large-budget-red.log` and
  `cargo-transport-green.log`. Rejecting an oversized budget after sending it
  previously left the next response with `ENGINE_IDENTITY`; local admission now
  precedes sending. Two focused tests pass for same-session reuse and stalled-input
  reclamation; one ignored test is child-process test infrastructure. These checks
  precede the next integration checkpoint and do not qualify all lifecycle gates.
- Projected-export checkpoint `9f4a60f`: Debug and Release each pass analytic
  28 cases / 1,983 assertions, import/weld 22 / 184, export 26 / 155, validation
  45 / 2,673 and practical/control 6 / 103 (127 cases / 5,098 assertions total).
  Full Ulamok36 and both full Pryanik retained2 exports are valid. Pryanik2's actual
  quantized copies consume 602,051,815 and 602,138,044 import units: 1,204,189,859
  combined, below the unchanged 1,300,000,000 cap. Its first-copy self-intersection
  phase falls from 944,182,452 to 536,225,009 units, including failed certificate
  attempts and legacy fallback work. All 40 ordered Ulamok/Pryanik poses match
  the preceding checkpoint in both builds. Ordinary import/weld/replay retains the legacy
  predicate policy because repair recipe bytes include its counters. Proof, work
  table and complete log index are in `.local/t010/native/stage2-projected-handoff.md`
  and `stage2-projected-audit.md`. `stage2-projected-identity.json` binds the 11
  source files, ten preserved executables and three practical source assets;
  `stage2-projected-pose-check.json` records the ordered comparisons. The observed
  diagnostic red is retained. Integrated export journeys and successful serial
  spectral fields remain pending; this is not a whole-Start speed result.
- Runtime integration checkpoint `be81da1` has a formatted Release engine in
  `.local/t010/integration/checkpoints/adapter-checkpoint-green/`, SHA-256
  `3aed0dc7cec25ba542f622e4a78806cce7d1e13a05ac253f08c5c849a0b24ace`.
  Its manifest binds all 34 owned source files and complete log paths. Release
  contracts pass 347 assertions / 42 cases; Debug passes 349 / 43, including its
  additional CRT allocation case. Eight real process tests and three presentation
  tests pass; Rust library tests pass 25 with two ignored child-process helper
  fixtures. Logs under `.local/t010/integration/` include
  `runtime-adapter-checkpoint-green.log`, `contracts-{debug,release}.log`,
  `ui-workflow-strengthened-green.log` and `cargo-library-admission-green.log`.
  Cases cover retained authority after source/report/PLY mutation, reuse without
  duplicate footprint charging, native clock-range rejection, no-incumbent timing,
  malformed shape/identity recovery and cold/phase-qualified preparation interruption.
  The 0.35 s cold-preparation budget records 0.377150 s native completion and
  0.027150 s overrun; Stop completes 0.036865 s after its marker. Separate
  phase-qualified preparation Stop/EOF complete in 0.008694/0.054285 s.
  The final process run treats ResourceWarning as an error and is clean; the
  earlier unclosed-stdin fixture warning remains preserved with its original log.
  The full-size wire-capacity red measured 1,510,974 bytes. Count-before-allocation
  serialization and capacity-based queue admission pass seven assertions / two
  cases, preserving a full 1 MiB legal record plus delimiter. Further lifecycle,
  actual I/O, parser/cached-copy memory and practical performance gates remain open.
- The fixed three-validator allocation probe measures 6,468,848 peak C++ payload
  bytes on Release, within the adapter reserve's 7 MiB catalog portion. The
  adapter reserve is 16 MiB within the unchanged aggregate host cap; retained
  assets, display, viewer and heavy operation owners are charged separately.
  `.local/t010/integration/adapter-memory-audit.md` records the scope and limits.
  The corresponding Debug probe exceeds that portion at 8,075,352 bytes. Shortening
  the run-settings validator's lifetime reduces the maximum simultaneous catalog
  count to two; focused probes then measure 5,385,296 Debug / 4,313,656 Release
  peak bytes. The complete simultaneous variable-buffer capacity ledger, bounded
  runtime diagnostics from nested I/O and whole-process peaks remain open. This
  is not a process-RSS bound or completed integrated memory gate.
- Additional hardening reproducers are now observed, before their fixes. A 1 MiB
  malformed runtime record allocates 26,712,966 parser/error bytes; a 512 KiB asset
  token allocates 2,000,168 scratch bytes beyond its admitted allowance; a finite
  129-character number passes the new auxiliary limit. Increasing escaped request
  paths grows the cached canonical capacity to 540,262 bytes above 524,352, and a
  real result rejection retains a 204,895-byte diagnostic despite requested
  16-issue/256-byte limits. Complete logs are
  `.local/t010/integration/runtime-memory-corrected-red.log`,
  `runtime-canonical-red.log` and `runtime-diagnostics-red.log`.
  Against the preserved `be81da1` engine, prepare A / prepare B / release B /
  prepare C fails with `ASSET_BUSY`; `runtime-replacement-release-red.log` records
  this transactional rollback defect. These are open failures, not qualification.
- Shared field-admission declaration `9eba3cd` compiles in the following native
  Release build. Its optional value remains empty until actual-layout admission
  is implemented. Serial raster, ranked-orientation and full-catalog reproducers
  show their intended failures in `stage3-{raster,ranked,ulamok}-red.log`; the existing
  10,000-cell cap and Ulamok's 48-correlation assertion remain unchanged.
  A further translated-window red shows the prior estimate allows 8 cells where
  actual voxelization at origin 2^50 mm uses 10. Its full log and source/binary
  identities are in `.local/t010/native/stage3-window-red-*`; ADR `85eb3bd`
  specifies interval-based admission.
- Serial raster/admission checkpoint `43f3864` passes focused Debug and Release
  gates: conservative fields 26 cases / 3,968 assertions, solver excluding the
  four `[qualification]` cases 110 / 4,803, and the separate hidden Ulamok advice
  case 1 / 237. Reusing already occupied boundary cells avoids redundant SAT
  predicates while charging each visit and lookup. The allocation-free interval
  window estimate covers actual voxelization for 468 solid/origin/pitch/rotation
  combinations, including translated origins; the ranked case retains its
  10,000-cell cap. The full log and command index, 15 source hashes, six preserved
  executable hashes, three asset hashes and primary verification are in
  `.local/t010/native/stage3-serial-small/`. Build logs have no compiler warnings;
  passing solver logs retain Catch2 numerical diagnostics. Both analytic benchmark
  scopes pass one-sample smoke checks with no warmups. These are not timing
  qualification. The retained full-catalog Ulamok red improves from 8 to 12 of
  48 correlations but still returns `SPECTRAL_PIPELINE_PLACED` at the unchanged
  work cap. Suggested-pitch evidence only establishes preflight admission so far.
  Per-Start field reuse, successful practical serial baselines and actual
  suggested-pitch execution remain open; threading targets are not frozen.
- Bounded blocked-field clone checkpoint `338f87e` passes the full conservative
  fields suite in Debug and Release: 27 cases / 4,683 assertions each, with both
  build and test exits 0. The clone shares the immutable mask while independently
  owning admitted counts, compressed footprints and IDs; overlap/removal, memory
  denial, Stop and deadline preserve its source. Actual Debug allocation failures
  exposed allocating `noexcept` PMR constructors/moves in the selected MSVC STL;
  throwing count constructors and same-resource swaps now return owned failures.
  A separate observed red measured 10,927 reported bytes against 11,015 required;
  the clone peak now includes its initial PMR storage. Full final logs, the four
  source hashes, two preserved executable hashes and primary hash verification
  are indexed in `.local/t010/native/stage4-clone/handoff.md`. Independent review
  read all final build/suite logs; no compiler warnings or errors were found.
  Earlier failing executable copies were overwritten, so their retained logs and
  cdb stacks are not presented as preserved red binaries. Partial-construction
  failure reporting was still pending at this checkpoint. The solver
  workspace remains unqualified; this checkpoint does not close a combined gate.
- Admission-stat checkpoint `ea8d60d` passes complete conservative fields in Debug
  (27 cases / 4,694 assertions) and Release (27 / 4,686), plus representation
  accounting in each configuration (13 / 239). Build and test exits are 0. The
  focused clone/add reruns are subsets, not additional full-suite coverage.
  Successful checked planned-live reservations now contribute to the separate
  `admitted_bytes_upper_bound`; refused requests do not. A constructor observer
  captures allocations freed after a later constructor failure, while caller
  reserve stays excluded. The actual Debug red reported 6,243 bytes where 6,259
  were required, and the successful clone reported a zero admitted bound in both
  builds. Both red executables and four final executables are preserved with the
  four source hashes in `.local/t010/native/stage4-admission/`; primary verified all
  ten hashes. Independent review read the complete red, final build/suite and exit
  logs. Five wrapper builds retain D9025 (`/EHc` overridden by `/EHc-`); the
  accounting-only builds have none. Final header wording changed comment-only
  after testing. This closes the bounded reporting defect, not workspace,
  integrated memory or practical performance acceptance.
- Cold full-Pryanik-2 characterization is retained in
  `.local/t010/integration/cold-red.json` and `cold-green.json`. The old legacy
  search-only 0.01 s request takes 10.162734 s total; its Stop test takes 8.752341 s
  after the marker. The new total-Start request expires before accepted loading
  and returns in 0.338446 s; Stop returns in 0.044026 s after the marker
  (0.257469 s total). Both new outcomes return owned interruption errors without a
  result. These establish pre-incumbent command boundaries; the fixed marker delay
  does not establish which heavy preparation phase was active.
- Runtime input/publication checkpoint `9303a8c` passes full I/O in Debug and
  Release (62 cases / 12,594 assertions each), memory checks (8 / 29 each), wire
  checks (3 / 11 each), three actual CLI settings probes, and 11 real protocol
  cases with no skips. The preceding protocol run's two skips remain recorded.
  Public JSON parsers pre-admit lexer/error scratch; the auxiliary runtime also
  bounds tokens before parsing, moves cached owners without copy growth, forwards
  diagnostic limits, and restores the prior asset after releasing a failed
  replacement. Settings admission now precedes the large raw-buffer allocation.
  The real CLI distinguishes `MEMORY_LIMIT`, `INVALID_SETTINGS` and `SETTINGS_LOAD`
  without publishing a result.
  An exact-filename red exposed `FILE_RENAME_INFO` missing its wide-NUL terminator:
  publication returned success for `packed.stl0300` instead of `packed.stl`.
  The corrected request preserves the certified handle through commit and tests
  exact directory contents/hash, read-only identical reuse, conflicting bytes and
  prior JSON preservation. The old 120 KiB hash test reached an earlier 128 KiB
  pin reservation; separate tests now exercise both the pin and actual hash
  boundaries. Full logs, failed attempts, 25 sources and preserved executables are
  indexed in `.local/t010/integration/checkpoints/io-rename-nul-green/manifest.json`;
  primary verified all 239 source/artifact/snapshot/object hashes. Independent
  review read complete final short logs; the large initial compiler-error dump
  was not exhaustively audited. Debug retains D9025; earlier include-order and
  test-compilation failures remain recorded. Release uses consistent native
  `43f3864` headers/libraries; Debug I/O uses native geometry `ea8d60d`. These do not
  qualify the current solver workspace. Full-Pryanik-2 cold deadline completes in
  0.410 s, cold Stop in 0.079 s after its marker, and phase-triggered Stop/EOF in
  0.019/0.014 s. Canonical case/junction references, post-rename pathname mutation,
  the complete simultaneous 16 MiB adapter ledger, remaining lifecycle/DTO work
  and final integrated practical/performance checks remain open.
- The retained-session benchmark harness in `benchmarks/runtime/` passes six
  evidence/transport safeguards, all-four-profile input verification and one real
  analytic cold/retained smoke pair. Each result is native-valid with one copy and
  the frozen ordered pose/work; both children exit 0 without forced termination or
  reader/writer/sampler faults. This is a smoke check, not repeated performance
  acceptance. It uses the held `9303a8c` integration executable, SHA-256
  `b15303e3dfdfa8ef5c169f58fef012d6acf90bfa4a46bf220282620fcec85028`.
  Full logs and command records are in `.local/t010/runtime-benchmark/HANDOFF.md`;
  primary verified 115 final source/input/executable hashes and independent log
  review read the complete referenced records. Failed timing-reader, blocked-pipe,
  nested-reference and old-settings probes remain recorded. The final change after
  smoke only strengthens frozen-settings provenance; safeguards and input checks
  were rerun. A consistent final engine and the full repeated series remain open.
- [ADR 0013](../spec/decisions/0013-cpu-runtime.md) now approves certified
  placed-field support reduction after selective architecture review. It requires
  outward-cell omission proofs, original-window fallback and full-solid component
  classification, and explicitly permits newly proved conservative masks to differ
  from historical false positives. Implementation, independent cell oracles and
  actual suggested-pitch success remain pending; this is no new passing gate.
- Original source preservation: all ten assets match their pinned SHA-256 values
  in `.local/t010/source-preservation.json`.

All performance runs share one host and run serially. Failed and aborted samples
remain in the evidence. Before delivery, record the final revision, toolchain,
engine hash, complete relevant Debug/Release and desktop checks, focused critical
review, and exact outcome of every row above. Update each row only from actual
observations; open a ready PR only after all mandatory gates pass.
