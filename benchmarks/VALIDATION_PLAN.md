# T-004 native validation qualification

This is a test/benchmark payload, not a product wire schema. The native executable
`spectrapack_validation_benchmark` uses the public validation API. The Python
runner qualifies its outputs independently and writes evidence under `.local/`.

## Invocation and timing

Native arguments: `--repo-root PATH --samples 3 --warmup 1`. The executable writes
one JSON document to stdout and diagnostics to stderr. Malformed arguments or a
failed expected outcome return nonzero. Samples are bounded to 1–20 and warmups
to 0–10; qualification requires at least three samples and one warmup. Source
imports and context/candidate setup occur once and are timed separately. Each
retained sample times a complete `validate()` call; no previous pose certificate
may replace that call. Validate every warmup and sample, including the opaque
snapshot's context, actual poses and immutable report on valid outcomes.

Python runner arguments: `--executable`, `--build-metadata`, `--output`, optional
`--samples`, `--warmup`, `--timeout-seconds` (default 300) and
`--overall-timeout-seconds` (default 600). Require Release metadata and matching
native build type. Verify all ten manifest source hashes before and after execution
(including the rejected simplified Pryanik), protect output/input aliases
including hardlinks, enforce deadlines through validation
and publication, and retain raw stdout/stderr. No timeout, uncertain verdict or
partial set of workloads qualifies. Median and nearest-rank p95 are informational;
there is no cross-machine regression threshold.

## Fixed workloads

All poses use identity rotation, fixed orientation permission and separate 1 mm
pair/wall clearances. Derive centers from actual accepted bounds. For each of the
three accepted items and six accepted STL containers (18 combinations), run:

| Case | Poses | Expected |
| --- | --- | --- |
| `centered_single` | One copy at container bounds center | valid |
| `separated_pair` | Two copies at center X/Z and Y at one-third/two-thirds of container depth | valid |
| `coincident_pair` | Two distinct copy IDs with the same centered pose | invalid |
| `outside_min_x` | One copy with its maximum X 1 mm below the container minimum X; center Y/Z | invalid |
| `short_wall_gap` | One copy with its minimum X 0.5 mm above the container minimum X; center Y/Z | invalid |

Use IDs `copy-0` and `copy-1`. Accepted local item bounds determine the offsets;
do not subtract the source center again. All centered single copies have over
47 mm minimum wall gap. The separated pairs have over 42 mm minimum Y gap; their
wall gaps also exceed 1 mm. These are constructive validation cases, not maximum
packing counts. The invalid simplified Pryanik remains in the T-003 import gate.

Add `analytic_separated_64`: unit cubes in a 14 mm analytic box, with centers
`(2+3*i, 2+3*j, 2+3*k)` for `i,j,k` in 0–3. IDs follow deterministic loop order.
All 64 copies are valid at 1 mm clearances. Retain AABB-pair/kernel work counters
to show what validation did; do not mistake elapsed time for evidence of pruning.

There are exactly 91 workloads. Their identities come from the fixed matrix,
not an arbitrary list supplied by the native report. The Python verifier derives
expected verdicts and pose rules independently using the pinned import bounds.

## Report contract

Native top-level keys: `schema_version: 1`,
`benchmark_kind: "native_validation"`, `compiler`, `build_type`, `parameters`
(`samples`, `warmup`), finite nonnegative `setup_ms`, and `workloads`.
Each workload has `case`, `object_path`, `container_path`, `poses`, `constraints`,
and `runs`. The analytic case uses `analytic_unit_cube` and `analytic_box_14` as
its path identifiers. Each pose has `copy_id`, `translation_mm` and
`quaternion_xyzw`. Constraints have `pair_clearance_mm` and `wall_clearance_mm`.

Each run has `phase` (`warmup` or `sample`), finite nonnegative `elapsed_ms`,
`report`, `has_validated_solution` and `validated_copy_count`. Report fields mirror
the native API: `status`, `code`, `message`, `epsilon_mm`, `kernel_revision`,
`aabb_pair_tests`, `kernel_work`, `working_bytes_peak`, `affected_copy_ids`,
`affected_ids_truncated`, and `checks` (`check`, `state`, `method`). Serialize enum
names as lowercase snake_case. A valid result has a real snapshot with exactly
the requested copies; invalid results have no snapshot. All six check states are
complete for valid results. Reports and work counts must be deterministic across
repetitions of identical inputs, excluding elapsed time.

The Python evidence report retains the validated native payload and per-workload
statistics, executable/source/manifest/expectation hashes, build metadata, host,
Git revision/dirty state and requested parameters. `qualified: true` is written
only after every independent check and the final deadline check pass. Native
claims such as an expected verdict or a precomputed statistic are never the
verifier's oracle.
