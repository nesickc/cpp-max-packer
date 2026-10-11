# ADR 0018: bounded cardinal support certificates and stagger seeds

Status: accepted for staged implementation, 2026-10-11. No acceptance result is implied.

## Context and scope decision

T015-P1 explicitly requires a practical stagger/rearrangement improvement. The
306-copy Pryanik-2 baseline leaves a nominally feasible 336-copy alternating-row
layout. Native restoration of that proposal timed out after 90 seconds. The
existing validator can reach quadratic triangle scans for its 490 pairs whose
AABBs overlap. A separately native-valid 339-copy residual-fill witness remains
diagnostic evidence; residual filling is not selected as an additional strategy.

Projection over every accepted vertex gives a diagnostic minimum normalized gap
of 1.7587698928169317 mm for directions `(1,3,0)` and `(1,-3,0)`, against a required
1 mm pair clearance. This ordinary-double calculation motivates the change but
cannot authorize any placement.

Architecture and critical validity review recommend staged continuation. Expected
additional production scope is 250–450 geometry/accounting lines and 150–250 solver
lines. With publication, the projected total is 1,302–1,602 lines including 209
schema lines. This is an explicit growth checkpoint. No hull, BVH, persistent
cache, new dependency, rotation mode or search portfolio is included.

## Private geometry contract

After the existing authoritative AABB certificate is inconclusive and before
materializing placed solids, the validator may attempt a support certificate.
It applies only to positive pair clearance, exact cardinal poses and the same
accepted asset. Other inputs continue through the existing validator.

Use 30 fixed directions: signed permutations of `(0,1,k)`, for `k=1,2,3`, modulo
overall sign and duplicates. This set is closed under the existing 24 proper
cardinal rotations. One immutable validation-local table encloses the minimum
and maximum dot products over **all accepted vertices** using existing outward
interval arithmetic. Bind it to the exact accepted asset and publish the table
only after its complete construction. It has no lifetime beyond that complete
validation call and introduces no public geometry interface.

For each direction `n`, map `R^T n` by the exact signed permutation. Reverse an
interval as `[-U,-L]` when the table contains the opposite direction. Add the
outward projection of the translation separately for each posed copy. Different
rotations must use their own mapped supports.

For either ordering, let `g` be a downward-rounded positive lower bound on the
separation of the enclosing projection intervals. Accept only when all arithmetic
and the floating environment are supported and finite, and
`lower(g*g) >= upper(c*c*(n dot n))`. The squared direction norms are exact integers
2, 5 or 10. These enclosing slabs prove material disjointness and Euclidean surface
clearance for the accepted solids, including hollow or disconnected solids.
No square-root approximation, epsilon subtraction, vertex-distance approximation
or rounded world mesh may establish authority. Uncertain bounds fall back;
arithmetic, resource and cancellation failures retain existing failure semantics.

Charge the table, owner/control blocks and scratch through the existing geometry
budget. Charge actual projection/check work and poll during the vertex scan.
There is no vertex scan or allocation per pair. Final validation and checked
export independently rebuild the table and repeat the proof.

## Solver contract

Only after the geometry gate below passes, add one bounded physical stagger seed:
half-column shifts on alternate rows, with row pitch 7/8 of cleared AABB pitch,
using only the existing rotation catalog. Rank deterministic complete proposals
and submit whole layouts through authoritative validation. Preserve the baseline
and internal equal-count exploration. Do not repeatedly validate every inserted
prefix or introduce a second residual-fill strategy.

A positive construction margin is allowed only where physical slack permits;
zero-clearance exact-contact fixtures must retain their dimensions. Manual voxel
pitch, pair/wall clearances, dimensions and orientation permissions are unchanged.
Preserve baseline ordering and its budget. Debit at most 1,024 complete candidate
evaluations and 24 orientation passes from existing spectral phase limits, with
unchanged memory and existing phase validation-work caps. The earlier phrase
"aggregate validation-work cap" was imprecise: the existing baseline and spectral
paths have independent 1.3-billion validation-work ledgers. Preserve the baseline
ledger; the seed and later FFT share the original spectral ledger with no reset
or added allowance. Aggregate run statistics sum their actual work. Report actual
operator work and completion separately from any later FFT resource failure.

### Bounded integration and command diagnostics

