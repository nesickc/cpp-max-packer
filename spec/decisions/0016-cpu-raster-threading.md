# ADR 0016: bounded CPU boundary raster threading

Date: 2026-10-09 UTC. Status: accepted and frozen by the primary before optimization.
Implementation and qualification remain pending. This narrows ADR 0013's pending
parallel lane; its preparation, Start deadline and publication contracts stand.
Requirements: SOL-08/SOL-06, SOL-02, GEO-04/GEO-06; T010-A1–A7.

## Evidence and target

Successful Release baseline: source `46d4bf3`, binary
`e5a17a174cda9f0509fac040747862c516e5debe648d72bafacf54c0738ae278`.
Evidence is `.local/t010/threading-baseline-20261009/`: `manifest.json`,
`summary.json`, four profile JSONs and `host.json`. Every profile has one warmup
and five complete samples, native revalidation, repeatable work and ordered poses.
The host record identifies i5-12450H, 8 cores/8 logical processors and Balanced
power; its revision field describes intake, while the manifest identifies the
measured source. Prepared-start includes context, baseline, spectral work and final
independent validation; accepted-asset preparation is excluded.

| Profile | Fixed-start median, ms | Nested raster median, ms |
| --- | ---: | ---: |
| Analytic | 25.8597 | 2.6607 |
| Ulamok | 9081.1437 | 922.7136 |
| Pryanik 1 | 2989.6351 | 1744.1928 |
| Pryanik 2 | 5887.4292 | 2274.5381 |

Select **boundary raster only**. Four-thread Pryanik 1 must achieve raster
**at least 1.5x** (median <=1162.7952 ms) and fixed-start **at least 1.15x**
(median <=2599.6827 ms), both against these retained medians and independently
against the new build's one-thread medians. Raster timing includes descriptor
preparation, dispatch and synchronization. Its 58.34% raster
share predicts about 1.24x overall at 1.5x raster before overhead; this is a
feasibility calculation, not a measured result. Ulamok's 10.16% raster share
cannot support a 1.15x overall target through this kernel alone.

At one thread, fixed-start median may regress at most 10% on any profile;
report raster changes separately. At every measured count, process peak may increase at most
32 MiB over each retained profile's maximum peak (7,073,792 / 7,827,456 /
52,826,112 / 83,939,328 bytes). Keep 512 MiB host admission and 1.3 billion
representation-work caps unchanged. The primary's
[target fixture](../../tests/fixtures/t010/threading-targets.json) is frozen
before optimization; failures remain failures, not grounds to lower thresholds.
Retain all profiles at 1/2/4/8 on this host, one warmup plus five sequential
samples each, including failures, same physical settings/work and independent
validation. Report other-count regressions and wall-clock quality separately.

## Execution and ownership

- Windows qualified support is `1..min(8,max(1,hardware_concurrency()))`;
  hardware query zero resolves to one. This host supports 1–8. New desktop
  requests default to `min(4,supported_max)`; omitted CLI/legacy requests keep
  one. Explicit unsupported counts fail before work; memory/start failure never
  silently reduces a request. Non-Windows builds advertise one until their stack
  and runtime reservation are separately implemented and qualified.
- Geometry owns one narrow `RasterExecution`, retained by the existing solver
  workspace for the operation. Create lazily before its first raster, after
  aggregate admission: `N-1` background threads plus the coordinator. No global
  pool, generic task API, cross-operation reuse or second compute subsystem.
  Import, material fill, clearance, FFT, ranking and validation remain serial.
  Pocketfft threading/cache/vector switches remain unchanged.
- Static ownership is a complete local X row: worker
  `(local_y + shape_y * local_z) % N`. Use the original checked flat-index and
  closed-cell arithmetic. The coordinator prepares **256 consecutive faces**
  once into a fixed descriptor batch: original interval triangle plus clipped
  cell bounds. Each worker processes its rows in face order. A batch barrier
  precedes descriptor reuse. One shared batch, at most N active cursors and N
  result slots; no queued batches or per-worker grid/mesh/BVH copies.
