# Packing algorithm: current design

This is the current search design. Unlike the [spec](../doc/spectrapack-spec.md),
everything here is **tunable**: change weights, page sizes, catalogs, schedules and
recovery behaviour freely when `python tools/scoreboard.py` shows an improvement and
the validity rules in spec §5.3 and §6 still hold. Update this file with the change;
no decision record is needed. This file began with spec v0.2 §6 and is updated
through 2026-10-19. Historical rationale lives in
`spec/decisions/` and `doc/T-*.md` (frozen).

## Baseline and staggered seed

For a box, generate a grid of oriented object AABBs for each allowed baseline orientation. For dimensions `b_x,b_y,b_z`, pair gap `c_p`, and wall gap `c_w`, the candidate count along axis `j` is:

`n_j = max(0, floor((L_j - 2*c_w + c_p) / (b_j + c_p)))`

Place the grid deterministically and validate it using authoritative geometry. Preserve its best result as a starting incumbent. Account conservatively for numerical uncertainty in the floor and check boundary placements. This baseline deliberately works in physical coordinates, so a coarse voxel grid cannot prevent an obvious exact box tiling.

The bounded T015-P1 package additionally investigates a physical staggered-row
seed within the existing permitted rotations and work budget. It preserves the
AABB incumbent and sends complete proposals to authoritative solid validation;
it does not silently alter manual pitch, clearance, dimensions or rotation policy.
Its seed and subsequent FFT work share the existing spectral-phase allowances;
the baseline keeps its existing separate allowance. Admit only the catalog prefix
permitted by the remaining pass budget, charging before oriented queries. Additive
command `diagnostics.stagger` records actual work and completion within that prefix,
independently of later FFT failure; persistent result schemas are unchanged.
Detailed bounded ownership and accounting follow [ADR 0018](../spec/decisions/0018-cardinal-support-stagger.md).
Its practical targets and remaining gates are recorded in [T-015](T-015.md).

Zero-clearance validation uses the same authoritative bounds as positive clearance.
For cardinal poses, exact dyadic comparisons keep source extrema and translations
separate: a nonnegative axis gap excludes positive interior overlap, including
exact contact. The oblique support certificate still requires a strictly positive
enclosed projected gap, even when the requested clearance is zero. Inconclusive
bounds retain full solid validation; no tolerance is subtracted from clearance.
Fresh result loading rebuilds these proofs from authoritative geometry and poses.
STL export applies the same exact bounds proof to the independently imported
quantized vertices, so original-model bounds cannot authorize a staged mesh.

This raises the zero-clearance Pryanik-2 result from 1 to 360 valid copies at both
2 and 4 mm manual pitch under the existing 60-second settings. The existing grid
produces 330 and the generated staggered proposal improves it to 360; no saved
item-specific poses are used. That validation fix alone left the positive-clearance
result at 340.

Timed runs extend the physical grid search with residual block layouts. For each
allowed orientation, fill a rectangular region with a uniform grid, then consider
all six orders of cutting its unused space into three disjoint slabs. Recursively
fill those slabs with permitted orientations. Search depths 1–3 in order; memoize
region dimensions/depth with a bounded cache (at most 32,768 nodes, also limited by
available scratch). Only complete proposals exceeding the retained count enter
full native validation. This rearranges the layout without changing manual pitch.
Positive pair/wall distances receive a small additional proposal margin, never a
reduction; zero pair clearance remains zero. The fixed-work path is unchanged.

The unchanged 60-second benchmark improves the positive-clearance Pryanik result
from 340 to 343 at both 2 and 4 mm manual pitch. Both saved layouts pass fresh
native validation. The zero-clearance floor remains 360; the 388-copy bounding-box
reference is still only a reference, not a native-validated solver result.

Timed runs then search interlocking rows using the accepted mesh. For each of up
to 24 permitted orientations, choose the shortest AABB axis for layers and the
longest for columns. Clip mesh triangles into 64 x 32 row/height bins and retain
their column intervals. These envelopes preserve sloping edges and thin wings
better than whole-triangle bounding boxes, but fill cavities and are only search
screens. They never establish validity.

