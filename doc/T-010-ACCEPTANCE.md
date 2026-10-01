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
| T010-A4 — aggregate resources | Unchanged 512 MiB host cap and old measured process peaks are pinned. | Worker/queue/stack/scratch admission, cap immediately below admission, active allocation failure, measured peak versus conservative estimate. |
| T010-A5 — Stop and races | Shared control and fresh-operation ownership contract are frozen. | Real parallel-phase Stop acknowledgement within 250 ms and safe completion within 5 s; failure/shutdown joins; no late publication. |
| T010-A6 — measured speed | Failed practical phase profiles identify raster work; these are not a successful speed baseline. | Successful serial revision, frozen phase/end-to-end/memory targets, one warmup plus five Release samples at 1/2/4/8 threads, preparation held fixed. |
| T010-A7 — product regression | Original full-catalog Ulamok and full36 checked-STL failures are reproduced. | Supported full-catalog Ulamok count >=36; both full Pryanik runs; pitch advice; invalid-original rejection; complete product journeys and independently reread exports; timed quality. |
| T011-A1 — one budget | Native injected-clock red allows work after expiry. The controlled checkpoint now expires before bootstrap without work or a new handle. | Cold production loading/preparation, planning and validation consume one Start budget; no work begins after deadline; pre-incumbent expiry and cleanup/overrun measurements. |
| T011-A2 — real reuse | Exact old preparation and fixed-work Start records are pinned. Rust retained-session code passes a compile check. | Native replay/LOD reuse and identity evidence; New/replacement/units/frame/repair/epoch invalidation; fresh job derivatives for changed constraints/pitch/catalog; bounded replacement ownership. |
| T011-A3 — tamper resistance | Native authority/pinned-input contract is frozen. | Independent source/PLY/repair/settings/catalog/pose mutations; moved archive restore; failed Open preserves complete state. |
| T011-A4 — Stop phases | Session availability red and shared cancellation APIs are retained. | Actual cold-preparation cancellation red and green; real loading/preparation/fields/FFT/validation/publication Stop; receipt <=250 ms and safe completion <=5 s; UI stopping/result behavior. |
| T011-A5 — lifecycle races | Rust retained transport and test targets compile. | Repeated/stale Stop, completion race, marker/transport failure, child failure, EOF/shutdown and late completion using built native code. |
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
- Native controlled checkpoint: Release passes 23 assertions / 3 cases in
  `.local/t010/native/stage2-controls-green-2.log`, covering expired inspect/weld/
  display/validation, pre-bootstrap expiry, and unsupported counts without work.
  Its intermediate 22/23 run exposed an empty bootstrap publication after expiry;
  that failure and its repair remain retained. Fields/FFT cancellation, whole
  process lifecycle and ordinary practical safe-stop measurements are still open.
- Original source preservation: all ten assets match their pinned SHA-256 values
  in `.local/t010/source-preservation.json`.

All performance runs share one host and run serially. Failed and aborted samples
remain in the evidence. Before delivery, record the final revision, toolchain,
engine hash, complete relevant Debug/Release and desktop checks, focused critical
review, and exact outcome of every row above. Update each row only from actual
observations; open a ready PR only after all mandatory gates pass.
