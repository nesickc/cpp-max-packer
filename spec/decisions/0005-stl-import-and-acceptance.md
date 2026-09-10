# ADR 0005: Source-preserving STL import and explicit repair acceptance

Accepted for T-003, 2026-09-10. Requirements: GEO-01–GEO-03, AT-03/AT-04,
spec §§5.1–5.2 and the CLI inspection boundary in §8.1.

## Native ownership and acceptance

`pack_geometry` owns STL parsing, local-mm meshes, cleanup, shell diagnostics and
immutable drafts/proposals/accepted solids. Its public interface contains C++
values and const views; no filesystem, JSON, solver or third-party kernel types.
`pack_io` owns original file snapshots, byte hashes, lossless mesh artifacts and
report publication. The CLI supplies explicit units/role and Unicode paths.
Async service imports, job lifecycle, placement validation and LODs remain later
work. A contract-valid document cannot construct an accepted solid.

Inspection returns an immutable draft or a structured input/resource failure.
Drafts may describe invalid or indeterminate geometry. Acceptance requires a
completed valid topology, intersection and shell-containment analysis. Accepted
solids expose only immutable owned buffers; callers cannot inject a validity flag
or mutate an alias. An accepted repair retains the original and candidate drafts.

## Content, frames and exact cleanup

Binary detection requires an exact `84 + 50*N` byte layout with checked arithmetic;
this wins over an ASCII-looking header. Otherwise parsing requires the complete
ASCII STL grammar, including multiple solid sections. Truncation, trailing junk,
invalid numeric tokens and nonfinite vertex coordinates are rejected. Binary
attribute words and facet normals are hints, not additional geometry or authority
over vertex winding. Number parsing is locale independent.

Native options require explicit role and units. Millimeters use scale 1, inches
25.4, and custom units require a finite positive scale. Scaled coordinates, extents,
anchors and local coordinates must be finite and representable. Objects use the
original scaled bounding-box center; containers use its minimum. Axes remain
unchanged. The original frame remains fixed through cleanup and proposed repairs.
`dimensions_mm` retains ADR 0004's source-extent meaning; actual cleaned/candidate
and accepted bounds are reported separately.

Automatic cleanup merges exactly equal coordinates, normalizes signed zero,
removes same-winding exact duplicate faces and mathematically zero-area faces,
and corrects provably unambiguous winding without moving vertices. Opposite-winding
coincident faces remain an ambiguity to diagnose. Tiny positive-area faces are
not discarded by an epsilon rule. Every cleanup count is recorded.

## Solid analysis

The full-resolution indexed mesh remains authoritative. Manifold's construction
or tolerance welding cannot replace it: its baseline tolerance may alter edges,
and its import status is not a self-intersection verdict.

Topology analysis checks edge incidence, orientability and vertex links. Bounded
triangle BVH traversal must consider overlap beyond any legitimately shared edge
or vertex, including coplanar overlap. Different shells may not intersect or touch.
Strict shell containment forms an acyclic parent tree; alternating depth represents
outer material, cavities and nested islands. Disjoint roots remain one rigid asset.
Only after these checks is meaningful material volume reported.

Numerical uncertainty or exhausted geometric work yields `indeterminate`, never
acceptance. Check states distinguish completed zero findings from checks not run
or capped; issue examples are bounded independently of finding counts. Kernel
characterization and independent analytic adversaries precede any validity claim.

Narrow predicates use exact signs for the represented binary64 coordinates.
The initial implementation may decompose finite doubles into signed dyadic
integers and evaluate degree-two/three determinants with checked integer
arithmetic. A fixed capacity must cover the complete binary64 exponent range;
overflow or an exhausted work budget must return uncertainty. Independent rational
oracle fixtures exercise determinant signs, including cancellation and extreme
exponents. Floating filters are permitted only with a documented error bound and
an exact fallback. This adds no approximate geometry permission or dependency.

Default resource caps are 256 MiB source bytes, 5,000,000 triangles, 50,000,000
candidate pairs, 500,000,000 predicate-work units and 64 issue examples. Callers may
provide smaller or larger explicit caps within representable allocation bounds.
These caps limit work; they do not change geometric permissions.