First search height-staggered rows: sweep box-fitting pitches, four lateral row
offsets, signed column rises derived from object height, and 32 vertical phases.
Wrap each column's height into a periodic stack and count its individual layers.
Screen neighboring columns, rows and layers against the requested Euclidean pair
gap. Extra proposal spacing is not an additional required clearance in the screen.
If no improving height proposal survives, try coplanar rows with 16 lateral
phases, fractional pitches and alternating/progressive offsets as before.
Work is bounded at 32 million units per orientation, with half the remaining work
reserved for the coplanar fallback. Shared cancellation, deadline and memory
admission remain enforced. Only the highest improving proposal from each
orientation enters native validation; invalid or indeterminate results cannot
replace the incumbent.
The search uses no asset names, saved poses or altered physical constraints, and
does not change manual voxel pitch or the fixed-work path.

After retaining those uniform layouts, timed search tries mixed orientations.
Pairs come from the permitted catalog and differ by a half-turn around a world
axis. Prioritize the retained layout's orientation, then test two-state patterns
alternating columns, rows, layers, or their checkerboards. Align each rotated
AABB minimum independently. First try paired layers with sideways offsets of
1/8, 3/16 and 1/4 of the oriented row extent and signed height shifts from -7/16
through -1/16 of the layer extent; then retain the existing single-axis sweeps.
These are relative translations, not rescaling of geometry or the FFT pitch.
Pitches come from the box spans, reserving space for both the sideways shift and
any positive height shift before spacing the layers. Full-span height spacing
remains an alternative because compression can obstruct interlocking neighbors.
Stretching unshifted layers to both walls can discard shifted copies in the last
layer. Every stack is counted within its actual wall limits. Finite layer pairs and rows do
not need to accommodate imaginary neighbors outside the proposed layout. A tiny
scale-aware inward proposal margin avoids rounding a zero-wall seed outside the box.
Reject impossible vertical stacks and column chains before sweeping row spacing
and offsets; these independent screens avoid repeating the expensive mesh checks.
The two envelopes share the remaining work/vertex allowance, with a 64-million
work cap each for mixed proposals and 32 million for single-orientation proposals.
Neighbor screens cover both orientations and nearby layers. Only
an improving complete layout that passes the independent native validator replaces
the incumbent. Fixed/catalog rotation permissions and the fixed-work path remain
unchanged.

Deep interlocking also needs an efficient independent native proof. Validator v5
can partition the authoritative solid into 64 closed slabs along its shortest
local axis. Every intersecting triangle contributes its complete in-plane
supports, so the slabs enclose material, including filled cavities. For compatible
cardinal poses, every occupied slab pair must prove the requested distance using
orthogonal slab-axis and normalized in-plane lower bounds. Directed interval
arithmetic encloses the original doubles; zero clearance still requires a real
separating gap in this certificate. Unsupported or inconclusive pairs use the
existing full classifier. Arithmetic/resource/control failures stay indeterminate.
Tables belong to one validation call and are rebuilt during fresh result loading.

Validator v6 adds interval shell witnesses. When world-coordinate addition is
inexact, shell witnesses retain their outward
intervals. Inverse-transform the entire interval into the target's local frame,
prove it misses every boundary triangle, then classify its midpoint with exact
ray predicates. This resolves separated solids without treating rounded vertices
as exact or relaxing overlap/clearance rules. Strict segment/triangle AABB rejection
skips irrelevant ray predicates; all broad-phase and exact work share one budget.
Boundary and clearance checks cache the second mesh's outward triangle bounds
per pair and reuse each first-triangle row bound. A downward-rounded gap on any
axis can prove strict Euclidean clearance; unresolved pairs retain the exact
distance checks. A budgeted spatial partition groups up to 32 triangles under
outward union bounds, allowing separated groups to be skipped. Unresolved leaves
retain the same exact predicates with their original face indices. Checked scratch
admission, cancellation and allocation failure remain fail-closed. This avoids
recomputing the same bounds millions of times during fresh result validation.

For inexactly represented transformed vertices, overlapping triangle bounds alone
cannot establish contact. Validator v7 tries face normals and edge-derived axes,
then projects outward vertex intervals onto each finite axis. A strict projection
gap certifies disjoint boundaries; unresolved leaves remain indeterminate. Shell
witness and requested-clearance checks still follow. Rounded vertices suggest
axes only and never supply the proof.