- Every cell has exactly one writer and sees the original triangle order, so
  the monotone boundary tag and skipped SAT decisions remain unchanged. Retain
  unknown-as-occupied, padding, origins, cavities and later serial fill/dilation.
  Extract common face-preparation/cell-visit helpers; one-thread direct dispatch
  uses those same predicates without a team allocation. Do not duplicate raster
  geometry logic. Small/thin fields may leave workers idle; resolved count means
  admitted team size, not a promise that every worker is always busy.
- Assets, placed intervals and windows are borrowed immutable inputs, pinned
  through completion. Only private output rows and private counters are mutable
  on workers. Neither existing `Budget`, PMR resources, profiling sinks nor
  publication callbacks are shared for concurrent mutation. Geometry never
  depends on solver or compute. Only the coordinator publishes a complete field;
  only existing independent validation can admit an incumbent.

## Work, memory and stop contract

Preserve geometric units: 32 per prepared face, 10 per tag lookup, 180 per SAT;
each candidate-cell visit is counted once. Scheduling is separately bounded
bookkeeping, not another geometric predicate charge or an unreported reduction
in work. Record actual batch count, worker descriptor inspections, grants and
participating thread IDs in the private evidence seam. Each descriptor is
inspected at most once per worker; quota resumptions retain their cursor.
Pryanik 2 has only **10,027,359** units below the cap: repeated face preparation
would already exceed that headroom at two threads.

The coordinator gives finite work/visit grants whose sums fit the remaining
aggregate allowances. Worker cursors pause **before** an unfunded primitive;
quota exhaustion alone is not global failure. At ordered barriers reconcile
actual work, refund unused grants and deterministically redistribute to unfinished
cursors. Give the lowest-index waiting cursor enough for its next primitive when
an even split would strand usable allowance. No restart/repeated SAT, atomic
per-cell budget contention, multiplied caps or worst-case whole-raster admission.
Merge completed counters even after Stop; genuine exhaustion preserves the
existing nested work/visit failure and discards the generation.