For a positive candidate/pass tranche, admit the catalog-order prefix
`N = min(24, remaining spectral passes, catalog size, max_orientations)`. Debit one
pass before each authoritative oriented-bounds query, create at most two parity
descriptors per admitted orientation, and rank only those descriptors by count,
catalog order and parity. Zero candidate/pass allowance means no seed work.
Use the shortest physical extent for layers and the longest for columns, with
axis-index tie breaking. Keep one complete candidate vector at a time.

Seed queries, candidate evaluations, passes, native validation work and AABB tests
debit the spectral phase's existing ledgers. Add counters with checked arithmetic
and combine memory peaks by maximum. Check remaining allowances before later FFT
trial setup. Its existing two-trial guard remains relative to FFT-local passes,
so seed passes cannot silently suppress an otherwise affordable retained trial.
Preserve the caller's baseline/spectral apportionment in fixed-work and timed runs.

A fixed-size `StaggerSummary` on `SpectralOutcome` records the operator identity
`half_column_seven_eighths_rows`, state (`not_started`, `completed`, `interrupted`),
a small completion reason, actual `RunStats`, admitted/ranked proposal scope and
counts before/after. Completion means the admitted descriptors were exhausted or
count-dominated; it does not claim the whole catalog was explored. Candidate
tranche exhaustion with promising admitted descriptors remaining is interrupted.
Stop, deadline, resource or error retains the last complete validated incumbent.
Later FFT failure never overwrites this summary or becomes seed success.

Expose the summary as additive `diagnostics.stagger` in existing solve command
replies and every post-solver failure, including valid-empty success, measurement,
Stop-monitor and build/export admission failures. Existing generic command result
and error-details objects permit it. Keep persistent result/project schemas
unchanged; retain command diagnostics alongside comparison artifacts. This
additive command evidence introduces no scheduler, runtime phase or saved-result
compatibility change.

## Required staged evidence

1. Observe a practical failing analytic validation case before implementation.
   Derive expected clearance independently using dyadic slanted prisms and exact
   integer squared comparisons. Include short-gap rejection, all cardinal
   mappings, asymmetric independently rotated solids, cancellation/large
   coordinates, unsupported arithmetic, work/memory limits and Stop. Confirm
   cache identity, complete construction, one scan per validation and existing
   non-applicable/overlap/enclosure fallback.
2. Obtain a complete native verdict and cost report for the proposed 336-copy
   layout under the unchanged caps. A timeout or projection oracle does not pass.
   Reassess before solver integration if this fails.
3. Observe the selected stagger solver fixture fail, then pass with independent
   validation. Use the unchanged diamond prism `|x|+|y| <= 1`, height 1, in a
   5.5 × 4 × 1 box with pair clearance 1/8, wall clearance 0 and fixed identity.
   Its AABB lattice holds two; centers `(1,1,0.5)`, `(3.25,1,0.5)`,
   `(2.125,2.875,0.5)`, `(4.375,2.875,0.5)` independently witness four with
   same-row gap 1/4 and nearest cross-row gap `1/sqrt(2)`, both greater than 1/8.
   First establish native witness four and baseline two, then observe the old
   solver's count failure under these same constraints before implementation.
   The originally planned zero-clearance 5 × 3.75 × 1 witness instead reaches
   existing `KERNEL_CONTACTED_DEGENERACY` before the solver assertion; retain it
   and its failure as characterization, not a solver red or passing acceptance.
   This reviewed fixture substitution avoids expanding contact classification;
   practical 1/1 mm constraints and zero-contact behavior remain unchanged.
   It proves staggering, not removal/reinsertion or full AT-11 completion.
4. Run the frozen old/new practical comparisons, relevant Ulamok/full-Pryanik/
   invalid-simplified regressions, real desktop publication and file consistency,
   then obtain a final stable critical correctness/concurrency review.

Governing requirements remain GEO-04/GEO-06, SOL-04 and QA-01, with affected
AT-06/AT-07/AT-09/AT-10/AT-17 subsets. Full parent-ticket gates remain open.

## Compatibility

Advance the private validator revision because a new sound certificate can turn
formerly resource-limited/indeterminate checks into valid checks. Physical
acceptance and clearance semantics do not change. Existing persistent schemas,
accepted assets, hashes, dimensions and pose conventions remain unchanged; older
results must remain readable and undergo current independent validation where
required. Preserve historical diagnostic revisions rather than rewriting source
artifacts. No existing threading qualification is repeated without an affected
path or unresolved risk.
