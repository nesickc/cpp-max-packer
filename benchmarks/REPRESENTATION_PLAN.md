# T-005 representation qualification

This plan governs generated test evidence, not a product wire schema. Qualify the
public native LOD/field APIs with the pinned Release build; keep large generated
meshes, fields and timing reports under `.local/`. Unit tests carry the detailed
analytic grid, cavity, thin-feature, clearance and transaction oracles.

## Fixed practical workloads

Use the three accepted items in `tests/fixtures/import-expectations.json`:
`pryanik_1.STL`, `pryanik_2.STL` and `ulamok_2kg_simplified.stl`. Pair each with
the accepted `5_kg_np.stl` container. Preserve its actual fractional dimensions;
derive the known valid physical pose from accepted container bounds, as T-004
does. All source coordinates are millimeters. Protect all ten manifest sources,
including the rejected simplified Pryanik, from changes or output aliases.

Each case compares two display settings: 20,000 triangles/0.1 mm and 2,000
triangles/0.5 mm. An unreachable count is permitted if actual count/error are
reported; useful reduction is additionally required by the curved analytic LOD
unit test. Use identity orientation and one shared physical grid per case:
4 mm pitch for each Pryanik and 10 mm for Ulamok, origin zero. The requested
environment starts at global index zero and spans the ceiling of each actual
container dimension divided by pitch. Pair and wall clearances are each 1 mm.

For each warmup and measured run, independently prepare authoritative geometry,
generate both LODs, build object/placed/container fields, and revalidate the known
physical pose. Source/accepted arrays and all field bytes must remain unchanged
across display settings. The completed physical result must stay valid and retain
its original context/poses; display geometry cannot replace that context.
LOD handles must actually share one immutable mesh across many copy references.

Check useful field outcomes, not hashes alone: prescribed authoritative surface
witnesses occupy their cells; cells well outside the object are free; all cells
known outside the permitted container/wall offset are blocked; designated interior
container cells are free. The analytic unit suite separately proves cavity and
noncardinal free-space behavior. Retain complete field windows/bits and enough
physical inputs for independent Python checks. No all-occupied fallback qualifies.

## Actual million-triangle workload

Generate a deterministic outward cube `[0,300]^3` with 300×300 squares and two
triangles per square on each face: 1,080,000 triangles, 540,002 unique vertices,
volume 27,000,000 mm³ and 54,000,084 binary STL bytes. Generate it with
`python benchmarks/subdivided_cube.py --output .local/cube_subdivided_n300.stl
--subdivisions 300` (an existing output is never overwritten). Small n=2/n=3
fixture tests check closure, orientation, volume and dimensions. Initial local
generator evidence is retained in `.local/t005/subdivision/`.
The generated original is protected like other input
files. Record its generator parameters and actual SHA-256.

Import as an object through the public native importer, with all nondegenerate
triangles retained and completed valid diagnostics. The default 1.3 billion work
budget stopped before acceptance; retain that negative probe. The positive
qualification uses an explicit maximum of 40 billion import predicate work units
and 50 million candidate pairs, without changing the product defaults. Record
actual work, duration and accepted counts; exhaustion is a failed qualification.

The first complete large import retained 540,002 vertices and 1,080,000
triangles, with volume 27,000,000 mm³, using 13,737,868,762 predicate work units
and 9,179,952 candidate pairs. Its first LOD reached 20,000 triangles. The next
phase exhausted the default 1.3 billion field work units in `rasterize-boundary`;
see `.local/t005/representation-pilot2.stderr`. Positive qualification therefore
first tried 8 billion field work units per factory for this fixed large case.
That probe completed import and LOD A, then exhausted the placed-field budget
at clearance-stencil construction after rasterization/component filling; see
`.local/t005/representation-pilot3.stderr`. The next fixed allowance is
**16 billion field work units per factory**, with the same mesh, pitch and memory
cap. Record actual completed work; these overrides are not product defaults and
failed pilots do not establish completed field qualification.

The 16-billion probe then exposed an independent useful-space failure: an
exterior object cell was conservatively occupied after its bounded parity query
remained uncertain. The field adapter now certifies strict separation between
the whole outward cell enclosure and the solid's conservative bounds before
parity. Contact and unresolved cells remain blocked. Keep that negative probe
(`representation-pilot4.stderr`) alongside the focused regression; it cannot
substitute for a completed run with the repaired implementation.

Run one complete large case, separately from the fast Debug suite. Compare
20,000 triangles/0.1 mm and 1,000 triangles/0.01 mm LOD settings; the planar cube
may produce identical final LODs. Prepare the full authoritative solid and generate
useful fields at 20 mm pitch. A placed cube centered at `(200,200,200)` in a
400 mm analytic box occupies physical `[50,350]^3`; the environment is 20³ cells
at origin zero, with separate 1 mm pair/wall clearances. Check material cells,
exterior cells and the valid physical pose before and after both LOD settings.
Reject a prescribed through-wall pose. Accepted geometry must still contain at
least one million triangles afterward. A preflight rejection cannot replace this
positive case; tiny limits are separate negative tests.

## Timing, resources and retained evidence

Smaller cases require one warmup and three measured samples. Time import/setup,
LOD, geometry preparation, field construction and physical validation separately.
Every timed result is checked. The million case runs once, with phase timings;
do not label its single observation a median or p95 distribution. Recompute
smaller-case median and nearest-rank p95 from raw samples in Python.

Use explicit bounded native/overall watchdogs (initial ceilings 900/1,200 seconds)
and at most 2 GiB representation storage for the large qualification. Check host
capacity before admitting that allowance. Keep ordinary API defaults unchanged,
and record all overrides. Track process peak working set separately from the
representation allocator counters; a declared estimate is not an RSS measurement.
These are workload watchdogs/admission limits, not latency or regression promises.

The local full matrix qualifies with the default 900/1,200-second limits. Hosted
[run 34710698429](https://github.com/nesickc/cpp-max-packer/actions/runs/34710698429)
reached the large case's completed first field set with the expected counts, then
hit the 900-second process watchdog. Its retained stdout is empty and no qualified
report was published. The hosted workflow therefore uses explicit
**1,500-second process / 1,800-second overall** limits within its 50-minute job
cap. This provides a bounded allowance for the slower hosted execution; mesh,
pitch, sample counts, field work/memory limits and all correctness oracles remain
the same. A subsequent completed hosted report is still required. Preserve the
timeout artifacts as negative evidence, not completed qualification.

The runner must reject incomplete/repeated/wrong workload sets, malformed or
nonfinite values, boolean numeric values, invalid verdict/snapshot combinations,
bad bit counts/windows, mismatched parameters, mutated sources/build inputs and
partial/time-limited outputs. Preserve raw stdout/stderr on failures. Bind source
manifest/expectations, generated input, executable/build metadata and Git revision;
publish qualified JSON atomically only after all checks and final identity/deadline
checks. Reuse existing harness helpers where their contracts fit rather than
introducing another checked-in schema or giant golden report.

No solver quality, maximum count, FFT/GPU throughput, viewport frame rate or
complete AT-05/AT-12/AT-16 claim follows from this qualification.