For N>1 reserve **256 KiB fixed + (N-1) * 2 MiB** before allocation/spawn,
and retain it in every live-owner admission until join/destruction; N=1 adds
zero team reserve. Fixed storage contains descriptors, cursors, synchronization,
handles and reports; compile-time size checks must establish the bound. Each
Windows background thread has an explicit 1 MiB stack reservation, using the
CRT thread entry with `STACK_SIZE_PARAM_IS_A_RESERVATION`; Windows distinguishes
reservation from initial commitment ([Microsoft CRT documentation](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/beginthread-beginthreadex),
[stack documentation](https://learn.microsoft.com/en-us/windows/win32/procthread/thread-stack-size)).
The other 1 MiB per thread is a declared runtime/OS qualification margin, **not
a universal proven bound on Windows internals**. Maximum team admission is
14.25 MiB at eight. Record actual stack reservation, managed peaks and process
peaks separately; qualify the margin on the supported build. No worker allocates
geometry, descriptors or diagnostics. Count persistent team ownership once in
workspace/current-residency, representation, correlation and subsequent growth
admission; worker stack reservation must not masquerade as tracked heap payload.

Workers poll user stop and an atomic operation abort/cause at least every 256
primitive attempts, including skip-only work, and on each dispatch/barrier.
Only the coordinator calls `OperationControl::poll()`/custom clock and phase
sinks; while waiting it polls at <=5 ms intervals. This preserves borrowed,
potentially non-thread-safe test clocks. Verify the worker floating environment
before predicates. A worker failure stores a static descriptor; the coordinator
retains the originating failure rather than sibling-cancellation noise.
Partial thread-start failure, worker failure, Stop and shutdown wake all waits,
join every created thread, then release descriptors/inputs/callbacks. No detached
threads, allocations during error transfer, late result or terminal before join.
On success the team parks between raster calls and joins before operation
terminal/destruction. Retain the latest native-valid incumbent. Existing 250 ms
acknowledgment / 5 s ordinary safe-stop gates remain unchanged.

## Exact implementation seams and compatibility

Bounded native interface contract:

```cpp
// New geometry/raster_execution.hpp; opaque noncopyable RAII owner.
class RasterExecution;  // destructor joins; reserved_bytes() const noexcept
std::optional<std::uint64_t> estimate_raster_execution_bytes(std::uint32_t) noexcept;
std::variant<std::unique_ptr<RasterExecution>, RepresentationFailure>
make_raster_execution(std::uint32_t count, std::uint64_t available_bytes);
// Successful count 1 returns a null owner, denoting direct serial dispatch.
// Append to FULL voxelize_object/voxelize_placed/voxelize_container overloads:
// ..., const runtime::OperationControl& control = {},
// RasterExecution* execution = nullptr);
// Internal rasterize_boundary likewise appends RasterExecution* = nullptr.
// Budget: coordinator-only merge of previously admitted completed work;
// bool commit_completed_work(std::uint64_t units) noexcept;
// This records even after Stop, checks capacity, and never polls/calls sinks.
std::string_view cpu_scheduling_policy(std::uint32_t count = 1) noexcept;
```

Native ownership: `pack_geometry` new narrow header/source plus
`validation_kernel.{hpp,cpp}` and `conservative_fields.{hpp,cpp}` plumbing;
`pack_geometry/CMakeLists.txt` registers only that source. `pack_solver`
`spectral_pipeline.{hpp,cpp}` owns/passes the team and admission;
`spectral.{hpp,cpp}` resolves supported count and records policy. Preserve full
factory wrappers/defaults and rebuild native consumers; no stable C++ ABI claim.
Use `serial-v1` at one and `raster-rows256-v1` above one. CLI
`src/solve.cpp`/`src/desktop.cpp` must compare/emit policy for the resolved count.
Legacy `serial-v1`/count-one results remain loadable and executable; do not rewrite
their provenance. Existing schema ranges/policy strings require no version change.
Rust supported-range handling and React options already derive capabilities;
verify their real behavior, do not add another configuration path.

## Test-first acceptance and scope

1. **A1 red:** existing real solver/CLI request for two threads fails on the
   retained serial build. Add a real-kernel observation asserting overlapping
   worker execution and distinct IDs; a configured count alone is insufficient.
   Green supports advertised counts, rejects zero/max+1 before useful work.
2. **A2/A3:** parameterize existing `raster_reuse`, conservative-field and
   solver oracle tests at 1/2/4/8. Independent analytic closed-box/cavity expected
   cells, exact plane contact, uncertain/rotated asymmetric fields, nonzero
   origins and halos supplement serial equality. Compare successful occupancy,
   geometric counters and fixed-work ordered poses to the retained baseline;
   independently validate each result and retain direct correlation/proximity
   oracles. Interrupted counters need not repeat across wall-clock schedules.
3. **A4 red/green:** admission one byte below/exact team bound; forced small
   grants with uneven work prove refund/resume without false exhaustion;
   unchanged near-cap Pryanik 2 succeeds. Private allocation-free fault hooks
   cover partial thread creation and active-worker failure, plus coordinator
   allocation failure while a real batch is active. Verify bounded storage,
   joined threads, unchanged incumbent and no leaked admission after failure.
4. **A5:** hooks identify dispatch, active row, parked team and before-barrier
   completion; prove a real raster worker is active when interrupted (the outer
   `voxelizing` phase signal alone is insufficient). Issue Stop/deadline/shutdown
   at each, then immediately start a new
   operation. Existing marker acknowledgment tests plus real native raster tail
   timing establish the two latency gates. Check callback thread affinity,
   failure origin, joins and no publication from a failed/old generation.
   Deterministic hooks are stress seams, not an exhaustive race proof; obtain
   the required focused critical review of a stable diff.
5. **A6/A7:** retain complete Release samples at frozen targets, measured memory,
   Ulamok36/full48, both full Pryanik workflows, old project/pending-result
   association and independently reread exports. Reuse unaffected final T-011
   acceptance. No fabricated timing red or historical failed-profile substitution.

Expected production scope remains 300–700 handwritten lines. Reassess near 1,000,
on a second subsystem/path or two failed repairs. Principal risks are batching
and memory bandwidth overhead, thin-field imbalance, near-cap grant correctness,
and Windows runtime margin qualification. They require evidence, not extra
kernels, cap increases or weakened targets. No builds or new measurements were
run for this ADR; checks were scoped source/contract and retained-evidence reads.
