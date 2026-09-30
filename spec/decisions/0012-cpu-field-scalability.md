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
workspace and existing caller reserve. Preserve dynamic residency checks at
actual allocation boundaries; a preliminary estimate does not replace them.
Overflow is an explicit resource failure. Do not raise caps to make a case pass.

Keep a supplied valid incumbent on failed admission. A supported solve still
runs the physical baseline and must demonstrate the Ulamok 36-copy gate.
Manual pitch is immutable. A suggestion must pass the same declared admission
checks; it is a resource estimate, not a packing-count guarantee. If no pitch
passes those checks, omit a suggestion instead of inventing support.

Container masks, pose-dependent fields and orientation kernels keep distinct
owners. No data survives a pipeline call in this ticket, so changed poses,
constraints, units, container or orientation cannot reuse stale data. Cross-call
reuse remains a separately bounded T-010 change.

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
These proposals are not passing results. All T009-A1–A7 gates remain required.
