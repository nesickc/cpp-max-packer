# ADR 0012: Bounded CPU field scalability

Date: 2026-09-30. Status: accepted design for T-009; qualification pending.
Requirements: SOL-01, SOL-02, SOL-05, GEO-06, DATA-01, DATA-03.

## Decision

Keep the existing solver, geometry, compute and desktop boundaries. Replace the
quadratic nearest-blocker scan; reuse the existing residency accounting and
error transport. Add no dependency, runtime framework or persistent field cache.
T-010 owns threading and preparation reuse.

### Distance and work

Proximity remains `exp(-sqrt(d2)/2)`, where `d2` is the minimum squared Euclidean
distance between integer cell centers and blocked cell centers on the common
lattice, including the existing exterior halo. Physical pitch cancels from
`distance_mm/(2*pitch_mm)`. No blocker gives zero proximity; a blocked cell gives
one. Origin, axis order, conservative occupancy, correlation and candidate score
weights stay unchanged. Ties retain orientation index then world Z/Y/X order.

Use a separable squared-distance transform with a one-dimensional lower envelope
on each axis. Keep squared distances as exact integers in the existing double
buffer and bound the supported squared-distance range to at most `2^53`.
Calculate integer envelope breakpoints with checked signed arithmetic; reject
unsupported arithmetic before computation. Scratch is proportional to the
longest axis, not another full grid. Convert to proximity with the same final
square root and exponential. Test every cell against the independent direct
oracle, including empty/full fields, asymmetric grids and ties.

The `proximity_terms` counter now counts actual transform work, rather than
all-pairs comparisons. The implementation documents its charged operations and
checks the cap before exceeding it. Evidence records the algorithm revision;
old and new term counts are not equivalent workloads. Compare identical grids,
occupancy, ordered proposals, candidate budgets and retained poses.

### Admission and ownership

Share compute's checked padding/workspace arithmetic through
`estimate_correlation_cpu(CorrelationSpec)`. Its estimate contains padded shape,
padded cells and working bytes excluding caller reserve. Include the existing
audited complex buffers, real output and pocketfft workspace/probes.

Solver admission stays private. Check shape/cell limits and aggregate live
storage before expensive unsupported field work. Include accepted assets,
retained solution, container mask, reference counts, occupancy, proximity,
distance scratch, orientation kernel, simultaneous correlation outputs, FFT
workspace and existing caller reserve. Geometry exposes a checked scalar prepare/place and metadata bound beside its private types; accepted inputs, grid arrays, per-copy footprints/IDs and caller reserve are excluded. Solver admission adds these separately, bounding each retained footprint by a full environment size_t array, expanded fill/dilation/crop scratch and simultaneous correlation outputs.

Preserve dynamic residency checks at
actual allocation boundaries; a preliminary estimate does not replace them.
Overflow is an explicit resource failure. Do not raise caps to make a case pass.

Keep a supplied valid incumbent on failed admission. A supported solve still
runs the physical baseline and must demonstrate the Ulamok 36-copy gate.
For field-only resource failures, keep the cheap admission result pending while
the independent physical baseline runs, then return that resource failure before
field construction. This preserves existing baseline results even at an
unsupported voxel pitch. An actual baseline failure takes precedence. Early
admission means no expensive unsupported field work; it does not suppress the
physical baseline or manufacture an empty replacement for it.
An observed Stop or elapsed deadline keeps its existing precedence before a
pending field-only failure; report that failure only when search would proceed.
Manual pitch is immutable. A suggestion must pass the same declared admission
checks; it is a resource estimate, not a packing-count guarantee. If no pitch
passes those checks, omit a suggestion instead of inventing support.

Container masks, pose-dependent fields and orientation kernels keep distinct
owners. No data survives a pipeline call in this ticket, so changed poses,
constraints, units, container or orientation cannot reuse stale data. Cross-call
reuse remains a separately bounded T-010 change.

### Physical baseline rounding repair

A pre-change native trace reproduces 17 retained Ulamok copies and exhaustion
of the 1,300,000,000-operation validation budget. An early second-Z-layer pose
has a represented bounding-box gap about `0.09999999999999432` mm, below the
requested binary64 `0.1`; its native report is `VALIDATION_PAIR`. The independently
constructed 36-copy witness is valid. This identifies a new-build reproducer,
not the missing historical executable's exact failure identity.

Keep the original exact planned axis count. For positive-clearance plans with
an inexact nominal span, step or first-anchor sum (checked by a local error-free
TwoSum under strict floating-point compilation), propose a search-only margin
of half the available axis slack divided by `n+1`: two walls and `n-1` gaps.
Re-plan with the existing exact physical query and use the margin only if it
preserves the original axis count. Otherwise retain the original plan. Leave
zero-clearance exact tilings and exactly representable grids unchanged.

This is a deterministic proposal heuristic, not permission to alter constraints
or a certificate of validity. The authoritative validator still checks each
placement. Physical dimensions and requested clearances remain unchanged in
results. Poses for affected formerly inexact grids may change; the frozen
dyadic-gap performance fixtures must remain identical. No validation cap is raised.