The positive-clearance benchmark now finds 435 validated Pryanik-2 copies, up
from 425 with mixed orientations, without changing the box, 1/1 mm clearances, permitted rotations or
60-second budget. The bounded search can still miss layouts; these counts are
best found, not optimality claims.

## Whole-arrangement compaction

After physical seeding, timed box searches compact the retained arrangement along
each world axis, starting with Z to match the occupied-height objective. Four sweeps alternate
directions so that space opened along one axis can be reused on a later sweep.
Existing copies retain their rotations. Search-only triangle interval envelopes
use 64 x 64 transverse bins relative to each oriented object's size; translations
stop at proposed mesh contacts, with a small positive numerical cushion.

Before each axis slide, bounded whole-layout trials contract the distances
between copy bounds by up to ten percent. Only translations change; each solid's
dimensions and catalog rotation stay fixed. Eight bisection trials retain only
native-valid improvements. This can recover diagonal slack even when the coarse
column envelopes overlap at the valid starting positions.

Mixed-orientation seeding also tries a relative headroom proposal when ordinary
screening finds no improvement. It adds one sixteenth of oriented object height
to the proposal range, then maps copy positions back into the actual box before
validation. The validation context, physical container, geometry and clearances
never change. This fallback addresses false rejection by filled transverse bins;
it is not evidence that a proposal fits until native validation accepts it.

After each axis, try refilling at retained transverse positions, wall alignments,
and quarter/half-extent offsets. New copies use permitted catalog orientations
with the retained AABB extents. Drops can search disconnected free intervals;
this is final-placement packing, not a collision-free insertion-path simulation.
No asset names, saved layouts, geometry scaling or model-specific distances enter
this search.

Every changed arrangement and every insertion passes native validation.
Compaction, refill and the growing AABB grid validate incrementally against the
last accepted solution: a copy whose ID and pose match the base copy at the same
index keeps that solution's containment and mutual pair results, and every other
copy is checked against the container and all copies. Input and orientation checks
still cover every copy, and saved results are always freshly validated in full.
Failed trials roll back to the last validated working arrangement. Equal-count
improvements use the existing enclosing-Z-span, then XY-span-sum ranking, so
compaction is visible even when another copy does not fit. Shared work, memory,
deadline and cancellation limits bound the pass. Interval filling remains
conservative and axis-wise motion can stop in a local minimum; this is not a
claim that all remaining gaps are removable.

