# ADR 0007: shared display LOD and conservative fields

Date: 2026-09-12. Status: accepted interface and numerical contract;
implementation and qualification are tracked in [T-005](../../doc/T-005.md).

Requirements: GEO-04, SOL-02, SOL-05; spec §§5.1, 6.3, 6.6; native portions
of AT-05/AT-12/AT-16. This extends the accepted-solid boundary from ADR 0006.
It adds no wire schema, solver, FFT, desktop or export behavior.

## Ownership and public boundary

`pack_geometry` owns immutable `DisplayLod`, `VoxelGeometry` and `CellField`
handles. They retain their accepted-solid identity; constructors and value
copy/move operations are private or disabled. Callers share const handles.
Derivatives cannot enter authoritative validation. There is no caller-asserted
geometry hash or hidden unbounded global cache. I/O resolves persistent hashes.

`make_display_lod` takes an accepted solid, display settings and representation
limits. `prepare_voxel_geometry` builds reusable preprocessing from an accepted
solid. `voxelize_object`, `voxelize_placed` and `voxelize_container` produce
object kernels, placed-copy clearance masks and container blockers respectively.
Display options do not occur in field inputs or identity. Prepared geometry is
shared across orientations/copies without reusing a prior physical verdict.

`BlockedField` owns mutable trial reference counts and immutable copy footprints.
Adding a duplicate ID, incompatible mask or an operation exceeding a limit fails
without mutation. Removal uses the recorded footprint, preserves overlapping
counts and never clears the immutable container mask. Counter overflow/underflow
cannot saturate or wrap silently. No mutable buffer escapes these APIs.

## Grid and conservative classification

The common lattice has finite origin `g` and one positive physical pitch `h`.
Indices are signed X/Y/Z integers. Cell `i` logically owns
`[g+h*i, g+h*(i+1))`; geometric queries use its closed enclosure. Exact surface
contact may therefore occupy both neighbors. This overblocking is permitted and
does not prove a continuous placement impossible.

Arrays use X-fast storage `x + nx*(y + ny*z)`, with checked arithmetic. A window
retains its signed first index and shape. An object kernel rotates accepted local
coordinates about zero, puts that anchor at `g`, and retains trimmed origin `a`.
An integer shift `t` means physical translation `g+h*t`; array index `j` occupies
global cell `a+j+t`. For environment first index `b`, future correlation is
`sum_e B[e] O[b+e-t-a]`. Trimming never introduces a second recentering.

STL containers stay at accepted local identity/zero translation; analytic boxes
remain `[0,width] × [0,depth] × [0,height]`. Off-grid placements are rasterized
from their actual rigid pose. Pitch, clearances, dimensions and poses never
change to fit memory or align a grid.

The existing numerical adapter supplies a narrow field seam. Conservatively
rasterize every authoritative triangle with outward transformed intervals and
triangle/cell separation certificates. Then flood boundary-free components with
six-neighbor adjacency and classify one bounded material witness per component.
This fills solid interiors, preserves cavities and disjoint components, and
avoids a full solid-distance test per cell. An uncertain component is occupied.
Useful exterior and cavity cells must still pass independent tests.

For noncardinal rotations, enclose a witness's inverse transform in local space,
prove that connected enclosure misses the authoritative boundary, then classify
a represented local point with the existing exact ray predicate. Unresolved
predicates stay uncertain. Reuse existing homogeneous rotation, interval and
exact arithmetic; do not create a second Boolean or distance kernel. Rounded
grid corners alone cannot certify free space. Existing validator semantics and
its regression gates remain unchanged.

## Clearance and mask composition

Conservative occupied cells enclose the object material. Dilate their closed
union by the **full pair clearance** `c`. A displaced cell can be excluded only
when the following Euclidean lower bound is proven greater than `c*c`:

`h*h * sum_axis(max(abs(delta_axis)-1, 0)^2)`.

Include equality and uncertainty. Use checked stencil/halo bounds, and charge
construction and application work. At zero clearance retain base occupancy.
Do not halve clearance, floor a fractional radius or substitute a Chebyshev
distance. Cell-union dilation may conservatively overblock actual solid offsets.

For STL containers, only boundary-free cells certified in permitted material
start free. Dilate the complementary blocker mask by the separate wall clearance.
Compute the outside halo before cropping to the requested window, so window
edges do not become invented walls. Analytic boxes may use direct whole-cell
inward-plane certificates. Unknown cells remain blocked.

## Display approximation and admission

Use pinned meshoptimizer 0.25 privately, with normal `meshopt_simplify` and
absolute-error mode. Defaults remain 20,000 triangles and 0.1 mm approximate
display error. Allocate destination capacity for the original index count.
Return selected original double coordinates in privately compacted storage;
never alter authoritative buffers. Reserve twice the float conversion
displacement from the requested approximation budget before simplification.
Nonfinite/excessive errors cannot be reported as satisfying that budget.

When conversion consumes the budget, an unchanged shared full-resolution view
with zero approximation error is permitted. If simplification cannot reach the
triangle target, report its actual count and `target_reached=false`. A target
miss is not permission for sloppy simplification or relaxing the error setting.
The error remains approximate, not a certified enclosure or Hausdorff bound.

Install meshoptimizer allocation hooks once, serialize adapter entry and use a
thread-local allocation ledger. Count known caller buffers and private scratch,
reject before allocation, unwind safely and verify a subsequent retry. Do not
temporarily swap process-global allocators around concurrent calls. A bounded
input-triangle gate covers the non-cancellable simplification operation; it does
not establish product stop latency.

All factories preflight checked geometry/grid sizes and their real concurrent
buffers, including resident accepted/prepared inputs and explicit caller reserve.
Fields account for halos, masks, flood queues/labels, outputs, stencil storage,
counts and stored/pending footprints. The shared default limits are 512 MiB
tracked working storage, 16,777,216 cells, 1.3 billion kernel work units,
200 million cell visits and five million input triangles. Qualification may
record explicit bounded overrides. Tracked storage is distinct from process RSS.
The exact allocation helper is internal to the buffer owner; an unchecked
caller-provided estimate never bypasses a factory's admission checks.

Global work/memory exhaustion returns an operational failure with no completed
field. Geometric uncertainty is counted and conservatively blocked. Full
FFT/device/OS memory policy remains a later solver/backend gate.

## Evidence and compatibility

Use practical public runtime RED/GREEN cases: useful simplification, unreachable
targets, source/field/validator isolation, exact grid alignment and fractional
origins, material interiors/cavities/thin features, noncardinal useful free cells,
Euclidean full-clearance stencils, halo cropping, transactional counts and failed
allocation retries. No all-occupied fallback can satisfy the positive gates.

The large fixture is a real outward cube subdivided 300×300 per face:
1,080,000 triangles, 540,002 unique vertices, 54,000,084 binary STL bytes. It must
pass the public importer with all source triangles retained before positive
large-mesh LOD/field qualification. The initial default-limit probe exhausted
1.3 billion predicate work units after 827,487 candidate pairs in 13.36 seconds;
topology completed, but the solid was **not accepted**. Retain that negative
evidence and use explicitly recorded qualification-only limits for a bounded
complete run. Do not change import defaults or bypass acceptance to pass this gate.

No source STL, accepted geometry, clearance rule, wire contract or compute
dependency version changes. Meshoptimizer's existing license joins the engine
distribution notices when linked. Deterministic solver results, exports and
viewer sharing remain their later integrations; this ticket qualifies the native
representation boundary and does not complete all of AT-05 or M1.