### Failure contract and compatibility

Add owned optional failure details to `BaselineOutcome` and the internal
pipeline result: reason, phase, underlying cause code, optional resource
name/required/limit, and optional suggested pitch. Keep the existing static
diagnostic code for compatibility and allocation-failure fallback. Copy nested
owned strings; never retain a view into a destroyed failure object. Propagate
resource classification explicitly and preserve the last validated solution.

CLI `error.details` and optional result run diagnostics carry
`failure: {phase, cause_code, resource?: {name, required, limit},
suggested_pitch_mm?}`. Serialize resource counts as decimal uint64 strings.
Update schemas and generated types before consumers. The desktop displays the
cause and actionable resource information and retains evidence through project
and JSON round trips. Existing documents without diagnostics remain valid;
geometry, repaired-asset interpretation and schema version numbers stay intact.
Older strict readers may reject new optional members: this is additive reader
compatibility, not a claim of forward compatibility with old binaries.
The CLI also corrects generic solve failures to the canonical internal-error
exit code 4 (§8.1); resource failures remain exit code 3. Scripts that treated
all failed solves as exit 3 must inspect the structured error or handle both.

## Qualification

Retain pre-change Release samples before optimizing measured paths: one warmup
and five serial samples, then freeze numerical targets in the ticket evidence.
Use three pinned grid sizes and independent oracle/count/pose checks. Historical
17-copy artifacts pin settings and result bytes but do not identify the missing
native cause or prove the exact dirty executable can be reconstructed.

Initial practical profiles to qualify are Ulamok's 400×350×285 mm box, cube
rotations, pair 0.1/wall 1 mm with 5 mm pitch (8 mm candidate if necessary), and
each full Pryanik in a 100×100×50 mm box, fixed identity, the same clearances,
4 mm pitch. Freeze supported profiles and sufficient budgets from measured
evidence before acceptance. The independent Ulamok witness is in T-009; each
Pryanik has a centered `[50,50,25]` candidate requiring native validation.
These proposals are not passing results. Qualification follows the dependency
revision below; original product requirements remain unchanged.

### Reviewed practical-work dependency (2026-10-01)

Actual Ulamok runs at 4 and 16 mm retain 36 copies but exhaust representation
work before completing the cube catalog. The rasterizer charges 32 units per
triangle plus 180 per tested cell, with at least 27 cells for an unclipped
triangle. The positive-clearance halo leaves the retained Ulamok copies
unclipped. Rebuilding them for all 24 orientations therefore requires at least
`768 * 36 * 24 * (32 + 27 * 180) = 3,246,096,384` units, exceeding the unchanged
1,300,000,000 cap before other field work. Coarsening cannot remove that floor.
Independent critical review confirmed this from the production loops and costs.

Use T-009's explicit scope-revision provision. T-009 delivers the distance
transform, physical lower bound and retained resource/error workflows. The
original A2 supported spectral run, A3 verified supported-pitch advice, and A6
successful practical spectral workflows remain open dependencies of T-010's
field/preparation work. Do not call a memory-admitted pitch supported or count
a retained resource failure as a successful spectral run. Do not add a cache,
raise caps or substitute a volume-based work guess in this tranche.

Suppress optional pitch advice when a provable, pitch-independent retained
raster-work floor already exceeds the cap. Keep its cost constants beside the
actual raster charges and use checked arithmetic; this is a necessary bound,
not an estimate of total work or a guarantee of support. Existing actual failure
codes/counts remain intact. This changes only optional advice, not requested
settings, geometry, validity or limits. T-010 must qualify full passes and the
original practical paths after reducing repeated work; parallelism alone cannot
remove a cumulative-work limit.

### Desktop JSON round-trip fidelity

The actual 36-copy Ulamok Save/move/Open journey exposed a binding failure.
The native result records a Y span of `90.46014404296875` mm; the saved project
records `90.46014404296876`. In total, 22 numeric fields changed by one binary64
ULP while passing through the desktop JSON representation; source and accepted
mesh bytes stayed unchanged. Native restore correctly rejects the derived-field
mismatch. Analytic integer-coordinate round trips did not expose it.

Preserve native binary64 values using the existing pinned `serde_json`
round-trip parsing feature. The focused parser reproducer and the full 36-copy
Save/move/Open/JSON regression pass after failing before this change.
Keep exact native result bindings and authoritative validation unchanged. This
adds no dependency or project-format version. New saves must preserve the native
numbers; existing archives are still validated as stored, without guessing or
silently repairing values already changed by an older writer.

The corrected practical round trip reaches a separate checked-STL failure,
`EXPORT_RESOURCE_LIMIT` / `EXPORT_IMPORT_WORK_LIMIT`. Preserve it as an open A6
qualification for T-010. The binding regression may qualify exact Save/Open/JSON
preservation separately, while the full STL assertion remains in the pending
qualification. No export validation, count or work limit is weakened.
