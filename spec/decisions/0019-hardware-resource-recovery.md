# ADR 0019: hardware-aware host admission and bounded CPU recovery

Date: 2026-10-11. Status: accepted implementation freeze for T015-P2. The coordinating
primary accepts the resource, ordering, ownership and compatibility decisions below
before production implementation. No
product implementation, performance measurement or acceptance result is claimed.
Requirements: SOL-05/SOL-08, SOL-04/SOL-06/SOL-07, AT-10/AT-12/AT-16/AT-17;
[spec section 6.6](../../doc/spectrapack-spec.md#66-compute-and-memory-policy).

## Decision and scope

Extend the existing serial solver and admission paths. New Starts use a sampled
hardware ceiling and distinguish timed allowances from explicit fixed work.
Consume completed FFT candidate pages before preparing the entire catalog in
the new timed policy. Recover only when an allowance, live ownership or execution
configuration has changed. Geometry and compute report outcomes; solver owns
retry/defer/order decisions; CLI owns OS sampling and serialization; Rust owns
cross-process retained-owner admission; React presents the resolved state.
Dependencies remain CLI -> solver -> geometry/compute, with dependency-free
resource values/callbacks in the existing common include target if needed.

Manual pitch, accepted geometry, physical dimensions, pair/wall clearance,
orientation catalog, native validation and count-only publication do not change.
The last immutable native-valid incumbent survives every unfinished trial.
Invalid and indeterminate candidates cannot publish; no FFT/field result conveys
validity. No GPU, new packing strategy, scheduler, dependency, geometry repair,
general resumable kernel framework or deferred DRY work is included.

## Host cap and nested ownership

At native Start admission, sample available physical RAM once before new large
allocations. With sample `A0` and optional downward user ceiling `U`, resolve
`H0 = min(floor(A0 / 2), U)`; omit the second operand when no user limit exists.
Record `A0`, `H0`, the requested user ceiling and the policy version. Unavailable
sampling fails explicitly for a newly resolved hardware policy; it must not
silently invent 512 MiB or use total installed RAM. The native CLI owns the
Windows sampler; tests inject numeric samples into its pure resolution seam.

`H0` is an aggregate operation ceiling, not an additional allowance for each
subsystem. Count existing accepted assets/source pins, prepared geometry/masks,
old and staged layout/footprints, retained and trial solutions, both correlation
outputs, full padded complex/real buffers and FFT plans/probes, parked worker
teams/scratch, page capacities, publication/serialization, adapter and viewer
reserves. Count shared owners once using the existing identity ledgers. Preserve
checked arithmetic and allocation-before-use admission. Report accounted peaks
separately from externally measured process peak memory; neither is the other.

The resolver explicitly supplies the operation ceiling to `SpectralLimits`, both
`BaselineLimits`, their `per_query`/`per_validation`, `per_representation`, and
`per_correlation`, and to loading/preparation/result construction/export. Existing
subtraction/reserve conventions remain: a callee that charges its input receives
an external reserve excluding that input; a scratch-only callee receives the
remaining allowance. Do not leave the default 128/512 MiB clamps active in a newly
resolved operation or allocate each nested call its own `H0`. Native library
defaults remain conservative for callers that do not opt into resolution.

Rust and native session/helper adapters share the admitted ceiling and subtract
retained session, viewer and adapter owners before concurrent helpers. Replace
the hard-coded 512 MiB transport validation with validation against that operation's
ceiling. The 1 MiB protocol record limit and bounded publication pools stay intact.
An auxiliary helper may receive only remaining bytes; it never independently
claims another half of RAM. Retained data exceeding a lower subsequent ceiling
causes refusal/eviction of disposable owners, not loss of the published result.

Timed recovery resamples at completed stage/page boundaries and before a retry.
For accounted live ownership `L`, additional allocation headroom is at most
`min(H0 - L, floor(available_now / 2))`. Use a checked stage admission ceiling
`min(previous_stage_ceiling, H0, L + floor(available_now / 2))`; it only decreases
within a run. This avoids treating the job's own already resident bytes as new
requests or repeatedly halving its original cap. Samples never authorize a cap
above `H0`. Stop/deadline polling precedes recovery and every re-entry.

## Finite recovery and useful work

Use one coordinator-local recovery state per logical operation key: layout
revision, orientation, page cursor/mode, and failing stage. A retry must strictly
change its admitted configuration; repeated identical denial is deferred, never
spun. Keep original nested cause/phase in a bounded recovery summary.

- Memory: first release disposable correlation data not needed by a completed
  page, unused LRU kernels and parked workers where useful. Retain prepared
  geometry, container mask and a committed layout while they fit. Retry once only
  if estimated live requirement decreased. Then permit one reduced-thread retry:
  select the highest smaller supported count whose checked estimate fits. If no
  count fits or its estimate does not decrease, do not retry for thread count.
  A single thread cannot remove shared grid storage or total work. Join/recreate
  execution owners before switching counts. An allocator failure uses the same
  finite ladder and is never itself evidence that a repeated allocation will fit.
- Timed work: existing finite work defaults become grant quanta, with the seed
  phase allocation clarified below, while actual
  cumulative counters never reset. At a completed stage/page boundary the
  coordinator can grant the next quantum while the original deadline remains;
  record cumulative granted ceilings independently from work consumed. A failed
  indivisible stage may be retried at most twice, with checked 2x then 4x its
  initial stage allowance. Charge failed work as well as successful work. Do not
  raise all limits to uint64 maximum. Exhaustion after those attempts defers the
  operation; it does not trigger whole-job termination when other pages or
  existing affordable refinement work remain.
- An explicitly lower fixed-work limit never renews. Structural bounds (indices,
  cells/padding, input triangles, copy/orientation counts) remain hard safety
  limits, independent of timed work grants. Counter overflow, malformed inputs
  and numerical/geometry errors do not become retryable budget exhaustion.

`SpectralWorkspace` already retains geometry, mask, committed layout, two kernels
and correlation outputs. Add separate validity for a successful binary correlation
and its proximity partner, keyed to exact context/lattice/layout revision and
orientation. A failed proximity call must reuse the successful binary result if
still resident. Completed pages own their candidate values independently of FFT
arrays. Partly built fields/pages never become completed values. On layout change,
invalidate affected pages/correlations together. Preserve existing transactional
blocked-field add/remove and committed old layout on failure.

`BlockedField` currently remembers creation limits and `operation_limits()` takes
their minimum with every later request. Add an explicit creation-time
`RepresentationLimits::renewable_work_limits` flag, default false. For flagged
solver-owned fields only, subsequent requested kernel-work/cell-visit allowances
are operation-local rather than clamped to the creation slice; memory and all
structural creation bounds still apply. Geometry does not grant work or retry.
Existing standalone/fixed callers retain the old clamp. This is necessary to
make coordinator renewals effective without rebuilding unchanged committed data.

Validation still returns indeterminate on exhausted resources. A candidate may
be reconsidered only after the same finite resource-change policy; a complete
native-valid pass is required. Preserve all attempt counts and reject unresolved
candidates. Do not introduce a second validator or weaken its checks.

### Timed phase handoff closure (2026-10-11)

The coordinating primary accepts the following minimal closure before widening
baseline/stagger loops. A grant is permission for bounded work, not a requirement
to retry the same phase. Baseline and stagger each receive **one finite seed-phase
grant** in this tranche. Their existing loop limits, including stagger's 1,024
proposal guard, remain local. On positively identified allowance exhaustion,
the timed coordinator records that phase as deferred and advances once to the
next existing affordable phase. It does not restart the seed phase or add
continuation state inside its loops. Preserve each phase's actual work, partial
native-valid result and interruption summary; deferred is not completed/exhausted.

The ongoing timed spectral coordinator renews nonzero candidate, pass and
refinement allowances at completed work boundaries using checked next-quantum
grants. Likewise ensure enough aggregate physical-query/scoring kernel-work and
vertex-visit headroom for the next configured operation allowance. Grant only
when remaining headroom is insufficient; use `consumed + quantum` (or a larger
configured next-operation allowance), record the old/new ceiling and never reset
consumed work. Renew headroom before the existing guards can mistake it for
whole-job completion, including counters already consumed by stagger. A zero
allowance remains an explicit disabled path. This creates no extra trials or
restarts beyond the existing empty/retained trials. Natural exhaustion can still
finish early. Structural bounds and explicit fixed-work limits never renew.

An individual physical query or secondary-scoring allowance may defer the
affected proposal/orientation/secondary score immediately; no additional query
retry loop is required. If reconsidered, the existing finite changed-resource
rule applies. A count improvement already admitted by `Incumbent::offer` remains
authoritative even when secondary score evidence is unavailable under its existing
contract. A still-unvalidated candidate stays rejected. Exhausted native validation
retries also defer that candidate when affordable alternatives remain. Defer once
per logical work item; do not repeat it after an unchanged denial or relabel a
run with deferred work as natural exhaustive search.

Classify from explicit nested causes, not `resource_limit` or a substring alone.
Known work cases include `PHYSICAL_WORK_LIMIT`, `PHYSICAL_VERTEX_LIMIT`,
`PHYSICAL_PASS_LIMIT`, `PHYSICAL_CANDIDATE_LIMIT`, `STAGGER_CANDIDATE_LIMIT` and
the exact native validation kernel-work/AABB-pair allowance codes. A finite
memory denial/allocation failure may similarly abandon a seed phase after its
temporary owners unwind and another path passes its own complete admission.
Generic `PHYSICAL_RESOURCE_LIMIT`, `PHYSICAL_SCORE_RESOURCE` and
`OfferIssue::resource_limit` can also represent arithmetic/accounting failure;
add a small precise cause at those producers when needed rather than blindly
deferring them. Counter overflow, arithmetic capacity, invalid ownership/input,
numerical/FPU and observer errors remain terminal. Poll Stop/deadline before
handoff; their existing precedence is unchanged. Preserve the transient cause
in recovery evidence instead of leaving a stale terminal failure on a resumed run.

The handoff must keep the exact best snapshot and any raw native-valid handle
across the next phase's assignment to `outcome.run`; aggregate work is not reset.
Stagger's existing entry accepts a snapshot only. If none is available, skip that
seed phase rather than restart bootstrap or lose a raw valid handle; restore the
central incumbent through the existing offer path. With no valid handle at all,
the existing spectral empty-trial validation may establish and offer a valid empty
incumbent before continuing. A missing handle is not proof of empty validity.
If that validation or necessary snapshot storage cannot be admitted, terminate
honestly, preserving any earlier raw handle; never synthesize a valid empty result.

This clarifies new timed policy version 1 before its release; legacy and explicit
fixed-work interpretation, saved poses and ordering remain unchanged. Tests must
force baseline and stagger allowance denials separately, then demonstrate real
downstream FFT candidate evaluation and native validation in the same job with
the original incumbent retained or improved. Cover candidate/pass/refinement and
query headroom with small quanta; cumulative counters and grant records must
advance without repeated unchanged work. Include raw/no-incumbent handoff,
unaffordable bootstrap, Stop/deadline and non-resource errors, and preserve fixed
red/green comparisons. No fake candidate/validator substitutes establish success.

Expected incremental scope is about 60–120 handwritten coordinator/diagnostic
lines beyond the prior checkpoint, giving roughly 820–980 non-schema production
additions, or 1,220–1,380 including the currently counted source schemas. These
are estimates; no baseline/stagger renewal subsystem is authorized by this closure.
Report a materially larger requirement before widening loops. Full practical,
desktop, compatibility and critical-review gates remain unchanged.

## Timed ordering, fixed work and compatibility

The new timed order is `completed-page-round-robin-v1`: visit orientations in
catalog order, obtain at most one bounded page, and evaluate its candidates in
the existing within-page score/tie order immediately. Advance to the next
orientation after the page is consumed; continue cursor pages in subsequent
rounds. A valid insertion invalidates stale pages and starts a new layout round.
Failed/deferred orientations do not suppress completed pages. Apply the same
bounded paging principle to existing low-overlap refinement without adding a
new strategy. Timed results may change poses; record this order explicitly.

Legacy settings without the new resolved resource record keep their existing
512 MiB/default work interpretation and catalog-wide merge order. Never add new
semantics when opening an old project. A user-triggered new Start resolves new
settings and records the new policy; old validated poses remain unchanged.

New explicit fixed-work runs resolve hardware memory once, then persist all
effective caps, per-operation work limits, thread count and ordering. Replay uses
that exact resolution: no work renewal, timed page ordering, hardware-dependent
thread reduction or adaptive ordering. Do not silently clamp a replay to today's
RAM; inability to execute its resolved admission returns a resource failure,
not a completed deterministic comparison. Actual allocation failure likewise
fails/retains, without adaptive search. Same resolved build/input/thread settings
retain deterministic candidate order, counters and poses; watchdog/Stop/resource
aborts are incomplete comparisons.

New timed desktop Starts retain `total_start`; all retries and worker calls share
the existing Start deadline. Legacy `search_only` keeps its old timing origin.
Stop precedence/acknowledgement and immutable publication remain unchanged.
Terminal reasons remain the existing enums with precise diagnostic codes:
Stop, deadline, explicit work completion, natural exhaustion, non-resource error,
or no remaining affordable work. A partial catalog with deferred operations is
not natural exhaustion or impossibility. Manual-pitch preflight rejection and any
supported-pitch suggestion occur after this finite recovery is exhausted; no
recovery path changes the manual value. Do not wait idly until 60 seconds.

## Integration contract and implementation brief

Use these additive names; update schemas/semantic checks before consumers:

- `settings.compute.host_memory_limit_bytes` and desktop Settings' equivalent:
  optional positive decimal uint64 string, a downward ceiling only.
- `settings.resolved.resource_policy`: optional version-1 object containing
  `available_ram_bytes`, `host_cap_bytes`, `work_mode` (`timed`/`fixed_work`),
  `search_order`, and `limits` with every effective numeric `SpectralLimits` /
  nested work/structural ceiling needed to replay. Byte/work values use decimal
  uint64 strings. Existing requested values and initial resolved thread count
  remain immutable; actual timed thread transitions live in run evidence.
- `search.diagnostics.resource_recovery` (and segment diagnostics): version,
  actual final threads, grants/recovery/deferred totals, FFT-derived candidate
  evaluations/native validation totals, and at most 16 most recent transition
  records with a monotonic sequence and omitted-record count. Each transition
  gives cause/phase/action, before/after cap or allowance/thread values and work
  consumed. Native storage is bounded plain values; serialization belongs to
  adapters. Keep transient denials separate from terminal `failure`.
- A bounded resource-status value accompanies existing runtime phase delivery
  and desktop `cpu_runtime` state: admitted cap, actual threads, recovery count
  and last cause/action. It is operational progress, not a new result revision.
  Native terminal records carry their admitted host ceiling alongside retained
  bytes so Rust can validate them. Live display shows this state; result save/
  JSON/export retains full resolved settings and the bounded summary.

Existing schema version numbers stay unchanged; new readers accept old documents.
Older strict readers may reject the new optional members. Unsupported resource
policy/order versions are explicit errors, not silently interpreted as legacy.
Generated TypeScript must follow schemas. Native appended value fields/defaults
preserve ordinary source calls but all static consumers rebuild; no ABI promise.

Primary implementation ownership is one expert across these existing boundaries:

| Paths / interface | Bounded change |
| --- | --- |
| `engine/cli/src/host_admission.hpp`, `solve.hpp`/`solve.cpp`, `desktop.cpp`, `desktop_session.cpp`, helper handling in `main.cpp` | Native sample/pure cap resolution; one resolved policy through `SolveRuntime`; all nested cap propagation; retained/publication/helper accounting |
| `engine/pack_solver/include/spectrapack/solver/spectral.hpp`, `src/spectral.cpp` | Append opt-in recovery mode/control/summary values with legacy defaults; own finite grants/retries and versioned page order; keep public `run_cpu_spectral` call forms |
| `engine/pack_solver/src/spectral_pipeline.hpp`/`.cpp` | Small workspace eviction/reconfigure methods reporting whether live bytes changed; separate binary/proximity completion; no general task scheduler |
| `engine/pack_geometry/include/spectrapack/geometry/representation_types.hpp`, `src/conservative_fields.cpp` | Explicit renewable work slices for solver-owned blocked fields only |
| `engine/pack_compute/include/spectrapack/compute/correlation.hpp`, `src/correlation.cpp` | Reuse existing estimates; failed/interrupted FFT owner is destroyed before reuse; change only if tests expose missing reporting |
| `spec/schemas/settings.schema.json`, `results.schema.json`, `desktop.schema.json`, existing I/O semantic validators/generated contracts | Additive settings/evidence, supported-policy validation and exact persistence |
| `desktop/src-tauri/src/core.rs`, `native_session.rs`, `model.rs`, existing React runtime display | Shared helper cap, accept accounted residency above 512 MiB, expose actual state and downward user cap |

Borrowed callback contexts live until all workers join. OS sampling is supplied
by CLI, never imported into geometry/compute. The smallest pure seams are cap
resolution and numeric grant/transition decisions; existing `OperationControl`
fake clock, allocation hooks, pipeline profile and correlation-entry hook cover
the real numerical path. Avoid mocks that replace FFT or native validation.

## Red/green sequence and open feasibility risks

1. Add cap-resolution/contract regressions for half available RAM, a lower user
   cap, overflow/unavailable sample, all nested ceilings and old-document
   semantics. Observe current missing fields/hard clamps fail; implement resolver
   and adapters. Exercise nested retained + scratch admission at a small cap.
2. Extend the existing `AT-12 CPU spectral accepts a real L-prism improvement`
   and actual spectral Stop/fault cases in
   `engine/pack_solver/tests/spectral_tests.cpp` (or a focused sibling source).
   Run small accepted analytic solids through actual FFT and independent
   native revalidation. Force a memory denial with disposable residency, recover
   in the same job, then validate an FFT-derived candidate and a count improvement.
   Add genuinely unaffordable/shared-grid and permanent-allocation cases proving
   finite attempts, no fake thread benefit and exact incumbent retention.
3. Force timed work exhaustion after completed preparation/binary FFT; grant more
   work and prove those successful stages are not repeated, total failed+successful
   work is monotonic, and candidate/native validation occurs before full catalog
   preparation. With the same low explicit fixed allowance, prove no renewal and
   repeat identical completed fixed runs at the same resolved configuration.
4. Inject Stop/deadline at recovery and between stages; preserve valid count/poses
   and publication revisions. Exercise Rust's resolved cap above 512 MiB, lower
   helper allowance, Save/Open/JSON exact poses and retained terminal errors.
5. On this PC run accepted full Pryanik-2, cube24, 400 x 340 x 285 mm,
   pair/wall 1/1 mm, manual 2 mm and explicit 4 mm, 60-second total Start. Each
   must reach real FFT-derived candidate evaluation and native validation beyond
   the former bottleneck; 4 mm must advance beyond four correlations/two pages.
   Retain at least 340 independently valid copies and Ulamok36. Record settings,
   thread changes, transitions, timings, useful work, count/poses and memory.
   Preserve full Pryanik-1 acceptance, simplified rejection and source hashes.

Existing native presets, `tools/Invoke-NativeTests.ps1`, `pnpm contracts:check`,
desktop test/check scripts and the existing `.local/t015/quality_runner.py` /
`restore-quality.py` are available starting points; primary owns actual commands,
fresh-build selection and logs. Reuse unaffected threading evidence. Existing
PR #15 hosted failures are prerequisites to diagnose, not recovery-test red runs.

Expected handwritten production scope is roughly 700–1,000 lines across the
existing paths, excluding generated schema/types and tests; this is an estimate,
not measured implementation size. Broad adapter propagation and fixed-budget
serialization may exceed it: reassess before expansion. The inspected code
proves reusable owners and page seams exist, not that full 2 mm work fits this
PC's current RAM or completes within 60 seconds. Those measured gates may reveal
an indivisible field/ranking stage above the bounded grants or memory ceiling.
If so, return a focused profile/diagnosis to the primary before adding resumable
kernels or another execution path; larger limits alone cannot satisfy acceptance.
