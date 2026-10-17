---
name: spectrapack-spectral
description: Implement or review SpectraPack voxel fields, FFT correlation, orientation catalogs, count search, memory preflight, or CPU/Vulkan compute behavior.
---

# Spectral and solver work

Use `./tools/Read-Spec.ps1 -Section 6.3` from the repository root for FFT/fields, §6.1–6.2 and §6.4–6.5 for search, or §6.6 and §4.3 for compute. The [spec §6](../../../doc/spectrapack-spec.md#6-packing-algorithm) is canonical. Select corresponding SOL rows and AT-08/AT-10–AT-12/AT-16–AT-17 with `-Id`.

CPU correctness precedes GPU tuning. Algorithm parameters (weights, page sizes, catalogs) are tunable; record them in code and `docs/ALGORITHM.md`, not the spec.

- Correlation: record axis order, kernel trim origin, conjugation/reversal, inverse normalization, padding, and index-to-world mapping. Each padded axis is at least environment + kernel − 1. Compare small asymmetric fields at every translation with a direct integer oracle, including non-power-of-two dimensions and boundary/origin cases.
- Fields: one physical pitch, conservative solid occupancy, eroded-container mask, and full one-sided pair-clearance dilation. Unknown cells are occupied. Maintain reference counts so removal preserves overlapping blocked regions.
- Feasibility: FFT proposes; discrete rechecks and authoritative validation decide. Bad FFT error checks trigger CPU recomputation or an unreliable pass, never an impossibility conclusion. Retain a bounded physical refinement path for contact cases missed by voxels.
- Count: retain validated AABB and initial spectral incumbents. Trials/removals cannot lower the published best count. Enforce each rotation mode during refinement; record catalogs, seeds, and fixed-work counters.
- Resources: estimate the dominant buffers (padded FFT fields x batch) before allocating and pick a batch size that fits. On allocation failure shrink the batch or coarsen the pitch with a visible notice, and keep the incumbent. Never change physical constraints.
- Quality: for near-box objects, physical block layouts are a strong baseline. Check ideas against `python tools/scoreboard.py` rather than internal counters.

Run the affected oracle/regression tests and the scoreboard. GPU qualification needs an actual Windows Radeon run; CPU success cannot establish it.
