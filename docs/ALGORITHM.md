# Packing algorithm: current design

This is the current search design. Unlike the [spec](../doc/spectrapack-spec.md),
everything here is **tunable**: change weights, page sizes, catalogs, schedules and
recovery behaviour freely when `python tools/scoreboard.py` shows an improvement and
the validity rules in spec §5.3 and §6 still hold. Update this file with the change;
no decision record is needed. The text below was moved from spec v0.2 §6 unchanged
and describes the code as of 2026-10-17. Historical rationale lives in
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
item-specific poses are used. The positive-clearance result remains 340.

Known gaps (2026-10-18, see the scoreboard `blocks` column):

- The baseline is a single uniform grid per orientation (315 Pryanik-2 copies in
  400×340×285 mm at 1/1 mm). A recursive block (guillotine) layout of bounding boxes
  reaches 343; the solver currently returns 340.
- Zero-clearance Pryanik reaches 360 where the block reference fits 388. The
  validator now completes the staggered proposal; subsequent spectral work still
  stops at a resource limit before using the whole 60-second allowance.

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

```text
prepare authoritative assets, baseline, fields, and orientation catalogs
best = best validated baseline, or a validated empty packing
while search budget remains:
    choose a reproducible fresh start or a neighborhood of best
    greedily insert identical copies using spectral candidates
    refine feasible poses to expose additional space
    try inserting another copy
    if blocked:
        remove a spatial neighborhood of k copies, k in {1, 2, 4, 8}
        attempt to reinsert those k copies plus at least one extra
        vary poses, candidate ranking, and neighborhood selection
    validate each proposed improvement before replacing best
    checkpoint best and emit its immutable solution revision
return best with the actual termination reason
```

Use a mixture of compact neighborhoods and dispersed removals. Identical-copy ID permutations are not useful restarts; diversify geometry, orientations, grid phases, and candidate choices. Keep a separate feasible working layout for equal-count exploration. Discard a failed neighborhood without damaging the saved incumbent.

Reserve budget for both a fresh greedy spectral run and subsequent improvement. Schedule multiple seeds within the chosen total budget, not one full budget per seed hidden from the user. All modes retain the AABB baseline. The full controller retains its initial spectral result as well. With identical configuration and a fixed work budget, continuing the same job cannot decrease the reported count.

Use coarse-to-fine passes where memory permits: start at `2h`, then `h`, preserving physical poses and rebuilding fields. A coarser pitch may miss feasible arrangements, but it cannot invalidate an already validated incumbent. Stop with `budget_exhausted`, `user_stopped`, `search_stalled`, `resource_limit`, or `error`; none of these means globally optimal.

In deterministic CPU mode, replace time-dependent search decisions with `search.work_budget.max_candidate_evaluations` and `max_search_passes`; stop at the first reached cap. One candidate evaluation is one proposed pose submitted to authoritative feasibility checking, including rejected proposals. A pass is one complete scheduled insertion/refinement/neighborhood attempt. Record both counters, RNG algorithm/state, and thread configuration. A watchdog may abort a hung run but cannot present that abort as a completed deterministic comparison. Wall-clock budgets remain the normal interactive mode.

## Resource handling

Spec v0.2 §6.6 required complete preflight accounting of every nested owner, a
50%-of-RAM host cap and terminal refusal for an unaffordable manual pitch. PR #16
(T015-P2, ADR 0019, not merged when this was written) adds a bounded recovery ladder
on top. Spec v0.3 §6.6 replaces those obligations with a simpler policy: estimate the
dominant buffers, fit the batch, degrade visibly on failure and keep searching.
Simplify the existing accounting toward that policy when it gets in the way.

Observed 2026-10-18 after the zero-clearance fix (Release, 60 s, 8 threads):
Pryanik-2 at 4 mm retains 340 copies with 1 mm pair clearance and 360 with zero
pair clearance, then stops with `FIELD_KERNEL_WORK_LIMIT` after four correlations.
At 2 mm both layouts survive `SPECTRAL_MEMORY_LIMIT` during spectral preflight.
The remaining work is to use the unspent time for useful improvements without
changing manual pitch or physical constraints. See the
[scoreboard artifacts](../.local/scoreboard/20261018-132633/scoreboard.json) and
[2 mm results](../.local/zero-clearance/pitch2-result.json).
