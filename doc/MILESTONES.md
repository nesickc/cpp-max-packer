# Delivery milestones after T-008

Status: planned, 2026-09-30. T-008 merged through PR #10 at `a05fa2c`.
T-009–T-023 below have specifications, not implementation or passing evidence.
The first four specification IDs form three delivery tickets: T-009, T-010
(including the T-011 work package), then T-012. No acceptance IDs are retired.
The user prioritizes CPU speed/multithreading, practical packing and explicit
repair. Live result rendering is lower priority; GPU work follows CPU improvement.

The canonical [M0–M6 milestones](spectrapack-spec.md#111-milestones) and
AT-01–AT-17 remain the full product gates. The delivery phases below organize
work; completing a phase does not automatically complete a product milestone.
[ADR 0011](../spec/decisions/0011-cpu-first-follow-up.md) records the scope and
compatibility decisions. [NEXT_WORK.md](NEXT_WORK.md) retains the motivating
evidence, including what has not been reproduced.

## Delivery phases and exit gates

| Phase | Tickets | Observable exit | Product gates advanced |
| --- | --- | --- | --- |
| A — Reliable, faster CPU packing | T-009, T-010 (includes T-011) | Native Ulamok baseline at least 36; supported practical desktop runs; efficient fields and useful measured multithreading; reused preparation; a Start-origin deadline and qualified Stop | M1/M2/M3 gaps; AT-05/AT-10, CPU AT-12, AT-15/AT-16 subsets; SOL-08 |
| B — Practical geometry workflows | T-012–T-013 | Explicit accepted repair of the simplified Pryanik survives project/export; box and STL interior-volume desktop journeys preserve dimensions/cavities | M1/M3; AT-03/AT-04/AT-07/AT-14/AT-15 subsets |
| C — Better packing and usable resolution | T-014–T-016 | Required orientation modes, refinement and two-copy L rearrangement; preserved incumbents; presets resolve to supported explicit memory/pitch/thread settings | M4 and M3 controls; AT-08/AT-11, AT-05/AT-16 subsets |
| D — Durable jobs | T-017–T-018 | CLI/UI share real jobs; duplicate Start is harmless; Continue preserves the incumbent; interruption recovers the last complete checkpoint | M3; AT-13/AT-14, CPU portions of AT-16 |
| E — Qualified Radeon acceleration | T-019–T-020 | CPU/GPU oracle agreement, recorded Radeon measurements, complete memory accounting and recoverable Auto CPU fallback | M5; AT-02, GPU AT-12/AT-16 |
| F — Deferred live viewer completion | T-021 | Coalesced validated revisions, selected full-resolution inspection, PNG and measured 1,000-copy viewer gate | Remaining M3/UI gates and M6 viewport evidence |
| G — Release evidence and delivery | T-022–T-023 | Frozen benchmark/quality artifacts, complete acceptance audit and offline Windows installer/CLI qualification | Outstanding M0–M6 and full AT-01–AT-17 |

## Ticket queue and prerequisites

The order below is the delivery preference. Prerequisites include integration
ordering where shared adapters make concurrent delivery unnecessarily costly;
they are not all dependencies of an isolated algorithm. Every delivery branch starts
from `main` after its prerequisites and this planning change are merged. A listed
branch name in a ticket is proposed, not created. No agent merges a PR.

| Ticket | Specification | Required merged prerequisites |
| --- | --- | --- |
| T-009 | [CPU scalability and practical packing](T-009.md) | T-008 |
| T-010 | [CPU runtime: multithreading, preparation and deadlines](T-010.md) | T-009 |
| T-011 (included) | [Preparation/deadline acceptance sheet within T-010](T-011.md); no separate branch/PR | Delivered with T-010; shares its T-009 prerequisite |
| T-012 | [Explicit library-backed STL repair](T-012.md) | T-010 |
| T-013 | [STL-container desktop workflow](T-013.md) | T-010, T-012 |
| T-014 | [Orientation catalogs and refinement](T-014.md) | T-010 |
| T-015 | [Count search and reinsertion](T-015.md) | T-014 |
| T-016 | [Presets and resource-aware resolution](T-016.md) | T-010, T-013, T-014 |
| T-017 | [Unified CLI/service jobs and Continue](T-017.md) | T-010, T-015, T-016 |
| T-018 | [Atomic checkpoints and recovery](T-018.md) | T-012, T-017 |
| T-019 | [Vulkan/VkFFT compute](T-019.md) | T-010, T-016 |
| T-020 | [Auto fallback and Radeon qualification](T-020.md) | T-018, T-019 |
| T-021 | [Deferred live viewer and inspection](T-021.md) | T-013, T-017, T-018 |
| T-022 | [Benchmark and regression qualification](T-022.md) | T-015, T-016, T-020, T-021 |
| T-023 | [Offline distribution and v1 release](T-023.md) | All delivery tickets T-009–T-022; T-011 is included in T-010 |

Technical discovery and document review can proceed independently. If a ticket's
implementation would require an unplanned module redesign, define a bounded
follow-up and update the dependency/acceptance map before expanding it. Retain the
original unmet gate; splitting work cannot turn missing behavior into completion.

## Parallel agent execution

Use one primary and at most two leaf workers for a delivery ticket. The first
wave remains three user-reviewed merges: **T-009 → T-010 (T-011 included) → T-012**.
The native and product-integration lanes below are worker assignments on the
same ticket branch, not independent tickets or stacked branches.

| Delivery | Native lane | Product-integration lane | Required serial points |
| --- | --- | --- | --- |
| T-009 | Ulamok baseline/independent witnesses, efficient proximity/fields and native resource/oracle checks | Nested diagnostics, CLI/desktop resource feedback and practical journey fixtures/tests | Freeze profiles/error contract first; characterize before optimizing; integrate and qualify the serial baseline before T-010 |
| T-010 including T-011 | Measured parallel kernel, bounded workers/scratch, prepared-asset APIs and native cancellation | Shared adapters, reuse lifetime, Start deadline/Stop transport, settings/provenance and desktop/project/export integration | Freeze one runtime/compatibility contract; publish required interfaces; then parallelize implementations; qualify together |
| T-012 | Actual-library evaluation, bounded reconstruction, native validity/metrics and resources | Accepted-repair schemas/replay, explicit preview/acceptance, archive and export journeys | Prove a useful candidate and freeze its recipe/authority contract before dependent product integration; then parallelize the adapter and workflow |

For T-009 and T-010 the native owner has `engine/pack_compute/`,
`engine/pack_solver/`, required `engine/pack_geometry/` internals and their tests.
The integration owner has `engine/cli/`, `engine/pack_io/`, `desktop/`, schemas,
generated types and protocol/journey tests. T-012 narrows the native lane to
repair/geometry/dependency integration. Assign exact paths, root build files and
shared helpers before dispatch; ownership is exclusive. One worker applies each
agreed contract change before both consume it. Transfer ownership explicitly when
a correction crosses a boundary; do not have both workers edit the shared CLI
or desktop state machine. The primary coordinates design, acceptance and fixes.

Each ticket starts with practical reproducers and retained baseline evidence.
Two workers may implement and run isolated correctness tests concurrently. Use
separate build/output directories or serialize access to shared build trees;
serialize mutations of dependency caches. Performance samples and substantial
CPU/memory loads must not overlap on the measurement host. Hold preparation state
fixed for threading comparisons and thread count fixed for reuse comparisons;
report the combined result as well. Overlap coding, not benchmark interference.

Integrate both lanes before the final affected gate run and scoped independent
review. Reuse a single real integration run for multiple acceptance IDs when its
assertions prove each case. Review/fix/test again only for changed behavior or
unresolved findings. T-010's delivery ledger must include every T010-A1–A7 and
T011-A1–A7 gate; neither a merged partial implementation nor a passing mock closes
the combined ticket. Later dependencies name T-010 as the delivery owner.

T-009 remains separate because its corrected serial behavior and profiling select
T-010's parallel kernel; merging their ticket labels would not remove that
decision dependency. T-012's library research can occur earlier using an available
worker slot and isolated scratch evaluation, after profiling/CPU work has priority.
This is optional discovery: it creates no dependent product branch, supplies no
reusable product commits, and does not claim T-012 acceptance. Its product branch
waits for T-010's merged lifecycle/Stop contract. Findings and pinned inputs can
be retained in `.local/`; incorporate any selected dependency through T-012.

The grouping reduces one handoff and permits meaningful overlap; no measured
agent speedup or proportional reduction in the earlier time estimate is claimed.

## Shared acceptance and evidence rules

- Each ticket identifies requirements, spec sections, observable acceptance IDs,
  exclusions, design decisions and review. `T009-A1`-style checks are ticket-local;
  they refine the canonical AT gates and do not replace or rename them.
- Product changes use practical red/green tests and independent oracles. Preserve
  source hashes and failed results. Count, ordered poses, settings and authoritative
  revalidation accompany performance results. Unit mocks alone cannot establish
  desktop/native integration or geometry correctness.
- Before optimization, commit the workload definitions and retain a compatible
  Release baseline. Freeze the host-specific numerical improvement target before
  edits. Use at least one warmup and five timed samples for the selected bounded
  comparison, unless the ticket records a justified alternate sampling plan before
  measurement. Compare medians and raw samples, not one fastest observation.
- Report 1/2/4 and available bounded higher thread counts, phase and total timings,
  peak memory, actual work and cold/warm state. Fixed-work runs expose speed without
  disguising less search; timed runs report count/quality as well as time. A timed
  search may properly consume its whole budget. A watchdog abort is a failure,
  never a completed performance comparison. No hardware-independent speed promise
  is implied by a local target.
- No unresolved numerical target can be treated as a passing performance gate.
  Record the target and acceptance profile in the ticket before optimization;
  characterization instrumentation and a failing reproducer may come first.
  Material changes require an explicit evidence/compatibility update, not a quiet
  rebaseline to hide a regression. T-009 must deliver an actual improvement and
  T-010 a useful measured multicore improvement.
- T-009 pins the supplied-asset reproduction/witness in portable test data. The
  existing `.local/` formula witness is analytical and is not a native result.
  Native validation and actual solver/desktop/export runs are still required.
  Pin the official Benchy as soon as it is needed; T-022 owns final qualification,
  not a reason to use unpinned moving inputs earlier.
- Retain full execution logs under `.local/tNNN/` and commit compact evidence
  summaries/settings/hashes. Record actual commands after the needed runners
  exist. Do not invent planned commands or count documentation checks as product
  tests. Use the relevant existing checks in [BUILDING.md](../docs/BUILDING.md),
  [tests](../tests/README.md) and [benchmarks](../benchmarks/README.md).
- Routine implementation gets independent code/contract review. Authoritative
  geometry, numerical uncertainty, shared-state concurrency and unresolved
  consequential concerns get focused critical review; architecture/shared
  contracts are settled before parallel edits. Keep workers' write scopes separate.

The [delivery policy](DELIVERY.md), [acceptance map](../tests/TEST_PLAN.md) and
[project status](PROJECT_STATUS.md) remain the publication, verification and
current-state records. Milestone completion needs the actual gate evidence;
ticket creation or merge alone does not establish a complete product milestone.