## Welding

Tolerance welding is a separate bounded proposal with a finite positive tolerance
and a default 10,000,000 candidate-pair cap. Deterministic representative selection
must keep every changed vertex within the requested displacement; transitive
chains cannot amplify that bound. The original draft/frame stays unchanged.
Candidate cleanup and solid analysis run again in full. Before/after meshes,
actual bounds, counts and maximum displacement remain inspectable. Explicit
acceptance can promote only a completely valid candidate. No hole filling, shell
union or topology reconstruction is silently substituted.

## Compatibility and verification

This establishes the first native import API and does not migrate existing user
projects. Existing version-1 source/frame meanings remain unchanged. The additive
inspection fields below may be rejected by older strict readers; they cannot be
silently treated as an accepted solid. No portable-project or desktop acceptance
is claimed here.

## Inspection command and report

`inspect --stl PATH --units mm|inch|custom --report PATH` requires all three
options. `--role object|container` defaults to object; `--scale-mm VALUE` is required
only for custom units and is rejected for other unit selections. Input and output
paths support Unicode. Duplicate or unknown options are errors.

The report is an existing version-1 assets record with additive optional fields:
`source.byte_size`, `diagnostics.import`, and `repair_proposal`. The detailed import
diagnostics serialize the native report's encoding, source/cleaned counts, edge
and vertex findings, cleanup counts, check states, actual mesh bounds, optional
material volume, bounded work counts, shell tree and bounded issue examples.
`diagnostics.status` remains the single validity field. Flat/degenerate inspected
inputs may have zero source extents; accepted or valid records require strictly
positive extents. The source-to-local matrix keeps its existing row-major meaning.

The report directory owns an `assets/` subdirectory containing content-addressed
copies of the exact original STL and full-resolution binary little-endian f64/u32
PLY meshes. Report paths are relative to that directory. Existing content-addressed
files are verified rather than overwritten. An output may not alias the source.
Reports publish atomically after artifacts are complete; a failed publication
preserves the previous complete report. These are inspection artifacts, not a
portable project implementation.

A valid unchanged import calls the native acceptance boundary and includes
`accepted_solid` and a null `repair_record`. Other imports remain `inspected` with
a full-resolution `preview`; their diagnostics must not authorize downstream use.

`--weld-tolerance-mm VALUE` requests a proposal. It writes original and candidate
PLY previews plus a deterministic proposal record identifying source, options,
mesh hashes, before/after diagnostics and maximum displacement. Its SHA-256 is
the acceptance token. The top-level asset remains the original inspected draft
and includes `repair_proposal` with `sha256`, `path`, `before`, `after`,
`tolerance_mm`, `max_displacement_mm` and `candidate_status`. Each preview uses the
existing path/hash shape. A later invocation with the same input/options and
`--accept-repair SHA256` may accept only that exact, fully valid candidate. The
accepted report contains the proposal's path/hash in `repair_record` and
`accepted_by_user: true`; it has no pending `repair_proposal`. Tokens contain no
timestamp or absolute path, and a changed input or option cannot reuse a token.

On completed inspection stdout is one compact JSON object with `report_path`,
`state`, `status` and, when present, `proposal_sha256`. The report is the detailed
artifact. Malformed input/options or I/O failures use the existing structured
protocol error envelope with a null request ID. Exit 0 means a completed report,
including an invalid solid or an unaccepted proposal; exit 2 means invalid
input/options, exit 3 means a resource/unsupported failure, and exit 4 means an
internal or publication failure. Consumers must check state/status before use.
An acceptance-token mismatch or rejected repair is an input error, never success.

Capabilities add `implemented_commands` containing `capabilities`, `serve` and
`inspect`; `features.asset_import` continues to describe service support and stays
false until async `asset.import` is implemented. No job lifecycle is added here.

Use practical parser/transform goldens, invalid and ambiguous shells, cavity/island
fixtures, immutable ownership and explicit-repair tests. Exercise all ten pinned
`rc/` inputs without modifying their bytes. Record behavioral red/green separately
from kernel characterization and build provisioning. Bounded import timings verify
fixed input and outcomes; they establish no solver-performance claim.
