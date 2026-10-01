# T-010 / T-011 joint acceptance ledger

Updated: 2026-10-01. Delivery branch: `feature/SOL-08-cpu-runtime`.
Prerequisite: merged T-009, `b82117a` (PR #12).

This ledger records demonstrated scope. **No combined acceptance gate is complete.**
The final integrated source revision and executable hashes remain pending.
[T-010](T-010.md), [T-011](T-011.md) and [ADR 0013](../spec/decisions/0013-cpu-runtime.md)
define the acceptance criteria and compatibility contract. A schema check, a
successful build, or a retained valid result after a resource failure does not
establish the corresponding runtime gate.

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
| T011-A6 — startup measurements | Four old profiles each have one warmup plus five successful preparation and Start samples; numerical targets and peaks are frozen. | Equivalent new cold/warm samples at thread 1, identical fixed work and ordered valid poses, measured peak/cleanup bounds, complete response time and regressions. |
| T011-A7 — compatibility | Optional runtime schemas, shared fixtures and generated types pass their checks. | Legacy and new Save/move/Open, original timing semantics, pending/result association, thread policy, independent JSON/STL export and repaired-geometry invariance. |

The original T009-A2/A3/A6 obligations are covered by T010-A7 and T011-A7 only
when their concrete full-catalog, pitch-advice and checked-export assertions pass.
A single shared journey may satisfy both rows; duplicate runs are unnecessary.

## Retained evidence and qualification

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
- The formatted integration engine is retained in
  `.local/t010/integration/checkpoints/admission-green/`, SHA-256
  `97a5d55b8e3d560951102162cdeb14a6def791c133d11280c847aa4a90fe58e2`.
  Its manifest binds source hashes and identifies the subsequent fixture-only
  stdin-close correction. Release contracts pass 347 assertions / 42 cases;
  six real process tests pass; Rust library tests pass 25 with two ignored
  child-process helper fixtures. Full logs are `contracts-release.log`,
  `runtime-admission-green-1.log` and `cargo-library-admission-green.log` under
  `.local/t010/integration/`. These include retained authority after source/report/
  PLY mutation, reuse without duplicate footprint charging, native clock-range
  rejection, no-incumbent timing and cold/phase-qualified preparation interruption.
  The 0.35 s cold-preparation budget records 0.378307 s native completion and
  0.028307 s overrun; Stop completes 0.027065 s after its marker. Separate
  phase-qualified preparation Stop/EOF complete in 0.028412/0.040922 s.
  The first process log includes an unclosed-stdin fixture warning; the clean
  corrected rerun and remaining malformed-record/lifecycle checks are pending.
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
- Cold full-Pryanik-2 characterization is retained in
  `.local/t010/integration/cold-red.json` and `cold-green.json`. The old legacy
  search-only 0.01 s request takes 10.162734 s total; its Stop test takes 8.752341 s
  after the marker. The new total-Start request expires before accepted loading
  and returns in 0.338446 s; Stop returns in 0.044026 s after the marker
  (0.257469 s total). Both new outcomes return owned interruption errors without a
  result. These establish pre-incumbent command boundaries; the fixed marker delay
  does not establish which heavy preparation phase was active.
- Original source preservation: all ten assets match their pinned SHA-256 values
  in `.local/t010/source-preservation.json`.

All performance runs share one host and run serially. Failed and aborted samples
remain in the evidence. Before delivery, record the final revision, toolchain,
engine hash, complete relevant Debug/Release and desktop checks, focused critical
review, and exact outcome of every row above. Update each row only from actual
observations; open a ready PR only after all mandatory gates pass.