After the sweeps, FFT gap filling inserts single copies in any permitted rotation.
It rasterizes the container and every placed copy (dilated by the pair clearance)
once on the run's pitch. The pitch is never finer than about 40^3 cells across the
part's bounds, and coarsens when the grid, one translation mask per rotation and the
FFT buffers would exceed the memory allowance; rasters and offsets are reused
between the fills of one compaction pass. One box-scale
correlation per rotation marks every translation clear of walls and copies. The
lowest free translation (Z, then Y, then X of the kernel's bottom cell) is proposed
and validated incrementally; a rejection clears only that translation. An
accepted copy clears the translations its field blocks, from cached offsets that
depend only on the two rotations: for cube rotations the lattice maps onto itself,
so the offsets for rotations A and B are A applied to those for the identity and
A^T B, and at most 24 small correlations serve every pair. Gap-fill checks are
bounded per check, not by the phase allowance that whole-layout trials consume.
The same filler then packs an empty box; that layout replaces the compacted one
only when it holds more native-valid copies. Fields are conservative proposals:
the native validator decides every insertion.

The positive-clearance L-prism scoreboard case improves from one copy to two.
The exact-contact L-prism case remains separate: its unresolved contact verdicts
are not bypassed by compaction.

## Layered double lattices

Flat parts often pack densest as identical layers in which a copy and its twin,
turned half a turn about an axis in the layer, alternate so that thin rims and
tabs nest over and under each other. Right after the seeded phase, timed box
searches look for such layouts. For every world axis `k` and every pair of cube
rotations with the same extent along `k` (unordered, one per class of the half
turns that preserve the box), the copies share slabs of that thickness. In the
slab plane rotation A sits on a lattice `i(px, 0) + j(sx, py)` and rotation B on
the same lattice offset by `s`; slabs repeat along `k`, spread evenly over the
height and at least the pair clearance apart. Flipped twins are searched first,
then single rotations, then the rest.

The part is rasterized once per bounds corner in its own frame (on the run's
raster threads), with the lattice anchored at that corner; with a positive pair
clearance the placed copy is also grown by it plus a micrometre-scale gap. A
cube rotation only permutes and reverses cell axes, so every rotation's field
anchored at its posed low corner comes from at most eight rasters without
resampling. One FFT correlation per rotation pair, read at zero offset along `k`,
gives the plan offsets at which the two copies would meet (the no-fit map). The
lattice search then only reads maps: periods must avoid the A-A and B-B maps, and
B offsets are lattice classes, so folding every blocked run of the A-B map into
the lattice cell (one difference array per cell row) leaves the free ones.
Periods are tried on coarse cells (about 20 across the part's plan), then on
single cells around the best coarse lattices; B offsets are always single cells,
because a twin's free window can be narrower than a coarse cell. Either
sublattice may start at the box corner, since a twin that nests ahead of its
partner can fit one more copy per row. Only improvements in copies per layer are
kept. Planning takes at most a tenth of the remaining time; the best few layouts
are then validated in full and the first valid one replaces the incumbent if it
has more copies. The row, stagger and compaction phases follow and start from it.

The simplified Pryanik in the 400 x 340 x 285 mm box at zero clearance goes from
324 on-edge copies to 336 in 120 s: fourteen 28.4 mm slabs across the 400 mm side,
each holding 12 copies and 12 flipped twins with every wing tucked against the
neighbouring twin (330 in 60 s). Lattice periods are multiples of the roughly
1 mm planning cell, and conservative cells keep copies up to a cell apart, so
tighter nesting than that is not found. On parts whose rasters do not finish
within the share, as for the exact Pryanik-2 mesh on one thread, the phase gives
up without a layout.

## Conservative fields and FFT placement

Use one physical voxel pitch `h` and a common world grid per search level. Do not independently normalize the object and container into unrelated voxel scales.

- `O_R`: conservative solid occupancy for the authoritative object rotated by catalog orientation `R` about its local anchor. Store the kernel's trimmed grid origin explicitly.
- `B`: blocked environment cells. Block cells not wholly certified inside the container eroded by wall clearance. Also block cells intersecting placed solids dilated by the **full pair clearance**. This one-sided dilation enforces pair distances without coupling them to wall clearance.
- Construct solid occupancy with conservative triangle/cell intersection and inside classification. Preserve cavities; surface-only voxelization is not sufficient. Any uncertain cell is blocked/occupied. Euclidean clearance offsets must be conservatively represented rather than rounded down to whole voxels.

For integer translation `t`, the overlap field is:

`K_R(t) = sum_x B(x) * O_R(x - t)`

Implement it using FFT cross-correlation, with a documented reversal/conjugation and indexing convention. Pad each dimension to at least `environment_size + kernel_size - 1` before selecting an efficient FFT length. Include required geometry/clearance halos; discard all translations outside the legitimate world domain. Circular wraparound must never create an apparent free placement.

Use the FFT to propose low-overlap candidates. Verify shortlisted positions using integer/bitset occupancy and then the authoritative validator. Floating-point near-zero thresholds affect proposal quality only; they cannot establish feasibility. The reference CPU implementation uses double FFTs. The GPU path may use single precision with a measured tolerance envelope and discrete rechecks. If a field fails its error check, recompute on CPU or mark the pass unreliable; do not report a negative feasibility conclusion from it.

Specify cell boundary ownership explicitly and test exact grid alignment. Conservative occupancy can still reject a geometrically valid contact. When ordinary insertion stalls, allow a bounded refinement pass over low-overlap proposals and physical feature/AABB alignment candidates, using authoritative validation directly. Voxel overlap may reject a fast-path candidate; it is not proof that no continuous placement exists. This escape path is especially important for zero-clearance tilings.

Rank collision-free candidates using a height term and a proximity correlation. Initial policy: minimize `0.65 * normalized_top_height - 0.35 * mean_proximity`, with proximity decaying as `exp(-distance / (2*h))` from blocked geometry. Normalize terms so scores are comparable across orientation kernels. These weights are application defaults, not a claim of exact paper reproduction. Tie-break by orientation index and world Z/Y/X. Return candidates in pages of 32 per orientation; expand the shortlist when validation rejects a page and budget remains.

Cache the identical object's orientation kernels and spectra by geometry hash, pitch, orientation catalog/version, padding, numeric precision, and backend. Cache fixed container data separately. A pose change invalidates the affected environment data; changing units, clearances, or container invalidates incompatible fields.

Maintain occupancy **reference counts** for placed, dilated copies, because their clearance regions can overlap. Removing a copy must not clear cells blocked by another copy. Combine these counts with the immutable container mask. Revoxelize after off-grid motion; never round a refined pose back to the grid silently.

## Orientation catalogs and refinement

The free catalog begins with the 24 cube rotations, then their compositions with a 45-degree local-Z rotation, then deterministic low-discrepancy unit quaternions. Generate the latter from Halton triples in bases 2/3/5 and the standard unit-quaternion map; record the implementation version and resolved catalog hash. Deduplicate antipodal quaternions. Balanced uses 256 catalog orientations and Thorough 2,048. Sampling is not exhaustive coverage of continuous rotations.

For implementation clarity, use Halton indices starting at 1 and map a triple `(u1,u2,u3)` to `[sqrt(1-u1)*sin(2*pi*u2), sqrt(1-u1)*cos(2*pi*u2), sqrt(u1)*sin(2*pi*u3), sqrt(u1)*cos(2*pi*u3)]` in XYZW order. Sort cube rotation matrices lexicographically, use composition `R_cube * R_z45`, canonicalize signs, and continue sampling until the requested number of unique orientations is reached. Orientation ordering/version is part of deterministic replay.

Refine promising layouts by translation steps `h/2`, `h/4`, `h/8`, and, for free mode, local angular steps 5°, 2°, 1°, with decreasing-step improvement passes. Upright refinement only changes yaw. Fixed and discrete catalogs cannot drift into forbidden orientations. Every accepted move is revalidated and its environment field rebuilt or correctly updated. Repeated failures can end local refinement early; log that outcome.

## Count controller

The current implementation is narrower than the original design:

```text
retain the validated uniform-grid baseline and staggered proposal
for timed runs, validate improving layered double lattices
for timed runs, validate improving residual-block proposals (depths 1–3)
for timed runs, validate improving mesh-envelope stagger proposals
for timed runs, compact, FFT-gap-fill, then FFT-greedy-pack the empty box in
    each axis order while time remains (layouts with fewer than 8 copies try one)
if fixed-work, or no seeded phase placed a copy, and FFT admission is affordable:
    greedily insert from an empty layout using cached spectral pages
    on stall, try bounded physical face alignments and low-overlap candidates
    if the first trial completes, try insertion into the retained layout
return the highest validated count with the actual termination reason
```

Every phase shares the Start deadline and Stop control. Physical block and
interlocking-row trials keep the entry incumbent while testing complete replacement
layouts. The spectral
controller does **not** yet perform the originally proposed spatial-neighborhood
removal/reinsertion, equal-count pose exploration, multiple randomized restarts or
continuous local pose refinement. Extra time can evaluate more candidates but does
not guarantee an improved retained count. These remain possible future changes,
not descriptions of current behavior.

Fixed-work mode preserves its candidate/pass limits and ordering. Timed recovery
can renew cumulative work allowances; per-operation safety bounds, finite retry
ladders, memory admission and cancellation remain enforced. Publication exposes
only strictly higher counts through the existing count-publication adapter.

## Resource handling

Spec §6.6 governs: estimate the dominant live buffers, fit available memory, keep
the incumbent and exhaust affordable work before reporting a resource limit.
Manual pitch is never changed by this implementation.

Finalization has a separate bounded 30-second allowance for fresh native validation
and saving the retained result; it does not extend search. The desktop response
watchdog includes that allowance plus one second. Actual elapsed time and budget
overrun remain reported. A failed final validation still refuses publication.

The early 340/360-copy benchmark stops had two separate causes. Hand-written CLI
settings without `resolved.resource_policy` silently selected legacy execution,
even for a timed job: cumulative raster work stopped 4 mm runs after four
correlations. The same path used a fixed 512 MiB fallback; the 2 mm preflight
estimated about 710 MB and refused admission. Neither result established physical
infeasibility.

Policy-less timed CLI runs now select the same measured-RAM-share policy as
desktop Start, honoring an explicit host-memory cap. Explicit resolved policies
and fixed-work behavior remain intact. Existing recovery retains completed
geometry, masks, kernels and correlation pages, can evict disposable state/reduce
threads, and grants bounded additional work. Incomplete raster operations can
still restart on a larger allowance; resumable raster cursors across failed calls
are not implemented. The physical block phase is independent of field admission
and provides useful rearrangement even when a field cannot fit.

Before the interlocking-row extension, the 4 mm benchmark reached 18 correlations and nine FFT candidates submitted
to native validation, compared with four correlations and no FFT candidates before
the fix. It retained 343 and ran to the unchanged 60-second deadline. The 2 mm run
passed memory admission and reached one correlation, but submitted no FFT
candidates within 60 seconds; its count improvement came from physical blocks.
Logs and budget-scaling measurements are in `.local/continued-packing/`.

Explicit 180- and 300-second runs at 4 mm still retained 343, despite reaching
76/138 correlations and 38/69 FFT candidates respectively. Each ended at its
requested budget and passed fresh native validation. This demonstrates continued
candidate evaluation, not continued count improvement or an optimality proof.
At 2 mm, an explicit 180-second run also retained 343, reaching 12 correlations
and six FFT candidates before budget exhaustion. Its peak host memory was about
462 MiB, versus 341 MiB for 60 seconds at 2 mm and about 90-91 MiB for the 4 mm
runs. Its saved layout also passed fresh native validation.


## Explicit import repair

Import settings stay in the editor until Start/Save. Inspection does not derive or
reset solver settings. Repair uses immutable source drafts, separate proposed
geometry, and a token checked by explicit acceptance before native preparation.

Welding scans spatial buckets, visits candidate representatives in original order,
and preserves the version-1 representative rule without an all-vertex pair scan.
Closed-surface reconstruction samples half-open triangle scanline parity into a
bounded grid; nested boundaries toggle material and separate regions share one
rigid asset. The locked Manifold LevelSet library reconstructs the interpolated
field at recorded resolution/isovalue, then the full-resolution native analyzer
checks the resulting mesh. This is an approximate new solid requiring acceptance,
not proof of the intended source shape. Thin features, cavities and components
below resolution may disappear. No sampled or certified surface bound is claimed.

Open boundaries, unmatched scanlines, unresolved topology and indeterminate
analysis remain blocked. Grid/work/output limits bound reconstruction. Preview
LOD and packing pitch never modify an accepted repair. Recipe replay, source
preservation and compatibility are defined in [ADR 0020](../spec/decisions/0020-explicit-desktop-repair.md).

## Conservative object envelopes

Desktop Import prepares and accepts an envelope in one native inspection call.
It retains exact source cleanup and frame mapping but skips source solid analysis;
the source draft remains indeterminate and cannot be accepted as a packing solid.
The versioned recipe records this surface-only policy, so reload reproduces the
same source bytes and mesh. Older recipes still replay their original full source
inspection. Full envelope analysis and source enclosure certification remain
mandatory. CLI `--packing-envelope auto --accept-repair auto` selects this policy;
automatic acceptance is rejected for welding and reconstruction.

CGAL Alpha Wrapping uses source bounds diagonal / 60 for alpha and / 2000 for offset, with seed 0. These are automatic preparation policy, unrelated to search pitch. The visitor polls cancellation and bounds refinement steps, Steiner insertions, triangulation cells and queued gates. Output is capped before normal solid analysis. Admission accounts for retained source, generator scratch and bounded output; allocator overhead and process RSS remain outside portable buffer accounting.

Independent enclosure checks traverse an envelope BVH against every retained source triangle using exact triangle predicates, reject all boundary contacts, then classify one point per connected source component. Vertex-only containment cannot pass a triangle crossing a concavity. Accepted and quantized envelopes must have no excluded cavity shells: otherwise surface enclosure would not imply material enclosure. Disconnected source components are checked separately. Finite source defects may remain inside the envelope; no manifold requirement is imposed on exported source triangles.

Packing uses the full envelope. Export rereads immutable source triangle ranges, checks exact expected float32 transforms, validates quantized envelopes with the existing placement validator, and checks source enclosure again. No display mesh or search field can authorize placement or export.
