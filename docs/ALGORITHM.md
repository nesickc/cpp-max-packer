# Packing algorithm: current design

This is the current search design. Unlike the [spec](../doc/spectrapack-spec.md),
everything here is **tunable**: change weights, page sizes, catalogs, schedules and
recovery behaviour freely when `python tools/scoreboard.py` shows an improvement and
the validity rules in spec §5.3 and §6 still hold. Update this file with the change;
no decision record is needed. This file began with spec v0.2 §6 and is updated
through 2026-10-18. Historical rationale lives in
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
longest for columns. Clip mesh triangles into 32 x 32 row/height bins and retain
their column intervals. These envelopes preserve sloping edges and thin wings
better than whole-triangle bounding boxes, but fill cavities and are only search
screens. They never establish validity.

Sweep up to 64 rows/columns, 16 lateral phases, box-fitting and fractional column
pitches, and both alternating and progressive row offsets. Derive the minimum
within-row pitch from the envelopes, screen neighboring rows with the requested
Euclidean pair gap, and keep full AABB layer spacing. Work is bounded at eight
million units per orientation, with shared cancellation, deadline and memory
admission. Only the highest improving proposal from each orientation enters full
native validation; invalid or indeterminate results cannot replace the incumbent.
The search uses no asset names, saved poses or altered physical constraints, and
does not change manual voxel pitch or the fixed-work path.

This finds 350 validated Pryanik-2 copies in the unchanged positive-clearance
benchmark, exceeding the previous 343. The bounded envelope search can still miss
layouts; a larger proposal count alone is not evidence of a valid packing.

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
for timed runs, validate improving residual-block proposals (depths 1–3)
for timed runs, validate improving mesh-envelope stagger proposals
if FFT admission is affordable:
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
