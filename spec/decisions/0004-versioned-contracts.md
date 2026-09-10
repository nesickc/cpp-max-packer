# ADR 0004: Version-1 contracts and bounded stdio service

Accepted 2026-09-10 for T-002. Requirements: DATA-01, §8.1–8.3 and §9.3;
contract groundwork for AT-13/AT-14. T-001 is merged at `2568c38`.

## Decision

`spec/schemas/` owns five Draft-07 families (settings, assets, results, protocol,
benchmark-summary) and shared definitions. `pack_io` embeds their exact contents,
compiles them with json-schema-validator 2.3.0#2, then checks semantic consistency.
Its resolver accepts only catalog IDs in `https://spectrapack.invalid/schemas/v1/`
and their local fragments. It never downloads schemas or opens document paths.
TypeScript declarations are generated with json-schema-to-typescript; Ajv provides
an independent schema oracle. Node tools are development dependencies only.

`ValidatedDocument` owns an immutable JSON snapshot. It means contract consistency;
physical validation, file/hash verification and incumbent publication belong to
later geometry/project work. Parsing rejects duplicate keys, malformed UTF-8,
nonfinite numbers and depth greater than 64. Failures carry bounded JSON Pointer
issues. Objects are closed except explicit generic envelope payloads and error
details. Counters use JavaScript-safe integers; seeds use canonical uint64 decimal
strings. Portable paths exclude absolute paths, backslashes, empty/dot components
and traversal. Timestamps require real UTC calendar dates with a `Z` suffix;
fractional seconds are optional, including the millisecond form produced by
JavaScript `Date.toISOString()`.

Requested settings reference runtime asset IDs; resolved settings reference source
and accepted-solid SHA-256 identities and record selected pitch/catalog/backend.
An explicitly requested compute backend must match the initial resolved backend;
`auto` may resolve to either available backend. `search.deterministic` selects the
CPU work-budget mode in §6.5: explicit Vulkan is incompatible, and resolved
deterministic settings must select CPU.
Requested quaternions allow either sign and custom duplicates; later resolution
explicitly canonicalizes/deduplicates them. Persisted/resolved quaternions must be
canonical, normalized within `1e-12`, and unique where they form a catalog. Derived
matrix rotation/bottom entries use absolute `1e-12` consistency tolerance;
translations use `1e-9 mm + 8*double_epsilon*max(abs(actual),abs(expected))`.
Source-unit scale entries use absolute `1e-12`. These are serialization tolerances,
independent of geometric clearance. Hand-computed transform goldens are the oracle.
Orientation permission checks use §5.2's angular distance tolerance of `1e-7 rad`,
including fixed, custom, cube and upright policies. This angular tolerance is
separate from quaternion normalization and matrix serialization tolerances.

Result search settings and seed describe the latest run segment. Each segment
records its own resolved settings and matching seed; Continue may change these
while preserving physical assets, container dimensions and clearance/orientation
permissions. Work budgets apply to each segment; result work and elapsed time
sum all segments. Segment IDs are unique; the first parent revision is null and
later parents cannot exceed the current solution revision. Backend transitions
reference a recorded segment and use times relative to that segment. This fixes
the initial serialization meaning of §8.2 without claiming continuation support.
The segment's `backend` and resolved backend identify its initial dispatch backend.
Ordered transition records describe subsequent changes from that backend; a
fallback does not rewrite the segment's original settings or discard its history.

Individual volume metrics may be null when unavailable. Utilization is null if
either volume is unavailable; otherwise it must equal count times solid volume
divided by container volume. A known volume remains reportable when the other
volume is unavailable. A reported box volume must match its dimensions.

Asset source units are `mm` (scale 1), `inch` (scale 25.4), or `custom`
(the selected positive scale). Source bounds remain in original coordinates;
dimensions and the recorded transform must agree after conversion to millimeters.
Benchmark summaries aggregate completed runs only, retaining failed runs separately.
Best/median/worst counts and success/failure totals must match those records; all
three count aggregates are null when no run completed. Each resolved run records
its matching seed. Improvement counts increase strictly and their timestamps are
ordered within the recorded search duration. These checks verify report consistency,
not the physical correctness of the referenced results.

`pack_service` owns transport, dispatch and replay; the executable is a thin CLI.
This ticket exposes `capabilities --json` and `serve --stdio` with only
`capabilities.get`. Future required methods/commands report `METHOD_UNSUPPORTED`;
no assets/jobs/results are fabricated. Capabilities advertise no compute backend
or packing/import/project/export implementation.

NDJSON records allow 1 MiB before LF, counting an optional final CR. CRLF is accepted;
BOM and blank records are invalid. Oversize input closes immediately after an error
(exit 2); a nonempty EOF fragment is truncated (exit 2). Other malformed complete
records return a recoverable error and permit the next record. Untrusted envelopes
and unsupported versions have null request IDs; valid-envelope errors preserve IDs.
Stdout contains records only, promptly flushed. Output/internal failure exits 4.

A session retains at most 1,024 request IDs and 8 MiB of encoded request/response
payload, plus bounded map metadata. Reserve a maximum response before dispatch.
Identical parsed requests replay exact response bytes; changed requests conflict.
Object member order and whitespace do not matter; distinct parsed numeric types
may conflict. IDs are never evicted/reused. Cache exhaustion emits nonrecoverable
`MEMORY_LIMIT` (`details.reason=request_cache_exhausted`) and closes with exit 3.
Malformed envelopes are not cached; valid unsupported/invalid-params responses are.

Process-local `AssetId` values use `asset-<32 lowercase hex session>-<positive uint64
generation>`. A registry owns immutable shared snapshots, never reuses generations,
and rejects null insertions/overflow. Syntax does not establish registry presence.
Import will supply a fresh OS-random session nonce and immutable real payloads.
Portable records contain content identities, never runtime handles.

## Compatibility and limits

This establishes initial strict version-1 shapes and previously unspecified framing,
replay and error behavior. New transport codes are `INVALID_JSON`, `INVALID_REQUEST`,
`INVALID_PARAMS`, `INVALID_DOCUMENT`, `RECORD_TOO_LARGE`, `TRUNCATED_RECORD`,
`METHOD_UNSUPPORTED`, `METHOD_NOT_FOUND`, `REQUEST_ID_CONFLICT`, `INTERNAL_ERROR`.
Existing §8.1 codes retain their meaning. Required shape/semantic changes require
a version bump or an explicit compatibility reader. No existing project migration
is claimed. Full job lifecycle, archive round trips, physical acceptance and M0
remain pending their complete gates.

The pinned validator port uses an upstream patch whose generated Git index metadata
drifted. The original SHA-512-verified bytes are preserved in `third_party/vcpkg/`
and seeded into vcpkg's download cache. The dependency baseline and source remain
unchanged; the normal vcpkg checksum checks remain enabled.

## Verification

Use shared practical fixtures with independent Ajv and native checks, transform
goldens and semantic mutations, generated-type freshness and negative compile
fixtures, registry/session checks, injected transport limits, and real subprocess
pipes including prompt flush and execution outside the checkout. Record behavioral
red/green evidence separately from provisioning failures. Bounded repeated contract
and service timings are informational; they establish no solver speed claim.
