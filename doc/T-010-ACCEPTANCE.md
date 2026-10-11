# T-010 / T-011 cross-ticket evidence ledger

Updated: 2026-10-11. T-011 merged at `977c27c` (PR #13), following T-009
`b82117a` (PR #12). Current delivery: `feature/SOL-08-cpu-threading`.

This ledger records demonstrated scope. **T-011 local acceptance is complete**;
its user merge is complete. **Bounded T-010 raster and FFT acceptance passes
locally** with the final native revision `3d3343e` and CSS-only follow-up `8950fea`.
PR #14 is returning to review; new-head hosted CI and user merge are separate.
Historical passes remain tied to their recorded workloads and revisions, including
two-copy Pryanik cases. Final native source/PE pins and actual desktop evidence,
including resource failures and slower GUI observations, are recorded below.
[T-010](T-010.md), [T-011](T-011.md) and [ADR 0013](../spec/decisions/0013-cpu-runtime.md)
define the acceptance criteria and compatibility contract. A schema check, a
successful build, or a retained valid result after a resource failure does not
establish the corresponding runtime gate.

[ADR 0014](../spec/decisions/0014-runtime-delivery-split.md) separates delivery:
T-011 first owns T011-A1–A7 and the carried T009-A2/A3/A6 practical obligations;
T-010 then owns T010-A1–A7 after the T-011 user merge. Keep historical evidence
and frozen targets here; changing the delivery order closes no gate. T010-A7
later regresses the successful practical workflows delivered by T-011.

## Practical threading completion prerequisites (2026-10-11)

The user authorized finishing measured multithreading before further packing
refinement. The bounded design is in T-010 and ADR 0016. This subsection records
the prerequisite checkpoint while PR #14 was draft.
Evidence is under `.local/t010/completion-20261010/`. These prerequisite checks
do not establish FFT parallelism or a new performance qualification.

- Box-baseline batching reaches 64 independently valid copies using 2,544 pair
  checks under the unchanged 3,000-check cap; the observed old implementation
  retained 26. Fresh full-layout validation uses 2,016 checks. The first new pose
  remains separately validated; STL containment retains individual submission.
- Critical review exposed MSVC Debug allocation in a default `noexcept` pending
  vector constructor. The corrected throwing construction and proxy admission
  pass the targeted allocation-refusal and exact-boundary checks. The isolated
  red exits through the installed terminate handler (86); its historical source
  reconstruction is documented, not represented as a contemporaneously pinned
  red binary. Reviewed final baseline files are in `stage1-frozen/`.
- Removing only speculative startup footprint reservation lets the real 64-copy
  pipeline fit within 2 MiB: observed admission 1,022,116 bytes versus the old
  hypothetical startup estimate of 6,657,624 bytes. Exact-cap transactional
  add/clone checks retain old and staged ownership and verify rollback. The
  original 2 mm full-container fixed-buffer refusal remains explicit.
- Fresh spectral search runs before retained-layout extension, while the valid
  baseline stays centrally retained and work remains cumulative. Its interruption
  regression first fails four assertions, then passes all 30: Stop/deadline during
  empty-layout validation retains actual spent work and the exact published
  handle, with the correct interruption status.

The corrected fixtures and carried passing cases cover all **151 Release / 153
Debug ordinary solver cases**, with eight heavy practical/qualification cases
explicitly omitted. This is a union of recorded runs, not one final clean module
run. The first Release collection passes 149/150; the Debug collection reaches its
240-second watchdog after 121 completed cases. A local-symbol debugger identifies
a 16-byte MSVC vector-proxy allocation in empty-trial construction. The narrow
throwing-construction/admission repair passes ten assertions in each build and
retains the exact publication. It does not justify a broader constructor rewrite.

Two corrected candidate-ID/fault-injection fixtures pass 96 assertions in each
build; the previously uncompleted Debug tail passes 30 cases / 613 assertions.
The candidate-ID case independently proves its two-copy witness and proposal,
then distinguishes a measured 860,244-byte long-ID peak from its 856,116-byte
Release cap, with native-valid rollback. Critical review approves the final
production and fixture changes. Complete logs, exclusions and source/binary pins
are indexed by `stage2-final-coverage.json` and `stage2-final-frozen/pins.json`.
The Release benchmark builds successfully. Prerequisite production C++ changes
are +291/-161 lines (net +130), excluding tests and the diagnostic benchmark.
The following checkpoint freezes the subsequent serial baseline and targets.

### FFT baseline and target freeze

All 36 Release observations on `68e3d6b` (six profiles, one warmup plus five
samples each) complete with independent native validation and identical work and
ordered poses within each profile. Ulamok 4 mm has medians of 15,466.9751 ms for
Start and 10,354.7242 ms for actual FFT axes; full-box Pryanik 2 at 4 mm/fixed
identity retains 280 copies with a 2,223.6807 ms Start median. These are bounded
one-candidate/one-pass runs, not full-search quality or 2 mm/cube qualification.

Evidence: `serial-baseline-68e3d6b/` below the completion artifact directory;
the manifest pins source, binary, inputs and all raw observations. Independent
log review confirms six exits 0, empty stderr, native validity, reproducibility
and matching raw hashes. [ADR 0016](../spec/decisions/0016-cpu-raster-threading.md#measured-fft-completion-decision-2026-10-11)
and [numeric targets](../tests/fixtures/t010/fft-threading-targets.json) are frozen
in `b46c9fb` before implementation: at four threads, at least 2x FFT and 1.5x
Start speed versus retained and same-build serial, at most 10% one-thread Start
regression and at most 32 MiB process-peak growth. At this freeze checkpoint,
parallel acceptance was pending; the final matrix below supplies that evidence.

### FFT functional checkpoint

The frozen `fft-green-review/` sources pass independent critical concurrency and
accounting review. The original real-call overlap regression exits 42 with four
failed assertions; the new implementation overlaps actual FFT calls at two/four
threads. Independent compute oracles and resource/control tests pass 23 cases /
25,565 assertions in each build; selected solver cases pass 12/826 in Release and
13/836 in Debug. Source/binary pins and carried-check identities are recorded in
that directory's manifests and `evidence-summary.json`.
The FFT production C++ delta is +531/-48 lines (net +483), excluding tests,
CMake and the 25-line benchmark extension. Together with the prerequisites,
this completion extension adds 822 production lines and removes 209 (net +613).

The frozen Release engine also passes all 13 desktop protocol tests and two real
runtime Stop/reuse cases in `integration-native-first/`, including current-policy
execution and preservation of legacy result history. Marker-write/safe-stop times
are 0.4232/48.3199 ms in voxelizing and 0.6896/99.0246 ms in FFT; marker-write time
is not UI acknowledgement. One four-thread Ulamok feasibility observation takes
5,841.4127 ms Start / 2,702.7248 ms FFT axes, retains the same native-valid 36 poses
and uses 1,150,976 extra process bytes. `fft-smoke-4/` is a single observation,
not the required repeated matrix or practical Stop qualification.

### Final FFT performance matrix on `3d3343e`

All **144 observations** pass: six profiles at 1/2/4/8 threads, each with one
warmup and five timed samples. Every output is independently native-valid;
geometric work and ordered poses match repetitions, retained serial and same-build
one thread. Independent raw-log review confirms all 24 process exits are zero,
stderr is empty and the frozen target/input hashes match. No benchmark sample was
retried. A local evaluator's initial misreading of validity enum `0` as a string
is retained separately; it is not a failed native observation.

| Fixed-work profile | 1 thread, ms | 2 threads, ms | 4 threads, ms | 8 threads, ms |
| --- | ---: | ---: | ---: | ---: |
| Analytic | 23.271 | 22.267 | 22.796 | 22.543 |
| Ulamok 16 mm | 712.772 | 467.874 | 354.034 | 333.685 |
| Ulamok 4 mm | 15592.940 | 8949.235 | 5898.726 | 4998.572 |
| Pryanik 1, small box | 868.980 | 602.811 | 442.312 | 406.852 |
| Pryanik 2, small box | 1543.998 | 1188.654 | 1004.623 | 944.116 |
| Pryanik 2, full box / 4 mm / fixed | 2258.309 | 1806.010 | 1574.756 | 1498.066 |

The table reports rounded Start medians; exact values and all samples remain in
`fft-final-targets-3d3343e.json`, `fft-final-one-3d3343e/` and
`fft-final-multi-3d3343e/`. Benchmark SHA-256:
`41bdbd5fc5891e7c4eabb4bee14a236895190ff078c910e54c6a7458bc6caa9a`.
Source is the committed, clean product revision; unrelated agent configuration
edits are excluded. Measurements use the same qualified host and unchanged caps.

Four-thread Ulamok 4 mm achieves **2.622x Start / 3.724x FFT** versus retained
serial and **2.643x / 3.746x** versus same-build one thread. Actual FFT medians
are 10416.7667 / 5141.9464 / 2780.6553 / 2043.6167 ms at 1/2/4/8 threads.
Both frozen speed gates pass. Maximum one-thread time ratio is 1.027345, below
1.1; maximum process-peak growth is 2,473,984 bytes, below 33,554,432. The tiny
analytic profile gains little overall, and eight-thread scaling is sublinear.

Maximum process peak is 87,531,520 bytes. Maximum conservative tracked peak is
202,515,477 bytes, including logical caller ownership where specified; it is not
RSS. Both worker teams and simultaneous scratch remain admitted under 512 MiB.
Observed FFT overlap reaches 2/4/8. Parallel groups contain at most 64 lines /
8,384 complex values in this matrix; larger serial whole-axis calls are reported
separately. These fixed-work results pass A3/A6 and the measured-memory portion
of A4; practical Stop/restart and timed-quality/product integration remain separate.

### Final packaged runtime and practical observations

The final engine built from `3d3343e` has SHA-256
`2582fabbdc07ed0951183b8d7dd9bf7b5d3afff927c1ba40f5345f0d1ade887d`;
the Release desktop has
`18cf0d2db43823bf274d05eb30d965b9dc5ecbcd941864b900bb265139f444f7`.
`package-3d3343e/pins.json` pins the package and identical staged engine. Embedded
revision text includes `-dirty` because unrelated agent configuration edits remain
in the checkout; binary hashes and the frozen product source pins identify the build.

`practical-final-3d3343e/` retains the exact user settings: box 400 x 340 x 285 mm,
pair/wall clearance 1/1 mm, cube rotations, seed 42 and 60 seconds from Start.
These single wall-budget observations are not repeated speed qualification:

| Profile | Threads | Wall seconds | Engine CPU seconds | Valid copies | Native terminal |
| --- | ---: | ---: | ---: | ---: | --- |
| Ulamok, 4 mm | 1 | 43.988 | 42.625 | 36 | Recoverable field-work limit |
| Ulamok, 4 mm | 8 | 13.920 | 53.984 | 36 | Recoverable field-work limit |
| Pryanik 2, 2 mm | 1 | 1.276 | 1.281 | 306 | Spectral memory admission refused |
| Pryanik 2, 2 mm | 8 | 1.296 | 1.266 | 306 | Spectral memory admission refused |

All four ordinary native responses have `ok:false`. Collection exit 0 means the
observations were recorded, not that search succeeded. Ulamok performs identical
work/poses at both counts, including 138 correlations / 69 pages, then stops at
`SPECTRAL_PIPELINE_OBJECT` / `FIELD_KERNEL_WORK_LIMIT`. Average engine occupancy
is 0.969 versus 3.878 core equivalents. Pryanik 2 mm performs zero correlations:
its conservative admission is 746,817,601 / 778,959,049 bytes against the unchanged
536,870,912-byte cap. The 306-copy baseline remains native-valid; it is not optimal.

The explicit alternative 4 mm Pryanik cube observation is in
`practical-supported-pitch4-final2582/`. Only requested/resolved pitch changes.
Memory admission succeeds and four correlations / two pages execute before the
unchanged field-work cap stops search. It retains the same 306 valid ordered poses
in 7.250 seconds; tracked peak is 215,001,501 bytes and process peak 110,292,992.
This exercises a memory-admitted pitch, not successful unrestricted cube search.
New refinement, orientation reuse and capacity changes remain future work.

The separate eight-thread practical FFT-phase Stop returns `user_stopped` with 36
valid copies. Marker write takes 3.2046 ms and marker-to-safe-terminal 34.8711 ms;
the next operation starts 25.5836 ms later in the same native process and completes
the ordinary Ulamok observation above. Both sessions exit normally. Marker timing
is not UI acknowledgement: the unchanged Rust receipt evidence is 0.6873 ms.
Together with actual active-FFT-worker Stop/failure/join tests and retained raster
Stop evidence, these checks satisfy A5 without repeating unaffected qualification.

The actual packaged desktop opens the legacy project, runs the new eight-thread
4 mm settings, and retains 36 native-valid copies after the field-work limit.
Save (0.2 s), Open/revalidation (1.3 s) and JSON export (0.6 s) complete. Independent
archive/export rereads verify all manifest hashes/sizes, exact settings/current
policy, asset bytes and all ordered poses. `gui/independent-audit.json` and
`gui/ui-observations.json` retain this evidence. Sampled FFT progress states keep
the previous completed scene visible without a shared-display-mesh loading message;
this is sampled evidence, not continuous responsiveness or live-incumbent delivery.

The first GUI run takes 34.6 seconds (saved native total 34.5019), versus 13.9092
native seconds in the isolated session with identical settings/assets/work/poses.
FFT accounts for 27.3848 versus 10.5060 seconds. A second GUI observation without
intermediate screenshots takes 42.9 seconds. `gui/cpu-observation.json` records
process CPU counters for the bounded discrepancy diagnosis; these slower observations
are retained, not replaced by the isolated native timings. The following bounded
desktop check addresses this discrepancy without another native qualification loop.

The only continuous CSS animation was the busy bar's background position. The
independently reviewed `8950fea` removes those 90 bytes while preserving the static
marker, phase text, elapsed time and Stop control. A rebuilt Release desktop passes
the same practical run with unchanged engine `2582fa...887d`; final desktop SHA-256
is `74c23c062e4f55cde5606796c1d6b505a4b84130ae3451dcb06a6d0b3b6d346e`.
Its archive independently matches all assets/settings/36 ordered poses and 698
candidates / 24 passes. Native elapsed is 29.412761 seconds; the UI reports 29.5.

Comparable middle windows, five through twenty-four seconds after observed Start,
show frontend CPU falling from 1.795 to 0.647 core equivalents (64%), with WebView
CPU falling from 1.720 to 0.498 (71%); engine occupancy rises from 1.796 to 2.897.
`gui/static-bar/cpu-comparison.json` retains timestamps, sums and sampling limits.
This supports the reduced UI-work claim, not a controlled packing-speed claim:
the contemporaneous headless run with the desktop idle also takes 27.433 seconds,
versus the earlier isolated 13.920, and other accessible host CPU varies. The
exact cause of that remaining host/runtime variation is not established. The
repeated fixed-work matrix, not these individual GUI observations, qualifies speed.
Final build logs, binary pins, saved archive and independent audit are in
`gui/static-bar/`. Unaffected Open/JSON/STL export evidence is carried across this
CSS-only change; no native code or dependency changed and no native matrix is rerun.

### Final acceptance mapping

| Case | Final applicable evidence |
| --- | --- |
| T010-A1 | Real 2/4/8 FFT overlap; supported/resolved policy tests; 13 real protocol cases; actual eight-thread desktop result and legacy Open |
| T010-A2 | 23 compute cases / 25,565 assertions per build, independent correlation oracles, unchanged authoritative validation and retained raster geometry oracles |
| T010-A3 | All 144 final observations preserve work and ordered poses across repetitions and 1/2/4/8 counts; original raster matrix retained |
| T010-A4 | Aggregate owner/scratch exact-boundary tests, partial startup and active-worker allocation/fault rollback, 2.36 MiB maximum measured process growth |
| T010-A5 | Active FFT-worker Stop/deadline/failure/join tests, practical 34.8711 ms safe Stop and same-session restart, retained Rust receipt and raster-stage checks |
| T010-A6 | Pre-implementation target hash preserved; four-thread Ulamok 2.622x Start / 3.724x FFT versus retained serial; all regression and memory limits pass |
| T010-A7 | Final packaged Save/Open/JSON export identity audit, unchanged source/accepted geometry, 36-copy lower bound; retained three practical checked-STL journeys and rejection cases; explicit timed resource outcomes and 306-copy limitation |

This closes the bounded CPU-threading slice locally. It does not establish
unrestricted many-copy cube search, optimal packing, live incumbent streaming,
near-linear whole-app speed, another platform, or full product milestone acceptance.

## Many-copy Pryanik repair (2026-10-10)

The user case uses full `pryanik_2.STL`, 400 x 340 x 285 mm, 2 mm pitch,
cube rotations, eight threads, pair/wall 1/1 mm and a 60 s Start budget.
Seed 42 is inherited; the failed operation's seed was not recoverable.
This extends the earlier two-copy evidence; it does not replace or generalize it.
Source SHA-256 is `6e69e7606566b608155d193c3db9baa920fff3507907f88ce12243887cfd62ac`.

| Check | Observed result |
| --- | --- |
| Diagnostic-only replay | 15 retained copies, failed five-second final publication, 65.009 s total. This reproduces the practical failure, not every field of the original 77.549 s report. |
| Fresh many-copy validation (`dcf2e8e`) | One warmup plus three samples at 1/8/32/128/280 copies: all 20 valid, identical poses and deterministic reports. Final 280-copy samples 24.691/20.515/27.760 ms, 587,472 validation bytes plus 96,252,273 caller bytes. Each pass charges 948,781 work units. Unchanged five-second/512 MiB targets pass. |
| Cardinal bounds/scoring (`2e08701`) | Intended public-query and 280-copy Incumbent red/green. All 24 cube rotations use exact accepted extrema with charged work and zero vertex visits; near-cardinal queries retain the bounded generic path. Focused critical review has no remaining findings. |
| Native geometry replay (`dcf2e8e`) | 306 copies independently validated and published in 25.247 s, following 9.656 s preparation before Start. Publication 9.847 ms, zero overrun, tracked peak 100,342,147 bytes including caller residency. |
| Remaining search limit | Recoverable `RESOURCE_LIMIT / PHYSICAL_VALIDATION_RESOURCE`: 1,299,999,999 charged work units under the unchanged 1.3-billion cumulative cap, 3,046 candidates, ten passes, no spectral work. Published result is retained; count is not an optimum. |

The geometry replay engine SHA-256 is
`7ff2b2d0493e07cae817482f0e890aab270eeb8df824e92c6ee771f8afd3124f`.
Complete observations, failed intermediate attempts, source/binary pins and review
snapshots are under `.local/t010/pryanik-many-20261010/bounds-first/`;
the final runtime case is `replay-final-setup-400x340x285-2mm8/`, and the final
fresh-validation profile is `final-validation.json`.
The old validation observations were single samples, so no repeated speedup ratio
is claimed. These repairs reduce unnecessary geometry work; they do not establish
near-linear threading, early desktop incumbent delivery or bulk STL export of
hundreds of full-resolution copies. PR #14 remains draft for the outstanding
native scaling and early-publication work.

Affected geometry gates on `dcf2e8e` pass in Debug/Release: validation 57 cases,
selected solver 34, conservative fields 46 and export validation 26 per build;
representation accounting passes 16 Debug / 14 Release. Physical bounds (16) and
allocation refusal (1) pass; their Release evidence is carried from unchanged
query code. Result builder passes all 67 Debug cases. The Release full run passed
66/67 before two expected-code assertions were corrected for validator-owned
memory refusal; the corrected case then passes all 25 assertions, including
forwarded cause and no output. The exact-boundary admission assertions stay intact.
Exact selected names and complete final logs are in `bounds-first/gates/final-setup/`.
An accidental interrupted rerun overwrote the first full solver log; that partial
log is not used as complete evidence. The final selected solver logs and focused
bootstrap red/green are retained.

The controlled deadline gate also exposed a pre-existing dangling failure-code
view. Repair `3fa60cf` owns up to 64 cause-code bytes inline, preserving allocation-
free outcome transfer and sizeof-based metadata admission. Deterministic red
reproduces source mutation; focused green verifies destruction, allocation-refused
copies/moves and capacity/overflow. Critical review has no remaining findings.
Its affected selection passes 37 Release / 38 Debug cases (769 / 811 assertions);
the additional raw-buffer-peak case is Debug-only. Geometry code and its measured
profile are unchanged by this diagnostic repair. Captured protocol outcomes are
recorded separately from the original unexplained EOF, whose child exit was lost.
All three final captured observations pass with native exit 0, one retained copy,
the exact search-work cause, the separate cleanup deadline and unchanged previous
result bytes. `bounds-first/ownership-evidence-summary.json` pins the final engine
`98813e250c89b4b7d3414fd7e5cd44d1ec53143bd629dd5bcaa08d7c7a78a128`,
its complete logs and carried geometry evidence. No root-cause claim is made for
the original EOF; the independently reproduced dangling cause is repaired.

The final Release desktop bundle uses source `3fa60cf`, the same final engine
above, and app SHA-256
`5d55e08f3afc5a300efdfc8aaf3264e3db3d250c1a84afb6b0192928a25c7deb`.
The complete package build exits 0; its engine pin and all 167 staged resources
match. Actual GUI verification imports full Pryanik 2 and runs the settings above:
**306 native-valid copies appear in 42.5 s** (rounded UI elapsed), with the
recoverable search-work warning and zero recorded Start overrun. This separate
observation is slower than the isolated native replay; no cause or speedup ratio
is inferred. The object preview remains present during sampled progress states;
new packing copies still appear only when the operation terminates.
GUI Save completes in 1.2 s and Open/revalidation in 32.6 s, preserving the
306-copy layout and settings. Initial full import takes 30.0 s, outside Start.
The saved project is `.local/t010/pryanik-many-20261010/gui/pryanik-many.spectrapack`;
package logs/pins are in the sibling `package/` directory and original GUI session
evidence is retained in `gui/`. Bulk packed-STL export was not rerun or qualified.

## Final T-011 local acceptance (2026-10-09)

The final critical integration review reports no actionable findings and confirms
retained evidence applicability. Its remaining normal Pryanik 1 desktop check
passes after the Codex restart restored Computer Use. No source change or broad
native rerun followed the reviewed checkpoint `373334a`.

| Gate | Final local disposition |
| --- | --- |
| T011-A1 | Pass: retained injected/real preparation expiry and work cutoff, truthful publication-expiry regression, both final native profiles and actual normal Pryanik 1 GUI completion. |
| T011-A2 | Pass: retained preparation/LOD performance and eighteen-row Debug/Release reuse/invalidation matrix, supported New/replacement paths and pinned ownership bounds. |
| T011-A3 | Pass: retained source/accepted/repair/settings/catalog/pose mutation checks, failed-Open preservation, moved archives and final affected IO/export reruns. |
| T011-A4 | Pass: composed phase-observed native Stop, Rust receipt/completion and actual GUI evidence; current validation/publication controls and bounded practical deadline tails. Artificial holds remain labeled, not ordinary-latency measurements. |
| T011-A5 | Pass: retained repeated/stale Stop, marker/transport/child/shutdown/EOF/backpressure and guarded state-transition evidence. No exhaustive scheduler claim. |
| T011-A6 | Pass: retained frozen startup/preparation/memory targets and corrected analytic samples; final review confirms applicability. No new full benchmark series or comparative GUI speedup claim. |
| T011-A7 | Pass: earlier actual Save/move/Open/legacy/pending-result/checked-export journeys remain applicable; final GUI normal Start and independently reread archive complete integration. |
| Carried T009-A2/A3/A6 | Pass for the recorded serial slice: Ulamok36/full48 correlations, resource/pitch advice, both full Pryanik normal spectral runs and practical desktop/export journeys. Simplified-source repair remains a later ticket. |

Final actual GUI evidence uses desktop `378548c8...ca8fb` and engine
`f3121d9d...720f`. Original full Pryanik 1 settings are unchanged: 84,820 triangles,
box 100 x 100 x 50 mm, pair/wall 0.1/1 mm, identity orientation, pitch 4 mm,
CPU 1, seed 0, total_start 30 s. The GUI shows `solve · finished`, two valid
copies and `budget exhausted`, with 32.5 s operation elapsed. No Stop was issued.
Saved precommit total is 32.2136227 s, overrun 2.213622701 s and cleanup
0.0001869 s. Save completes in 0.8 s. Independent ZIP reread verifies all five
manifest files, unchanged source/accepted bytes, original settings and exact
prior placements. This reread verifies archive integrity/association; the
recorded native validator and separately completed checked exports establish
their respective validity/export evidence.

Archive SHA256: `1597edeafccb06aeae681c64c0d0e11492f1b68102eebdd2eba1c27971199dd1`.
Artifacts: `.local/t011/gui-qualification/final-20261009/normal-start-result.png`,
`normal-start-result.txt`, `gui-observation.json`, `normal-start.spectrapack`,
`archive-verification.json`. Original bundles and failed observations remain.
The ready PR/user review and merge are delivery steps, not permission to begin
dependent T-010 early. Full AT/M milestones are not closed by this slice.

## Final T-010 local acceptance (2026-10-10)

Source `6235d1c28d93cc1554783d8cc84decd5d5572b8f` implements only boundary
raster parallelism. Focused critical review closes both identified corrections
with no remaining actionable finding. Windows resolves 1 through
`min(8, hardware_concurrency)`; other platforms retain one pending qualification.
Legacy count-one `serial-v1` results remain executable/loadable; counts above one
use `raster-rows256-v1`. Existing desktop capability handling supplies the range
and a new-draft default of at most four, without a second configuration path.

| Gate | Final local disposition |
| --- | --- |
| T010-A1 | Pass: real overlapping workers, distinct IDs and useful work on every requested lane; supported/unsupported counts, two-thread CLI execution/restore and legacy serial regression. |
| T010-A2 | Pass: independent conservative-field expected outcomes and every-translation binary/proximity oracles at 1/2/4/8; all 96 practical/analytic fixed-work observations independently native-valid. |
| T010-A3 | Pass: one warmup plus five timed samples for each of sixteen configurations; exact counters and ordered poses match repetitions and retained serial. Native fixed-work has no RNG input; product journeys use seed 0. |
| T010-A4 | Pass on the qualified host: below/exact admission, partial startup, active allocation/worker failure, precise quota refund/resume, measured 1 MiB worker stacks and retained memory observations. OS margin limits are stated below. |
| T010-A5 | Pass by composed evidence: actual-worker Stop/join/deadline tests at dispatch, active row, barrier and parked boundaries; fresh operation and retained incumbent checks; corrected 256-primitive bound. Unchanged Rust Stop receipt evidence is reused (0.6873 ms); ordinary practical deadline tails below remain within five seconds. No exhaustive race or universal latency claim. |
| T010-A6 | Pass: all frozen speed, one-thread regression and process-memory thresholds in the complete Release matrix below. |
| T010-A7 | Pass: full Ulamok36/48-correlation and Pryanik fixed-work profiles, three new four-thread Rust Save/move/Open/export journeys and independent STL rereads. Unaffected original-invalid, pitch advice, legacy/pending-result and actual GUI evidence from final T-011 remains applicable. |

Three product runs use the original practical requests with explicit four threads,
seed 0 and `total_start`. Each Rust journey passes one test, with zero failures or
ignored cases and thirteen unrelated tests filtered out. Each independent reread
verifies exported float32 vertices, copy ranges and unchanged source identity.

| Product profile | Valid copies | Start-to-observed-completion | Native precommit total | Deadline overrun | Candidates / passes |
| --- | ---: | ---: | ---: | ---: | ---: |
| Ulamok, 60 s budget | 36 | 16.800 s | 16.592 s | 0 s | 824 / 24 |
| Full Pryanik 1, 30 s budget | 2 | 30.563 s | 30.343 s | 0.343 s | 14 / 1 |
| Full Pryanik 2, 30 s budget | 2 | 30.814 s | 30.690 s | 0.690 s | 27 / 1 |

All three report `budget_exhausted`: Ulamok reaches bounded work before its wall
deadline; Pryanik runs reach their time budget. Counts remain best-found results,
not optimality claims. These observations report time-budget quality separately
from fixed-work speed. No comparative GUI speed claim is made. Product artifacts,
requests, logs, moved archives and rereads are in
`.local/t010/threading-product-6235d1c/` (engine SHA256
`e9e6a0514670c334d724b5dbee92ecf494756dcd7539569858af122b044946fa`).

Final threading C++ delta is +703/-122 (net +581), including benchmark/support
headers and extracted existing code, excluding tests and CMake. New raster
header/internal/source files total 411 lines. Cleanup separately removes a net
sixteen lines; the larger estimated consolidations remain excluded. There is no
second kernel, generic scheduler, GPU/SIMD change or speculative cache. Full
AT/M milestones remain open. Hosted run `37995333831` is pending; local acceptance
does not claim a hosted pass.

## Current T-010 delivery (2026-10-09)

Base: merged T-011 `977c27c`; branch `feature/SOL-08-cpu-threading`.
Minimal compile repair `2fa15da` reproduces the current-main C2676/C2512 and passes
the same representation-benchmark target in Debug and Release. It does not claim
native test-suite completion. Complete logs and terminal records are in
`.local/t010/intake/` and `.local/t010/ci-repair/`.

Hosted run `37988936772` at `2fa15da` confirms native compilation, then reports
518/525 Debug tests passed and seven 30-second watchdog aborts, with no CTest
skips; Release and downstream qualification did not run. Configuration repair
`005169e` assigns finite watchdogs only to those seven named tests. Six practical
cases receive 1,200 s, supported by retained successful Debug qualification
(the Ulamok/Pryanik field pair took 547.683 s, individual export phases exceeded
33 s). The 468-rasterization matrix receives 120 s: its held pre-threading Debug
binary passes one case/3,747 assertions in 39.032 s wall time (34.518 s Catch time).
The held source matches `43f3864`; the exact named test body remains unchanged,
although later added tests change the containing file's hash. This is watchdog
evidence, not current-threading-build qualification.

Independent configuration review and CTest discovery verify exactly seven added
TIMEOUT properties, preserving fifteen existing explicit timeouts and the ordinary
30-second default. An initial `if(TEST)` guard was ineffective in CTest's
interpreter; Catch discovery-list membership fixes it. Before/after discovery,
matrix commands/hashes and source applicability are retained in
`.local/t010/ci-repair/`. No hosted pass is claimed before the completed branch runs.

Cleanup `46d4bf3` removes R-3/S1 only (+4/-20, net -16). Independent source review
and focused Debug 12/618, Release 11/599 solver cases/assertions plus two desktop
cases each pass. Configuration-specific Debug allocation cases explain counts;
no skips are treated as passes. `.local/t010/cleanup/evidence.txt` records the
path-dependent residency comparison, its bounded equal-length-path check and
unchanged preview/prepare outcomes. Larger consolidation proposals remain excluded.

Serial threading baseline at `46d4bf3` passes with the existing `spectrapack_t010_benchmark`
`--scope prepared-start --samples 5 --warmups 1` for analytic, Ulamok, full Pryanik 1
and full Pryanik 2 profiles. Accepted preparation is fixed/excluded; fresh job
context, baseline, spectral work and final independent validation are included.
All four profiles complete the full recorded work with identical repetition
counters/ordered poses and unchanged 512 MiB/1.3-billion-work caps. The process memory high-water includes
accepted preparation and earlier samples; it is not a per-sample allocation measure.
Do not substitute the T-011 one-copy/zero-correlation preparation series.

The host inventory in `.local/t010/intake/host.json` records i5-12450H, eight
exposed cores/logical processors, Windows 11 build 26200, about 32 GiB RAM and
Balanced power. Compare 1/2/4/8 threads with serial host sampling. The
[threading targets](../tests/fixtures/t010/threading-targets.json) are frozen before
implementation: at four threads, Pryanik 1 raster must reach at least 1.5x and
fixed-work Start at least 1.15x against both retained serial and same-build
one-thread measurements. Every profile's one-thread Start median may regress by
at most 10%; process peak may increase by at most 32 MiB at every qualified count.
These limits do not raise the managed host or geometric-work caps.

| Serial profile | Fixed-work Start median | Raster median | Process peak |
| --- | ---: | ---: | ---: |
| Analytic | 25.860 ms | 2.661 ms | 7,073,792 B |
| Ulamok | 9,081.144 ms | 922.714 ms | 7,827,456 B |
| Full Pryanik 1 | 2,989.635 ms | 1,744.193 ms | 52,826,112 B |
| Full Pryanik 2 | 5,887.429 ms | 2,274.538 ms | 83,939,328 B |

Raster is the sole selected parallel kernel. Pryanik 2 uses 1,289,972,641 of
1,300,000,000 representation-work units, so duplicated geometric preparation
cannot be hidden or admitted by raising the cap. Raw samples, complete commands,
host/build metadata and source/binary identity are in
`.local/t010/threading-baseline-20261009/`. At this baseline checkpoint all
T010-A1–A7 remained open; this serial table alone is not speedup evidence.

Current native TDD red: `T010 two-thread CPU request completes real spectral work`
builds and links successfully in Debug, then fails its first assertion with
`CPU_THREAD_COUNT_UNSUPPORTED` (one case/one failed assertion, CTest exit 8).
The intended missing-support failure is independently confirmed from the complete
`.local/t010/threading-implementation/windows-ninja-debug-red.log`; later work
assertions are not claimed as executed after that fatal assertion. This red alone
does not establish parallel execution; subsequent checks are recorded below.

The first stable implementation passes thirteen selected geometry tests and four
solver tests in both Debug and Release, plus real two-thread CLI execution/restore
and omitted-count legacy checks. CTest's passing logs do not expose assertion
totals; none are inferred. Production delta at that checkpoint is +700/-123 C++
lines (net +577), including extracted existing code. The source manifest and
complete attempted/final checks are in
`.local/t010/threading-implementation/HANDOFF.md`.

Focused critical review found two corrections before acceptance: restore the
existing coordinator-side allocation-failure hook on cell-visit exhaustion, and
bound Stop checks by 256 primitive attempts rather than 256 two-primitive cell
iterations. Both reproduce before repair: three existing allocation regressions
observe missing hook calls, and the new long-row test observes 510 primitives
after Stop at two and four threads. The bounded repair changes five production
lines and removes one; 117 test lines check coordinator affinity, settlement,
original failure preservation and the real-worker primitive bound. Debug and
Release each pass eleven focused cases, with full logs and setup failures in
`.local/t010/threading-review-fix/HANDOFF.md`. The existing hook is `noexcept`;
its interface is unchanged. Independent closure review verifies the final manifest
with no mismatches and closes both findings, with no actionable issue in the
repair delta. Final source is committed as `6235d1c`; Release engine and benchmark
are rebuilt against that revision. Hosted run `37995333831` is in progress;
the completed local matrix and product journeys are mapped above. The initial
single-sample probes remain diagnostic only.

### Frozen Release matrix completed on `6235d1c`

All sixteen profile/count configurations complete one warmup and five timed
samples (96 observations). Every sample is independently native-valid and has
the same geometric-work counters and ordered placements as the retained serial
baseline. All frozen numerical targets pass. Reproduction commands,
source/build/host identities and raw samples remain under
`.local/t010/threading-qualified-6235d1c/`; `summary.json` records all comparisons.

| Profile | Threads | Fixed-work Start median, ms | Raster median, ms | Start speedup vs retained serial |
| --- | ---: | ---: | ---: | ---: |
| Analytic | 1 | 27.012 | 3.037 | 0.957x |
| Analytic | 2 | 26.327 | 1.812 | 0.982x |
| Analytic | 4 | 26.034 | 1.379 | 0.993x |
| Analytic | 8 | 26.706 | 1.624 | 0.968x |
| Ulamok | 1 | 9,258.785 | 942.718 | 0.981x |
| Ulamok | 2 | 8,867.969 | 518.378 | 1.024x |
| Ulamok | 4 | 8,816.750 | 327.013 | 1.030x |
| Ulamok | 8 | 8,531.075 | 290.627 | 1.064x |
| Full Pryanik 1 | 1 | 3,093.716 | 1,811.307 | 0.966x |
| Full Pryanik 1 | 2 | 2,410.859 | 1,090.876 | 1.240x |
| Full Pryanik 1 | 4 | 1,994.687 | 682.340 | 1.499x |
| Full Pryanik 1 | 8 | 1,793.196 | 539.829 | 1.667x |
| Full Pryanik 2 | 1 | 6,052.512 | 2,379.169 | 0.973x |
| Full Pryanik 2 | 2 | 5,096.479 | 1,453.285 | 1.155x |
| Full Pryanik 2 | 4 | 4,796.941 | 990.199 | 1.227x |
| Full Pryanik 2 | 8 | 4,401.158 | 731.312 | 1.338x |

The qualifying four-thread Pryanik 1 result achieves 1.499x Start / 2.556x raster
versus retained serial and 1.551x / 2.655x versus same-build count one. Both exceed
the frozen 1.15x / 1.5x thresholds. One-thread Start regressions are 4.46%, 1.96%,
3.48% and 2.80% respectively, all below 10%. Small analytic jobs gain no overall
speed versus the old serial build; Ulamok's serial work limits total scaling.
These are fixed-work results, not time-budget quality claims.

`worker-memory-evidence.json` verifies distinct actual worker IDs and nonzero
primitive counts for every requested lane in all 96 observations. Each background
worker's measured virtual stack extent is 1 MiB. Team admission is 2.25 / 6.25 /
14.25 MiB at 2 / 4 / 8 threads, including a 256 KiB fixed allocation and 1 MiB
runtime/OS margin per worker beyond its stack reservation. The largest process
peak increase is 573,440 B (0.547 MiB), below the frozen 32 MiB limit; practical
process peaks stay about 8 / 50.4 / 80.1 MiB. Raw records retain field admission,
tracked peak and accepted-input residency separately. Process peak includes
preparation and earlier samples; it is not a virtual reservation or heap cap.
The margin is qualified for this Windows host and these profiles, not asserted
as a universal OS guarantee. Existing 512 MiB and 1.3-billion-work caps are unchanged.

## Gate evidence accumulated before final closure

The outstanding columns below describe historical pre-closure evidence needs,
now resolved for the local delivery by the final T-011 and T-010 mappings above.

| Gate | Evidence retained so far | Evidence still required |
| --- | --- | --- |
| T010-A1 — configuration | Old CLI rejects two threads; native red tests show unsupported counts execute work. The controlled serial checkpoint now rejects unsupported 0/2 requests without work. | Real selected parallel kernel; actual supported/resolved counts; specific rejection outside range; desktop pending/result association. |
| T010-A2 — numerical correctness | Existing oracle suite is available. | Affected direct field/correlation/proximity oracles at 1/2/4 threads, asymmetric/rotated/offset/halo/uncertain cases, independent validation of every compared result. |
| T010-A3 — reproducibility | Old one-candidate Start baseline repeats identical count, work and ordered poses. | New fixed-work repetitions for each supported measurement count, with seed, policy, counters and failed samples retained. |
| T010-A4 — aggregate resources | Unchanged 512 MiB host cap and old measured process peaks are pinned. Native import/repair admission passes below/exact-bound tests and the full Pryanik 2 profile in Debug/Release. | Integrated owner/worker/queue/stack/scratch admission, active allocation failure, measured peak versus conservative estimate. |
| T010-A5 — Stop and races | Shared control and fresh-operation ownership contract are frozen. | Real parallel-phase Stop acknowledgement within 250 ms and safe completion within 5 s; failure/shutdown joins; no late publication. |
| T010-A6 — measured speed | Failed practical phase profiles identify raster work; these are not a successful speed baseline. | Successful serial revision, frozen phase/end-to-end/memory targets, one warmup plus five Release samples at 1/2/4/8 threads, preparation held fixed. |
| T010-A7 — product regression | Original full-catalog Ulamok and full36 checked-STL failures are reproduced. Native checked exports pass Ulamok36 and both full Pryanik retained2 cases in Debug/Release at `9f4a60f`, within unchanged caps and with identical ordered placements. | Supported full-catalog Ulamok count >=36; both full Pryanik spectral runs; pitch advice; invalid-original rejection; complete product journeys and independently reread exports; timed quality. |
| T011-A1 — one budget | Controlled native expiry rejects new bootstrap work/handles; cold preparation expires without a result. Final Release Pryanik 1/2 each publish two valid copies under the original 30 s profile, with 0.4524/0.7815 s precommit overrun. Deterministic finalization expiry reports failure and preserves native/disk authority. Earlier actual Pryanik 2 desktop timing remains recorded. | Final integration mapping and actual final Pryanik 1 desktop observation; no work-after-deadline proof from wall timing alone. |
| T011-A2 — real reuse | Exact old preparation and fixed-work Start records are pinned. Debug/Release native transition checks verify real constraint/pitch/catalog effects, units authority changes, frame/repair rejection, old-token preservation and cross-build session-token rejection. | Final mapping of practical preparation/LOD timing, supported New/replacement paths and bounded ownership; final practical integration. |
| T011-A3 — tamper resistance | Held Debug/Release restore/repair mutation and active reuse cases pass, including settings/catalog and rejected replacement preservation. All three moved practical archives restore; Rust failed-Open preserves state. Final IO binding/checked-STL gates and both independent practical rereads pass. | Final evidence mapping; final actual desktop practical observation remains separate. |
| T011-A4 — Stop phases | Final Debug/Release loading Stop passes in 19/436 ms and preserves previous result/preview/token authority. Preparation Stop/EOF, analytic fields/FFT Stop, late-validation control/usage regressions and valid terminal-publication Stop pass. Rust receipt and GUI stopping have distinct evidence. | Practical phase/cleanup coverage and final integration mapping. Artificial-barrier and Python marker-write measurements do not independently establish ordinary publication latency or Rust receipt. |
| T011-A5 — lifecycle races | Passed native/Rust evidence covers repeated/stale/finished Stop, marker and transport failure, child loss, shutdown, active EOF, backpressure and pre-registration New. Shared mutex guards serialize completion/Stop; active New/replacement is rejected as `JOB_BUSY`. | Final evidence mapping; do not invent an in-flight completion after successful active New, which the supported workflow forbids. No exhaustive scheduler claim. |
| T011-A6 — startup measurements | Four-profile series at 86a42db completes 48 operations; three practical Start targets pass. The schema correction's 12-operation analytic rerun passes at 38.3 ms versus 87.6 ms, with frozen work/count/pose and preparation/memory checks. Pinned Debug/Release adapter payload bounds now fit 16 MiB. | Final integration mapping and remaining phase-cleanup evidence; the analytic-only summary correctly does not claim a new four-profile series. |
| T011-A7 — compatibility | Optional runtime schemas, shared fixtures and generated types pass. Actual legacy Open displays search-only timing; changing pending pitch preserves original result settings. Ulamok and both full Pryanik Save/move/Open and independently reread JSON/STL exports pass on their recorded bundles. Final native Pryanik 1/2 restore/export/rereads preserve both pose fingerprints. | Final applicability mapping and actual final Pryanik 1 normal Start/result; earlier GUI journeys are not relabeled as current-bundle observations. |

The original T009-A2/A3/A6 obligations must pass in T-011, and remain T010-A7
regressions with their concrete full-catalog, pitch-advice and checked-export assertions.
A single shared journey may satisfy both rows; duplicate runs are unnecessary.

## Retained evidence and qualification

- Final publication correction (2026-10-09) passes its deterministic expiry
  red/green: a current-run nonempty incumbent no longer becomes successful empty
  completion when publication expires, and previous disk bytes remain intact.
  The admitted CLI path uses one source-private fresh-validation/shared-writer
  operation. Supplied-document binding, checked float32 STL, resource limits and
  the five-second cleanup allowance remain. Final Debug/Release each pass 67 IO
  cases / 12,854 assertions and 13/13 registered CLI CTests. Critical source and
  ownership review reports no actionable findings; the separate field-transfer
  review is closed. Exact sources, full gate logs and final engines are pinned in
  `.local/t011/publication-finalization-20261009/evidence-summary.json`.
  Release engine: `f3121d9d67d343ee18c7f03c8e9af960c4fac4c6f158c1f4b81dc1fcba80720f`.
  Debug engine: `c1a64d71f4e98c9bcd5b08a7c312e150bf00df27a664c9cbdc8f2fd75725c0da`.
- Both final original 30 s Release profiles pass in one run each: Pryanik 1 at
  30.4524016 s and Pryanik 2 at 30.7815421 s before commit, each with two valid
  copies, explicit result paths and consistent nonempty flags. Both prior pose
  fingerprints are unchanged. Fresh authoritative restore, checked STL export
  and complete independent JSON/STL/source/companion/every-vertex reread pass.
  These are native integration observations, not comparative speed measurements
  or actual GUI runs. Intermediate failures and the geometry-only engines remain.
- Final desktop build exits zero. The new preserved bundle and 169 file hashes
  are at `.local/t011/gui-qualification/final-20261009/`; desktop SHA256 is
  `378548c8b72f683acea6f78822d6fa2e83bb5db84d7b7459f161c063001ca8fb`, with the
  final Release engine above. All 50 desktop/contract/tool source pins match the
  earlier GUI qualification; the frontend is reused unchanged. The remaining
  actual final Pryanik 1 normal Start/result check is blocked before app access:
  Computer Use runtime initialization exits with `windows sandbox failed:
  helper_unknown_error: setup refresh had errors`. No new focus/lock/approval
  conclusion or GUI success follows from that tool failure.
- Final independent critical integration review reports no actionable findings.
  It verifies 22 source/log/engine entries, ten practical input pins, all 169
  bundle files and 50 unchanged desktop sources. A2/A3/A5 evidence and A6
  measurements remain applicable; A1/A4 compose existing budget/Stop observations
  with current validation controls and publication-expiry evidence. Earlier A7
  journeys remain applicable alongside affected native export reruns. Optional
  runtime skips carry prior evidence, not new observations. The only remaining
  acceptance observation is normal 30 s Pryanik 1 in the final actual desktop,
  followed by ledger update. Review: `.local/t011/remaining-native-controls/`
  `final-integration-review-20261009.md`; no blanket rerun is justified.
- The 2026-10-09 combined constant-scratch geometry repair passes Debug/Release
  validation (48 cases / 2,782 assertions each), ordinary solver/control gates
  (137 / 5,995 Debug; 135 / 5,942 Release) and the full saved practical case
  (1 / 27 each). Candidate eleven rejects the physical pair at 35,223,643 of its
  original 48,639,566 work allowance. Production scope is 98 added / 2 removed
  lines; prior numerical rejection, real cap failures and interval proofs remain.
  Release engine `e98f7246bdb00a03b67b00bb4a0cf066638f7eed77c86763b97bcc71d82264ab`
  completes original 30 s Pryanik 1 with two valid copies in 32.9386345 s, overrun
  2.938634501 s. Fresh authoritative restore, checked STL and the existing full
  vertex/source/companion reread pass; physical poses remain unchanged. Evidence:
  `.local/t011/validation-path-fix-20261009/FINDINGS.md` and
  `completed-evidence-summary.json`. This is native evidence, not a new GUI pass.
- Historical geometry-only Pryanik 2 reaches a retained nonempty incumbent but fails publication
  at 35.0072032 s. The unchanged session mapper reports `ok:true` and top-level
  `no_nonempty_incumbent:true`, contradicting native runtime `false`; no result
  file is committed. Exact settings and source/accepted hashes match the earlier
  GUI pass. The trace strongly supports expiry during a second fresh validation
  inside one five-second cleanup window; slower phase timing has no established
  cause. Preserve this failed run; no timing retry was used. The same critical
  reviewer approved a bounded correction in [T-011](T-011.md) and clarified
  [ADR 0013](../spec/decisions/0013-cpu-runtime.md): one IO-owned fresh-validation
  build-and-publish operation using the shared writer, explicit failure reporting,
  unchanged ownership, memory limits, supplied-document binding and checked STL.
  The deferred field-transfer review is independently closed against its original
  source/pins and allocation-failure logs, without repeating the field runs.
  The later native integration pass is recorded above; final actual GUI qualification remains open.
- The measured boundary-only guard is not a passing repair. Its practical red
  and attempted green each fail one of 27 assertions: the latter finishes the
  boundary scan at 17,681,549 work, spends 30,120,970 in a witness ray, then
  exhausts its remaining 837,047 in clearance leaf rejection without an exact
  distance call. The 36-line draft and reproducer are archived locally. A bounded
  clearance census rejects 83,380/84,820 rows, but its two-million-leaf prefix
  remains incomplete. An existing whole-solid zero-clearance separation proof
  succeeds in 12 units and could remove boundary/witness work; combining it with
  clearance-row rejection still needs review and practical proof. No success,
  cap change or new hierarchy is inferred. Evidence:
  `.local/t011/pryanik1-physical-resource/row-rejection/`, including
  `gap-diagnosis/` and `clearance-count/`. At the earlier checkpoint, fresh review was
  refused by the agent service's thread limit; the primary mapping in
  `.local/t011/remaining-native-controls/FINAL_MAPPING.md` was not independent.
  Critical review resumed on 2026-10-09; its outcomes are recorded above.
- Full Pryanik 2's actual desktop journey passes on engine `a44743a0...` with
  desktop `df36a1c3...`. Original 30 s Start-origin settings reach real fields,
  FFT and authoritative validation, then return `budget_exhausted` and two valid
  copies. Saved precommit total is 33.1675743 s, overrun 3.167574301 s and cleanup
  0.0002568 s; fields/FFT/validation are 7.2414913/0.0146414/25.7708694 s.
  Save (1.3 s), unchanged-hash Unicode/space relocation, New/Open (81.5 s), JSON
  (34.7 s) and checked-STL export (78.4 s) finish. Independent committed reread
  exits zero: five archive entries, original settings/result/source/accepted
  authority, both untouched export bundles, companion mapping and every float32
  vertex occurrence across 278,424 packed triangles match. The archive hash is
  `aff6314741056a4d50edd72ba5689e8a4c1828a22e10b1bb82c43aafdc40f2a8`.
  Evidence: `.local/t011/gui-qualification/JOURNEY.md`, `artifact-reread.md` and
  `pryanik2/artifact-verification/`. Import time is not a qualified performance
  sample because diagnostic compilation overlapped. Pryanik 1 remains separate.
- The bounded numerical/resource routing correction passes final Debug/Release
  geometry 45 cases / 2,673 assertions and affected solver 46 / 551 each.
  A fresh critical recheck reports no actionable findings after both review
  regressions turn green. Production scope is +76/-16 lines; caps, predicates,
  assets and schemas are unchanged. Final Release engine is `a44743a0...` and
  Debug `073dac66...`; all ten pinned source/binary identities match.
  `.local/t011/pryanik1-physical-resource/FINDINGS.md` records red/green evidence.
  The actual final-engine retained-session 30 s run nevertheless stops at
  25.603 s with `PHYSICAL_VALIDATION_RESOURCE`, after eleven candidates and two
  correlations, retaining two valid copies. Fields/FFT/validation take
  2.536/0.0096/23.031 s. The focused diagnosis identifies actual global validation
  work exhaustion: candidate eleven consumes its remaining 48,639,566 units in
  the exhaustive boundary triangle-pair scan. Memory, arithmetic and interruption
  flags are clear. See `final-diagnosis/FINDINGS.md` under the same evidence root.
  A bounded architecture review recommends measuring constant-scratch conservative
  row rejection before implementation. The earlier slower instrumented deadline
  result does not qualify the integrated run.
- Focused final-engine Debug/Release A2/A3 reuse transitions pass eighteen
  observable rows each. Constraint changes produce counts 8 to 1/1/12; finer
  pitch performs real correlations with larger tracked field storage; custom
  catalog changes actual poses and malformed hash/version/content is rejected.
  Frame/real repair tampering rejects replacement while the original token,
  complete result and preview remain usable. The identical source imported as
  inches changes accepted authority, yields 254 mm geometry and scales volume
  by 25.4 cubed. An actual older-build token is rejected by the final process,
  and verified re-preparation succeeds. One initial fixture expected the wrong
  stage's error for frame tampering; that failure is retained and the corrected
  assertion passes both builds. No product changes. Commands, full logs, source
  and executable pins: `.local/t011/reuse-transitions/README.md`. These analytic
  cases do not replace practical timing or prove every future version migration.
- Final-engine loading Stop passes in Release/Debug after one fixture repair
  adds the schema-required baseline marker path. Initial `INVALID_DOCUMENT`
  failures are preserved as setup failures. Corrected cases cancel before any
  preparing event, preserve exact previous JSON/preview bytes, reuse the old
  token for identical valid poses, and shut down cooperatively. Full logs and
  the failed integrated 30 s observation are in
  `.local/t011/remaining-native-controls/LOADING_TOTAL_START.md`.
  `REUSE_LIFECYCLE_MAPPING.md` maps passed lifecycle and reuse checks and the
  finite remaining units/frame/repair/settings/catalog/engine-transition cases.
- Actual GUI simplified-Pryanik import remains invalid with thirteen
  self-intersections, no accepted solid and disabled Start. The original source
  hash and untouched native report are verified under
  `.local/t011/gui-qualification/simplified/`. The temporary Computer Use
  foreground-PID failure was resolved by resetting its JavaScript connection;
  no product correction was involved.
- Actual legacy GUI Open restores two valid copies in 31.8 s and displays
  `Legacy search only`, original 4 mm pitch and 26.81 s search time. Changing
  pending pitch to 8 mm preserves the displayed 4 mm result and original 30 s
  settings. The legacy archive has neither new budget scope nor runtime fields;
  its historical resource-limit diagnostic remains historical output. Archive
  identity and screenshots are in `.local/t011/gui-qualification/legacy-input.json`
  and `screenshots/legacy-{reopened,pending-pitch}.*`.
- Six selected Debug/Release native checks pass: phase-observed preparation
  Stop/EOF, ordinary publication Stop, and the affected publication monitor
  failure regression. Publication Stop reaches the real native token and commits
  eight valid copies with the already-frozen `search_stalled` reason. Marker to
  terminal is 126/812 ms Release/Debug, including artificial holds of 40/25 ms;
  this is not ordinary publication latency or Rust receipt. Exact prior poses
  were not independently exposed. Publication uses explicitly pinned older
  compatible helpers; preparation uses the phase-corrected engines. Full logs,
  source identity mapping and limits are in
  `.local/t011/remaining-native-controls/PUBLICATION_PREPARATION.md`.
- Actual held desktop qualification completes Ulamok 36 at advised 4 mm within
  its 60 s budget, with native-valid `budget_exhausted` output; Save, unchanged-hash
  move to a Unicode/space path, New/Open, JSON and checked-STL exports pass.
  The committed independent reread verifies both bundles and every float32
  vertex in all 36 x 768 triangles, plus archive/source/accepted identities and
  exact saved poses/search/metrics/settings. See
  `.local/t011/gui-qualification/artifact-reread.md`. Held engine `76d2d9d9...`
  contains the schema correction but predates field/FFT phase emissions; do not
  use its phase totals to qualify corrected observability. Its unrestricted
  Pryanik 1 GUI run fails `PHYSICAL_VALIDATION_RESOURCE` on candidate three after
  retaining two valid copies. This is a new reproduced practical gap, preserved
  under `gui-qualification/pryanik1/failure`; earlier bounded profile successes
  do not cover it. A separate actual GUI Stop shows stopping then user-stopped
  completion with two valid copies. UI screenshots do not establish precise
  receipt/safe-completion latency; the original resource failure remains open.
  The separately user-stopped Pryanik 1 result completes Save/move/New/Open and
  both GUI exports; its committed reread also passes all 2 x 84,820 triangles'
  vertices and companion ranges. Its original unrestricted search is still a
  failing observation. These distinct results are indexed in
  `.local/t011/gui-qualification/JOURNEY.md` and `artifact-reread.md`.
- A focused instrumented current-source replay reproduces Pryanik 1's exact
  retained poses, count two and third-candidate failure, but exposes
  `KERNEL_BOUNDARY_UNRESOLVED` after only 14,629,609 of 1.3 billion work units and
  21,901,332 of 526,837,046 available validation bytes. Rounded world vertices
  make the boundary predicate unresolved; this is not demonstrated exhaustion.
  Some exhausted paths share that generic code, so the bounded repair must
  preserve their actual budget flags before rejecting ordinary numerical
  uncertainty. The replay uses pinned current libraries, not the byte-identical
  GUI executable; only seven phase emissions separate their native numerical
  implementations. See `.local/t011/pryanik1-physical-resource/` and the
  repair checkpoint in T-011. The subsequent bounded correction and final
  integrated outcome are recorded above.
- Four focused restore/repair mutation executions pass on the phase-corrected
  held Debug/Release engines. Existing cases reject five malformed restored
  result variants and six independent repair/source/accepted-asset mutations;
  accepted repair replay, JSON/STL restore and relocation also succeed.
  `.local/t011/restore-tamper-final/mutation-coverage.md` indexes exact commands,
  unchanged source/input/PE hashes and full logs/exits. No test or product change
  was needed, and these cases do not alone close every T011-A3 reuse/invalidation
  obligation.
- The existing cold total-Start preparation check on the phase-corrected held
  Release engine passes: native deadline completion 405 ms (55 ms overrun,
  11 ms cleanup), and Stop 85 ms after its marker. Debug correctly expires
  during loading with no result, but fails the fixture's assumption that a
  fixed 350 ms budget reaches preparation; its Stop branch does not execute.
  The unchanged failure and precise remaining loading/validation/publication
  coverage are in `.local/t011/remaining-native-controls/COVERAGE.md`. The later
  phase-observed preparation case passes both builds; the fixed-delay failure
  remains a failed fixture assumption, not a passing test.
- The former adapter residual term is closed for the pinned MSVC Debug/Release
  builds. Complete fixed-constructor event ledgers (zero drops, balanced at every
  event) plus focused source accounting bound the adapter payload by 16,560,714
  / 15,690,138 bytes, below the unchanged 16 MiB allowance. The bound includes
  separate two-catalog and late one-catalog filesystem/error peaks; direct
  exception allocations are accounted analytically. Source/layout dominance
  permits the conservative Debug variable terms in Release. See
  `.local/t011/adapter-bound-certificate.md` and
  `.local/t011/adapter-scratch-trace/{FINDINGS.md,release/FINDINGS.md}` for pins,
  formulas, failed instrumentation attempt and complete trace evidence. This
  closes the specific payload inequality without product changes; it is not
  an RSS, whole-process or cross-toolchain certificate or a full AT-16 gate.
- Seven public field/FFT phase emissions now connect real pipeline work to the
  existing runtime sink. Intended native red fails only missing/incorrect phases;
  affected Debug passes 29 cases / 774 assertions, Release 28 / 764. Four actual
  retained-session Stop observations complete in 27.28/102.87 ms Release and
  299.11/330.88 ms Debug for fields/FFT; marker writes are 0.419–0.572 ms. All
  preserve the identical validated one-copy baseline and shut down cooperatively.
  `.local/t011/runtime-phases/README.md` records exact held engines, full logs,
  fixture/setup failures and limits. These analytic observations do not qualify
  every practical workload, other phases, Rust receipt or GUI responsiveness.
  A fresh independent Sol review of this patch plus the schema hint reports no
  actionable findings (`.local/t011/serial-checkpoint-review.md`).
- Selective schema compilation preserves default eager construction, cross-kind
  validation, geometry checks and schema semantics. Its affected Release analytic
  series passes at 0.0382987-second retained Start (target 0.0876001), versus the
  preserved 0.1151591 failure. Debug contracts pass 35 cases / 1,097 assertions;
  Release 34 / 1,095; result binding passes 16 / 326 each. Production change is
  +48/-17 lines and tests +55. `.local/t011/analytic-start-fix/verification.md`
  and `log-review.md` index complete build/test/measurement logs, held engines,
  unchanged six-source pins, and the analytic subset's limits. The same held
  Debug engine passes both unchanged cube64 and Unicode-relocation subprocess
  gates, including all 64 placements and independently checked STL vertices.
  `.local/t011/debug-publication-after-schema/` preserves native output and actual
  artifacts; its initial evidence-wrapper import error happened before product
  testing and is recorded. The shared five-second allowance remains unchanged.
  A separate local export probe attributed 468 prior-copy imports, but the actual
  gates passing after the schema correction removed the need for a geometry edit.
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
