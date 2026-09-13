# T-006 bounded physical baseline qualification

Status: clean local Release qualification passed at implementation revision
`1d1c034d3e5b6b3f630ddb041e14e46be7009dd9`; see [evidence](../doc/T-006.md).
Pilot timings remain unqualified observations; measured timings are informational.
Requirements: SOL-01, SOL-02, QA-01;
native AT-10 scope. See [ADR 0008](../spec/decisions/0008-physical-baseline.md).

## Workloads and independent expectations

Use Fixed identity, millimeters and unchanged authoritative assets. Analytic
objects are generated through the existing fixture builders and public import/
acceptance API. Each recorded result is `best_found`.

| Workload | Physical inputs | Search caps | Observable expectation |
| --- | --- | --- | --- |
| `cube_exact` | 10 mm cube; 40³ mm box; pair/wall 0 | 64 proposals, one pass | 64; all centers `(5+10*x,5+10*y,5+10*z)` for x/y/z in 0..3; utilization 1 |
| `cube_clearance` | 10 mm cube; 45³ mm box; pair/wall 1 mm | 64 proposals, one pass | 64; all centers `(6+11*x,6+11*y,6+11*z)`; independent physical distances |
| `oversized` | 50 mm cube; 40³ mm box; pair/wall 0 | Default work/pass caps | Valid empty snapshot, zero proposals, one completed zero-cell pass, `search_stalled`, utilization 0 |
| Each accepted `rc` item/container pair | All three accepted items × six accepted STL containers; pair/wall 1 mm | Eight proposals, one pass | Retain/revalidate eight placements; fixed source-derived physical grid prefix; no optimality claim |

All 18 reviewed source AABBs have at least eight identity-grid cells with these
gaps. The supplied containers currently validate as cuboids, but each emitted
layout still passes true STL containment through the public validator. Derive
expected grid sizes/translation prefixes independently from the pinned bounds
using Python `Fraction` of the exact binary64 values. Do not accept a benchmark's
self-reported count or grid as its oracle. Reject the invalid simplified Pryanik
as an input diagnostic; it is not an accepted packing workload.

Root first runs three pilots (one per accepted item in the 5 kg container) to
check the envelope's tractability. A pilot cannot publish full qualification.
The registered full workload set is exactly 21 cases. Changing a budget or oracle
requires an explicit recorded decision; retained failures are not erased.

## Execution and observation bounds

- One warmup and three measured samples per workload; use the same resolved
  settings for each repeat. Do not reset cumulative limits within a solver run.
- Use ADR 0008's resource defaults and the search caps above. Record every actual
  limit, caller reserve, seed/score-order version and thread configuration.
- Start with a 600-second whole-native-process watchdog and 900-second entire
  Python-harness deadline. A watchdog abort is incomplete qualification.
- Record import/preparation, baseline search and fresh-context revalidation
  separately where observable, plus process duration. Retain raw timing samples
  and derive informational median/p95. No throughput/regression threshold is
  claimed before comparable host evidence exists.
- Record solver tracked peak bytes and actual process peak working set
  separately. Imports, native report construction and any retained observation
  history are benchmark-owned storage. Set the benchmark caller reserve to
  64 MiB for retained imports outside the current validation context, observation
  history and report construction; report that value with every run. The public
  API default reserve remains zero. Keep the 512 MiB solver storage ceiling.
  Do not present tracked payload accounting as process RSS enforcement.

## Required payload and report checks

The native diagnostic must retain resolved context/constraints, source paths,
actual import status/bounds/material volumes, build identity, limits, counters,
termination, observations and final count/poses/metrics. Its observations include
monotone revisions/counts and valid context identity. Retain enough evidence to
check that previously observed snapshots remain unchanged after later offers.
The first admitted snapshot has revision one; the empty bootstrap is observable.

Serialize final poses with round-trip binary64 precision, parse them back into a
new candidate under a fresh equivalent validation context, and invoke the real
independent validator. Preserve that report and actual reconstructed poses.
This is a native diagnostic serialization check, not the product result/STL
export acceptance deferred to T-007.

The Python qualifier checks the exact workload/repeat set, build and constraints,
all prescribed analytic/physical grid positions, unique nonempty copy IDs,
quaternion permissions, counts, finite metrics, independent volume utilization,
work/resource caps, termination consistency, monotone observations and complete
successful fresh-context revalidation. Compare deterministic non-timing values
across repeats. Keep known physical predicates independent of solver helpers.

Strictly reject duplicate JSON keys, booleans masquerading as numeric counters,
non-finite/overflowing numbers, missing/extra cases, mismatched poses or metadata,
invalid/indeterminate validation and incomplete output. Add practical mutation
tests for these acceptance checks and meaningful tests of actual runner
orchestration, deadlines, source/output alias protection and atomic publication.
Do not add another product schema or large checked-in result dump.

Bind original inputs to `rc-manifest.json` and `import-expectations.json`. Protect
all ten source files, those oracle files, executable/build metadata, and the
runner/checker/shared helper sources with before/after identities. Match clean
source revision to measured Release build metadata for positive qualification.
Reuse established guarded process, hashing, metadata and atomic-publication
helpers without changing the older benchmark contracts.

Keep complete stdout/stderr and the qualified report under `.local/`, with host,
compiler, dependency and source provenance. Publish `qualified: true` only after
all registered checks complete and protected identities remain unchanged. Pilots
and failed runs remain explicitly unqualified observations.
